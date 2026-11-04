// gu605.h: ASUS ROG Zephyrus G16 GU605MY (Meteor Lake-H, iGPU 8086:7D55, Samsung ATNA60DL01 OLED eDP) additions to
// XeGfxIntelFB. Everything here is reached only when the provider is device 7D55; other GPUs keep the 0.9.4 behaviour.
//
//  * xifbGu605OwnsPanel(): the iGPU is the panel's scan-out (G-Helper Standard or Eco mode). In Ultimate the MUX puts
//    the panel on the NVIDIA GPU and the boot console lives in NVIDIA's BAR1; then the iGPU has no lit pipe and this
//    framebuffer must not start, or the console range would be added to GFX0 and a second framebuffer would sit on
//    NVIDIA's memory next to NullMoth's. Read-only: BAR0 is mapped only when the firmware left memory decoding on.
//  * Panel brightness over DPCD (the panel is an OLED: no PWM), following Linux i915 intel_dp_aux_backlight.c and
//    drm_dp_helper.c (drm_edp_backlight_*): Intel's HDR interface (DPCD 0x340-0x35x, nits, range from the EDID's HDR
//    static metadata, else 512) or VESA eDP (AUX level, or luminance on eDP 1.5). Probing is read-only except the
//    Intel source OUI when nothing else works; the DPCD control is switched on at the first slider/key change, never
//    at boot; floor 5 %. Boot-args: -xeintelfbnobl (off), xeintelfbbl=intel|vesa (force), xeintelfbnits=<max>.
//
// Register and DPCD numbers: Linux i915 intel_dp_aux_regs.h, intel_dp_aux_backlight.c, drm_dp.h (MIT).
// <wtl> fork: thebcd69/WTLIntelGfx gu605-mtl | BSD-2-Clause, as XeGfxIntelFB </wtl>
#pragma once

#define XIFB_GU605_DEV      0x7d55
#define XIFB_TRANSCONF_A    0x70008
#define XIFB_PLANE_CTL_1A   0x70180
#define XIFB_PLANE_SIZE_1A  0x70190

static bool xifbIsGu605(IOService *provider)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    return pci && pci->configRead16(kIOPCIConfigVendorID) == 0x8086 && pci->configRead16(kIOPCIConfigDeviceID) == XIFB_GU605_DEV;
}

// true when pipe A's transcoder and plane 1 are on and the plane is the boot console's size
static bool xifbGu605OwnsPanel(IOService *provider)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;
    if (!(pci->configRead16(kIOPCIConfigCommand) & kIOPCICommandMemorySpace)) {
        XIFBLOG("gu605: iGPU memory decoding is off (MUX on the NVIDIA GPU?): not the panel's GPU");
        return false;
    }
    IOMemoryMap *m = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    if (!m || m->getLength() < 0x80000) { OSSafeReleaseNULL(m); XIFBLOG("gu605: BAR0 not mappable"); return false; }
    volatile uint32_t *r = (volatile uint32_t *)m->getVirtualAddress();
    const uint32_t conf = r[XIFB_TRANSCONF_A / 4], ctl = r[XIFB_PLANE_CTL_1A / 4], size = r[XIFB_PLANE_SIZE_1A / 4];
    m->release();
    const uint32_t w = (size & 0xffff) + 1, h = ((size >> 16) & 0xffff) + 1;
    PE_Video v; bzero(&v, sizeof v);
    const bool cons = IOService::getPlatform() && IOService::getPlatform()->getConsoleInfo(&v) == kIOReturnSuccess && v.v_baseAddr;
    const bool lit = conf != ~0u && (conf & (1u << 31)) && (ctl & (1u << 31));
    const bool same = cons && v.v_width == w && v.v_height == h;
    XIFBLOG("gu605: pipe A transconf %08x plane %08x %ux%u, console %lux%lu at %#llx -> %s", conf, ctl, w, h,
            cons ? v.v_width : 0, cons ? v.v_height : 0, cons ? (unsigned long long)v.v_baseAddr : 0ull,
            lit && same ? "the iGPU scans out the panel (Standard/Eco)" : "the panel is not on the iGPU (Ultimate): staying off");
    return lit && same;
}

