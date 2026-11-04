// SPDX-License-Identifier: BSD-2-Clause
// XeGfxIntelFramebuffer, GU605 display path (docs/GU605-DISPLAY-DESIGN.md), included inside the class body.
// Nothing here runs until XeGfx's stage 4 let Linux i915 own the display (XeGfxKmsReady); until then every method
// below answers exactly as IONDRVFramebuffer does. Then:
//  * the boot raster is offered at every refresh rate i915 validated for the panel (IDs kKmsBase | index << 8, the
//    firmware's mode stays as it was), and setDisplayMode commits the chosen one through i915 (checked first);
//  * one display at a time: when a monitor is connected to the left USB-C port and has a mode that fits the boot
//    framebuffer, it gets the picture (the panel goes off) and this connection becomes that monitor (EDID, not
//    built-in, its modes); unplugged, the panel comes back at the refresh it had. Boot-arg -xeintelfbnoext keeps the
//    panel. Every change fires the connect interrupt once, so IOFramebuffer and WindowServer read the modes again.
// Written against IOFramebuffer's interface only (NullMoth's NVKMS bridge was a design reference, none of its code).
    static constexpr IODisplayModeID kKmsBase = 0x10000;
    static constexpr int kKmsMax = 32;
    IOFBInterruptProc fConnProc = nullptr; OSObject *fConnTarget = nullptr; void *fConnRef = nullptr;
    volatile bool fConnEnabled = false; int fConnToken = 0; unsigned fConnFired = 0;
    thread_call_t fKmsCall = nullptr; IOLock *fKmsLock = nullptr;
    bool fKmsUp = false, fExternal = false; unsigned fKmsHot = ~0u;
    IODisplayModeID fBootId = 0, fCurId = 0; IOIndex fBootDepth = 0;
    uint32_t fBootW = 0, fBootH = 0, fBootHz16 = 0, fStartHz16 = 0, fIntMhz = 0;
    XifbKmsMode fKm[kKmsMax]; int fKmN = 0; bool fKmOffer[kKmsMax]; char fConnName[16] = {};
    uint8_t fEdidInt[256]; int fEdidIntLen = 0;

    static void kmsThunk(thread_call_param_t me, thread_call_param_t) { ((XeGfxIntelFramebuffer *)me)->kmsTick(); }
    void kmsArm(unsigned ms) {
        thread_call_t c = fKmsCall; if (!c) return;
        uint64_t dl; clock_interval_to_deadline(ms, kMillisecondScale, &dl); thread_call_enter_delayed(c, dl);
    }
    void kmsWatchStart() {
        if (!fKmsLock) fKmsLock = IOLockAlloc();
        if (!fKmsCall) fKmsCall = thread_call_allocate(kmsThunk, this);
        if (fKmsCall && fKmsLock) kmsArm(2000);
    }
    void kmsFireConnect(const char *why) {
        fConnFired++;
        XIFBLOG("display path: connect interrupt #%u (%s), %s", fConnFired, why, fConnProc && fConnEnabled ? "delivered" : "no handler");
        if (fConnProc && fConnEnabled) fConnProc(fConnTarget, fConnRef);
        setProperty("XeGfxKmsConnection", fConnName);
        setProperty("XeGfxKmsConnectFired", (unsigned long long)fConnFired, 32);
    }
    static uint32_t kmsHz16(const XifbKmsMode &m) { return (uint32_t)(((uint64_t)m.mhz << 16) / 1000); }
    // which of fKm are offered: internal -> the boot raster at other rates; external -> what fits the boot framebuffer
    void kmsOffer() {
        for (int i = 0; i < fKmN; i++) {
            const XifbKmsMode &m = fKm[i];
            bool ok = !(m.flags & 0x10) && m.mhz;   // DRM_MODE_FLAG_INTERLACE
            if (ok && !fExternal) {
                const uint32_t hz = kmsHz16(m), d = hz > fBootHz16 ? hz - fBootHz16 : fBootHz16 - hz;
                ok = m.w == fBootW && m.h == fBootH && d > (3u << 15);   // not the firmware's rate (1.5 Hz)
            } else if (ok) ok = m.w <= fBootW && m.h <= fBootH;
            for (int j = 0; ok && j < i; j++)
                if (fKmOffer[j] && fKm[j].w == m.w && fKm[j].h == m.h && (fKm[j].mhz > m.mhz ? fKm[j].mhz - m.mhz : m.mhz - fKm[j].mhz) < 500)
                    ok = false;
            fKmOffer[i] = ok;
            XIFBLOG("display path: %s mode %d %ux%u %u.%03u Hz clock %u kHz%s -> %s", fConnName, i, m.w, m.h, m.mhz / 1000, m.mhz % 1000,
                    m.clock_khz, m.preferred ? " preferred" : "", ok ? "offered" : "not offered");
        }
    }
    // the connector's modes from i915; true when it is connected. cur gets the mode on its CRTC (w == 0: none)
    bool kmsLoad(const char *name, XifbKmsMode *cur, XifbKmsMode *out, int *n) {
        const size_t sz = sizeof(XifbKmsMode) * (kKmsMax + 1);
        XifbKmsMode *b = (XifbKmsMode *)IOMallocZero(sz); int io[3] = { 0, 0, 0 };
        if (!b) return false;
        bool ok = xifbKmsCall(3, (void *)name, b, io) && io[0] >= 0;
        if (ok) {
            *n = io[0] > kKmsMax ? kKmsMax : io[0];
            bcopy(b, out, sizeof(XifbKmsMode) * (unsigned)*n);
            if (cur) *cur = b[kKmsMax];
        }
        IOFree(b, sz);
        return ok && io[1];
    }
    IODisplayModeID kmsIdFor(const XifbKmsMode &cur) {
        if (!fExternal && cur.w == fBootW && cur.h == fBootH) {
            const uint32_t hz = kmsHz16(cur), d = hz > fBootHz16 ? hz - fBootHz16 : fBootHz16 - hz;
            if (d <= (3u << 15)) return fBootId;
        }
        for (int i = 0; i < fKmN; i++)
            if (fKmOffer[i] && fKm[i].w == cur.w && fKm[i].h == cur.h && (fKm[i].mhz > cur.mhz ? fKm[i].mhz - cur.mhz : cur.mhz - fKm[i].mhz) < 500)
                return kKmsBase | (i << 8);
        if (!fExternal) return fBootId;
        for (int i = 0; i < fKmN; i++) if (fKmOffer[i]) return kKmsBase | (i << 8);
        return fBootId;
    }
    bool kmsCommit(const char *name, uint32_t w, uint32_t h, uint32_t mhz) {
        uint32_t v[4] = { w, h, mhz, 0 }; char msg[128] = {};
        bool ok = xifbKmsCall(1, (void *)name, v, msg);
        XIFBLOG("display path: %s %ux%u %u mHz -> %s (%s)", name, w, h, mhz, ok ? "committed" : "REFUSED", msg);
        return ok;
    }
    void kmsUpNow() {
        IODisplayModeID id = 0; IOIndex depth = 0; IODisplayModeInformation bi; bzero(&bi, sizeof bi);
        if (IONDRVFramebuffer::getCurrentDisplayMode(&id, &depth) != kIOReturnSuccess ||
            IONDRVFramebuffer::getInformationForDisplayMode(id, &bi) != kIOReturnSuccess || !bi.nominalWidth) {
            XIFBLOG("display path: i915 is up, but the boot mode is unreadable: staying on the boot mode"); fKmsUp = true; return;
        }
        XifbKmsMode cur; bzero(&cur, sizeof cur);
        IOLockLock(fKmsLock);
        fBootId = id; fBootDepth = depth; fBootW = bi.nominalWidth; fBootH = bi.nominalHeight;
        fBootHz16 = fStartHz16 ? fStartHz16 : (bi.refreshRate ? bi.refreshRate : (60u << 16));
        bcopy(fEdid, fEdidInt, sizeof fEdidInt); fEdidIntLen = fEdidLen;
        strlcpy(fConnName, "eDP-1", sizeof fConnName);
        kmsLoad(fConnName, &cur, fKm, &fKmN);
        kmsOffer();
        fCurId = kmsIdFor(cur); fIntMhz = cur.mhz;
        fKmsUp = true;
        IOLockUnlock(fKmsLock);
        XIFBLOG("display path: i915 owns the display; boot mode 0x%x %ux%u %u.%02u Hz, %d i915 modes, current 0x%x (%u mHz)",
                (unsigned)fBootId, fBootW, fBootH, fBootHz16 >> 16, ((fBootHz16 & 0xffff) * 100) >> 16, fKmN, (unsigned)fCurId, cur.mhz);
        int off = 0;
        if (!PE_parse_boot_argn("-xeintelfbnoext", &off, sizeof off)) kmsHotplug(true);
        kmsFireConnect("i915 modes");
    }
    // single display: an external monitor takes over when it appears, the panel comes back when it goes
    void kmsHotplug(bool quiet = false) {
        int off = 0;
        if (PE_parse_boot_argn("-xeintelfbnoext", &off, sizeof off)) return;
        static const char *const ports[] = { "DP-1", "DP-2", "DP-3", "DP-4" };
        XifbKmsMode *mm = (XifbKmsMode *)IOMallocZero(sizeof(XifbKmsMode) * kKmsMax); if (!mm) return;
        const char *ext = nullptr; int en = 0; XifbKmsMode pick; bzero(&pick, sizeof pick);
        for (const char *p : ports) {
            int n = 0; bzero(mm, sizeof(XifbKmsMode) * kKmsMax);
            if (!kmsLoad(p, nullptr, mm, &n)) continue;
            // the preferred raster if it fits, else the largest that does; the fastest refresh at that raster
            for (int i = 0; i < n; i++) if (mm[i].preferred && mm[i].w <= fBootW && mm[i].h <= fBootH) { pick = mm[i]; break; }
            if (!pick.w) for (int i = 0; i < n; i++) if (mm[i].w <= fBootW && mm[i].h <= fBootH && (uint64_t)mm[i].w * mm[i].h > (uint64_t)pick.w * pick.h) pick = mm[i];
            if (pick.w) { ext = p; en = n; break; }
            XIFBLOG("display path: %s connected, but no mode fits the %ux%u boot framebuffer", p, fBootW, fBootH);
        }
        IOFree(mm, sizeof(XifbKmsMode) * kKmsMax);
        if (ext && (!fExternal || strcmp(ext, fConnName))) {
            if (!fExternal) { XifbKmsMode c; int n2 = 0; XifbKmsMode t[kKmsMax]; if (kmsLoad("eDP-1", &c, t, &n2) && c.w) fIntMhz = c.mhz; }
            if (!kmsCommit(ext, pick.w, pick.h, 0)) return;
            uint8_t *e = (uint8_t *)IOMallocZero(1024); int el = 0;
            if (e && xifbKmsCall(5, (void *)ext, e, &el) && el > 0) { bcopy(e, fEdid, el < 256 ? el : 256); fEdidLen = el < 256 ? el : 256; }
            else fEdidLen = 0;
            if (e) IOFree(e, 1024);
            XifbKmsMode cur; bzero(&cur, sizeof cur);
            IOLockLock(fKmsLock);
            fExternal = true; strlcpy(fConnName, ext, sizeof fConnName);
            kmsLoad(fConnName, &cur, fKm, &fKmN); kmsOffer(); fCurId = kmsIdFor(cur);
            IOLockUnlock(fKmsLock);
            XIFBLOG("display path: %s has the picture (%d modes, %ux%u, EDID %d B); the panel is off", ext, en, cur.w, cur.h, fEdidLen);
            if (!quiet) kmsFireConnect("monitor connected");
        } else if (!ext && fExternal) {
            if (!kmsCommit("eDP-1", fBootW, fBootH, fIntMhz)) return;
            XifbKmsMode cur; bzero(&cur, sizeof cur);
            IOLockLock(fKmsLock);
            fExternal = false; strlcpy(fConnName, "eDP-1", sizeof fConnName);
            bcopy(fEdidInt, fEdid, sizeof fEdidInt); fEdidLen = fEdidIntLen;
            kmsLoad(fConnName, &cur, fKm, &fKmN); kmsOffer(); fCurId = kmsIdFor(cur);
            IOLockUnlock(fKmsLock);
            XIFBLOG("display path: monitor gone, the panel is back at %u mHz", fIntMhz);
            if (!quiet) kmsFireConnect("monitor disconnected");
        }
    }
    void kmsTick() {
        if (!fKmsUp) { if (xifbKmsService()) kmsUpNow(); }
        else { unsigned h = 0; if (xifbKmsCall(4, &h, nullptr, nullptr) && h != fKmsHot) { fKmsHot = h; kmsHotplug(); } }
        kmsArm(fKmsUp ? 1000 : 2000);
    }
    const XifbKmsMode *kmsMode(IODisplayModeID id) const {
        if (!fKmsUp || id < kKmsBase || id >= kKmsBase + (kKmsMax << 8) || (id & 0xff)) return nullptr;
        const int i = (int)((id - kKmsBase) >> 8);
        return i < fKmN && fKmOffer[i] ? &fKm[i] : nullptr;
    }
    void kmsInfo(const XifbKmsMode *m, IODisplayModeInformation *info) {
        info->nominalWidth = m->w; info->nominalHeight = m->h; info->refreshRate = kmsHz16(*m);
        info->flags = (info->flags & ~(UInt32)kDisplayModeDefaultFlag) | kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeValidForHiResFlag;
        if (fExternal && m->preferred) info->flags |= kDisplayModeDefaultFlag;
        if (fEdidLen && fEdid[21] && fEdid[22]) { info->imageWidth = fEdid[21] * 10; info->imageHeight = fEdid[22] * 10; }
    }
