// SPDX-License-Identifier: BSD-2-Clause
/*
 * XeGfx, GU605 branch: the bridge between i915's own display code (KMS) and the macOS framebuffer
 * (docs/GU605-DISPLAY-DESIGN.md). Only reached when the kext turned the display code on (xg_kms_on == 1).
 *
 *   xg_kms_dump()       every connector i915 set up: status, TC mode and lanes, link and DSC caps, every mode i915
 *                       validated, the active CRTC's link configuration, the panel backlight
 *   xg_kms_set()        one display at a time: the named connector gets the requested mode on one CRTC, scanning
 *                       the framebuffer i915 took over from the firmware (cropped when the raster is smaller); every
 *                       other CRTC is turned off. Checked first (TEST_ONLY), then committed; atomic, so a refused
 *                       mode leaves the panel as it was
 *   xg_kms_backlight()  the eDP panel's brightness through intel_backlight (PWM or DPCD, whichever i915 chose)
 *   xg_kms_client_init() an in-kernel drm client whose hotplug callback counts and notifies the macOS side
 *   xg_kms_i2c()        DDC/CI (0x37) and EDID (0x50) over an external connector's DDC bus: i915's DP AUX adapter
 *                       (drm_dp_i2c_xfer, I2C-over-AUX) for DP and USB-C, GMBUS for HDMI (gu605-ddc)
 *
 * Written against the Linux DRM/i915 interfaces only; NullMoth's NVKMS bridge (PolyForm-Noncommercial) was a design
 * reference, none of its code is here.
 */
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/i2c.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_client.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_lock.h>
#include <drm/display/drm_dp.h>

#include "i915_drv.h"
#include "i915_reg.h"
#include "display/intel_backlight.h"
#include "display/intel_de.h"
#include "display/intel_display.h"
#include "display/intel_display_types.h"
#include "display/intel_dp.h"
#include "display/intel_tc.h"

#include "xg_kms.h"

extern int xg_kms_on;

/* set by the kext: called (task context) after a hotplug event changed what i915 reports */
void (*xg_kms_notify)(void);
unsigned int xg_kms_hotplugs;

static struct drm_framebuffer *xg_kms_fb;	/* the firmware image i915 took over; one reference held for good */
static struct drm_client_dev xg_kms_client;
static bool xg_kms_client_up;

struct xg_buf {
	char *p;
	size_t len, off;
};

static void
xg_out(struct xg_buf *b, const char *fmt, ...) __printflike(2, 3);

static void
xg_out(struct xg_buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (b->off + 1 >= b->len)
		return;
	va_start(ap, fmt);
	n = vsnprintf(b->p + b->off, b->len - b->off, fmt, ap);
	va_end(ap);
	if (n > 0)
		b->off = min(b->off + (size_t)n, b->len - 1);
}

static struct drm_i915_private *
xg_kms_i915(void)
{
	struct pci_dev *pdev = NULL;

	if (xg_kms_on != 1)
		return (NULL);
	while ((pdev = lkpi_pci_get_device(PCI_VENDOR_ID_INTEL, PCI_ANY_ID, pdev)) != NULL)
		if (pdev_to_i915(pdev) != NULL) {
			struct drm_i915_private *i915 = pdev_to_i915(pdev);

			pci_dev_put(pdev);
			return (HAS_DISPLAY(i915) ? i915 : NULL);
		}
	return (NULL);
}

/* mHz from the mode itself: drm_mode_vrefresh rounds to whole Hz */
static unsigned int
xg_mode_mhz(const struct drm_display_mode *m)
{
	u64 t = (u64)m->htotal * m->vtotal;

	return (t ? (unsigned int)div64_u64((u64)m->clock * 1000000ull, t) : 0);
}

static void
xg_kms_mode_from(struct xg_kms_mode *o, const struct drm_display_mode *m)
{
	o->w = m->hdisplay;
	o->h = m->vdisplay;
	o->clock_khz = m->clock;
	o->htotal = m->htotal;
	o->hss = m->hsync_start;
	o->hse = m->hsync_end;
	o->vtotal = m->vtotal;
	o->vss = m->vsync_start;
	o->vse = m->vsync_end;
	o->flags = m->flags;
	o->mhz = xg_mode_mhz(m);
	o->preferred = (m->type & DRM_MODE_TYPE_PREFERRED) != 0;
}

static const char *
xg_status(enum drm_connector_status s)
{
	return (s == connector_status_connected ? "connected" : s == connector_status_disconnected ? "disconnected" :
	    "unknown");
}

/* the framebuffer the firmware's plane became; taken from whichever primary plane scans it */
static struct drm_framebuffer *
xg_kms_bootfb(struct drm_device *dev)
{
	struct drm_plane *plane;

	if (xg_kms_fb != NULL)
		return (xg_kms_fb);
	drm_for_each_plane(plane, dev) {
		if (plane->type != DRM_PLANE_TYPE_PRIMARY || plane->state == NULL || plane->state->fb == NULL)
			continue;
		xg_kms_fb = plane->state->fb;
		drm_framebuffer_get(xg_kms_fb);
		break;
	}
	return (xg_kms_fb);
}

static void
xg_dump_connector(struct xg_buf *b, struct drm_connector *connector)
{
	struct intel_connector *ic = to_intel_connector(connector);
	struct intel_encoder *enc = intel_attached_encoder(ic);
	struct drm_display_mode *m;
	int n = 0;

	xg_out(b, "connector %s id %u type %d status %s edid %s", connector->name, connector->base.id,
	    connector->connector_type, xg_status(connector->status), connector->edid_blob_ptr ? "yes" : "no");
	if (connector->display_info.width_mm)
		xg_out(b, " size %ux%u mm", connector->display_info.width_mm, connector->display_info.height_mm);
	xg_out(b, " bpc %u\n", connector->display_info.bpc);
	if (enc != NULL) {
		xg_out(b, "  encoder %s port %c possible-crtcs 0x%x", enc->base.name, port_name(enc->port),
		    enc->base.possible_crtcs);
		if (intel_encoder_is_tc(enc)) {
			struct intel_digital_port *dig = enc_to_dig_port(enc);

			xg_out(b, " tc %s max-lanes %d pin-mask 0x%x",
			    intel_tc_port_in_dp_alt_mode(dig) ? "dp-alt" : intel_tc_port_in_tbt_alt_mode(dig) ? "tbt-alt" :
			    intel_tc_port_in_legacy_mode(dig) ? "legacy" : "disconnected",
			    intel_tc_port_max_lane_count(dig), intel_tc_port_get_pin_assignment_mask(dig));
		}
		xg_out(b, "\n");
		if (intel_encoder_is_dp(enc) && connector->status == connector_status_connected) {
			struct intel_dp *dp = enc_to_intel_dp(enc);

			xg_out(b, "  dp dpcd-rev 0x%02x sink-max-rate %d sink-max-lanes %d max-rate %d max-lanes %d edp %d"
			    " dsc 0x%02x dsc-bpp-max 0x%02x%02x dsc-slices 0x%02x fec 0x%02x\n",
			    dp->dpcd[DP_DPCD_REV], drm_dp_bw_code_to_link_rate(dp->dpcd[DP_MAX_LINK_RATE]),
			    dp->dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK, intel_dp_max_link_rate(dp),
			    intel_dp_max_lane_count(dp), intel_dp_is_edp(dp),
			    ic->dp.dsc_dpcd[DP_DSC_SUPPORT - DP_DSC_SUPPORT],
			    ic->dp.dsc_dpcd[DP_DSC_MAX_BITS_PER_PIXEL_HI - DP_DSC_SUPPORT],
			    ic->dp.dsc_dpcd[DP_DSC_MAX_BITS_PER_PIXEL_LOW - DP_DSC_SUPPORT],
			    ic->dp.dsc_dpcd[DP_DSC_SLICE_CAP_1 - DP_DSC_SUPPORT], ic->dp.fec_capability);
		}
	}
	if (connector->connector_type == DRM_MODE_CONNECTOR_eDP) {
		struct intel_panel *p = &ic->panel;

		xg_out(b, "  backlight present %d enabled %d level %u min %u max %u pwm-max %u\n",
		    p->backlight.present, p->backlight.enabled, p->backlight.level, p->backlight.min, p->backlight.max,
		    p->backlight.pwm_level_max);
	}
	list_for_each_entry(m, &connector->modes, head) {
		xg_out(b, "  mode %d %ux%u %u.%03u Hz clock %d htotal %u hss %u hse %u vtotal %u vss %u vse %u flags 0x%x%s\n",
		    n++, m->hdisplay, m->vdisplay, xg_mode_mhz(m) / 1000, xg_mode_mhz(m) % 1000, m->clock, m->htotal,
		    m->hsync_start, m->hsync_end, m->vtotal, m->vsync_start, m->vsync_end, m->flags,
		    (m->type & DRM_MODE_TYPE_PREFERRED) ? " preferred" : "");
		if (b->off + 200 >= b->len)
			break;
	}
	if (connector->state != NULL && connector->state->crtc != NULL) {
		struct intel_crtc_state *cs = to_intel_crtc_state(connector->state->crtc->state);
		const struct drm_display_mode *am = &cs->hw.adjusted_mode;

		xg_out(b, "  active crtc %s %ux%u %u.%03u Hz clock %d port-clock %d lanes %d pipe-bpp %d dsc %d"
		    " output-format %d\n", connector->state->crtc->name, am->hdisplay, am->vdisplay, xg_mode_mhz(am) / 1000,
		    xg_mode_mhz(am) % 1000, am->clock, cs->port_clock, cs->lane_count, cs->pipe_bpp,
		    cs->dsc.compression_enable, cs->output_format);
	}
}

/* every connector, probed afresh (i915 reads the EDID and the DPCD again); returns the text length */
int
xg_kms_dump(char *buf, unsigned long len)
{
	struct drm_i915_private *i915 = xg_kms_i915();
	struct xg_buf b = { buf, len, 0 };
	struct drm_connector_list_iter it;
	struct drm_connector *connector;
	struct drm_device *dev;
	struct drm_crtc *crtc;
	struct drm_framebuffer *fb;

	if (len)
		buf[0] = 0;
	if (i915 == NULL) {
		xg_out(&b, "no i915 display (xg_kms_on %d)\n", xg_kms_on);
		return ((int)b.off);
	}
	dev = &i915->drm;
	mutex_lock(&dev->mode_config.mutex);
	drm_connector_list_iter_begin(dev, &it);
	drm_for_each_connector_iter(connector, &it)
		connector->funcs->fill_modes(connector, dev->mode_config.max_width, dev->mode_config.max_height);
	drm_connector_list_iter_end(&it);
	/*
	 * drm_modeset_lock_all() takes mode_config.mutex itself (drm_modeset_lock.c) and drm_modeset_unlock_all() drops
	 * it: holding it here as well recursed on the non-recursive sx and panicked the GU605 at the first
	 * 'sysctl debug.xegfx_kms' after a KMS stage 4 (2026-11-09 17:06, xg_kms_dump -> drm_modeset_lock_all -> _sx_xlock).
	 */
	mutex_unlock(&dev->mode_config.mutex);
	drm_modeset_lock_all(dev);
	fb = xg_kms_bootfb(dev);
	xg_out(&b, "xegfx-kms hotplugs %u client %d cdclk %u kHz", xg_kms_hotplugs, xg_kms_client_up,
	    i915->display.cdclk.hw.cdclk);
	if (fb != NULL)
		xg_out(&b, " bootfb %ux%u pitch %u format 0x%08x modifier 0x%llx", fb->width, fb->height, fb->pitches[0],
		    fb->format->format, (unsigned long long)fb->modifier);
	xg_out(&b, "\n");
	drm_for_each_crtc(crtc, dev) {
		u32 f0 = 0, f1 = 0;

		/* the refresh the pipe really runs at: its frame counter over 250 ms */
		if (crtc->state->active) {
			f0 = intel_de_read(i915, PIPE_FRMCOUNT_G4X(i915, to_intel_crtc(crtc)->pipe));
			msleep(250);
			f1 = intel_de_read(i915, PIPE_FRMCOUNT_G4X(i915, to_intel_crtc(crtc)->pipe));
		}
		xg_out(&b, "crtc %s id %u enable %d active %d %ux%u measured %u Hz\n", crtc->name, crtc->base.id,
		    crtc->state->enable, crtc->state->active, crtc->state->mode.hdisplay, crtc->state->mode.vdisplay,
		    (f1 - f0) * 4);
	}
	drm_connector_list_iter_begin(dev, &it);
	drm_for_each_connector_iter(connector, &it)
		xg_dump_connector(&b, connector);
	drm_connector_list_iter_end(&it);
	drm_modeset_unlock_all(dev);
	return ((int)b.off);
}

static struct drm_connector *
xg_kms_connector(struct drm_device *dev, const char *name)
{
	struct drm_connector_list_iter it;
	struct drm_connector *connector, *found = NULL;

	drm_connector_list_iter_begin(dev, &it);
	drm_for_each_connector_iter(connector, &it)
		if (strcmp(connector->name, name) == 0) {
			drm_connector_get(connector);
			found = connector;
			break;
		}
	drm_connector_list_iter_end(&it);
	return (found);
}

/*
 * The mode on `name`: w x h at the refresh closest to mhz. w == 0: the preferred mode's raster; mhz == 0: the preferred
 * mode itself (no raster) or the fastest at that raster. Turns every other CRTC off. test != 0: atomic check only. Returns 0 or a negative errno; msg says why.
 */
int
xg_kms_set(const char *name, unsigned int w, unsigned int h, unsigned int mhz, int test, char *msg, unsigned long msglen)
{
	struct drm_i915_private *i915 = xg_kms_i915();
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_list_iter it;
	struct drm_connector *connector, *c;
	struct drm_atomic_state *state;
	struct drm_display_mode *m, *best = NULL, mode;
	struct drm_crtc_state *crtc_state;
	struct drm_connector_state *conn_state;
	struct drm_plane_state *plane_state;
	struct drm_crtc *crtc, *target = NULL;
	struct drm_framebuffer *fb;
	struct drm_device *dev;
	struct intel_encoder *enc;
	unsigned int bd = ~0u, d;
	int ret;

	if (msglen)
		msg[0] = 0;
	if (i915 == NULL) {
		snprintf(msg, msglen, "no i915 display");
		return (-ENODEV);
	}
	dev = &i915->drm;
	if ((connector = xg_kms_connector(dev, name)) == NULL) {
		snprintf(msg, msglen, "no connector %s", name);
		return (-ENOENT);
	}
	mutex_lock(&dev->mode_config.mutex);
	connector->funcs->fill_modes(connector, dev->mode_config.max_width, dev->mode_config.max_height);
	/* no raster given: the preferred mode's (the panel's native one), at the refresh asked for */
	if (w == 0)
		list_for_each_entry(m, &connector->modes, head)
			if ((m->type & DRM_MODE_TYPE_PREFERRED) || w == 0) {
				w = m->hdisplay;
				h = m->vdisplay;
				best = m;
				if (m->type & DRM_MODE_TYPE_PREFERRED)
					break;
			}
	if (mhz == 0 && best != NULL && (best->type & DRM_MODE_TYPE_PREFERRED))
		w = 0;	/* exactly the preferred mode */
	list_for_each_entry(m, &connector->modes, head) {
		if (w == 0)
			break;
		if (m->hdisplay != w || m->vdisplay != h || (m->flags & DRM_MODE_FLAG_INTERLACE))
			continue;
		d = mhz ? (unsigned int)abs((int)xg_mode_mhz(m) - (int)mhz) : ~0u - xg_mode_mhz(m);
		if (d < bd) {
			bd = d;
			best = m;
		}
	}
	if (best != NULL)
		drm_mode_copy(&mode, best);
	mutex_unlock(&dev->mode_config.mutex);
	if (best == NULL || connector->status != connector_status_connected) {
		snprintf(msg, msglen, "%s %s: no mode %ux%u near %u mHz", name, xg_status(connector->status), w, h, mhz);
		drm_connector_put(connector);
		return (-EINVAL);
	}
	enc = intel_attached_encoder(to_intel_connector(connector));
	state = drm_atomic_state_alloc(dev);
	if (state == NULL || enc == NULL) {
		if (state != NULL)
			drm_atomic_state_put(state);
		drm_connector_put(connector);
		snprintf(msg, msglen, "%s", enc == NULL ? "connector without encoder" : "no memory");
		return (enc == NULL ? -ENODEV : -ENOMEM);
	}
	drm_modeset_acquire_init(&ctx, 0);
	state->acquire_ctx = &ctx;
retry:
	target = NULL;
	ret = drm_modeset_lock_all_ctx(dev, &ctx);
	if (ret)
		goto out;
	fb = xg_kms_bootfb(dev);
	if (fb == NULL || mode.hdisplay > fb->width || mode.vdisplay > fb->height) {
		snprintf(msg, msglen, "mode %ux%u does not fit the boot framebuffer %ux%u", mode.hdisplay, mode.vdisplay,
		    fb ? fb->width : 0, fb ? fb->height : 0);
		ret = -ENOSPC;
		goto out;
	}
	/*
	 * The CRTC: the one scanning the boot framebuffer (pipe A: XeGfxIntelFB's VBL and the accelerator's zero-copy flip
	 * follow pipe A), else the connector's own, else any enabled one being freed (single display), else the first the
	 * encoder can use.
	 */
	drm_for_each_crtc(crtc, dev)
		if (crtc->state->enable && crtc->primary->state != NULL && crtc->primary->state->fb == fb &&
		    (enc->base.possible_crtcs & drm_crtc_mask(crtc))) {
			target = crtc;
			break;
		}
	if (target == NULL)
		target = connector->state->crtc;
	if (target == NULL)
		drm_for_each_crtc(crtc, dev)
			if (crtc->state->enable && (enc->base.possible_crtcs & drm_crtc_mask(crtc))) {
				target = crtc;
				break;
			}
	if (target == NULL)
		drm_for_each_crtc(crtc, dev)
			if (enc->base.possible_crtcs & drm_crtc_mask(crtc)) {
				target = crtc;
				break;
			}
	if (target == NULL) {
		snprintf(msg, msglen, "no CRTC for %s", name);
		ret = -ENODEV;
		goto out;
	}
	/* every other connector off its CRTC, every other CRTC and its primary plane off */
	drm_connector_list_iter_begin(dev, &it);
	drm_for_each_connector_iter(c, &it) {
		if (c == connector || c->state->crtc == NULL)
			continue;
		conn_state = drm_atomic_get_connector_state(state, c);
		if (IS_ERR(conn_state)) {
			ret = PTR_ERR(conn_state);
			break;
		}
		if ((ret = drm_atomic_set_crtc_for_connector(conn_state, NULL)) != 0)
			break;
	}
	drm_connector_list_iter_end(&it);
	if (ret)
		goto out;
	drm_for_each_crtc(crtc, dev) {
		if (crtc == target || !crtc->state->enable)
			continue;
		crtc_state = drm_atomic_get_crtc_state(state, crtc);
		if (IS_ERR(crtc_state)) {
			ret = PTR_ERR(crtc_state);
			goto out;
		}
		if ((ret = drm_atomic_set_mode_for_crtc(crtc_state, NULL)) != 0)
			goto out;
		crtc_state->active = false;
		plane_state = drm_atomic_get_plane_state(state, crtc->primary);
		if (IS_ERR(plane_state)) {
			ret = PTR_ERR(plane_state);
			goto out;
		}
		if ((ret = drm_atomic_set_crtc_for_plane(plane_state, NULL)) != 0)
			goto out;
		drm_atomic_set_fb_for_plane(plane_state, NULL);
	}
	crtc_state = drm_atomic_get_crtc_state(state, target);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto out;
	}
	if ((ret = drm_atomic_set_mode_for_crtc(crtc_state, &mode)) != 0)
		goto out;
	crtc_state->active = true;
	conn_state = drm_atomic_get_connector_state(state, connector);
	if (IS_ERR(conn_state)) {
		ret = PTR_ERR(conn_state);
		goto out;
	}
	if ((ret = drm_atomic_set_crtc_for_connector(conn_state, target)) != 0)
		goto out;
	plane_state = drm_atomic_get_plane_state(state, target->primary);
	if (IS_ERR(plane_state)) {
		ret = PTR_ERR(plane_state);
		goto out;
	}
	if ((ret = drm_atomic_set_crtc_for_plane(plane_state, target)) != 0)
		goto out;
	drm_atomic_set_fb_for_plane(plane_state, fb);
	plane_state->crtc_x = 0;
	plane_state->crtc_y = 0;
	plane_state->crtc_w = mode.hdisplay;
	plane_state->crtc_h = mode.vdisplay;
	plane_state->src_x = 0;
	plane_state->src_y = 0;
	plane_state->src_w = mode.hdisplay << 16;
	plane_state->src_h = mode.vdisplay << 16;
	plane_state->rotation = DRM_MODE_ROTATE_0;
	ret = test ? drm_atomic_check_only(state) : drm_atomic_commit(state);
out:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	if (ret == 0)
		snprintf(msg, msglen, "%s %s %ux%u %u.%03u Hz clock %d on %s", test ? "test ok" : "committed", name,
		    mode.hdisplay, mode.vdisplay, xg_mode_mhz(&mode) / 1000, xg_mode_mhz(&mode) % 1000, mode.clock,
		    target ? target->name : "?");
	else if (msglen && msg[0] == 0)
		snprintf(msg, msglen, "%s %s %ux%u %u.%03u Hz: error %d", test ? "test" : "commit", name, mode.hdisplay,
		    mode.vdisplay, xg_mode_mhz(&mode) / 1000, xg_mode_mhz(&mode) % 1000, ret);
	drm_atomic_state_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	drm_connector_put(connector);
	return (ret);
}

/*
 * eDP brightness in per mille of the panel's range (i915 maps it onto [min, max]); permille < 0 only reads.
 * Returns the current level in per mille, or a negative errno (-ENOTCONN when the panel is off).
 */
int
xg_kms_backlight(int permille, char *msg, unsigned long msglen)
{
	struct drm_i915_private *i915 = xg_kms_i915();
	struct drm_connector_list_iter it;
	struct drm_connector *connector, *edp = NULL;
	struct intel_panel *p;
	struct drm_device *dev;
	int ret;

	if (msglen)
		msg[0] = 0;
	if (i915 == NULL)
		return (-ENODEV);
	dev = &i915->drm;
	drm_connector_list_iter_begin(dev, &it);
	drm_for_each_connector_iter(connector, &it)
		if (connector->connector_type == DRM_MODE_CONNECTOR_eDP) {
			drm_connector_get(connector);
			edp = connector;
			break;
		}
	drm_connector_list_iter_end(&it);
	if (edp == NULL)
		return (-ENOENT);
	p = &to_intel_connector(edp)->panel;
	drm_modeset_lock(&dev->mode_config.connection_mutex, NULL);
	if (!p->backlight.present)
		ret = -EOPNOTSUPP;
	else if (edp->state->crtc == NULL)
		ret = -ENOTCONN;
	else {
		if (permille >= 0)
			intel_backlight_set_acpi(edp->state, (u32)clamp(permille, 20, 1000), 1000);
		ret = p->backlight.max > p->backlight.min ?
		    (int)(((u64)(p->backlight.level - min(p->backlight.level, p->backlight.min)) * 1000) /
		    (p->backlight.max - p->backlight.min)) : 0;
	}
	snprintf(msg, msglen, "backlight present %d enabled %d level %u min %u max %u -> %d", p->backlight.present,
	    p->backlight.enabled, p->backlight.level, p->backlight.min, p->backlight.max, ret);
	drm_modeset_unlock(&dev->mode_config.connection_mutex);
	drm_connector_put(edp);
	return (ret);
}

static int
xg_kms_hotplug(struct drm_client_dev *client)
{
	xg_kms_hotplugs++;
	if (xg_kms_notify != NULL)
		xg_kms_notify();
	return (0);
}

static const struct drm_client_funcs xg_kms_client_funcs = {
	.hotplug = xg_kms_hotplug,
};

/* after stage 4: an in-kernel client, so i915's hotplug events reach the macOS side (there is no fbdev client) */
int
xg_kms_client_init(void)
{
	struct drm_i915_private *i915 = xg_kms_i915();
	int ret;

	if (i915 == NULL)
		return (-ENODEV);
	if (xg_kms_client_up)
		return (0);
	ret = drm_client_init(&i915->drm, &xg_kms_client, "xegfx", &xg_kms_client_funcs);
	if (ret)
		return (ret);
	drm_client_register(&xg_kms_client);
	xg_kms_client_up = true;
	return (0);
}

/*
 * For the macOS framebuffer: the connector's validated modes (fill_modes ran in xg_kms_dump/xg_kms_set or here) as
 * plain numbers. Returns the count (at most max), -ENOENT without that connector; *connected and *active tell its state,
 * cur (when active) the mode on its CRTC.
 */
int
xg_kms_modes(const char *name, struct xg_kms_mode *out, int max, int *connected, struct xg_kms_mode *cur)
{
	struct drm_i915_private *i915 = xg_kms_i915();
	struct drm_connector *connector;
	struct drm_display_mode *m;
	struct drm_device *dev;
	int n = 0;

	if (i915 == NULL)
		return (-ENODEV);
	dev = &i915->drm;
	if ((connector = xg_kms_connector(dev, name)) == NULL)
		return (-ENOENT);
	mutex_lock(&dev->mode_config.mutex);
	connector->funcs->fill_modes(connector, dev->mode_config.max_width, dev->mode_config.max_height);
	*connected = connector->status == connector_status_connected;
	list_for_each_entry(m, &connector->modes, head) {
		if (n >= max)
			break;
		xg_kms_mode_from(&out[n++], m);
	}
	drm_modeset_lock(&dev->mode_config.connection_mutex, NULL);
	if (cur != NULL) {
		memset(cur, 0, sizeof(*cur));
		if (connector->state->crtc != NULL && connector->state->crtc->state->active)
			xg_kms_mode_from(cur, &connector->state->crtc->state->mode);
	}
	drm_modeset_unlock(&dev->mode_config.connection_mutex);
	mutex_unlock(&dev->mode_config.mutex);
	drm_connector_put(connector);
	return (n);
}

/* the connector's EDID as i915 last read it; returns its length (0 without one), -ENOENT without that connector */
int
xg_kms_edid(const char *name, unsigned char *buf, int len)
{
	struct drm_i915_private *i915 = xg_kms_i915();
	struct drm_connector *connector;
	struct drm_device *dev;
	int n = 0;

	if (i915 == NULL)
		return (-ENODEV);
	dev = &i915->drm;
	if ((connector = xg_kms_connector(dev, name)) == NULL)
		return (-ENOENT);
	mutex_lock(&dev->mode_config.mutex);
	if (connector->edid_blob_ptr != NULL) {
		n = min_t(int, len, (int)connector->edid_blob_ptr->length);
		memcpy(buf, connector->edid_blob_ptr->data, n);
	}
	mutex_unlock(&dev->mode_config.mutex);
	drm_connector_put(connector);
	return (n);
}

/*
 * gu605-ddc: one I2C transfer on the named connector's DDC adapter (connector->ddc: &intel_dp->aux.ddc for DP and
 * USB-C, the GMBUS pin's adapter for HDMI), so i915 does the AUX power, Type-C port lock, deferral retries and 16-byte
 * AUX chunking itself, under the adapter's bus lock. Safety, enforced here whatever the caller asks: only 7-bit 0x37
 * (DDC/CI) is written and read, 0x50 (EDID) only read after at most a one-byte offset; never the internal panel
 * (eDP); bounded sizes (XG_KMS_I2C_WMAX / _RMAX); the connector must be connected. Returns 0 or -errno.
 */
int
xg_kms_i2c(const char *name, struct xg_kms_i2c *x)
{
	struct drm_i915_private *i915 = xg_kms_i915();
	struct drm_connector *connector;
	struct i2c_msg msgs[2];
	int n = 0, ret;

	if (i915 == NULL)
		return (-ENODEV);
	if (x == NULL || (x->wlen == 0 && x->rlen == 0) || x->wlen > XG_KMS_I2C_WMAX || x->rlen > XG_KMS_I2C_RMAX ||
	    (x->wlen != 0 && x->wbuf == NULL) || (x->rlen != 0 && x->rbuf == NULL))
		return (-EINVAL);
	if (x->addr != 0x37 && !(x->addr == 0x50 && x->wlen <= 1 && x->rlen != 0))
		return (-EPERM);
	x->rdone = 0;
	if ((connector = xg_kms_connector(&i915->drm, name)) == NULL)
		return (-ENOENT);
	if (connector->connector_type == DRM_MODE_CONNECTOR_eDP || connector->connector_type == DRM_MODE_CONNECTOR_LVDS ||
	    connector->status != connector_status_connected || connector->ddc == NULL) {
		drm_connector_put(connector);
		return (-ENODEV);
	}
	memset(msgs, 0, sizeof(msgs));
	if (x->wlen != 0) {
		msgs[n].addr = (uint16_t)x->addr;
		msgs[n].flags = 0;
		msgs[n].len = (uint16_t)x->wlen;
		msgs[n].buf = x->wbuf;
		n++;
	}
	if (x->rlen != 0) {
		msgs[n].addr = (uint16_t)x->addr;
		msgs[n].flags = I2C_M_RD;
		msgs[n].len = (uint16_t)x->rlen;
		msgs[n].buf = x->rbuf;
		n++;
	}
	ret = i2c_transfer(connector->ddc, msgs, n);
	drm_connector_put(connector);
	if (ret < 0)
		return (ret);
	if (ret != n)
		return (-EIO);
	x->rdone = (int)x->rlen;
	return (0);
}