// ---- DPCD over native AUX (AUX A), with the request/reply framing of xifbAux() ----
static IOLock *gXifbAuxLock;
static int xifbDpcdRead(uint32_t addr, uint8_t *buf, int n)
{
    if (!gRegs || n < 1 || n > 16) return -1;
    uint8_t tx[4] = { (uint8_t)(0x90 | ((addr >> 16) & 0xf)), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(n - 1) }, rx[20];
    for (int t = 0; t < 5; t++) {
        int got = xifbAux(tx, 4, rx, n + 1);
        if (got >= 1 && (rx[0] & 0x30) == 0 && got == n + 1) { bcopy(rx + 1, buf, n); return n; }
        IODelay(400);   // DEFER or a short reply: retry as drm_dp_dpcd_access does
    }
    return -1;
}
static int xifbDpcdWrite(uint32_t addr, const uint8_t *buf, int n)
{
    if (!gRegs || n < 1 || n > 16) return -1;
    uint8_t tx[20] = { (uint8_t)(0x80 | ((addr >> 16) & 0xf)), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(n - 1) }, rx[2];
    bcopy(buf, tx + 4, n);
    for (int t = 0; t < 5; t++) {
        int got = xifbAux(tx, 4 + n, rx, 1);
        if (got >= 1 && (rx[0] & 0x30) == 0) return n;
        IODelay(400);
    }
    return -1;
}

enum { XIFB_BL_NONE = 0, XIFB_BL_INTEL_HDR = 1, XIFB_BL_VESA = 2, XIFB_BL_VESA_LUMINANCE = 3 };
struct XifbBacklight {
    int kind = XIFB_BL_NONE;
    uint32_t max = 0, min = 0;          // level range of the chosen interface (nits for HDR/luminance, PWM steps for VESA)
    bool vesa16 = false, auxEnable = false, enabled = false;
    uint8_t edp[4] = {}, hdrCtl = 0, pn = 0, pnWrite = 0xff;
    int startPercent = 100;
};
static const char *xifbBlName(int k) { return k == XIFB_BL_INTEL_HDR ? "Intel HDR (nits)" : k == XIFB_BL_VESA ? "VESA eDP (AUX level)" :
                                              k == XIFB_BL_VESA_LUMINANCE ? "VESA eDP 1.5 luminance (nits)" : "none"; }

// CTA-861 HDR static metadata in the EDID's extension block: max frame-average and min luminance codes, as
// drm_calculate_luminance_range() turns them into nits. false = no HDR static metadata (type 1).
static bool xifbEdidLuminance(const uint8_t *e, int len, uint32_t *maxNits, uint32_t *minNits)
{
    static const uint8_t pre[32] = { 50, 51, 52, 53, 55, 56, 57, 58, 59, 61, 62, 63, 65, 66, 68, 69,
                                     71, 72, 74, 75, 77, 79, 81, 82, 84, 86, 88, 90, 92, 94, 96, 98 };
    if (len < 256 || e[128] != 0x02) return false;
    const uint8_t *x = e + 128; int end = x[2];
    if (end < 4 || end > 127) return false;
    for (int o = 4; o < end; ) {
        const uint8_t *db = x + o; int n = db[0] & 0x1f;
        if (o + 1 + n > end) break;
        if ((db[0] >> 5) == 7 && n >= 6 && db[1] == 0x06 && (db[3] & 0x01)) {
            uint32_t cv = db[5], q = cv >> 5, r = cv & 31, mx = (1u << q) * pre[r];
            uint32_t qq = (db[6] + 127) / 255;
            *maxNits = mx; *minNits = mx * ((qq * qq + 50) / 100);
            return true;
        }
        o += 1 + n;
    }
    return false;
}

