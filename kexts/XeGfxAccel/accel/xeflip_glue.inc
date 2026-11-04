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
#define XF_PAUSE(n) IOSleep(1)
#include "xeflip_core.h"

static int gXeFlip = 0, gXeFlipIdleMs = 1000, gXfLastWhy = 0, gXfMapUs = 0, gXfFlipUs = 0, gXfLogs = 0;
static struct xf gXf;
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

static void nvXeFlipSysctls(void)
{
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip);          sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_idle_ms);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_last_why); sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_map_us);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_flip_us);  sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_flips);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_maps);     sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_homes);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_idle_homes); sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_kept);
    sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_copied);   sysctl_register_oid(&sysctl__debug_nvaccel_xeflip_ggtt);
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

// true: the frame is on the panel (or kept back); false: copy it as before
static bool nvXeFlip(NVAccel *a, IOService *fb, unsigned head, IOSurface *s)
{
    if (!gXeFlip || head != 4 || !s || !a) return false;
    if (!gXeMmio && a->fPCI) gXeMmio = a->fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    if (!gXeMmio || gXeMmio->getLength() < 0x1000000) return xfNo(-8);
    if (s->getPixelFormat() != 0x42475241u) return xfNo(-9);   // 'BGRA': the plane's XRGB8888
    const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
    if (!W || !H || W > 16384 || H > 16384 || P < 4 * W || (P & 63)) return xfNo(-10);
    IOMemoryDescriptor *md = s->getMemoryDescriptor();
    const unsigned long long bytes = H * P;
    if (!md || md->getLength() < bytes) return xfNo(-11);
    volatile xf_u8 *bar = (volatile xf_u8 *)gXeMmio->getVirtualAddress();
    const int p = xf_pipe_find(bar, (xf_u32)W, (xf_u32)H, (xf_u32)P);
    if (p < 0) return xfNo(p);
    // as the copy path: the composite must be finished, and its newest pixels in the surface's memory
    nvSurfWaitReady(s->getSurfaceID());
    if (!nvAccelSurfCopyReady(s, head)) { OSAddAtomic64(1, &gCntXfKept); return true; }
    IOLock *l = xfLock();
    if (!l) return xfNo(-8);
    IOLockLock(l);
    gXf.bar = bar; gXf.pte = gXfPte; gXf.pipe = p;
    const xf_u64 key = (xf_u64)(uintptr_t)md;
    int i = xf_lookup(&gXf, key);
    if (i < 0) {
        const xf_u32 pages = (xf_u32)((bytes + 4095) >> 12);
        if (pages > XF_SLOT_PAGES) { IOLockUnlock(l); return xfNo(XF_TOO_BIG); }
        i = xf_pick(&gXf);
        if (i < 0) { IOLockUnlock(l); return xfNo(XF_NO_SLOT); }
        xfRelease(i);
        if (md->prepare() != kIOReturnSuccess) { IOLockUnlock(l); return xfNo(-11); }
        const uint64_t t0 = mach_absolute_time();
        bool whole = true;
        for (xf_u32 k = 0; k < pages && whole; k++) {
            IOByteCount len = 0;
            const addr64_t pa = md->getPhysicalSegment((IOByteCount)k << 12, &len, kIOMemoryMapperNone);
            if (!pa || (pa & 0xfff)) whole = false; else gXfPhys[k] = pa;
        }
        const int r = whole ? xf_map(&gXf, i, key, gXfPhys, pages) : -11;
        if (r != XF_OK) { md->complete(); IOLockUnlock(l); return xfNo(r); }
        md->retain(); gXfMd[i] = md;
        gXfGeo[i][0] = W; gXfGeo[i][1] = H; gXfGeo[i][2] = P;
        gXfMapUs = (int)nvXeUs(t0);
        const SInt64 nm = OSAddAtomic64(1, &gCntXfMaps);
        if (nm < 8) ALOG("xeflip: surface 0x%x %llux%llu pitch %llu -> slot %d at GGTT 0x%08x (%u pages, %d us)",
                         s->getSurfaceID(), W, H, P, i, xf_slot_addr(&gXf, i), pages, gXfMapUs);
    }
    const uint64_t t1 = mach_absolute_time();
    const int r = xf_flip(&gXf, i);
    gXfFlipUs = (int)nvXeUs(t1);
    if (r == XF_OK) { gXfFb = fb; gXfLastFlip = mach_absolute_time(); }
    IOLockUnlock(l);
    if (r != XF_OK) return xfNo(r);
    const SInt64 n = OSAddAtomic64(1, &gCntXfFlips);
    if (n < 3 || n == 600) ALOG("xeflip: frame #%lld on the panel straight from surface 0x%x (slot %d, %d us to latch, boot 0x%08x)",
                                (long long)n + 1, s->getSurfaceID(), i, gXfFlipUs, gXf.boot);
    xfArmIdle();
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
    gXeFlip = 0;
    IOLock *l = gXfLock;
    if (!l) return;
    IOLockLock(l);
    if (xfHomeWithCopy(NVAccel::gAccel)) { for (int i = 0; i < XF_SLOTS; i++) xfRelease(i); xf_forget(&gXf); }
    IOLockUnlock(l);
}
#define XEFLIP_TRY(a, fb, head, s) nvXeFlip((a), (fb), (head), (s))
// </xeflip>
