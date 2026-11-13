// boot.inc (inside XeGfxIntelFramebuffer): [gu605-mac] 2026-11-13, the Intel desktop before WindowServer's first start.
//
// WAS: everything after this kext's start was done by scripts from a LaunchDaemon (gu605-auto.sh, gu605-stages.sh,
// gu605-desktop.sh): XgFbMove, load XeGfx, its stages 1-7 (i915 takes the display at 4), AGDC, the accelerator's
// framebuffer registration, the scan-out pipe. That ends ~80 s after power-on, long after the automatic login, and
// WindowServer has to be restarted once to find the accelerator: the session ends and the owner logs in a second time
// (owner 2026-11-13: "boot sekansina tasi, tipki NullMoth NVIDIA gibi").
// NOW: the same sequence, the same sysctl writes in the same order, from a thread this kext starts at the end of its
// own start(). While it runs this service stays busy (adjustBusy), so IOKit is not quiet and the boot waits for it the
// way it waits for any GPU driver; WindowServer's first start then finds i915 on the display and the accelerator armed.
// If WindowServer does not wait, nothing is lost: it starts on the firmware framebuffer as before and the runner
// restarts it by its usual rules (gu605-auto.sh sees XeGfxBootRun).
//
// Opt-in and guard, NVRAM xeintelfb-boot (first byte): '1' run at start; '2' a run began and nobody confirmed the
// desktop after it (hang, panic, power button) -> no run until it is '1' again; anything else: off. This kext writes
// '2' before the first step; gu605-auto.sh writes '1' back once the desktop is confirmed (or at an orderly restart).
// Only on GU605, only in Eco (no live NVIDIA device on the bus), only when stage 2's takeover preflight says "ok".
// XeGfx's stage watchdog (debug.xegfx_wdog, 60 s: panics on purpose) covers a stall inside a stage, as before.
//
// The stage and accelerator switches are sysctl OIDs without CTLFLAG_KERN, which kernel sysctlbyname() refuses (EPERM),
// so the handlers are called directly with a request built here (debug.* leaf names only).
    static int xbOld(struct sysctl_req *req, const void *p, size_t l) {
        size_t i = l;
        if (req->oldptr) {
            if (i > req->oldlen - req->oldidx) i = req->oldlen - req->oldidx;
            if (i > 0) bcopy(p, (char *)(uintptr_t)req->oldptr + req->oldidx, i);
        }
        req->oldidx += l;
        return (req->oldptr && i != l) ? ENOMEM : 0;
    }
    static int xbNew(struct sysctl_req *req, void *p, size_t l) {
        if (!req->newptr) return 0;
        if (req->newlen - req->newidx < l) return EINVAL;
        bcopy((const char *)(uintptr_t)req->newptr + req->newidx, p, l);
        req->newidx += l;
        return 0;
    }
    // debug.<leaf>: read into old/oldlen and/or write newp/newlen; ENOENT while the kext that owns it is not up yet
    static int xbSysctl(const char *leaf, void *oldp, size_t *oldlen, const void *newp, size_t newlen) {
        struct sysctl_oid *o = nullptr, *it;
        SLIST_FOREACH(it, &sysctl__debug_children, oid_link) if (it->oid_name && !strcmp(it->oid_name, leaf)) { o = it; break; }
        if (!o || !o->oid_handler) return ENOENT;
        struct sysctl_req req; bzero(&req, sizeof req);
        req.p = current_proc(); req.lock = 1;
        req.oldptr = (user_addr_t)(uintptr_t)oldp; req.oldlen = oldlen ? *oldlen : 0; req.oldfunc = xbOld;
        req.newptr = (user_addr_t)(uintptr_t)newp; req.newlen = newlen; req.newfunc = xbNew;
        int e = o->oid_handler(o, o->oid_arg1, o->oid_arg2, &req);
        if (oldlen) *oldlen = req.oldidx;
        return e;
    }
    static bool xbGet(const char *leaf, int *v) { size_t l = sizeof *v; return xbSysctl(leaf, v, &l, nullptr, 0) == 0; }
    static int xbSet(const char *leaf, int v) { return xbSysctl(leaf, nullptr, nullptr, &v, sizeof v); }
    static bool xbWait(const char *leaf, int want, unsigned ms) {
        for (unsigned t = 0; ; t += 100) { int v = -1; if (xbGet(leaf, &v) && (want < 0 || v == want)) return true; if (t >= ms) return false; IOSleep(100); }
    }
    static char xbNvram() {
        IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return 0;
        char c = 0;
        if (OSObject *p = o->copyProperty("xeintelfb-boot")) {
            if (OSData *d = OSDynamicCast(OSData, p)) { if (d->getLength() >= 1) c = *(const char *)d->getBytesNoCopy(); }
            else if (OSString *s = OSDynamicCast(OSString, p)) c = s->getChar(0);
            p->release();
        }
        o->release();
        return c;
    }
    static void xbNvramSet(char c) {
        IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return;
        if (OSData *d = OSData::withBytes(&c, 1)) { o->setProperty("xeintelfb-boot", d); d->release(); }
        o->release();
    }
    // a display-class PCI function of NVIDIA that answers in config space: Standard or Ultimate, not Eco (in Eco the
    // node can stay in the registry with its cached IDs while the device reads ffff)
    static bool xbNvidiaLive() {
        bool live = false;
        OSDictionary *m = IOService::serviceMatching("IOPCIDevice"); if (!m) return true;
        OSIterator *it = IOService::getMatchingServices(m); m->release(); if (!it) return true;
        while (OSObject *obj = it->getNextObject()) {
            IOPCIDevice *d = OSDynamicCast(IOPCIDevice, obj); if (!d) continue;
            OSData *ven = OSDynamicCast(OSData, d->getProperty("vendor-id")), *cls = OSDynamicCast(OSData, d->getProperty("class-code"));
            if (!ven || ven->getLength() < 2 || !cls || cls->getLength() < 3) continue;
            const uint8_t *v = (const uint8_t *)ven->getBytesNoCopy(), *c = (const uint8_t *)cls->getBytesNoCopy();
            if (v[0] != 0xde || v[1] != 0x10 || c[2] != 0x03) continue;
            if (d->configRead16(kIOPCIConfigVendorID) == 0x10de) { live = true; break; }
        }
        it->release();
        return live;
    }
    thread_call_t fBootCall = nullptr, fBootWd = nullptr; volatile SInt32 fBootBusy = 0; char fBootState[160] = {};
    void bootSay(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap; va_start(ap, fmt); vsnprintf(fBootState, sizeof fBootState, fmt, ap); va_end(ap);
        uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        XIFBLOG("boot run [%llu ms up]: %s", ns / 1000000, fBootState);
        setProperty("XeGfxBootRun", fBootState);
    }
    void bootRelease(const char *why) {
        if (!OSCompareAndSwap(1, 0, &fBootBusy)) return;
        adjustBusy(-1);
        uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        XIFBLOG("boot run: IOKit busy released (%s) %llu ms after power-on", why, ns / 1000000);
        setProperty("XeGfxBootRunReleasedMs", ns / 1000000, 64);
    }
    static void bootThunk(thread_call_param_t p0, thread_call_param_t) { XeGfxIntelFramebuffer *me = (XeGfxIntelFramebuffer *)p0; me->bootRun(); me->bootRelease("run ended"); }
    static void bootWdThunk(thread_call_param_t p0, thread_call_param_t) { ((XeGfxIntelFramebuffer *)p0)->bootRelease("45 s limit"); }
    // from start(): decides, and holds IOKit busy for the run
    void bootRunStart() {
        static bool once; if (once) return; once = true;
        int off = 0; const char nv = xbNvram();
        setProperty("XeGfxBootNvram", (unsigned long long)(unsigned char)nv, 8);
        if (!fGu605 || nv != '1') { if (nv == '2') XIFBLOG("boot run: off, the run of an earlier boot was never confirmed (nvram xeintelfb-boot=2)"); return; }
        if (PE_parse_boot_argn("-xeintelfbnoboot", &off, sizeof off) || PE_parse_boot_argn("-xegfxoff", &off, sizeof off) ||
            PE_parse_boot_argn("-xeintelfbnokms", &off, sizeof off) || PE_parse_boot_argn("-xegfxnokms", &off, sizeof off)) { bootSay("off by boot-arg"); return; }
        if (xbNvidiaLive()) { bootSay("not Eco: an NVIDIA display device is live"); return; }
        fBootCall = thread_call_allocate(bootThunk, this); fBootWd = thread_call_allocate(bootWdThunk, this);
        if (!fBootCall || !fBootWd) return;
        fBootBusy = 1; adjustBusy(1);
        uint64_t dl; clock_interval_to_deadline(45, kSecondScale, &dl); thread_call_enter_delayed(fBootWd, dl);
        bootSay("starting");
        thread_call_enter(fBootCall);
    }
    bool bootStage(int s) {
        int e = xbSet("xegfx_stage", s), now = -1; xbGet("xegfx_stage", &now);
        if (e || now < s) { bootSay("FAILED at stage %d (error %d, stage now %d)", s, e, now); return false; }
        return true;
    }
    void bootRun() {
        xbNvramSet('2');
        // 1. the boot framebuffer out of i915's GuC slot, before XeGfx exists in this boot; 2. XeGfx itself (idle)
        int st = -1;
        if (!xbGet("xegfx_stage", &st)) {
            OSReturn r1 = OSKextLoadKextWithIdentifier("org.local.XgFbMove"); IOSleep(300);
            OSReturn r2 = OSKextLoadKextWithIdentifier("org.watchthelight.XeGfx");
            XIFBLOG("boot run: XgFbMove load 0x%x, XeGfx load 0x%x", r1, r2);
            if (!xbWait("xegfx_stage", -1, 8000)) { bootSay("FAILED: XeGfx did not come up (load 0x%x)", r2); return; }
            xbGet("xegfx_stage", &st);
        }
        if (st != 0) { bootSay("XeGfx is already at stage %d: left to the runner", st); return; }
        xbSet("xegfx_wdog", 60);
        if (!bootStage(1) || !bootStage(2)) return;
        // the takeover preflight of stage 2 (the kext refuses KMS without it too)
        char why[96] = "not recorded";
        if (OSDictionary *m = IOService::serviceMatching("XeGfxResident")) {
            if (IOService *r = IOService::copyMatchingService(m)) {
                if (OSObject *p = r->copyProperty("XeGfxProbe")) {
                    if (OSDictionary *d = OSDynamicCast(OSDictionary, p)) if (OSString *s = OSDynamicCast(OSString, d->getObject("kms-preflight"))) strlcpy(why, s->getCStringNoCopy(), sizeof why);
                    p->release();
                }
                r->release();
            }
            m->release();
        }
        if (strcmp(why, "ok")) { bootSay("stopped after stage 2: takeover preflight says: %s (left to the runner)", why); return; }
        char env[512] = {}; size_t el = sizeof env - 1;
        if (xbSysctl("xegfx_env", env, &el, nullptr, 0)) { bootSay("FAILED: cannot read debug.xegfx_env"); return; }
        env[sizeof env - 1] = 0;
        bool has = false;   // (strnstr is not exported to kexts)
        for (size_t i = 0, n = strlen(env); i + 11 <= n && !has; i++) has = !memcmp(env + i, "xegfx.kms=1", 11);
        if (!has) {
            if (strlcat(env, " xegfx.kms=1", sizeof env) >= sizeof env || xbSysctl("xegfx_env", nullptr, nullptr, env, strlen(env))) { bootSay("FAILED: cannot write debug.xegfx_env"); return; }
        }
        for (int s = 3; s <= 7; s++) if (!bootStage(s)) return;
        bootSay("stages to 7 passed");
        // the accelerator (XeGfxAccel matches once stage 7 claimed the GPU) and this framebuffer published to it
        if (!xbWait("nvaccelfb", -1, 10000)) { bootSay("FAILED: no accelerator after stage 7"); return; }
        for (unsigned t = 0; t < 5000; t += 100) { int hp = 0; if (xbGet("nvaccel_heads_published", &hp) && (hp & 0x10)) break; IOSleep(100); }
        // AGDC + K5, register the framebuffer, scan-out pipe, WindowServer marks, frame transactions (gu605-desktop.sh 2-4b)
        int e;
        if ((e = xbSet("xegfx_agdc", 1)) || !xbWait("xegfx_agdc", 1, 5000)) { bootSay("FAILED: AGDC did not come up (%d)", e); return; }
        if ((e = xbSet("xegfx_agdc_k5", 1))) { bootSay("FAILED: debug.xegfx_agdc_k5 (%d)", e); return; }
        if ((e = xbSet("nvaccelfb", 1))) { bootSay("FAILED: debug.nvaccelfb=1 (%d)", e); return; }
        IOSleep(1000);
        if ((e = xbSet("nvaccelfb", 3))) { bootSay("FAILED: debug.nvaccelfb=3 (%d)", e); return; }
        IOSleep(1000);
        int armed = 0; xbGet("nvaccelfb", &armed);
        if (armed != 1) { int hp = 0, hr = 0; xbGet("nvaccel_heads_published", &hp); xbGet("nvaccel_heads_registered", &hr);
            bootSay("NOT armed (debug.nvaccelfb %d, heads published 0x%x registered 0x%x): left to the runner", armed, hp, hr); return; }
        xbSet("nvaccel_surface_gpu_on", 1);
        if ((e = xbSet("nvaccel_iop", 1))) { bootSay("FAILED: debug.nvaccel_iop=1 (%d)", e); return; }
        uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        setProperty("XeGfxBootRunArmedMs", ns / 1000000, 64);
        bootSay("armed");
    }