// Which DPCD backlight interface to use, in i915's order (intel_dp_aux_init_backlight_funcs with VBT unknown, i.e.
// enable_dpcd_backlight=1): Intel HDR first when eDP <= 1.4b and the EDID has HDR static metadata (or boot-arg
// xeintelfbbl=intel), else VESA (luminance on eDP 1.5 panels that can, AUX level otherwise). The Intel interface
// needs Intel's source OUI in DPCD 0x300; it is written only when the panel has no usable VESA interface, because
// some panels lose VESA control once they see Intel's OUI (i915's comment). Read-only otherwise.
static void xifbBacklightProbe(XifbBacklight *b, const uint8_t *edid, int edidLen)
{
    int off = 0; char want[16] = {};
    if (PE_parse_boot_argn("-xeintelfbnobl", &off, sizeof off)) { XIFBLOG("gu605 backlight: off by boot-arg"); return; }
    PE_parse_boot_argn("xeintelfbbl", want, sizeof want);   // intel | vesa
    if (gXifbAuxLock) IOLockLock(gXifbAuxLock);
    uint8_t *ed = b->edp, tc[4] = {}, oui[3] = {}, mode = 0, lv[3] = {};
    if (xifbDpcdRead(0x700, ed, 4) != 4) { XIFBLOG("gu605 backlight: DPCD 0x700 unreadable over AUX A"); if (gXifbAuxLock) IOLockUnlock(gXifbAuxLock); return; }
    xifbDpcdRead(0x300, oui, 3);
    uint32_t hmax = 0, hmin = 0; const bool hdrMeta = xifbEdidLuminance(edid, edidLen, &hmax, &hmin);
    const bool intelOui = oui[0] == 0x00 && oui[1] == 0xaa && oui[2] == 0x01;
    // VESA possible? (drm_edp_backlight_supported + check_if_vesa_backlight_possible, or eDP 1.5 luminance)
    uint8_t bmin = 0, bmax = 0, bcnt = 0;
    const bool lum = ed[0] >= 0x06 && (ed[3] & 0x10) && (ed[3] & 0x40);
    const bool vesaLevel = (ed[1] & 0x01) && (ed[2] & 0x02) && xifbDpcdRead(0x725, &bmin, 1) == 1 && xifbDpcdRead(0x726, &bmax, 1) == 1 &&
                           (bmin & 0x1f) >= 1 && (bmax & 0x1f) >= (bmin & 0x1f);
    const bool vesa = lum || vesaLevel;
    bool intel = false;
    if (ed[0] <= 0x05 && strcmp(want, "vesa") != 0) {
        bool rd = xifbDpcdRead(0x340, tc, 4) == 4;
        if (rd && tc[0] < 1 && !intelOui && (!vesa || !strcmp(want, "intel"))) {
            static const uint8_t io[3] = { 0x00, 0xaa, 0x01 };
            if (xifbDpcdWrite(0x300, io, 3) == 3) { IOSleep(30); rd = xifbDpcdRead(0x340, tc, 4) == 4; XIFBLOG("gu605 backlight: wrote Intel source OUI"); }
        }
        intel = rd && tc[0] >= 1 && (tc[1] & 0x10) && (tc[2] & 0x01) && (hdrMeta || !strcmp(want, "intel"));
    }
    XIFBLOG("gu605 backlight: eDP DPCD 0x700 %02x %02x %02x %02x, source OUI %02x%02x%02x, Intel tcon %02x %02x %02x %02x, "
            "EDID HDR metadata %s (max %u min %u nits), VESA %s", ed[0], ed[1], ed[2], ed[3], oui[0], oui[1], oui[2],
            tc[0], tc[1], tc[2], tc[3], hdrMeta ? "yes" : "no", hmax, hmin, lum ? "luminance" : vesaLevel ? "aux level" : "no");
    if (intel) {
        uint32_t nits = 0;
        b->kind = XIFB_BL_INTEL_HDR;
        if (PE_parse_boot_argn("xeintelfbnits", &nits, sizeof nits) && nits >= 50 && nits <= 4000) { b->max = nits; b->min = 0; }
        else if (hdrMeta && hmax) { b->max = hmax; b->min = hmin; }
        else { b->max = 512; b->min = 0; }
        uint8_t ctl = 0, cur[2] = {};
        if (xifbDpcdRead(0x344, &ctl, 1) == 1) b->hdrCtl = ctl;
        if ((ctl & 0x10) && xifbDpcdRead(0x354, cur, 2) == 2) {   // INTEL_EDP_HDR_TCON_BRIGHTNESS_AUX_ENABLE = BIT(4)
            uint32_t now = cur[0] | cur[1] << 8; b->enabled = true;
            b->startPercent = (int)(now >= b->max ? 100 : now * 100 / (b->max ? b->max : 1));
        }
    } else if (vesa) {
        b->auxEnable = (ed[1] & 0x04) != 0; b->vesa16 = (ed[2] & 0x04) != 0;
        xifbDpcdRead(0x721, &mode, 1);
        if (lum) {
            b->kind = XIFB_BL_VESA_LUMINANCE; b->max = hdrMeta && hmax ? hmax : 512; b->min = 0;
            if ((mode & 3) == 2 && xifbDpcdRead(0x734, lv, 3) == 3) {
                uint32_t now = (lv[0] | lv[1] << 8 | lv[2] << 16) / 1000; b->enabled = true; b->startPercent = (int)(now >= b->max ? 100 : now * 100 / b->max);
            }
        } else {
            b->kind = XIFB_BL_VESA;
            xifbDpcdRead(0x724, &bcnt, 1);
            uint8_t pn = bcnt & 0x1f, pmin = bmin & 0x1f, pmax = bmax & 0x1f;
            if (pn < pmin) { pn = pmin; b->pnWrite = pn; }
            if (pn > pmax) pn = pmax;
            b->pn = pn; b->max = (1u << pn) - 1; b->min = 0;
            if ((mode & 3) == 2 && xifbDpcdRead(0x722, lv, b->vesa16 ? 2 : 1) >= 1) {
                uint32_t now = b->vesa16 ? (uint32_t)(lv[0] << 8 | lv[1]) : lv[0]; b->enabled = true;
                b->startPercent = (int)(now >= b->max ? 100 : now * 100 / (b->max ? b->max : 1));
            }
        }
    }
    if (gXifbAuxLock) IOLockUnlock(gXifbAuxLock);
    XIFBLOG("gu605 backlight: %s, range %u..%u, %s, now %d%%", xifbBlName(b->kind), b->min, b->max,
            b->enabled ? "DPCD control already on" : "DPCD control not on yet (first slider move turns it on)", b->startPercent);
}

