// hybrid.inc: Standard mode (iGPU drives the displays, NVIDIA live): this framebuffer as a head of NullMoth's NVIDIA
// accelerator, so that WindowServer composites on the NVIDIA GPU and every frame reaches the Intel GPU's displays
// (owner, 2026-11-16: "Standart'ta ... bütün ağır yükü NVIDIA'ya yaptırmak ama görüntüyü sol usb-c portundan intel
// üzerinden almak"). Included in the class body of XeGfxIntelFramebuffer.
//
// What NullMoth 3.0's NVAccel (kexts/NVRM/accel/nvrm-accel.cpp, read 2026-11-16) wants from another GPU's framebuffer:
//   1. an object that matches serviceMatching("NMIntelFB"): the subclass at the end of intelfb.cpp has that name and
//      nothing else, and its own personality in Info.plist. Upstream's NMIntelFB kext (panel only, takes over the
//      firmware's mode, untested on display version 14) must not be installed beside this one: same class name.
//   2. the property IOFBDependentIndex = 4 when its publish notification runs (NVAccel::fbPublished reads it for the
//      slot; slot 4 is "another GPU's panel").
//   3. an answer to callPlatformFunction("nvScanoutInfo", NVRMVramRequest version 2): a CPU mapping of the scan-out,
//      its size and pitch, and phys = 0 ("not NVIDIA VRAM": NVAccel then copies each frame, performTransaction).
//   4. debug.nvaccel_igpu = 1 before NVAccel registers its heads (NVRM's auto-go): slot 4 is skipped otherwise.
// Measured before any of this (Standard, NullMoth 3.0, 14:38): NVRMFB publishes one head (a 1920x1080 display that is
// the NVIDIA side of the internal panel's mux), NVAccel is armed and composites it; this framebuffer had no accelerator,
// so the monitor on the left USB-C showed the cursor plane over a black framebuffer.
//
// IOFBDependentIndex has a second reader: IOFramebuffer::open() puts the framebuffer at that index of its controller,
// and IOFBController uses fFbs[0] without a check all over (power, connect changes). Upstream's NMIntelFB therefore
// takes NVRMFB's IOFBDependentID and lives in NVRMFB's controller. This driver keeps its own controller (its own
// IOFBDependentID, as in Eco), where it has to be number 0: the property reads 4 except during open().
//
// Opt-in: NVRAM xeintelfb-hybrid, first byte '1' (sudo nvram xeintelfb-hybrid=1). Without it Standard is as before.
public:
    static bool hybridNvram() {
        IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return false;
        char c = 0;
        if (OSObject *p = o->copyProperty("xeintelfb-hybrid")) {
            if (OSData *d = OSDynamicCast(OSData, p)) { if (d->getLength() >= 1) c = *(const char *)d->getBytesNoCopy(); }
            else if (OSString *s = OSDynamicCast(OSString, p)) c = s->getChar(0);
            p->release();
        }
        o->release();
        return c == '1' || c == 1;
    }
    // GU605, the iGPU has the panel, an NVIDIA display device answers, and the owner switched it on
    static bool hybridWanted(IOService *provider) {
        return xifbIsGu605(provider) && xifbGu605OwnsPanel(provider) && hybridNvram() && holdNvidiaLive();
    }
    // true in the subclass named NMIntelFB only
    virtual bool hybridClass() const { return false; }
    bool fHybrid = false;
    // NullMoth's NVRMVramRequest, version 2 (kexts/NMIntelFB/nvrm_vram_abi.h of 3.0): the layout only, to answer it
    struct XifbVramReq {
        unsigned int version, flags; unsigned long long size, actualSize, phys; void *kva; void *handle;
        unsigned long long mappedTotal; unsigned int width, height, pitch; unsigned long long allocationCookie;
    };
    IOLock *fHyLock = nullptr;
    IOMemoryMap *fHyMap = nullptr, *fHyMapOld = nullptr; IOPhysicalAddress fHyPhys = 0;
    void *fHyKva = nullptr; uint32_t fHyW = 0, fHyH = 0, fHyPitch = 0; volatile bool fHyOK = false;
    volatile SInt64 fHyAsked = 0, fHyDescribed = 0;
    thread_call_t fHyCall = nullptr; unsigned fHyTries = 0;
    // a mode change can put other memory behind the display (the HiDPI desktops): the next ask describes it again. The
    // mapping in use stays until the change after this one: a frame being copied on another thread may still write to it.
    void hybridForget() { if (fHybrid) fHyOK = false; }
    // Until i915 owns the display (the display path, kms.inc) this framebuffer shows whatever the firmware lit and knows
    // neither which display that is nor its EDID: the EDID it has is the panel's, read over AUX A. In Ultimate the panel
    // is on the NVIDIA GPU with that same EDID, and macOS took both for the built-in display and gave the panel this
    // framebuffer's 2560x1440 (15:03, wrong aspect, pointer off). So as a hybrid head it is an unnamed external display
    // until the display path says what is connected.
    bool hyAnon() const { return fHybrid && !fKmsUp; }
    volatile bool fHyOpened = false;
    IOReturn hybridScanout(XifbVramReq *r) {
        if (!r || r->version != 2) return kIOReturnBadArgument;
        // nothing is asked of the boot NDRV before IOFramebuffer::open() has enabled it; NVAccel copies no frame before then
        if (!fHyLock || !fHyOpened) return kIOReturnNotReady;
        OSIncrementAtomic64(&fHyAsked);
        IOLockLock(fHyLock);
        if (!fHyOK) {
            IODisplayModeID mode = 0; IOIndex depth = 0; IOPixelInformation pi = {};
            IOReturn e = getCurrentDisplayMode(&mode, &depth);
            if (e == kIOReturnSuccess) e = getPixelInformation(mode, depth, kIOFBSystemAperture, &pi);
            IODeviceMemory *mem = e == kIOReturnSuccess ? getApertureRange(kIOFBSystemAperture) : nullptr;
            if (!mem) { IOLockUnlock(fHyLock); return e == kIOReturnSuccess ? kIOReturnNoMemory : e; }
            const IOPhysicalAddress phys = mem->getPhysicalAddress();
            if (!fHyMap || phys != fHyPhys || fHyMap->getLength() < (IOByteCount)pi.bytesPerRow * pi.activeHeight) {
                IOMemoryMap *m = mem->map(kIOMapWriteCombineCache);
                if (!m) { mem->release(); IOLockUnlock(fHyLock); return kIOReturnNoMemory; }
                if (fHyMapOld) fHyMapOld->release();
                fHyMapOld = fHyMap; fHyMap = m; fHyPhys = phys;
            }
            mem->release();
            fHyKva = (void *)fHyMap->getVirtualAddress(); fHyW = pi.activeWidth; fHyH = pi.activeHeight; fHyPitch = pi.bytesPerRow;
            fHyOK = fHyKva && fHyW && fHyH && fHyPitch >= 4 * fHyW;
            OSIncrementAtomic64(&fHyDescribed);
            XIFBLOG("hybrid: scan-out for the NVIDIA accelerator: mode 0x%x, %u x %u pitch %u at physical 0x%llx (%llu bytes mapped)%s",
                    (unsigned)mode, fHyW, fHyH, fHyPitch, (unsigned long long)phys, (unsigned long long)fHyMap->getLength(), fHyOK ? "" : " -- NOT usable");
            char b[96]; snprintf(b, sizeof b, "mode 0x%x %ux%u pitch %u phys 0x%llx", (unsigned)mode, fHyW, fHyH, fHyPitch, (unsigned long long)phys);
            setProperty("XeGfxHybridScanout", b);
        }
        const bool ok = fHyOK;
        if (ok) { r->kva = fHyKva; r->phys = 0; r->actualSize = 0; r->width = fHyW; r->height = fHyH; r->pitch = fHyPitch; }
        IOLockUnlock(fHyLock);
        return ok ? kIOReturnSuccess : kIOReturnNotReady;
    }
    // debug.nvaccel_igpu belongs to NVAccel and exists once NVAccel has started: written as soon as it is there (every
    // 100 ms for up to two minutes). NVRM arms the accelerator seconds after that, when its own heads have published.
    static void hybridIgpuThunk(thread_call_param_t me, thread_call_param_t) { ((XeGfxIntelFramebuffer *)me)->hybridIgpuTick(); }
    void hybridIgpuTick() {
        int one = 1, now = -1; size_t len = sizeof now;
        const int w = sysctlbyname("debug.nvaccel_igpu", nullptr, nullptr, &one, sizeof one);
        if (w == 0) {
            (void)sysctlbyname("debug.nvaccel_igpu", &now, &len, nullptr, 0);
            int pub = -1, reg = -1, armed = -1; size_t l2 = sizeof pub, l3 = sizeof reg, l4 = sizeof armed;
            (void)sysctlbyname("debug.nvaccel_heads_published", &pub, &l2, nullptr, 0);
            (void)sysctlbyname("debug.nvaccel_heads_registered", &reg, &l3, nullptr, 0);
            (void)sysctlbyname("debug.nvaccelfb", &armed, &l4, nullptr, 0);
            XIFBLOG("hybrid: debug.nvaccel_igpu = %d after %u tries (%u ms after this start); heads published 0x%x, registered 0x%x, armed %d",
                    now, fHyTries + 1, (fHyTries + 1) * 100, pub, reg, armed);
            char b[96]; snprintf(b, sizeof b, "igpu=%d after %u ms; published 0x%x registered 0x%x armed %d", now, (fHyTries + 1) * 100, pub, reg, armed);
            setProperty("XeGfxHybridIgpu", b);
            return;
        }
        if (++fHyTries >= 1200) { XIFBLOG("hybrid: debug.nvaccel_igpu never appeared (NVAccel did not start): no head on the NVIDIA accelerator");
                                  setProperty("XeGfxHybridIgpu", "debug.nvaccel_igpu never appeared"); return; }
        uint64_t dl; clock_interval_to_deadline(100, kMillisecondScale, &dl); thread_call_enter_delayed(fHyCall, dl);
    }
    // before IONDRVFramebuffer::start(): the publish notification of that start is where NVAccel reads the index
    void hybridBeforeStart(IOService *provider) {
        if (!fHybrid) return;
        fHyLock = IOLockAlloc();
        // [2026-11-16 15:01 panic, first hybrid boot] NVRM's auto-go armed the accelerator before WindowServer had opened
        // this framebuffer: NVAccel::deliverModeChange -> IOAccelLegacyDisplayPipe::displayModeDidChange ->
        // IOFramebuffer::getAttributeExt took the controller's gate, and there was no controller (page fault at 0x120).
        // IOFramebuffer::start() creates the controller only when IOFBDependentID is there already; without it the
        // controller comes with the first open(). So the ID goes in before the start (as upstream's NMIntelFB does it).
        setProperty("IOFBDependentID", provider->getRegistryEntryID(), 64);
        setProperty("IOFBDependentIndex", 4ULL, 32);
        setProperty("XeGfxHybrid", "head 4 of the NVIDIA accelerator (nvram xeintelfb-hybrid=1)");
        XIFBLOG("hybrid: Standard, NVIDIA live, nvram xeintelfb-hybrid=1: publishing as NMIntelFB, IOFBDependentIndex 4");
        if (!fHyCall) fHyCall = thread_call_allocate(hybridIgpuThunk, this);
        if (fHyCall) { uint64_t dl; clock_interval_to_deadline(100, kMillisecondScale, &dl); thread_call_enter_delayed(fHyCall, dl); }
    }
    IOReturn open() override {
        if (!fHybrid) return IONDRVFramebuffer::open();
        // number 0 of its own controller for IOGraphics (see the header), 4 again for NVAccel
        setProperty("IOFBDependentIndex", 0ULL, 32);
        const IOReturn r = IONDRVFramebuffer::open();
        setProperty("IOFBDependentIndex", 4ULL, 32);
        if (r == kIOReturnSuccess) fHyOpened = true;
        static unsigned n; if (++n <= 4) XIFBLOG("hybrid: open %u -> 0x%x", n, (unsigned)r);
        return r;
    }
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override {
        if (fHybrid && fn && fn->isEqualTo("nvScanoutInfo")) return hybridScanout((XifbVramReq *)p1);
        return IONDRVFramebuffer::callPlatformFunction(fn, wait, p1, p2, p3, p4);
    }
