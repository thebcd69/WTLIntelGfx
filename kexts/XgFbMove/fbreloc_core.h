/* Copied from samwu07/xegfx-thinkbook14plus-mtl (BSD-2-Clause, Copyright (c) 2026 samwu07), GU605 changes marked. */
/*
 * fbreloc_core.h -- move the firmware's boot framebuffer out of i915's GuC area in the GGTT.
 *
 * ThinkBook 14+ 155H (8086:7D55): the GOP scans out 3072x1920 (22.5 MiB) at GGTT 0xFE800000, which crosses
 * GUC_GGTT_TOP 0xFEE00000. i915 reserves [0xFEE00000, 4 GiB) for the uC images and maps the GuC binary at its start,
 * and pins rings just below it, so those panel rows show GPU data (WTLIntelGfx issue #13, XeGfx stage 4 -28).
 *
 * The fix here: copy the framebuffer's GGTT entries to a free range far from both ends (the same physical pages, so
 * the CPU side and macOS see no change) and point PLANE_SURF_1_A at it. The write latches at the next vblank; if the
 * live surface does not follow within a few frames, the old surface is put back.
 *
 * Shared by the UEFI driver (fbreloc.c) and the host test DLL (fbr_test_dll.c); register offsets as in i915
 * (display 14) and XeGfx's stage 2 probe.
 */
#ifndef FBRELOC_CORE_H
#define FBRELOC_CORE_H

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define REG_TRANSCONF_A		0x70008u
#define REG_FRMCOUNT_A		0x70040u
#define REG_PLANE_CTL_1A	0x70180u
#define REG_PLANE_STRIDE_1A	0x70188u
#define REG_PLANE_SIZE_1A	0x70190u
#define REG_PLANE_SURF_1A	0x7019cu
#define REG_PLANE_SURFLIVE_1A	0x701acu
#define REG_GFX_FLSH_CNTL	0x101008u	/* GFX_FLSH_CNTL_GEN6: GGTT TLB flush */
#define GSM_OFFSET		0x800000u	/* GGTT entries live at BAR0 + 8 MiB, 8 bytes each */
#define GUC_GGTT_TOP		0xfee00000u
#define GGTT_SIZE		0x100000000ull
#define FB_MAX_SIZE		0x4000000ull	/* 64 MiB: anything larger is a misread */

/* far from i915's low allocations and from its PIN_HIGH rings just below GUC_GGTT_TOP */
static const u32 fbr_targets[] = { 0x80000000u, 0x40000000u, 0xc0000000u };
#define FBR_NTARGETS	(sizeof(fbr_targets) / sizeof(fbr_targets[0]))

/* fbr_run's mode bits; 0/1 keep their old meaning (dry run / move) */
#define FBR_APPLY	1
/*
 * Take the first target even when none is free. Only for a caller that runs before any GPU driver in this boot
 * (XgFbMove): then every GGTT entry outside the framebuffer is a leftover of an earlier boot or OS (the GGTT lives in
 * DRAM and survives a warm restart), and the display engine reads nothing but the enabled planes' surfaces, which a
 * reclaimed target never overlaps. 2026-11-02 15:09: after a panic restart all three targets held foreign entries,
 * 1.1 refused (-5) and stage 4 drew the GuC image as a band.
 */
#define FBR_RECLAIM	2
/*
 * GU605 branch: move only a surface that overlaps i915's GuC slot [GUC_GGTT_TOP, GUC_SLOT_END). A 2560x1600 GOP
 * framebuffer sits at 0xFF000000 (WTL's Legion, the same panel size as the GU605's): above the slot, inside i915's
 * reserved top, and XeGfx runs it unmoved (N-256). FBR_FORCE keeps samwu07's rule (move anything that ends above
 * GUC_GGTT_TOP).
 */
#define FBR_FORCE	4
#define GUC_SLOT_END	0xff000000u

enum {
	FBR_MOVED = 1,
	FBR_NOT_NEEDED = 0,		/* the surface already ends below GUC_GGTT_TOP */
	FBR_PIPE_OFF = -1,
	FBR_PLANE_OFF = -2,
	FBR_PTE_INVALID = -3,
	FBR_BAD_SIZE = -4,
	FBR_NO_TARGET = -5,
	FBR_NO_LATCH = -6,		/* the panel did not take the new surface; the old one was restored */
};

struct fbr_result {
	int status;
	u32 old_surf, new_surf, pages;
	u64 size;
	u32 reclaimed;			/* 1: the target was not free and FBR_RECLAIM took it */
	u32 tvalid[FBR_NTARGETS];	/* per target before the move: valid entries */
	u32 tsame[FBR_NTARGETS];	/* ...and entries equal to the framebuffer's at the same offset */
};

/* test builds advance the fake hardware from here; NULL in the driver */
static void (*fbr_poll_hook)(volatile u8 *bar);

static inline u32 fbr_rd(volatile u8 *bar, u32 off) { return *(volatile u32 *)(bar + off); }
static inline void fbr_wr(volatile u8 *bar, u32 off, u32 v) { *(volatile u32 *)(bar + off) = v; }
static inline volatile u64 *fbr_gtt(volatile u8 *bar) { return (volatile u64 *)(bar + GSM_OFFSET); }

/*
 * a target is free when none of its entries is valid, or all of them hold the same (scratch) entry, or it already
 * holds exactly this framebuffer's entries: the copy made in an earlier boot, since the GGTT survives a warm restart
 * (2026-11-02: the second boot found 0x80000000 taken by its own copy and used 0x40000000)
 */
static int fbr_overlaps(u32 a, u32 an, u32 b, u32 bn) { return a < b + bn && b < a + an; }

static int fbr_target_free(volatile u64 *g, u32 t, u32 pages, u32 src)
{
	u64 first = g[t];
	u32 i;

	if (t + pages > (u32)(GGTT_SIZE >> 12) || fbr_overlaps(t, pages, src, pages))
		return 0;
	for (i = 0; i < pages && g[t + i] == g[src + i]; i++)
		;
	if (i == pages)
		return 1;
	for (i = 0; i < pages; i++) {
		u64 e = g[t + i];
		if ((e & 1) && e != first)
			return 0;
	}
	/* a shared valid entry must not be one of the framebuffer's own pages */
	if (first & 1)
		for (i = 0; i < pages; i++)
			if (g[src + i] == first)
				return 0;
	return 1;
}

/* a reclaimed target must stay clear of every surface the display engine reads: plane 1 of each enabled pipe A-D
   (pipe stride 0x1000, as i915's _PIPE() for display 14). Pipe A's own surface is the source and is checked too. */
static int fbr_target_scanned(volatile u8 *bar, u32 t, u32 pages)
{
	u32 p, s, stride, h;

	for (p = 0; p < 4; p++) {
		if (!(fbr_rd(bar, REG_TRANSCONF_A + p * 0x1000u) & 0x80000000u) ||
		    !(fbr_rd(bar, REG_PLANE_CTL_1A + p * 0x1000u) & 0x80000000u))
			continue;
		s = fbr_rd(bar, REG_PLANE_SURF_1A + p * 0x1000u) >> 12;
		stride = fbr_rd(bar, REG_PLANE_STRIDE_1A + p * 0x1000u) & 0x7ffu;
		h = ((fbr_rd(bar, REG_PLANE_SIZE_1A + p * 0x1000u) >> 16) & 0xffffu) + 1;
		if (fbr_overlaps(t, pages, s, (u32)(((u64)stride * 64 * h + 0xfff) >> 12)))
			return 1;
	}
	return 0;
}

static int fbr_wait_live(volatile u8 *bar, u32 want)
{
	u32 f0 = fbr_rd(bar, REG_FRMCOUNT_A), n;

	for (n = 0; n < 1000000u; n++) {
		if (fbr_poll_hook)
			fbr_poll_hook(bar);
		if ((fbr_rd(bar, REG_PLANE_SURFLIVE_1A) & ~0xfffu) == want)
			return 1;
		if (fbr_rd(bar, REG_FRMCOUNT_A) - f0 > 4)
			return (fbr_rd(bar, REG_PLANE_SURFLIVE_1A) & ~0xfffu) == want;
	}
	return 0;
}

