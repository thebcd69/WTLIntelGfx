/*
 * xeflip_core.h -- zero-copy scan-out on the Intel panel for XeGfxAccel (NMX_XEGFX), the hardware half.
 *
 * Today every frame WindowServer composites on the GPU is copied by the CPU into the boot framebuffer (22.5 MiB at
 * 3072x1920), because upstream's zero-copy flip is an NVIDIA display-engine call. The Intel display engine reads the
 * panel through PLANE_SURF, a GGTT address, so it can scan the composited surface directly: map the surface's pages
 * into a GGTT slot once, then point PLANE_SURF at the slot. The write latches at the next vblank (no tearing), so a
 * frame costs one register write. The same two moves XgFbMove makes once at boot (fbreloc_core.h).
 *
 * Slots: XF_SLOTS ranges of 40 MiB (24 MiB until 2026-11-13: a HiDPI desktop of 4096x2560 is 40 MiB, and a surface
 * that does not fit a slot is copied instead, XF_TOO_BIG), placed by xf_place() in ranges that read all one entry (see there); before that
 * at XF_SLOT_BASE, between XgFbMove's targets 0x40000000 and 0x80000000. i915 places
 * its GGTT objects at the bottom (bottom-up) and just under GUC_GGTT_TOP (PIN_HIGH), and clears every hole to its
 * scratch entry at init, so a slot must read all-scratch before its first use; after that every entry must be scratch
 * or what we wrote, or the slot is abandoned. Entries: present + PAT index 2 (uncached), what i915 gives scanout on MTL.
 *
 * Shared by the accelerator glue (xeflip_glue.inc, kernel) and the host test (test_xeflip.py through xf_test_dll.c).
 * Register offsets as fbreloc_core.h and NMIntelFB (display 14, pipe stride 0x1000).
 */
#ifndef XEFLIP_CORE_H
#define XEFLIP_CORE_H

typedef unsigned long long xf_u64;
typedef unsigned int xf_u32;
typedef unsigned char xf_u8;

#define XF_SLOTS		4
#define XF_SLOT_BASE		0x50000000u
#define XF_SLOT_STRIDE		0x02800000u		/* 40 MiB; display wants 256 KiB alignment for linear */
#define XF_SLOT_PAGES		(XF_SLOT_STRIDE >> 12)
#define XF_GSM			0x800000u		/* GGTT entries at BAR0 + 8 MiB, 8 bytes each */
#define XF_PTE_FLAGS		(1ull | (1ull << 53))	/* present, PAT index 2 */
#define XF_PTE_ADDR		0x00003ffffffff000ull	/* address bits of an entry */
/* bits that are zero in every entry i915 or the firmware writes (2-11, 46-51, 54-63; 52-53 are the PAT index). The
 * GGTT outside i915's objects holds stale memory in this boot (2026-11-03, ggtt-peek.py: random 64-bit values from
 * 0x81680000 up), which passes this test with odds of about 2^-27 per entry. */
#define XF_PTE_MBZ		0xffcfc00000000ffcull
static inline int xf_pte_real(xf_u64 v) { return (v & 1) && !(v & XF_PTE_MBZ); }

#define XF_TRANSCONF(p)		(0x70008u + (xf_u32)(p) * 0x1000u)
#define XF_FRMCOUNT(p)		(0x70040u + (xf_u32)(p) * 0x1000u)
#define XF_PLANE_CTL(p)		(0x70180u + (xf_u32)(p) * 0x1000u)
#define XF_PLANE_STRIDE(p)	(0x70188u + (xf_u32)(p) * 0x1000u)
#define XF_PLANE_SIZE(p)	(0x70190u + (xf_u32)(p) * 0x1000u)
#define XF_PLANE_SURF(p)	(0x7019cu + (xf_u32)(p) * 0x1000u)
#define XF_PLANE_SURFLIVE(p)	(0x701acu + (xf_u32)(p) * 0x1000u)
#define XF_GFX_FLSH_CNTL	0x101008u		/* GGTT TLB invalidate */
#define XF_GUC_TLB_INV		0xcee8u			/* GuC is on: its TLB too, as the accelerator's fb_pat does */

/* why a frame is not flipped (the glue counts these) */
enum {
	XF_OK = 0,
	XF_NO_PIPE = -1,	/* no enabled pipe shows a plane of this size */
	XF_PLANE_FMT = -2,	/* the plane is not linear XRGB8888 */
	XF_PITCH = -3,		/* the surface's row pitch is not the plane's stride */
	XF_NO_SLOT = -4,	/* every slot is dead or the one on screen */
	XF_SLOT_BUSY = -5,	/* the slot's range holds entries that are neither scratch nor ours: abandoned */
	XF_TOO_BIG = -6,	/* more pages than a slot holds */
	XF_NO_LATCH = -7,	/* the panel did not take the slot; the boot surface was put back */
};

struct xf_slot {
	xf_u64 key;		/* the surface's identity (the glue uses its memory descriptor); 0 = unmapped */
	xf_u64 lru;
	xf_u32 pages;		/* entries we wrote last time */
	int state;		/* 0 never checked, 1 usable, 2 abandoned */
	xf_u64 clear;		/* what an entry we no longer use goes back to: scratch, or 0 in a range of stale data */
};

struct xf {
	volatile xf_u8 *bar;
	xf_u64 (*pte)[XF_SLOT_PAGES];	/* XF_SLOTS rows: what we wrote into each slot */
	xf_u64 scratch;			/* i915's scratch entry, read from the first slot range checked */
	int scratch_known;
	int pipe;
	xf_u32 boot;			/* the boot framebuffer's GGTT address (PLANE_SURF before the first flip) */
	xf_u32 front;			/* what PLANE_SURF shows now: boot or a slot */
	xf_u64 tick;
	struct xf_slot s[XF_SLOTS];
	xf_u32 base[XF_SLOTS];		/* GGTT address of each slot once xf_place() ran; 0 = no usable range */
	int placed;
};

/* the kernel sleeps here (IOSleep), the host test advances its fake panel */
#ifndef XF_PAUSE
#define XF_PAUSE(x) do { (void)(x); } while (0)
#endif
static void (*xf_poll_hook)(volatile xf_u8 *bar);

static inline xf_u32 xf_rd(volatile xf_u8 *bar, xf_u32 off) { return *(volatile xf_u32 *)(bar + off); }
static inline void xf_wr(volatile xf_u8 *bar, xf_u32 off, xf_u32 v) { *(volatile xf_u32 *)(bar + off) = v; }
static inline volatile xf_u64 *xf_gtt(volatile xf_u8 *bar) { return (volatile xf_u64 *)(bar + XF_GSM); }
static inline xf_u32 xf_slot_addr(const struct xf *x, int i)
{
	return x->placed ? x->base[i] : XF_SLOT_BASE + (xf_u32)i * XF_SLOT_STRIDE;
}
/* the slot whose range starts at addr, or -1 */
static inline int xf_slot_of(const struct xf *x, xf_u32 addr)
{
	int i;

	for (i = 0; i < XF_SLOTS; i++)
		if (xf_slot_addr(x, i) == addr && (!x->placed || x->base[i]))
			return i;
	return -1;
}

/* the pipe that shows a w x h linear XRGB8888 plane with this pitch; XF_* < 0 when none does */
static int xf_pipe_find(volatile xf_u8 *bar, xf_u32 w, xf_u32 h, xf_u32 pitch)
{
	int p, why = XF_NO_PIPE;

	for (p = 0; p < 4; p++) {
		xf_u32 ctl = xf_rd(bar, XF_PLANE_CTL(p)), size = xf_rd(bar, XF_PLANE_SIZE(p));
		if (!(xf_rd(bar, XF_TRANSCONF(p)) & 0x80000000u) || !(ctl & 0x80000000u))
			continue;
		if ((size & 0xffffu) + 1 != w || ((size >> 16) & 0xffffu) + 1 != h)
			continue;
		if (((ctl >> 23) & 0x1fu) != 8 || ((ctl >> 10) & 0x7u) != 0) {
			why = XF_PLANE_FMT;
			continue;
		}
		if ((xf_rd(bar, XF_PLANE_STRIDE(p)) & 0x7ffu) * 64u != pitch) {
			why = XF_PITCH;
			continue;
		}
		return p;
	}
	return why;
}

/* the slot already holding this surface, or -1 */
static int xf_lookup(struct xf *x, xf_u64 key)
{
	int i;

	for (i = 0; i < XF_SLOTS; i++)
		if (key && x->s[i].key == key && x->s[i].state == 1) {
			x->s[i].lru = ++x->tick;
			return i;
		}
	return -1;
}

/* a slot for a new surface: never the one on screen, never an abandoned one; the least recently used */
static int xf_pick(struct xf *x)
{
	int i, best = -1;

	for (i = 0; i < XF_SLOTS; i++) {
		if (x->s[i].state == 2 || xf_slot_addr(x, i) == x->front)
			continue;
		if (best < 0 || x->s[i].lru < x->s[best].lru)
			best = i;
	}
	return best;
}

static void xf_tlb_flush(volatile xf_u8 *bar)
{
	xf_wr(bar, XF_GFX_FLSH_CNTL, 1);
	(void)xf_rd(bar, XF_GFX_FLSH_CNTL);
	xf_wr(bar, XF_GUC_TLB_INV, 1);
}

/*
 * Point slot i at the n pages in phys[] (4 KiB aligned physical addresses) for surface key. The slot's range is checked
 * first: all one entry the first time (i915's scratch), afterwards each entry scratch or what we wrote. Entries past n
 * that we wrote before go back to scratch.
 */
static int xf_map(struct xf *x, int i, xf_u64 key, const xf_u64 *phys, xf_u32 n)
{
	volatile xf_u64 *g = xf_gtt(x->bar) + (xf_slot_addr(x, i) >> 12);
	struct xf_slot *sl = &x->s[i];
	xf_u64 *mine = x->pte[i];
	xf_u32 j;

	if (n > XF_SLOT_PAGES)
		return XF_TOO_BIG;
	if (sl->state == 0) {
		xf_u64 first = g[0];
		int uniform = 1;
		for (j = 1; j < XF_SLOT_PAGES && uniform; j++)
			if (g[j] != first)
				uniform = 0;
		if (uniform && (!x->scratch_known || first == x->scratch)) {
			x->scratch = first;
			x->scratch_known = 1;
			sl->clear = first;
		} else {
			/* stale data: free only when no entry is a real mapping; what is there now counts as ours */
			for (j = 0; j < XF_SLOT_PAGES; j++)
				if (xf_pte_real(g[j])) {
					sl->state = 2;
					return XF_SLOT_BUSY;
				}
			sl->clear = 0;
		}
		for (j = 0; j < XF_SLOT_PAGES; j++)
			mine[j] = g[j];
		sl->pages = 0;
		sl->state = 1;
	}
	for (j = 0; j < XF_SLOT_PAGES; j++)
		if (g[j] != mine[j] && g[j] != sl->clear) {
			sl->state = 2;
			sl->key = 0;
			return XF_SLOT_BUSY;
		}
	for (j = 0; j < n; j++) {
		mine[j] = (phys[j] & XF_PTE_ADDR) | XF_PTE_FLAGS;
		g[j] = mine[j];
	}
	for (; j < sl->pages; j++) {
		mine[j] = sl->clear;
		g[j] = sl->clear;
	}
	(void)g[n ? n - 1 : 0];		/* posting read */
	xf_tlb_flush(x->bar);
	sl->pages = n;
	sl->key = key;
	sl->lru = ++x->tick;
	return XF_OK;
}

/* PLANE_SURF := surf (its low flag bits kept); 1 when SURFLIVE shows it within a few frames */
static int xf_show(struct xf *x, xf_u32 surf)
{
	volatile xf_u8 *bar = x->bar;
	xf_u32 reg = xf_rd(bar, XF_PLANE_SURF(x->pipe)), f0, n;

	xf_wr(bar, XF_PLANE_SURF(x->pipe), surf | (reg & 0xfffu));
	f0 = xf_rd(bar, XF_FRMCOUNT(x->pipe));
	for (n = 0; n < 200u; n++) {
		if (xf_poll_hook)
			xf_poll_hook(bar);
		if ((xf_rd(bar, XF_PLANE_SURFLIVE(x->pipe)) & ~0xfffu) == surf)
			return 1;
		if (xf_rd(bar, XF_FRMCOUNT(x->pipe)) - f0 > 3)
			break;
		XF_PAUSE(n);
	}
	return (xf_rd(bar, XF_PLANE_SURFLIVE(x->pipe)) & ~0xfffu) == surf;
}

/* show slot i; if the panel does not take it, the boot surface goes back up */
static int xf_flip(struct xf *x, int i)
{
	xf_u32 want = xf_slot_addr(x, i);

	if (!x->boot)
		x->boot = xf_rd(x->bar, XF_PLANE_SURF(x->pipe)) & ~0xfffu;
	if (x->front == want)
		return XF_OK;
	if (xf_show(x, want)) {
		x->front = want;
		x->s[i].lru = ++x->tick;
		return XF_OK;
	}
	(void)xf_show(x, x->boot);
	x->front = x->boot;
	return XF_NO_LATCH;
}

/* back to the boot framebuffer; 1 when the panel took it (or was there) */
static int xf_home(struct xf *x)
{
	if (!x->boot || !x->bar || x->front == x->boot)
		return 1;
	if (!xf_show(x, x->boot))
		return 0;
	x->front = x->boot;
	return 1;
}

/*
 * Place the slots (2026-11-03: the fixed ranges at 0x50000000 read as in use, all four were abandoned, nothing was
 * flipped). The free ranges are found, not assumed. Every XF_SLOT_STRIDE step from XF_PLACE_LO to XF_PLACE_HI (i915's
 * objects at both ends stay out; the framebuffer has real entries) is free when its entries all read one value -- the
 * value most such ranges hold is i915's scratch -- or when none of them is a real mapping (xf_pte_real): this i915
 * leaves stale memory in the holes. The slots go to the free ranges nearest XF_PLACE_MID, the farthest from both ends.
 * Only while the boot framebuffer is on screen. Returns the slots placed (the rest are abandoned), -1 when a slot is
 * shown. map[64] gets one character per 64 MiB of the 4 GiB GGTT from 64 samples: '.' all scratch, '~' no real entry
 * (stale data), '#' real entries, 's' a slot.
 */