public:
    void stop(IOService *provider) override {
        if (fKmsCall) {   // a tick already running finishes; none is armed again (fKmsCall cleared first)
            thread_call_t c = fKmsCall; fKmsCall = nullptr; thread_call_cancel(c);
            while (!thread_call_free(c)) { thread_call_cancel(c); IOSleep(1); }
        }
        IONDRVFramebuffer::stop(provider);
    }
    IOItemCount getDisplayModeCount() override {
        if (!fKmsUp || !fBootId) return IONDRVFramebuffer::getDisplayModeCount();
        IOItemCount n = fExternal ? 0 : 1;
        for (int i = 0; i < fKmN; i++) if (fKmOffer[i]) n++;
        return n ? n : 1;
    }
    IOReturn getDisplayModes(IODisplayModeID *all) override {
        if (!fKmsUp || !fBootId) return IONDRVFramebuffer::getDisplayModes(all);
        if (!all) return kIOReturnBadArgument;
        IOLockLock(fKmsLock);
        IOItemCount n = 0;
        if (!fExternal) all[n++] = fBootId;
        for (int i = 0; i < fKmN; i++) if (fKmOffer[i]) all[n++] = kKmsBase | (i << 8);
        if (!n) all[n++] = fBootId;
        IOLockUnlock(fKmsLock);
        return kIOReturnSuccess;
    }
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override {
        if (!fKmsUp || !fBootId) return IONDRVFramebuffer::getCurrentDisplayMode(mode, depth);
        if (mode) *mode = fCurId;
        if (depth) *depth = fBootDepth;
        return kIOReturnSuccess;
    }
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override {
        if (!fKmsUp || !fBootId) return IONDRVFramebuffer::setDisplayMode(mode, depth);
        if (mode == fCurId) return kIOReturnSuccess;
        const XifbKmsMode *km = kmsMode(mode);
        if (!km && (mode != fBootId || fExternal)) return kIOReturnUnsupportedMode;
        const uint32_t w = km ? km->w : fBootW, h = km ? km->h : fBootH, mhz = km ? km->mhz : (uint32_t)(((uint64_t)fBootHz16 * 1000) >> 16);
        if (!kmsCommit(fConnName, w, h, mhz)) return kIOReturnUnsupportedMode;
        fCurId = mode;
        if (!fExternal) fIntMhz = mhz;
        setProperty("XeGfxKmsMode", (unsigned long long)mode, 32);
        return kIOReturnSuccess;
    }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override {
        const XifbKmsMode *km = kmsMode(mode);
        IOReturn r = IONDRVFramebuffer::getPixelInformation(km ? fBootId : mode, depth, aperture, pi);
        if (r == kIOReturnSuccess && km && pi) { pi->activeWidth = km->w; pi->activeHeight = km->h; }
        return r;
    }
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
        const XifbKmsMode *km = kmsMode(mode);
        if (!km || !info) return IONDRVFramebuffer::getTimingInfoForDisplayMode(km ? fBootId : mode, info);
        bzero(info, sizeof *info);
        info->appleTimingID = kIOTimingIDInvalid;
        info->flags = kIODetailedTimingValid;
        IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
        d.pixelClock = d.minPixelClock = d.maxPixelClock = (UInt64)km->clock_khz * 1000;
        d.horizontalActive = km->w; d.horizontalBlanking = km->htotal - km->w;
        d.horizontalSyncOffset = km->hss - km->w; d.horizontalSyncPulseWidth = km->hse - km->hss;
        d.verticalActive = km->h; d.verticalBlanking = km->vtotal - km->h;
        d.verticalSyncOffset = km->vss - km->h; d.verticalSyncPulseWidth = km->vse - km->vss;
        d.horizontalSyncConfig = (km->flags & 1) ? 1 : 0; d.verticalSyncConfig = (km->flags & 4) ? 1 : 0;   // +hsync, +vsync
        d.signalConfig = kIODigitalSignal;
        return kIOReturnSuccess;
    }
public:
