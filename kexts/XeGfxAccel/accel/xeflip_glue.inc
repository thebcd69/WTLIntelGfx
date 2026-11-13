// <xeflip> Zero-copy scan-out on the Intel panel: a local addition to WTLIntelGfx v0.0.47 (ThinkBook 14+ 155H, 7D55).
// Included by nvrm-accel.cpp inside its NMX_XEGFX block, after nvXeWaitVblank. The hardware half (plane checks, GGTT
// slots, PLANE_SURF and its latch) is xeflip_core.h, tested on the host by work/xeflip/test_xeflip.py.
//
// performTransaction tries nvXeFlip first for head 4 (the Intel panel). It answers true when the frame is on the panel
// (or kept back, as the copy path keeps frames whose pixels are not in memory yet) and false when the frame should be
// copied as before; the reason is in debug.nvaccel_xeflip_last_why. After a copied frame nvXeFlipHome shows the boot
// framebuffer again. When no frame came for nvaccel_xeflip_idle_ms the idle call copies the frame on screen into the
// boot framebuffer and shows that, so a WindowServer that stops (or exits) leaves its last picture, not a frozen slot.
// Off by default:  sudo sysctl -w debug.nvaccel_xeflip=1  (0 goes back to copying from the next frame).
// the latch wait: IOSleep(1) steps, or (gXfPrecise, the asynchronous flip's thread) by the scanline: sleep while the
// vblank is more than an eighth of a frame away, then 25 us steps, so the flip is seen landing within ~25 us, not a
// millisecond or more late
static void xfPause(unsigned n);
#define XF_PAUSE(n) xfPause(n)
#include "xeflip_core.h"

static int gXeFlip = 0, gXeFlipIdleMs = 1000, gXfLastWhy = 0, gXfMapUs = 0, gXfFlipUs = 0, gXfLogs = 0;
static struct xf gXf;
static volatile int gXfPrecise;   // set under gXfLock by the asynchronous flip's thread only
static void xfPause(unsigned n)
{
    (void)n;
    if (!gXfPrecise || !gXf.bar || gXf.pipe < 0 || gXf.pipe > 3) { IOSleep(1); return; }
    volatile xf_u8 *bar = gXf.bar;
    const xf_u32 vt = xf_rd(bar, 0x6000Cu + (xf_u32)gXf.pipe * 0x1000u), act = (vt & 0xffffu) + 1;   // TRANS_VTOTAL: active lines
    const xf_u32 line = xf_rd(bar, 0x70000u + (xf_u32)gXf.pipe * 0x1000u) & 0xfffffu;               // PIPEDSL
    if (act > 8 && line < act && act - line > act / 8) IOSleep(1); else IODelay(25);
}
static xf_u64 gXfPte[XF_SLOTS][XF_SLOT_PAGES];
static xf_u64 gXfPhys[XF_SLOT_PAGES];
static IOMemoryDescriptor *gXfMd[XF_SLOTS];         // retained and prepared while its slot maps it
static unsigned long long gXfGeo[XF_SLOTS][3];      // width, height, row pitch of the surface in each slot
static IOLock *gXfLock;
static struct thread_call *gXfIdleCall;
static IOService *gXfFb;                            // framebuffer of the last flip (held by the accelerator)
static uint64_t gXfLastFlip;
static SInt64 gCntXfFlips, gCntXfMaps, gCntXfHomes, gCntXfIdleHomes, gCntXfKept, gCntXfCopied;
static char gXfGgtt[192] = "not placed yet (written when debug.nvaccel_xeflip goes to 1)";
static bool xfHomeWithCopy(NVAccel *a);
static void xfRelease(int i);
static IOLock *xfLock(void);
// switching on (0 -> 1) places the slots again: scan the GGTT for ranges that hold only scratch (xf_place). Here, in
// the sysctl's thread, not in WindowServer's frame path: the scan reads about 700,000 entries once.
static void xfPlaceNow(void)
{
    NVAccel *a = NVAccel::gAccel;
    if (!gXeMmio && a && a->fPCI) gXeMmio = a->fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    if (!gXeMmio || gXeMmio->getLength() < 0x1000000) { snprintf(gXfGgtt, sizeof gXfGgtt, "no BAR0 mapping: not placed"); return; }
    IOLock *l = xfLock(); if (!l) return;
    IOLockLock(l);
    if (gXf.boot && gXf.front != gXf.boot && !xfHomeWithCopy(a)) {
        IOLockUnlock(l); snprintf(gXfGgtt, sizeof gXfGgtt, "a slot is on screen and the boot framebuffer did not come back: not placed"); return;
    }
    for (int i = 0; i < XF_SLOTS; i++) xfRelease(i);
    gXf.bar = (volatile xf_u8 *)gXeMmio->getVirtualAddress(); gXf.pte = gXfPte;
    char map[XF_MAP_CHUNKS + 1]; int cand = 0, uni = 0;
    const uint64_t t0 = mach_absolute_time();
    const int n = xf_place(&gXf, map, &cand, &uni);
    uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    snprintf(gXfGgtt, sizeof gXfGgtt, "%d slot(s) %08x %08x %08x %08x; scratch %llx; %d/%d ranges free; %llu ms; map %s",
             n, gXf.base[0], gXf.base[1], gXf.base[2], gXf.base[3], gXf.scratch, uni, cand, ns / 1000000ull, map);
    IOLockUnlock(l);
    ALOG("xeflip: GGTT scan: %s", gXfGgtt);
    gXfLogs = 0;
}
static int nvxeflip_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gXeFlip;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    if (v && !gXeFlip) { gXeFlip = 0; xfPlaceNow(); }
    gXeFlip = v ? 1 : 0;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_xeflip, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
            nvxeflip_sysctl, "I", "1 = the Intel panel scans WindowServer's composited surface directly (no copy; "
            "places the slots first); 0 = copy every frame (default)");
SYSCTL_STRING(_debug, OID_AUTO, nvaccel_xeflip_ggtt, CTLFLAG_RD | CTLFLAG_LOCKED, gXfGgtt, sizeof gXfGgtt,
              "the last slot placement: slots, scratch entry, uniform ranges, a map of the GGTT in 64 MiB ('.' scratch, "
              "'~' stale data, no real entry; '#' real entries; 's' slot)");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_xeflip_idle_ms, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gXeFlipIdleMs, 0,
           "no frame for this long: copy the frame on screen into the boot framebuffer and show that (min 100)");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_xeflip_last_why, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gXfLastWhy, 0,
           "why the last frame was copied: -1 no pipe, -2 plane format, -3 pitch, -4 no slot, -5 slot range in use, -6 too big, "
           "-7 no latch, -8 no BAR0, -9 pixel format, -10 geometry, -11 surface memory");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_xeflip_map_us, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gXfMapUs, 0, "last surface mapped into a slot, us");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_xeflip_flip_us, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gXfFlipUs, 0, "last flip until the panel took it, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_xeflip_flips, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXfFlips, "frames shown straight from the surface (zero copy)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_xeflip_maps, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXfMaps, "surfaces mapped into a GGTT slot");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_xeflip_homes, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXfHomes, "boot framebuffer shown again after a copied frame");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_xeflip_idle_homes, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXfIdleHomes, "boot framebuffer shown again after an idle panel (frame copied first)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_xeflip_kept, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXfKept, "frames kept back: the surface's newest pixels were not in memory yet");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_xeflip_copied, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXfCopied, "frames handed to the copy path (see last_why)");
// <xfasync> GU605 2026-11-09: the flip completes off WindowServer's thread, as NVIDIA's (nvAccelIopAsyncRun). Synchronous,
// performTransaction waited for the composite's GPU work (nvSurfWaitReady) and then for the panel to latch (~3.4 ms of a
// 4.17 ms frame at 240 Hz) on WindowServer's thread: the desktop composited ~55 frames/s. 0 (default) = synchronous.
static int gXeFlipAsync = 0, gXfAsyncWaitUs = 5200, gXfLatchUs = 0, gXfTxnUs = 0, gXfTxnUsMax = 0;
static SInt64 gCntXfAsync, gCntXfAsyncKept, gCntXfAsyncDrop, gCntXfAsyncQueued, gCntXfAsyncRefused;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_xeflip_async, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gXeFlipAsync, 0,
           "1 = a flip waits for its composite and the panel's latch on a thread of its own and completes through "
           "signalTransactionInterrupt (WindowServer's thread returns at once); 0 = synchronous (default)");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_xeflip_async_wait_us, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gXfAsyncWaitUs, 0,
           "a present that finds the last flip still in flight waits this long, then queues behind it (min 500)");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_xeflip_latch_us, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gXfLatchUs, 0, "last asynchronous flip: PLANE_SURF written until the panel showed it, us");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_xeflip_txn_us, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gXfTxnUs, 0, "last Intel panel present: time WindowServer's thread spent in it, us");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_xeflip_txn_us_max, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gXfTxnUsMax, 0, "longest of those, us (write 0 to reset)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_xeflip_async_flips, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXfAsync, "frames flipped by the asynchronous thread");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_xeflip_async_kept, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXfAsyncKept, "asynchronous frames kept back (newest pixels not in memory)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_xeflip_async_queued, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXfAsyncQueued, "presents queued behind a flip still in flight");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_xeflip_async_drops, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXfAsyncDrop, "queued presents replaced by a newer one (dropped frames)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_xeflip_async_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXfAsyncRefused, "asynchronous flips that failed (3 in a row: async off)");