#define XF_PLACE_LO		0x40000000u
#define XF_PLACE_HI		0xf0000000u
#define XF_PLACE_MID		0xb8000000u
#define XF_PLACE_MAX		((XF_PLACE_HI - XF_PLACE_LO) / XF_SLOT_STRIDE)
#define XF_MAP_CHUNKS		64
static int xf_place(struct xf *x, char map[XF_MAP_CHUNKS + 1], int *cand_out, int *free_out)
{
	volatile xf_u64 *g = xf_gtt(x->bar);
	xf_u64 val[8];
	int cnt[8], nv = 0, best = -1, nfree = 0, placed = 0, i, k;
	static xf_u64 cval[XF_PLACE_MAX];
	static unsigned char kind[XF_PLACE_MAX];	/* 0 in use, 1 uniform, 2 no real entry */

	if (x->boot && x->front != x->boot)
		return -1;
	for (k = 0; k < (int)XF_PLACE_MAX; k++) {
		xf_u32 first = (XF_PLACE_LO + (xf_u32)k * XF_SLOT_STRIDE) >> 12, j;
		xf_u64 v = g[first];
		int uniform = 1, real = xf_pte_real(v);
		for (j = 1; j < XF_SLOT_PAGES; j++) {
			xf_u64 e = g[first + j];
			if (e != v)
				uniform = 0;
			if (xf_pte_real(e))
				real = 1;
			if (!uniform && real)
				break;
		}
		kind[k] = uniform ? 1 : real ? 0 : 2;
		cval[k] = v;
		if (!uniform)
			continue;
		for (i = 0; i < nv && val[i] != v; i++)
			;
		if (i == nv && nv < 8) {
			val[nv] = v;
			cnt[nv++] = 0;
		}
		if (i < nv)
			cnt[i]++;
	}
	for (i = 0; i < nv; i++)
		if (best < 0 || cnt[i] > cnt[best])
			best = i;
	if (best >= 0) {
		x->scratch = val[best];
		x->scratch_known = 1;
	}
	for (k = 0; k < (int)XF_PLACE_MAX; k++) {
		if (kind[k] == 1 && !(best >= 0 && cval[k] == val[best]) && xf_pte_real(cval[k]))
			kind[k] = 0;		/* one real entry repeated: someone's mapping, not scratch */
		else if (kind[k] == 1 && !(best >= 0 && cval[k] == val[best]))
			kind[k] = 2;
		if (kind[k])
			nfree++;
	}
	for (i = 0; i < XF_SLOTS; i++) {
		x->base[i] = 0;
		x->s[i].key = 0;
		x->s[i].lru = 0;
		x->s[i].pages = 0;
		x->s[i].state = 2;
	}
	/* nearest the middle first: k by distance of its centre from XF_PLACE_MID */
	while (placed < XF_SLOTS) {
		int pick = -1;
		xf_u32 pd = 0;
		for (k = 0; k < (int)XF_PLACE_MAX; k++) {
			xf_u32 c = XF_PLACE_LO + (xf_u32)k * XF_SLOT_STRIDE + XF_SLOT_STRIDE / 2;
			xf_u32 d = c > XF_PLACE_MID ? c - XF_PLACE_MID : XF_PLACE_MID - c;
			if (!kind[k])
				continue;
			for (i = 0; i < placed; i++)
				if (x->base[i] == XF_PLACE_LO + (xf_u32)k * XF_SLOT_STRIDE)
					break;
			if (i < placed)
				continue;
			if (pick < 0 || d < pd) {
				pick = k;
				pd = d;
			}
		}
		if (pick < 0)
			break;
		x->base[placed] = XF_PLACE_LO + (xf_u32)pick * XF_SLOT_STRIDE;
		x->s[placed].state = 0;
		placed++;
	}
	x->placed = 1;
	if (map) {
		for (k = 0; k < XF_MAP_CHUNKS; k++) {
			xf_u32 e0 = (xf_u32)k << 14, j;	/* 64 MiB = 16384 entries */
			xf_u64 v = g[e0];
			int uniform = 1, real = xf_pte_real(v);
			char c;
			for (j = 1; j < 64; j++) {
				xf_u64 e = g[e0 + j * 256u];
				if (e != v)
					uniform = 0;
				if (xf_pte_real(e))
					real = 1;
			}
			c = (uniform && best >= 0 && v == val[best]) ? '.' : real ? '#' : '~';
			for (i = 0; i < placed; i++)
				if ((x->base[i] >> 26) == (xf_u32)k)
					c = 's';
			map[k] = c;
		}
		map[XF_MAP_CHUNKS] = 0;
	}
	if (cand_out)
		*cand_out = (int)XF_PLACE_MAX;
	if (free_out)
		*free_out = nfree;
	return placed;
}

/* forget every slot's surface (the glue releases them); the ranges stay ours */
static void xf_forget(struct xf *x)
{
	int i;

	for (i = 0; i < XF_SLOTS; i++)
		x->s[i].key = 0;
}

#endif