/* checks everything; moves only with FBR_APPLY in 'mode'. Returns r->status. */
static int fbr_run(volatile u8 *bar, int mode, struct fbr_result *r)
{
	volatile u64 *g = fbr_gtt(bar);
	u32 surfreg, surf, stride, h, src, t = 0, i, k;
	u64 size;

	r->status = 0; r->old_surf = 0; r->new_surf = 0; r->pages = 0; r->size = 0; r->reclaimed = 0;
	for (k = 0; k < FBR_NTARGETS; k++)
		r->tvalid[k] = r->tsame[k] = 0;
	if (!(fbr_rd(bar, REG_TRANSCONF_A) & 0x80000000u))
		return r->status = FBR_PIPE_OFF;
	if (!(fbr_rd(bar, REG_PLANE_CTL_1A) & 0x80000000u))
		return r->status = FBR_PLANE_OFF;
	surfreg = fbr_rd(bar, REG_PLANE_SURF_1A);
	surf = surfreg & ~0xfffu;
	stride = fbr_rd(bar, REG_PLANE_STRIDE_1A) & 0x7ffu;	/* linear: 64-byte units, as XeGfx reads it */
	h = ((fbr_rd(bar, REG_PLANE_SIZE_1A) >> 16) & 0xffffu) + 1;
	size = (u64)stride * 64 * h;
	r->old_surf = surf;
	r->size = size;
	if (stride == 0 || size > FB_MAX_SIZE || (u64)surf + size > GGTT_SIZE)
		return r->status = FBR_BAD_SIZE;
	r->pages = (u32)((size + 0xfff) >> 12);
	if ((u64)surf + size <= GUC_GGTT_TOP)
		return r->status = FBR_NOT_NEEDED;
	if (!(mode & FBR_FORCE) && surf >= GUC_SLOT_END)
		return r->status = FBR_NOT_NEEDED;	/* clear of the GuC slot (see FBR_FORCE) */
	src = surf >> 12;
	for (i = 0; i < r->pages; i++)
		if (!(g[src + i] & 1))
			return r->status = FBR_PTE_INVALID;
	for (k = 0; k < FBR_NTARGETS; k++) {	/* what each target holds, for the report */
		t = fbr_targets[k] >> 12;
		for (i = 0; i < r->pages && t + i < (u32)(GGTT_SIZE >> 12); i++) {
			r->tvalid[k] += (u32)(g[t + i] & 1);
			r->tsame[k] += g[t + i] == g[src + i];
		}
	}
	for (k = 0; k < FBR_NTARGETS; k++) {
		t = fbr_targets[k] >> 12;
		if (fbr_target_free(g, t, r->pages, src))
			break;
	}
	if (k == FBR_NTARGETS && (mode & FBR_RECLAIM))
		for (k = 0; k < FBR_NTARGETS; k++) {
			t = fbr_targets[k] >> 12;
			if (t + r->pages <= (u32)(GGTT_SIZE >> 12) && !fbr_target_scanned(bar, t, r->pages)) {
				r->reclaimed = 1;
				break;
			}
		}
	if (k == FBR_NTARGETS)
		return r->status = FBR_NO_TARGET;
	r->new_surf = t << 12;
	if (!(mode & FBR_APPLY))
		return r->status = FBR_MOVED;	/* would move */
	for (i = 0; i < r->pages; i++)
		g[t + i] = g[src + i];
	(void)g[t + r->pages - 1];		/* posting read */
	fbr_wr(bar, REG_GFX_FLSH_CNTL, 1);
	(void)fbr_rd(bar, REG_GFX_FLSH_CNTL);
	fbr_wr(bar, REG_PLANE_SURF_1A, r->new_surf | (surfreg & 0xfffu));
	if (!fbr_wait_live(bar, r->new_surf)) {
		fbr_wr(bar, REG_PLANE_SURF_1A, surfreg);
		(void)fbr_wait_live(bar, surf);
		r->new_surf = 0;
		return r->status = FBR_NO_LATCH;
	}
	return r->status = FBR_MOVED;
}

#endif