// percent of the interface range, floor 5 %; the first call switches DPCD control on the way i915's enable hooks do
static bool xifbBacklightSet(XifbBacklight *b, uint32_t percent)
{
    if (b->kind == XIFB_BL_NONE) return false;
    if (percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    const uint32_t span = b->max > b->min ? b->max - b->min : 0;
    const uint32_t lvl = b->min + (uint32_t)((uint64_t)span * percent / 100);
    bool ok = false;
    if (gXifbAuxLock) IOLockLock(gXifbAuxLock);
    if (b->kind == XIFB_BL_INTEL_HDR) {
        if (!b->enabled) {
            uint8_t ctl = (uint8_t)(b->hdrCtl | 0x10);
            if (ctl == b->hdrCtl || xifbDpcdWrite(0x344, &ctl, 1) == 1) b->hdrCtl = ctl;
            uint8_t ov[4] = { (uint8_t)b->min, (uint8_t)(b->min >> 8), (uint8_t)b->max, (uint8_t)(b->max >> 8) };
            xifbDpcdWrite(0x34a, ov, 4);   // INTEL_EDP_HDR_PANEL_LUMINANCE_OVERRIDE, as i915's setup
            b->enabled = (b->hdrCtl & 0x10) != 0;
        }
        uint8_t v[4] = { (uint8_t)lvl, (uint8_t)(lvl >> 8), 0, 0 };
        ok = b->enabled && xifbDpcdWrite(0x354, v, 4) == 4;
    } else {
        if (!b->enabled) {
            if (b->pnWrite != 0xff) xifbDpcdWrite(0x724, &b->pnWrite, 1);
            uint8_t m = (uint8_t)(0x02 | (b->kind == XIFB_BL_VESA_LUMINANCE ? 0x80 : 0));   // DPCD mode (+ luminance enable)
            bool mw = xifbDpcdWrite(0x721, &m, 1) == 1;
            if (mw && b->auxEnable) { uint8_t c = 0; if (xifbDpcdRead(0x720, &c, 1) == 1 && !(c & 1)) { c |= 1; xifbDpcdWrite(0x720, &c, 1); } }
            b->enabled = mw;
        }
        if (b->kind == XIFB_BL_VESA_LUMINANCE) {
            uint32_t mn = lvl * 1000; uint8_t v[3] = { (uint8_t)mn, (uint8_t)(mn >> 8), (uint8_t)(mn >> 16) };
            ok = b->enabled && xifbDpcdWrite(0x734, v, 3) == 3;
        } else {
            uint8_t v[2] = { (uint8_t)(b->vesa16 ? lvl >> 8 : lvl), (uint8_t)lvl };
            ok = b->enabled && xifbDpcdWrite(0x722, v, b->vesa16 ? 2 : 1) >= 1;
        }
    }
    if (gXifbAuxLock) IOLockUnlock(gXifbAuxLock);
    return ok;
}
