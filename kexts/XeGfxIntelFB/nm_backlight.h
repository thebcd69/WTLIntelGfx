/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
 *
 * GU605 branch of XeGfx: adapted from NullMoth 2.7.1 kexts/NVRMFB/fb/nvrm-fb.cpp (NVRMBacklightHandler) and
 * kexts/NVRMFB/nvrm_off.h (backlight mapping and NVRAM record), for the Intel panel framebuffer (XeGfxIntelFB).
 * Changes: the level goes to xifbBacklightSet() (i915's intel_backlight through XeGfx when i915 owns the display,
 * else the panel's DPCD backlight over AUX A) instead of NVRM; the NVRAM variable is xegfx-backlight, so NullMoth's
 * own nullmoth-backlight (Ultimate mode, NVIDIA panel) is never touched; plain thread calls only.
 * This file is under the PolyForm Noncommercial License 1.0.0 (licenses/PolyForm-Noncommercial-1.0.0.md), not BSD-2.
 *
 * Laptop panel brightness. macOS gives the brightness keys and the slider to its built-in display, an
 * AppleBacklightDisplay, which IOGraphics creates for a connection that reports kIOConnectionBuiltIn and an LCD from
 * getAppleSense(), provided a service named "backlight" existed when the framebuffer was opened. IODisplay finds this
 * handler below the framebuffer and offers "brightness" and "linear-brightness" through it, at a logical 0..65535
 * precision; the hardware gets 5 + round(level * 95 / 65535) percent (the 5 % floor protects panels that go fully dark
 * at zero). Requests run from a thread call, because IODisplay calls doIntegerSet() with the framebuffer lock held. A
 * user target is saved two seconds after the last user request and restored at the next start; temporary power
 * requests (dim, display sleep) never replace it.
 */
#pragma once

#define XIFB_BACKLIGHT_VAR "xegfx-backlight"
enum { kXifbBacklightMax = 0xFFFF, kXifbMinBacklight = 5 };
static const SInt32 kXifbAppleBacklightDefaultMax = 255;   // AppleBacklightDisplay's span without a power-state range
static const UInt32 kXifbBacklightSaveDelaySeconds = 2;

static inline UInt32 xifb_backlight_from_percent(UInt32 percent)
{
    if (percent <= kXifbMinBacklight) return 0;
    if (percent > 100) percent = 100;
    return ((percent - kXifbMinBacklight) * kXifbBacklightMax + (100 - kXifbMinBacklight) / 2) / (100 - kXifbMinBacklight);
}
static inline UInt32 xifb_backlight_to_percent(UInt32 level)
{
    return kXifbMinBacklight + (level * (100 - kXifbMinBacklight) + kXifbBacklightMax / 2) / kXifbBacklightMax;
}
// record: version 2, then the little-endian 16-bit logical target
static inline bool xifb_backlight_read(IORegistryEntry *options, UInt32 *level)
{
    if (!options || !level) return false;
    OSObject *o = options->copyProperty(XIFB_BACKLIGHT_VAR);
    OSData *d = OSDynamicCast(OSData, o);
    bool valid = false;
    if (d && d->getLength() == 3) {
        const unsigned char *b = (const unsigned char *)d->getBytesNoCopy();
        if (b[0] == 2) { *level = (UInt32)b[1] | ((UInt32)b[2] << 8); valid = true; }
    }
    if (o) o->release();
    return valid;
}
static inline bool xifb_backlight_write(IORegistryEntry *options, UInt32 level)
{
    if (!options || level > kXifbBacklightMax) return false;
    UInt32 cur;
    if (xifb_backlight_read(options, &cur) && cur == level) return true;
    unsigned char bytes[] = { 2, (unsigned char)level, (unsigned char)(level >> 8) };
    OSData *d = OSData::withBytes(bytes, sizeof bytes);
    if (!d) return false;
    bool ok = options->setProperty(XIFB_BACKLIGHT_VAR, d);
    d->release();
    return ok;
}

class XeGfxIntelBacklight : public IODisplayParameterHandler {
    OSDeclareDefaultStructors(XeGfxIntelBacklight)
    XifbBacklight *fBl = nullptr;
    thread_call_t fApply = nullptr, fPersist = nullptr;
    volatile SInt32 fPending = -1;
    SInt32 fLevel = kXifbBacklightMax;
    volatile SInt32 fUserLevel = kXifbBacklightMax;
    volatile SInt32 fPersistPending = -1;
    volatile UInt64 fPersistDeadline = 0;
    SInt32 fRestoreLevel = -1;
    OSDictionary *fParameters = nullptr;
    const OSSymbol *fBrightnessProbe = nullptr, *fLinearBrightnessProbe = nullptr;

    static void applyFire(thread_call_param_t p, thread_call_param_t) {
        XeGfxIntelBacklight *me = (XeGfxIntelBacklight *)p;
        SInt32 v;
        do { v = me->fPending; } while (!OSCompareAndSwap((UInt32)v, (UInt32)-1, (volatile UInt32 *)&me->fPending));
        if (v < 0) return;
        const UInt32 percent = xifb_backlight_to_percent((UInt32)v);
        if (xifbBacklightSet(me->fBl, percent)) {
            me->fLevel = v;
            XIFBLOG("gu605 backlight: applied logical %d/65535 = %u%%, user target %d", (int)v, percent, (int)OSAddAtomic(0, &me->fUserLevel));
            const uint64_t deadline = __atomic_load_n(&me->fPersistDeadline, __ATOMIC_SEQ_CST);
            if (deadline && OSAddAtomic(0, &me->fUserLevel) == v) {   // only an applied user target is saved
                __atomic_store_n(&me->fPersistPending, v, __ATOMIC_SEQ_CST);
                thread_call_enter_delayed(me->fPersist, deadline);
            }
        } else XIFBLOG("gu605 backlight: setting %u%% did not reach the panel", percent);
    }
    static void persistFire(thread_call_param_t p, thread_call_param_t) {
        XeGfxIntelBacklight *me = (XeGfxIntelBacklight *)p;
        const SInt32 target = OSAddAtomic(0, &me->fPersistPending);
        if (target < 0 || OSAddAtomic(0, &me->fUserLevel) != target) return;
        uint64_t deadline = __atomic_load_n(&me->fPersistDeadline, __ATOMIC_SEQ_CST);
        if (mach_absolute_time() < deadline) { thread_call_enter_delayed(me->fPersist, deadline); return; }
        IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
        const bool saved = options && xifb_backlight_write(options, (UInt32)target);
        OSSafeReleaseNULL(options);
        if (saved) OSCompareAndSwap((UInt32)target, (UInt32)-1, (volatile UInt32 *)&me->fPersistPending);
        else {
            XIFBLOG("gu605 backlight: saving target %d failed; retrying", (int)target);
            clock_interval_to_deadline(kXifbBacklightSaveDelaySeconds, kSecondScale, &deadline);
            thread_call_enter_delayed(me->fPersist, deadline);
        }
    }
public:
    static XeGfxIntelBacklight *with(XifbBacklight *bl) {
        XeGfxIntelBacklight *h = OSTypeAlloc(XeGfxIntelBacklight);
        if (!h || !h->init()) { OSSafeReleaseNULL(h); return nullptr; }
        h->fBl = bl;
        h->fBrightnessProbe = OSSymbol::withCStringNoCopy("brightness-probe");
        h->fLinearBrightnessProbe = OSSymbol::withCStringNoCopy("linear-brightness-probe");
        h->fApply = thread_call_allocate(applyFire, h);
        h->fPersist = thread_call_allocate(persistFire, h);
        h->fLevel = (SInt32)xifb_backlight_from_percent(bl->kind != XIFB_BL_NONE ? (UInt32)bl->startPercent : 100);
        UInt32 saved;
        IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (options && xifb_backlight_read(options, &saved)) { h->fUserLevel = (SInt32)saved; h->fRestoreLevel = (SInt32)saved; }
        else h->fUserLevel = h->fLevel;
        OSSafeReleaseNULL(options);
        if (!h->fApply || !h->fPersist || !h->fBrightnessProbe || !h->fLinearBrightnessProbe) { h->release(); return nullptr; }
        return h;
    }
    // IONDRVFramebuffer reports no display parameters, so the display may have none yet; IODisplay::setProperties()
    // only accepts parameters listed there
    bool setDisplay(IODisplay *display) override {
        if (!display) return false;
        OSObject *old = display->copyProperty(gIODisplayParametersKey);
        OSDictionary *cur = OSDynamicCast(OSDictionary, old);
        OSDictionary *params = cur ? OSDictionary::withDictionary(cur) : OSDictionary::withCapacity(4);
        OSSafeReleaseNULL(old);
        if (!params) return false;
        const SInt32 initial = OSAddAtomic(0, &fUserLevel);
        IODisplay::addParameter(params, gIODisplayBrightnessKey, 0, kXifbBacklightMax);
        IODisplay::setParameter(params, gIODisplayBrightnessKey, initial);
        IODisplay::addParameter(params, gIODisplayLinearBrightnessKey, 0, kXifbBacklightMax);
        IODisplay::setParameter(params, gIODisplayLinearBrightnessKey, initial);
        // CoreDisplay converts between user and linear brightness by writing one probe and reading the other
        IODisplay::addParameter(params, fBrightnessProbe, 0, kXifbBacklightMax);
        IODisplay::setParameter(params, fBrightnessProbe, initial);
        IODisplay::addParameter(params, fLinearBrightnessProbe, 0, kXifbBacklightMax);
        IODisplay::setParameter(params, fLinearBrightnessProbe, initial);
        display->setProperty(gIODisplayParametersKey, params);
        OSSafeReleaseNULL(fParameters);
        fParameters = params;
        if (fRestoreLevel >= 0) { fPending = fRestoreLevel; fRestoreLevel = -1; thread_call_enter(fApply); }
        XIFBLOG("gu605 backlight: brightness parameters on %s, logical %d/65535", display->getName(), (int)initial);
        return true;
    }
    // user changes carry the parameter's own { min, max, value }; AppleBacklightDisplay's power states (dim, display
    // sleep, lid) pass the whole IODisplayParameters
    bool doIntegerSet(OSDictionary *params, const OSSymbol *name, UInt32 value) override {
        const bool probe = name == fBrightnessProbe || name == fLinearBrightnessProbe;
        if (!probe && name != gIODisplayBrightnessKey && name != gIODisplayLinearBrightnessKey) return false;
        SInt32 level = value > kXifbBacklightMax ? kXifbBacklightMax : (SInt32)value;
        if (probe)   // no transfer function: the probe pair mirrors the request
            return fParameters && IODisplay::setParameter(fParameters, name == fBrightnessProbe ? fLinearBrightnessProbe : fBrightnessProbe, level);
        const bool user = params && params->getObject(gIODisplayValueKey);
        if (user) {
            uint64_t deadline;
            clock_interval_to_deadline(kXifbBacklightSaveDelaySeconds, kSecondScale, &deadline);
            __atomic_store_n(&fPersistDeadline, deadline, __ATOMIC_SEQ_CST);
            __atomic_store_n(&fUserLevel, level, __ATOMIC_SEQ_CST);
            if (fParameters) IODisplay::setParameter(fParameters, name == gIODisplayBrightnessKey ? gIODisplayLinearBrightnessKey : gIODisplayBrightnessKey, level);
        } else {
            const SInt32 target = OSAddAtomic(0, &fUserLevel);
            if (value == (UInt32)kXifbAppleBacklightDefaultMax || level > target) level = target;
        }
        fPending = level;
        thread_call_enter(fApply);
        return true;
    }
    bool doDataSet(const OSSymbol *, OSData *) override { return false; }
    bool doUpdate() override { return true; }
    void free() override {
        if (fApply) { thread_call_cancel(fApply); while (!thread_call_free(fApply)) { thread_call_cancel(fApply); IOSleep(1); } fApply = nullptr; }
        if (fPersist) { thread_call_cancel(fPersist); while (!thread_call_free(fPersist)) { thread_call_cancel(fPersist); IOSleep(1); } fPersist = nullptr; }
        OSSafeReleaseNULL(fParameters);
        OSSafeReleaseNULL(fBrightnessProbe);
        OSSafeReleaseNULL(fLinearBrightnessProbe);
        IODisplayParameterHandler::free();
    }
};
OSDefineMetaClassAndStructors(XeGfxIntelBacklight, IODisplayParameterHandler)