static void nvXeTxnNote(uint64_t t0)
{
    gXfTxnUs = (int)nvXeUs(t0);
    if (gXfTxnUs > gXfTxnUsMax) gXfTxnUsMax = gXfTxnUs;
}
static void nvXeFlipSysctls(void)
{
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip);          sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_idle_ms);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_last_why); sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_map_us);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_flip_us);  sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_flips);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_maps);     sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_homes);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_idle_homes); sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_kept);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_copied);   sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_ggtt);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_async);    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_async_wait_us);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_latch_us); sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_txn_us);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_txn_us_max); sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_async_flips);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_async_kept); sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_async_queued);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_async_drops); sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_async_refused);
}

static bool xfNo(int why)
{
    gXfLastWhy = why; OSAddAtomic64(1, &gCntXfCopied);
    if (gXfLogs < 16) { gXfLogs++; ALOG("xeflip: frame copied instead (reason %d, see debug.nvaccel_xeflip_last_why)", why); }
    return false;
}
static IOLock *xfLock(void)
{
    if (!gXfLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gXfLock)) IOLockFree(l); }
    return gXfLock;
}
// lock held: the slot's surface is let go (its range keeps our entries until the slot is mapped again)
static void xfRelease(int i)
{
    if (gXfMd[i]) { gXfMd[i]->complete(); gXfMd[i]->release(); gXfMd[i] = nullptr; }
    gXf.s[i].key = 0;
}
// lock held: copy the frame on screen into the boot framebuffer, then show the boot framebuffer
static bool xfHomeWithCopy(NVAccel *a)
{
    if (!gXf.bar || !gXf.boot || gXf.front == gXf.boot) return true;
    const int i = xf_slot_of(&gXf, gXf.front);
    if (a && gXfFb && i >= 0 && i < XF_SLOTS && gXfMd[i]) {
        struct NVRMVramRequest q; bzero(&q, sizeof q); q.version = NVRM_VRAM_ABI_VERSION;
        if (a->askFramebufferForScanout(&q, gXfFb) == kIOReturnSuccess && q.kva && q.pitch) {
            if (IOMemoryMap *m = gXfMd[i]->map(kIOMapReadOnly)) {
                const unsigned long long w = gXfGeo[i][0], h = gXfGeo[i][1], p = gXfGeo[i][2];
                const unsigned long long rows = h < q.height ? h : q.height;
                const unsigned long long row = 4 * w < q.pitch ? 4 * w : q.pitch;
                const unsigned char *src = (const unsigned char *)m->getVirtualAddress();
                unsigned char *dst = (unsigned char *)q.kva;
                if (m->getLength() >= rows * p)
                    for (unsigned long long y = 0; y < rows; y++) memcpy(dst + y * q.pitch, src + y * p, (size_t)row);
                m->release();
            }
        }
    }
    return xf_home(&gXf) != 0;
}
static void xfIdle(void *, void *)
{
    IOLock *l = gXfLock;
    if (!l) return;
    IOLockLock(l);
    uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - gXfLastFlip, &ns);
    const uint64_t idle = (uint64_t)(gXeFlipIdleMs < 100 ? 100 : gXeFlipIdleMs) * 1000000ull;
    if (gXf.boot && gXf.front != gXf.boot && ns + 2000000ull >= idle) {
        if (xfHomeWithCopy(NVAccel::gAccel)) {
            OSAddAtomic64(1, &gCntXfIdleHomes);
            for (int i = 0; i < XF_SLOTS; i++) xfRelease(i);
        }
    }
    IOLockUnlock(l);
}
static void xfArmIdle(void)
{
    if (!gXfIdleCall) gXfIdleCall = thread_call_allocate(xfIdle, nullptr);
    if (!gXfIdleCall) return;
    uint64_t dl = 0; clock_interval_to_deadline((uint32_t)(gXeFlipIdleMs < 100 ? 100 : gXeFlipIdleMs), kMillisecondScale, &dl);
    thread_call_enter_delayed(gXfIdleCall, dl);
}

// The second half of a flip, after the checks: wait for the composite, map the surface into a slot if it is not in one,
// show the slot. 1 on the panel, 0 kept back (the newest pixels are not in memory yet), < 0 the XF_* reason it was not
// flipped (the caller counts it). precise: the latch wait follows the scanline (the asynchronous thread).
static int xfShow(NVAccel *a, IOSurface *s, int p, bool precise)
{
    (void)a;
    const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
    IOMemoryDescriptor *md = s->getMemoryDescriptor();
    const unsigned long long bytes = H * P;
    if (!md || md->getLength() < bytes) return -11;
    // as the copy path: the composite must be finished, and its newest pixels in the surface's memory
    nvSurfWaitReady(s->getSurfaceID());
    if (!nvAccelSurfCopyReady(s, 4)) return 0;
    IOLock *l = xfLock();
    if (!l) return -8;
    IOLockLock(l);
    gXf.bar = (volatile xf_u8 *)gXeMmio->getVirtualAddress(); gXf.pte = gXfPte; gXf.pipe = p;
    const xf_u64 key = (xf_u64)(uintptr_t)md;
    int i = xf_lookup(&gXf, key);
    if (i < 0) {
        const xf_u32 pages = (xf_u32)((bytes + 4095) >> 12);
        if (pages > XF_SLOT_PAGES) { IOLockUnlock(l); return XF_TOO_BIG; }
        i = xf_pick(&gXf);
        if (i < 0) { IOLockUnlock(l); return XF_NO_SLOT; }
        xfRelease(i);
        if (md->prepare() != kIOReturnSuccess) { IOLockUnlock(l); return -11; }
        const uint64_t t0 = mach_absolute_time();
        bool whole = true;
        for (xf_u32 k = 0; k < pages && whole; k++) {
            IOByteCount len = 0;
            const addr64_t pa = md->getPhysicalSegment((IOByteCount)k << 12, &len, kIOMemoryMapperNone);
            if (!pa || (pa & 0xfff)) whole = false; else gXfPhys[k] = pa;
        }
        const int r = whole ? xf_map(&gXf, i, key, gXfPhys, pages) : -11;
        if (r != XF_OK) { md->complete(); IOLockUnlock(l); return r; }
        md->retain(); gXfMd[i] = md;
        gXfGeo[i][0] = W; gXfGeo[i][1] = H; gXfGeo[i][2] = P;
        gXfMapUs = (int)nvXeUs(t0);
        const SInt64 nm = OSAddAtomic64(1, &gCntXfMaps);
        if (nm < 8) ALOG("xeflip: surface 0x%x %llux%llu pitch %llu -> slot %d at GGTT 0x%08x (%u pages, %d us)",
                         s->getSurfaceID(), W, H, P, i, xf_slot_addr(&gXf, i), pages, gXfMapUs);
    }
    const uint64_t t1 = mach_absolute_time();
    gXfPrecise = precise ? 1 : 0;
    const int r = xf_flip(&gXf, i);
    gXfPrecise = 0;
    gXfFlipUs = (int)nvXeUs(t1);
    if (precise) gXfLatchUs = gXfFlipUs;
    if (r == XF_OK) gXfLastFlip = mach_absolute_time();
    IOLockUnlock(l);
    if (r != XF_OK) return r;
    const SInt64 n = OSAddAtomic64(1, &gCntXfFlips);
    if (n < 3 || n == 600) ALOG("xeflip: frame #%lld on the panel straight from surface 0x%x (slot %d, %d us to latch, boot 0x%08x%s)",
                                (long long)n + 1, s->getSurfaceID(), i, gXfFlipUs, gXf.boot, precise ? ", asynchronous" : "");
    xfArmIdle();
    return 1;
}

// <xfasync> one flip in flight on its own thread call; a present that comes while it is still landing waits up to
// debug.nvaccel_xeflip_async_wait_us, then queues behind it (replacing an older queued one: a dropped frame). The pipe's
// isTransactionComplete answers "not yet" while busy, and the thread ends each flip with signalTransactionInterrupt.
extern "C" void _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(void *pipe, void *arg);
struct XfAsync { void *pipe; IOService *fb; IOSurface *s; int p; IOSurface *next; int nextP; volatile SInt32 busy; struct thread_call *call; };
static XfAsync gXfAs;
static IOLock *gXfAsLock;
static SInt32 gXfAsyncRefuseRun;
static bool nvXeFlipAsyncBusy(const void *pipe)
{
    return gXfAs.pipe == pipe && __atomic_load_n(&gXfAs.busy, __ATOMIC_ACQUIRE);
}
static void xfAsyncRun(void *, void *)
{
    XfAsync *e = &gXfAs;
    IOSurface *s = e->s; int p = e->p; e->s = nullptr;
  again:
    {
        const int r = xfShow(NVAccel::gAccel, s, p, true);
        if (r == 1) { gXfFb = e->fb; gXfAsyncRefuseRun = 0; OSAddAtomic64(1, &gCntXfAsync); }
        else if (r == 0) { OSAddAtomic64(1, &gCntXfKept); OSAddAtomic64(1, &gCntXfAsyncKept); }
        else {
            (void)xfNo(r); OSAddAtomic64(1, &gCntXfAsyncRefused);
            if (++gXfAsyncRefuseRun >= 3 && gXeFlipAsync) {
                gXeFlipAsync = 0;
                ALOG("xeflip: 3 asynchronous flips failed in a row (last reason %d) -- debug.nvaccel_xeflip_async=0 (synchronous, which copies on failure)", r);
            }
        }
        ((OSObject *)s)->release();
    }
    IOSurface *queued = nullptr;
    IOLockLock(gXfAsLock);
    queued = e->next; e->next = nullptr;
    if (queued) p = e->nextP;
    else { __atomic_store_n(&e->busy, 0, __ATOMIC_RELEASE); IOLockWakeup(gXfAsLock, (event_t)&e->busy, false); }
    IOLockUnlock(gXfAsLock);
    _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(e->pipe, nullptr);
    if (queued) { s = queued; goto again; }
}
static bool xfAsyncStart(void *pipe, IOService *fb, IOSurface *s, int p)
{
    XfAsync *e = &gXfAs;
    if (!gXfAsLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gXfAsLock)) IOLockFree(l); }
    if (!gXfAsLock) return false;
    if (!e->call) e->call = thread_call_allocate(xfAsyncRun, nullptr);
    if (!e->call) return false;
    if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE)) {
        uint64_t end = 0; clock_interval_to_deadline((uint32_t)(gXfAsyncWaitUs < 500 ? 500 : gXfAsyncWaitUs), kMicrosecondScale, &end);
        IOLockLock(gXfAsLock);
        while (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE) && mach_absolute_time() < end)
            IOLockSleepDeadline(gXfAsLock, (event_t)&e->busy, end, THREAD_UNINT);
        if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE)) {
            // still in flight: this frame follows it, replacing an older queued one
            IOSurface *old = e->next;
            ((OSObject *)s)->retain();
            e->next = s; e->nextP = p;
            IOLockUnlock(gXfAsLock);
            OSAddAtomic64(1, &gCntXfAsyncQueued);
            if (old) { OSAddAtomic64(1, &gCntXfAsyncDrop); ((OSObject *)old)->release(); }
            return true;
        }
        IOLockUnlock(gXfAsLock);
    }
    ((OSObject *)s)->retain();
    e->pipe = pipe; e->fb = fb; e->s = s; e->p = p;
    __atomic_store_n(&e->busy, 1, __ATOMIC_RELEASE);
    thread_call_enter(e->call);
    const SInt64 n = gCntXfAsync;
    if (n == 0 && gXfLogs < 16) { gXfLogs++; ALOG("xeflip: asynchronous flips ON (debug.nvaccel_xeflip_async=1): WindowServer's thread no longer waits for the GPU or the latch"); }
    return true;
}
// waits (at most 100 ms) for a flip in flight to land, so the accelerator can stop
static void xfAsyncDrain(void)
{
    if (!gXfAsLock) return;
    uint64_t end = 0; clock_interval_to_deadline(100, kMillisecondScale, &end);
    IOLockLock(gXfAsLock);
    while (__atomic_load_n(&gXfAs.busy, __ATOMIC_ACQUIRE) && mach_absolute_time() < end)
        IOLockSleepDeadline(gXfAsLock, (event_t)&gXfAs.busy, end, THREAD_UNINT);
    IOLockUnlock(gXfAsLock);
}

// true: the frame is on the panel (or kept back, or handed to the asynchronous thread); false: copy it as before
static bool nvXeFlip(NVAccel *a, IOService *fb, unsigned head, IOSurface *s, void *pipe)
{
    if (!gXeFlip || head != 4 || !s || !a) return false;
    if (!gXeMmio && a->fPCI) gXeMmio = a->fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    if (!gXeMmio || gXeMmio->getLength() < 0x1000000) return xfNo(-8);
    if (s->getPixelFormat() != 0x42475241u) return xfNo(-9);   // 'BGRA': the plane's XRGB8888
    const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
    if (!W || !H || W > 16384 || H > 16384 || P < 4 * W || (P & 63)) return xfNo(-10);
    IOMemoryDescriptor *md = s->getMemoryDescriptor();
    if (!md || md->getLength() < H * P) return xfNo(-11);
    volatile xf_u8 *bar = (volatile xf_u8 *)gXeMmio->getVirtualAddress();
    const int p = xf_pipe_find(bar, (xf_u32)W, (xf_u32)H, (xf_u32)P);
    if (p < 0) return xfNo(p);
    if (gXeFlipAsync && pipe && xfAsyncStart(pipe, fb, s, p)) return true;
    const int r = xfShow(a, s, p, false);
    if (r == 0) { OSAddAtomic64(1, &gCntXfKept); return true; }
    if (r < 0) return xfNo(r);
    gXfFb = fb;
    return true;
}
// after a copied frame (it went into the boot framebuffer): show the boot framebuffer again
static void nvXeFlipHome(void)
{
    IOLock *l = gXfLock;
    if (!l) return;
    IOLockLock(l);
    if (gXf.bar && gXf.boot && gXf.front != gXf.boot && xf_home(&gXf)) OSAddAtomic64(1, &gCntXfHomes);
    IOLockUnlock(l);
}
// the accelerator stops: last frame into the boot framebuffer, boot framebuffer on the panel, surfaces let go
static void nvXeFlipShutdown(void)
{
    gXeFlip = 0; gXeFlipAsync = 0;
    xfAsyncDrain();
    IOLock *l = gXfLock;
    if (!l) return;
    IOLockLock(l);
    if (xfHomeWithCopy(NVAccel::gAccel)) { for (int i = 0; i < XF_SLOTS; i++) xfRelease(i); xf_forget(&gXf); }
    IOLockUnlock(l);
}
// [gu605-mac] HiDPI (2026-11-13): a mode change can put another framebuffer on the plane (XeGfx keeps one per desktop
// size next to the firmware's), and WindowServer's surfaces change size with it. So before the change the plane goes
// back to the framebuffer it had, every slot lets go of its surface, and "boot" (the address PLANE_SURF returns to when
// nothing is flipped) is forgotten: xf_flip reads it again from PLANE_SURF at the first flip after i915's commit. The
// accelerator's description of the framebuffer (size, pitch, CPU mapping: the copy target) is read again too.
static void nvXeFlipModeChange(void)
{
    xfAsyncDrain();
    IOLock *l = gXfLock;
    NVAccel *a = NVAccel::gAccel;
    if (l) IOLockLock(l);
    if (l && gXf.bar) {
        // with the frame on screen copied into it first: not every "mode change" ends in a commit that redraws
        if (gXf.boot && gXf.front != gXf.boot) (void)xfHomeWithCopy(a);
        for (int i = 0; i < XF_SLOTS; i++) xfRelease(i);
        xf_forget(&gXf);
        gXf.boot = 0; gXf.front = 0;
    }
    if (a) a->forgetBootDesc();
    if (l) IOLockUnlock(l);
}
#define XEFLIP_TRY(a, fb, head, s, pipe) nvXeFlip((a), (fb), (head), (s), (pipe))
// </xeflip>
