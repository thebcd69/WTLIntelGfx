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
        OSIncrementAtomic(&fKmsBusy);   // the tick leaves the cursor alone while i915 commits
        kmsCursorPark();
        bool ok = xifbKmsCall(1, (void *)name, v, msg);
        XIFBLOG("display path: %s %ux%u %u mHz -> %s (%s)", name, w, h, mhz, ok ? "committed" : "REFUSED", msg);
        kmsCursorArm(ok ? "after a commit" : "after a refused commit");
        OSDecrementAtomic(&fKmsBusy);
        return ok;
    }
    // Hardware cursor while i915 owns the display (2026-11-10). WAS: software cursor from the takeover on, because the
    // GOP-era setup (curDdb: blocks 2016..2047, fixed watermarks) is wrong under i915 and underran the pipe. With a
    // software cursor WindowServer composites the pointer, so it moved at the desktop's frame rate (owner: "mouse 60 Hz
    // gibi ama 240 degil", and 5 fps without Metal).
    // i915 keeps the pipe's cursor share of the DBUF reserved while its own cursor plane is off
    // (skl_crtc_allocate_plane_ddb) and writes it to CUR_BUF_CFG, with every cursor watermark off (read on this
    // machine, pipe A alone: CUR_BUF_CFG 0x0fff0f16 = blocks 3862..4095, plane 1 0..3861, CUR_WM* all 0). So the
    // cursor plane can be ours inside i915's allocation; only its watermarks are missing. They follow from plane 1's:
    // the same latency gives the same line count per level, and a 256-pixel ARGB row (1024 B in 512 B blocks, +1 as
    // skl_compute_wm_params does for linear surfaces) is 3 blocks per line, result +1. Plane 1 confirms the formula:
    // level 0 is 3 lines * 21 + 1 = 64 blocks, level 1 39 * 21 + 1 = 820. The transition watermarks add i915's
    // offset to level 0 (plane 1: 78 - 64 = 14). Levels are enabled exactly where plane 1 has them enabled.
    // An i915 commit turns cursor A and its watermarks off again: kmsCommit parks the cursor before and arms it
    // after, and the tick arms it again after a commit that did not come through here (gu605-display.sh).
    // NVRAM xeintelfb-kmscur=0 keeps the software cursor (read every tick: takes effect within a second).
    bool fKmsCur = false; unsigned fKmsCurLogs = 0; volatile SInt32 fKmsBusy = 0;
    static bool kmsCurWanted() {
        IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return true;
        bool want = true;
        if (OSObject *p = o->copyProperty("xeintelfb-kmscur")) {
            if (OSData *d = OSDynamicCast(OSData, p)) { if (d->getLength() >= 1 && *(const char *)d->getBytesNoCopy() == '0') want = false; }
            else if (OSString *st = OSDynamicCast(OSString, p)) { if (st->getChar(0) == '0') want = false; }
            p->release();
        }
        o->release();
        return want;
    }
    // cursor A and its watermarks as an i915 commit expects to find them: off
    void kmsCursorPark() {
        if (!fKmsCur || !fCurLock) return;
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(fCurLock);
        fKmsCur = false;
        wr(XIFB_CUR_CTL, 0);
        for (uint32_t o = 0; o < 0x20; o += 4) wr(XIFB_CUR_WM0 + o, 0);
        wr(0x70168, 0);
        wr(XIFB_CUR_BASE, rd(XIFB_CUR_BASE));
        IOSimpleLockUnlockEnableInterrupt(fCurLock, is);
    }
    bool kmsCursorArm(const char *why) {
        if (!fKmsUp && !gXifbKms) return false;
        if (!fCurLock || !fCurKva || !kmsCurWanted()) return false;
        const uint32_t buf = rd(XIFB_CUR_BUF), s = buf & 0xfffu, e = (buf >> 16) & 0xfffu, size = e >= s && buf ? e - s + 1 : 0;
        uint32_t cw[9] = {}, need = 0; const uint32_t en = 1u << 31, p0 = rd(0x70240);
        for (int l = 0; l < 7; l++) {   // levels 0..5 and the SAGV level (0x70258)
            const uint32_t pw = rd(0x70240 + 4 * (uint32_t)l), lines = (pw >> 14) & 0x1fffu;
            if (!(pw & en)) continue;
            cw[l] = en | (pw & (1u << 30)) | (lines << 14) | (lines * 3 + 1);
            if (lines * 3 + 1 > need) need = lines * 3 + 1;
        }
        const uint32_t pst = rd(0x7025c), pt = rd(0x70268);   // SAGV transition, transition
        if ((pst & en) && cw[6]) cw[7] = en | (((cw[6] & 0xfffu) + ((pst & 0xfffu) - (rd(0x70258) & 0xfffu))) & 0xfffu);
        if ((pt & en) && cw[0]) cw[8] = en | (((cw[0] & 0xfffu) + ((pt & 0xfffu) - (p0 & 0xfffu))) & 0xfffu);
        for (int l = 7; l < 9; l++) if ((cw[l] & 0xfffu) > need) need = cw[l] & 0xfffu;
        if (!(p0 & en) || !cw[0] || !size || need > size || !(rd(0x70180) & en)) {
            if (fKmsCurLogs++ < 6) XIFBLOG("display path: no hardware cursor (%s): CUR_BUF_CFG %08x (%u blocks), needs %u, plane 1 CTL %08x WM0 %08x", why, buf, size, need, rd(0x70180), p0);
            return false;
        }
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(fCurLock);
        for (int l = 0; l < 8; l++) wr(XIFB_CUR_WM0 + 4 * (uint32_t)l, cw[l]);
        wr(0x70168, cw[8]);
        fCurDdb = true; fCurReady = true; fKmsCur = true;
        curPtes();
        curRegs(fCurX, fCurY, fCurVis && fCurImg);
        IOSimpleLockUnlockEnableInterrupt(fCurLock, is);
        if (fKmsCurLogs++ < 12) XIFBLOG("display path: hardware cursor armed (%s): DBUF %u..%u, level 0 %08x, SAGV %08x, trans %08x", why, s, e, cw[0], cw[6], cw[8]);
        return true;
    }
    // software cursor: the cursor plane is not usable under i915 right now (no DBUF share, or xeintelfb-kmscur=0)
    void kmsCursorOff() {
        if (!fCurReady || !fCurLock) return;
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(fCurLock);
        fCurReady = false; fKmsCur = false;
        wr(XIFB_CUR_CTL, 0);
        for (uint32_t o = 0; o < 0x20; o += 4) wr(XIFB_CUR_WM0 + o, 0);
        wr(0x70168, 0);
        wr(XIFB_CUR_BASE, rd(XIFB_CUR_BASE));
        IOSimpleLockUnlockEnableInterrupt(fCurLock, is);
        XIFBLOG("display path: hardware cursor off, software cursor from now on");
    }
    void kmsUpNow() {
        if (!kmsCursorArm("i915 took the display")) kmsCursorOff();
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
        else {
            unsigned h = 0; if (xifbKmsCall(4, &h, nullptr, nullptr) && h != fKmsHot) { fKmsHot = h; kmsHotplug(); }
            // the cursor follows xeintelfb-kmscur and comes back after an i915 commit from elsewhere; a change of
            // path reaches IOFramebuffer through the connect interrupt (it asks for every cursor image again)
            const bool want = kmsCurWanted();
            if (fKmsBusy) { }
            else if (fCurReady && !want) { kmsCursorOff(); kmsFireConnect("software cursor asked for"); }
            else if (want && fCurKva && fCurLock && (!fCurReady || !fKmsCur || !(rd(XIFB_CUR_WM0) & (1u << 31)))) {
                const bool was = fCurReady;
                const bool ok = kmsCursorArm("tick");
                if (ok && !was) kmsFireConnect("hardware cursor back");
                // "ready" without watermarks would be an invisible pointer: software until the next tick succeeds
                else if (!ok && was && !fKmsBusy) { kmsCursorOff(); kmsFireConnect("no cursor share from i915"); }
            }
        }
        kmsArm(fKmsUp ? 1000 : 500);
    }
    const XifbKmsMode *kmsMode(IODisplayModeID id) const {
        if (!fKmsUp || id < kKmsBase || id >= kKmsBase + (kKmsMax << 8) || (id & 0xff)) return nullptr;
        const int i = (int)((id - kKmsBase) >> 8);
        return i < fKmN && fKmOffer[i] ? &fKm[i] : nullptr;
    }
    // the monitor's default mode: the fastest offered one at its preferred raster. WAS: every mode the EDID marks
    // preferred (PG27AQDM: 239.972 and 59.950 Hz), and WindowServer took the 60 Hz one (2026-11-10 14:41)
    const XifbKmsMode *kmsBest() const {
        const XifbKmsMode *b = nullptr;
        for (int i = 0; i < fKmN; i++) if (fKmOffer[i] && fKm[i].preferred && (!b || fKm[i].mhz > b->mhz)) b = &fKm[i];
        if (b) for (int i = 0; i < fKmN; i++) if (fKmOffer[i] && fKm[i].w == b->w && fKm[i].h == b->h && fKm[i].mhz > b->mhz) b = &fKm[i];
        if (!b) for (int i = 0; i < fKmN && !b; i++) if (fKmOffer[i]) b = &fKm[i];
        return b;
    }
    // the i915 mode that IS the boot mode (the boot raster at the firmware's rate; not offered under an ID of its own)
    const XifbKmsMode *kmsBootKm() const {
        if (!fKmsUp || fExternal || !fBootId) return nullptr;
        for (int i = 0; i < fKmN; i++) {
            const uint32_t hz = kmsHz16(fKm[i]), d = hz > fBootHz16 ? hz - fBootHz16 : fBootHz16 - hz;
            if (fKm[i].w == fBootW && fKm[i].h == fBootH && !(fKm[i].flags & 0x10) && fKm[i].mhz && d <= (3u << 15)) return &fKm[i];
        }
        return nullptr;
    }
    static void kmsDetailed(const XifbKmsMode *km, IODetailedTimingInformationV2 &d) {
        bzero(&d, sizeof d);
        d.pixelClock = d.minPixelClock = d.maxPixelClock = (UInt64)km->clock_khz * 1000;
        d.horizontalActive = km->w; d.horizontalBlanking = km->htotal - km->w;
        d.horizontalSyncOffset = km->hss - km->w; d.horizontalSyncPulseWidth = km->hse - km->hss;
        d.verticalActive = km->h; d.verticalBlanking = km->vtotal - km->h;
        d.verticalSyncOffset = km->vss - km->h; d.verticalSyncPulseWidth = km->vse - km->vss;
        d.horizontalSyncConfig = (km->flags & 1) ? 1 : 0; d.verticalSyncConfig = (km->flags & 4) ? 1 : 0;   // +hsync, +vsync
        d.signalConfig = kIODigitalSignal;
    }
    void kmsInfo(const XifbKmsMode *m, IODisplayModeInformation *info) {
        info->nominalWidth = m->w; info->nominalHeight = m->h; info->refreshRate = kmsHz16(*m);
        info->flags = (info->flags & ~(UInt32)kDisplayModeDefaultFlag) | kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeValidForHiResFlag;
        if (fExternal && m == kmsBest()) info->flags |= kDisplayModeDefaultFlag;
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
    // IONDRVFramebuffer::connectFlags asks the boot NDRV for the mode's timing (cscGetModeTiming). The NDRV knows the
    // firmware's mode only, so every i915 mode ID failed there, and IOFramebuffer's getInformationForDisplayMode
    // user-client call returns that error to CoreDisplay. With the monitor on the left USB-C (2026-11-10 14:11, i915
    // had committed DP-1 2560x1440 239.972 Hz): "IOFBSetDisplayModeAndDepth: Setting mode 0x10000 ... Failed to
    // obtain mode info", "Setting main display offline", WindowServer on a virtual display, one still frame on the
    // monitor. The flags replace the mode's safety flags (valid, safe, default) in that call.
    IOReturn connectFlags(IOIndex ci, IODisplayModeID mode, IOOptionBits *flags) override {
        if (!kmsMode(mode)) return IONDRVFramebuffer::connectFlags(ci, mode, flags);
        if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | (fExternal && kmsMode(mode) == kmsBest() ? kDisplayModeDefaultFlag : 0);
        return kIOReturnSuccess;
    }
    // the boot NDRV's preferred configuration is the firmware's mode, which is not in the list while a monitor has
    // the picture; and it cannot remember an i915 mode
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override {
        if (!fKmsUp || !fBootId || !fExternal) return IONDRVFramebuffer::getStartupDisplayMode(mode, depth);
        if (mode) *mode = fCurId;
        if (depth) *depth = fBootDepth;
        return kIOReturnSuccess;
    }
    IOReturn setStartupDisplayMode(IODisplayModeID mode, IOIndex depth) override {
        if (kmsMode(mode)) return kIOReturnSuccess;
        return IONDRVFramebuffer::setStartupDisplayMode(mode, depth);
    }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override {
        const XifbKmsMode *km = kmsMode(mode);
        IOReturn r = IONDRVFramebuffer::getPixelInformation(km ? fBootId : mode, depth, aperture, pi);
        if (r == kIOReturnSuccess && km && pi) { pi->activeWidth = km->w; pi->activeHeight = km->h; }
        return r;
    }
    // The boot mode needs its real timing too once i915 owns the display (2026-11-10). IOFramebuffer::setVBLTiming
    // turns its VBL throttle on for a mode with a detailed timing whose min and max pixel clock are equal (every i915
    // mode here): the VBL interrupt is then enabled once in 5 seconds and the refresh times are calculated. For a mode
    // WITHOUT a detailed timing it clears the calculated period but leaves the throttle on. The boot NDRV has no
    // detailed timing for the boot mode, so after the monitor on the left USB-C had the picture (throttle on) and the
    // panel came back on the boot mode, WindowServer got one VBL per 5 seconds: 6-7 frames/s on the panel with its
    // main thread idle 61 % of the time (sample 14:44), while the same desktop on the monitor was fluid.
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
        const XifbKmsMode *km = kmsMode(mode);
        if (!km && info && mode == fBootId) {
            if (const XifbKmsMode *bk = kmsBootKm()) {
                IOReturn r = IONDRVFramebuffer::getTimingInfoForDisplayMode(mode, info);   // the NDRV's timing ID stays
                if (r != kIOReturnSuccess) return r;
                info->flags |= kIODetailedTimingValid;
                kmsDetailed(bk, info->detailedInfo.v2);
                return kIOReturnSuccess;
            }
        }
        if (!km || !info) return IONDRVFramebuffer::getTimingInfoForDisplayMode(km ? fBootId : mode, info);
        bzero(info, sizeof *info);
        info->appleTimingID = kIOTimingIDInvalid;
        info->flags = kIODetailedTimingValid;
        kmsDetailed(km, info->detailedInfo.v2);
        return kIOReturnSuccess;
    }
public:
