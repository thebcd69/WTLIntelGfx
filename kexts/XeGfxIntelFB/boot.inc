// boot.inc (inside XeGfxIntelFramebuffer): [gu605-mac] 2026-11-13, the hold. The boot's first WindowServer is kept
// waiting until the runner has brought XeGfx to stage 7 and armed the accelerator, so it comes up on the Intel GPU and
// is not restarted afterwards (owner 2026-11-13: "boot sekansina tasi, tipki NullMoth NVIDIA gibi").
//
// What is known (logs of 2026-11-13). WindowServer starts ~18 s after power-on and then waits: on both Eco boots it
// went on a few seconds after this kext's start() had ended ("Setting offline display ... CGXConstructVirtualFramebuffer"
// 10.5 s and 2.5 s later), opened the framebuffer and asked for the Intel Metal device at once; the stages need
// another 15-40 s, so the answer was always "debug.nvaccelfb reads 0 -- no device". (On the 13:58 boot it waited 60 s
// more, "GPUWranglerWaitForPublish: timed out": the powered-off NVIDIA device was still in the registry without a
// framebuffer. A cold Eco boot has no such node and no such wait.)
// The hold: this service stays busy (adjustBusy) after its start, so IOKit is not quiet. The runner
// (gu605-auto.sh/gu605-desktop.sh, "early") does the stages and the arming exactly as before and then writes
// debug.xeintelfb_hold=0. If WindowServer does not wait for a quiet IOKit, nothing is lost: it opens the framebuffer as
// before and the runner goes its usual way.
//
// WAS (v5-v8, abandoned): XgFbMove, XeGfx's stages and the arming were run from a thread of this kext. Four Eco boots
// were lost to that on 2026-11-13 (a freeze right after the first step among them). NOTHING is started from here any
// more: no kext load, no stage, no sysctl of another kext.
//
// Opt-in and guard, NVRAM xeintelfb-hold (first byte): '1' = hold at this start; this kext writes '2' before it holds,
// and the runner writes '1' back only once the desktop of that boot is confirmed. Anything else: no hold. The old
// variable xeintelfb-boot is not read any more. Only on GU605, only in Eco (no live NVIDIA device on the bus).
// The hold ends by itself after kXifbHoldSeconds whatever the runner does.
public:
    enum { kXifbHoldSeconds = 70 };
    static char holdNvram() {
        IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return 0;
        char c = 0;
        if (OSObject *p = o->copyProperty("xeintelfb-hold")) {
            if (OSData *d = OSDynamicCast(OSData, p)) { if (d->getLength() >= 1) c = *(const char *)d->getBytesNoCopy(); }
            else if (OSString *s = OSDynamicCast(OSString, p)) c = s->getChar(0);
            p->release();
        }
        o->release();
        return c;
    }
    static void holdNvramSet(char c) {
        IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return;
        if (OSData *d = OSData::withBytes(&c, 1)) { o->setProperty("xeintelfb-hold", d); d->release(); }
        o->release();
    }
    // a display-class PCI function of NVIDIA that answers in config space: Standard or Ultimate, not Eco (in Eco the
    // node can stay in the registry with its cached IDs while the device reads ffff)
    static bool holdNvidiaLive() {
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
    thread_call_t fHoldWd = nullptr; volatile SInt32 fHoldBusy = 0;
    static uint64_t holdUpMs() { uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000; }
    void holdSay(const char *what) { XIFBLOG("hold [%llu ms up]: %s", holdUpMs(), what); setProperty("XeGfxHold", what); }
    // from the runner (debug.xeintelfb_hold=0), from the time limit, or from stop(): whichever comes first
    void holdRelease(const char *why) {
        if (!OSCompareAndSwap(1, 0, &fHoldBusy)) return;
        adjustBusy(-1);
        char s[96]; snprintf(s, sizeof s, "released (%s)", why);
        holdSay(s);
        setProperty("XeGfxHoldReleasedMs", holdUpMs(), 64);
    }
    static void holdWdThunk(thread_call_param_t p0, thread_call_param_t) { ((XeGfxIntelFramebuffer *)p0)->holdRelease("time limit"); }
    // from start(): decides, and holds
    void holdStart() {
        static bool once; if (once) return; once = true;
        int off = 0; const char nv = holdNvram();
        setProperty("XeGfxHoldNvram", (unsigned long long)(unsigned char)nv, 8);
        if (!fGu605 || nv != '1') {
            if (nv == '2') XIFBLOG("hold: off, the boot that last held was never confirmed (nvram xeintelfb-hold=2)");
            return;
        }
        if (PE_parse_boot_argn("-xeintelfbnohold", &off, sizeof off) || PE_parse_boot_argn("-xegfxoff", &off, sizeof off) ||
            PE_parse_boot_argn("-xeintelfbnokms", &off, sizeof off) || PE_parse_boot_argn("-xegfxnoauto", &off, sizeof off)) { holdSay("off by boot-arg"); return; }
        if (holdNvidiaLive()) { holdSay("not Eco: an NVIDIA display device is live"); return; }
        fHoldWd = thread_call_allocate(holdWdThunk, this);
        if (!fHoldWd) return;
        holdNvramSet('2');
        gXifbHold = this;
        sysctl_register_oid(&sysctl__debug_xeintelfb_hold); gXifbHoldSysctl = true;
        fHoldBusy = 1; adjustBusy(1);
        uint64_t dl; clock_interval_to_deadline(kXifbHoldSeconds, kSecondScale, &dl); thread_call_enter_delayed(fHoldWd, dl);
        holdSay("holding");
    }
    void holdStop() {
        holdRelease("stop");
        if (gXifbHoldSysctl) { sysctl_unregister_oid(&sysctl__debug_xeintelfb_hold); gXifbHoldSysctl = false; }
        if (gXifbHold == this) gXifbHold = nullptr;
        if (thread_call_t c = fHoldWd) { fHoldWd = nullptr; while (!thread_call_free(c)) { thread_call_cancel(c); IOSleep(1); } }
    }
