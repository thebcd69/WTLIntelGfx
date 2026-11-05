// XeGfxIntelFramebuffer: Apple's IONDRVFramebuffer under a class name with "Intel" in it. CoreDisplay picks the GPU
// vendor from the IOFramebuffer class name, and IONDRVFramebuffer reads as no vendor, which turns IOPresentment off
// for WindowServer (I-108). Lives in the AuxKC because IONDRVSupport is in the system collection.
// <wtl> fork: nvgfx | author: watchthelight </wtl>
#include <IOKit/ndrvsupport/IONDRVFramebuffer.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <mach/mach_time.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/graphics/IODisplay.h>

#define XIFBLOG(fmt, ...) IOLog("XeGfxIntelFB: " fmt "\n", ##__VA_ARGS__)

// Boot guard as in NMIntelFB: each start counts in NVRAM, 120 s of uptime after start clears it, and two counted starts
// in a row make probe decline so IONDRVFramebuffer drives the panel again. Boot-arg -xeintelfboff declines always.
#define XIFB_GUARD_VAR "xeintelfb-starts"
// About This Mac shows "model" followed by the VRAM size; name the chip generation and graphics architecture
static const char *xifbModelName(uint16_t dev)
{
    switch (dev) {
    case 0x7d67: case 0x7d41: return "Intel Arrow Lake Xe-LPG";
    case 0x7d51: case 0x7dd1: return "Intel Arrow Lake Xe-LPG+";
    case 0x7d40: case 0x7d45: case 0x7d55: case 0x7dd5: return "Intel Meteor Lake Xe-LPG";
    default: return "Intel Xe Graphics";
    }
}
static unsigned xifbGuardStarts(IORegistryEntry *o)
{
    OSObject *v = o->copyProperty(XIFB_GUARD_VAR); OSData *d = OSDynamicCast(OSData, v);
    unsigned n = (d && d->getLength() >= 1) ? *(const unsigned char *)d->getBytesNoCopy() : 0;
    if (v) v->release();
    return n;
}
static bool xifbGuardTripped()
{
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return false;
    bool t = xifbGuardStarts(o) >= 2; o->release(); return t;
}
static void xifbGuardCount()
{
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return;
    unsigned char n = (unsigned char)(xifbGuardStarts(o) + 1);
    if (OSData *d = OSData::withBytes(&n, 1)) { if (o->setProperty(XIFB_GUARD_VAR, d)) XIFBLOG("boot guard: start %u without WindowServer so far (off after 2)", n); d->release(); }
    o->release();
}
static void xifbGuardClear()
{
    static bool done; if (done) return; done = true;
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return;
    // removeProperty did not reach NVRAM (N-283); a zero count does, the same way the count itself is written
    unsigned char z = 0;
    if (OSData *d = OSData::withBytes(&z, 1)) { if (o->setProperty(XIFB_GUARD_VAR, d)) XIFBLOG("boot guard cleared: still up 120 s after start"); d->release(); }
    o->release();
}

// kmutil places a kext in a kernel collection only with a _kmod_info symbol (Xcode generates it; this build does not)
#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(org.watchthelight.XeGfxIntelFB, "0.9.5", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }


// The panel's EDID over the eDP AUX channel (AUX A, display version 14 layout), which the firmware left powered for
// its own modeset. Without it macOS sees "Unknown Display" and none of the panel's overrides (name, HiDPI sizes) apply.
#define XIFB_AUX_CTL  0x64010
#define XIFB_AUX_DATA 0x64014
static volatile uint32_t *gRegs;
static uint32_t xr(uint32_t o) { return gRegs[o / 4]; }
static void xw(uint32_t o, uint32_t v) { gRegs[o / 4] = v; }
static int xifbAux(const uint8_t *tx, int ntx, uint8_t *rx, int nrx)
{
    for (int i = 0; i < 100 && (xr(XIFB_AUX_CTL) & (1u << 31)); i++) IODelay(100);
    if (xr(XIFB_AUX_CTL) & (1u << 31)) return -1;
    for (int i = 0; i < ntx; i += 4) {
        uint32_t v = 0;
        for (int j = 0; j < 4 && i + j < ntx; j++) v |= (uint32_t)tx[i + j] << (24 - 8 * j);
        xw(XIFB_AUX_DATA + i, v);
    }
    // send busy, done, irq, timeout/receive error clears, max timeout, size, sync pulses 18 and 32, power request kept
    uint32_t ctl = (1u << 31) | (1u << 30) | (1u << 29) | (1u << 28) | (3u << 26) | (1u << 25) | ((uint32_t)ntx << 20) |
                   ((18u - 1) << 5) | (32u - 1) | (1u << 19);
    xw(XIFB_AUX_CTL, ctl);
    uint32_t st = 0;
    for (int i = 0; i < 200; i++) { st = xr(XIFB_AUX_CTL); if (!(st & (1u << 31))) break; IODelay(50); }
    xw(XIFB_AUX_CTL, st | (1u << 30) | (1u << 28) | (1u << 25));
    if ((st & (1u << 31)) || (st & ((1u << 28) | (1u << 25)))) return -1;
    int n = (st >> 20) & 0x1f; if (n > nrx) n = nrx;
    for (int i = 0; i < n; i++) rx[i] = (uint8_t)(xr(XIFB_AUX_DATA + (i & ~3)) >> (24 - 8 * (i & 3)));
    return n;
}
// one I2C-over-AUX transfer to the EDID address 0x50, retried while the sink defers
static int xifbI2c(uint8_t req, const uint8_t *data, int nd, uint8_t *rx, int nrx)
{
    uint8_t tx[20] = { (uint8_t)(req << 4), 0, 0x50, (uint8_t)(nd ? nd - 1 : (nrx > 1 ? nrx - 2 : 0)) };
    int ntx = (nd || nrx > 1) ? 4 : 3;
    for (int i = 0; i < nd; i++) tx[ntx++] = data[i];
    for (int t = 0; t < 7; t++) {
        int n = xifbAux(tx, ntx, rx, nrx);
        if (n >= 1 && !(rx[0] & 0xf0)) return n;
        IODelay(500);
    }
    return -1;
}
static bool xifbReadEdid(uint8_t *edid, int *len)
{
    uint8_t off = 0, rx[17];
    if (xifbI2c(0x4, &off, 1, rx, 1) < 1) return false;    // write, middle of transaction: offset 0
    int want = 128;
    for (int pos = 0; pos < want; pos += 16) {
        if (xifbI2c(0x5, nullptr, 0, rx, 17) != 17) return false;
        bcopy(rx + 1, edid + pos, 16);
        if (pos == 112) want = edid[126] ? 256 : 128;       // base block plus at most one extension (no segment pointer)
    }
    xifbI2c(0x1, nullptr, 0, rx, 1);                        // stop
    static const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    if (memcmp(edid, hdr, 8)) return false;
    for (int b = 0; b < want; b += 128) { uint8_t c = 0; for (int i = 0; i < 128; i++) c += edid[b + i]; if (c) return false; }
    *len = want;
    return true;
}
// pipe A's frame counter over about half a second: the refresh rate the firmware's mode runs at, in 16.16
static uint32_t xifbMeasureHz()
{
    const uint32_t fc = 0x70040;
    uint32_t a = xr(fc), b = a;
    for (int i = 0; i < 100 && (b = xr(fc)) == a; i++) IODelay(500);
    if (b == a) return 0;
    uint64_t t0 = mach_absolute_time(); IOSleep(500);
    uint32_t c = xr(fc); for (int i = 0; i < 100 && xr(fc) == c; i++) IODelay(100);
    uint64_t t1 = mach_absolute_time(), ns; absolutetime_to_nanoseconds(t1 - t0, &ns);
    uint32_t frames = xr(fc) - b;
    return ns ? (uint32_t)(((uint64_t)frames << 16) * 1000000000ull / ns) : 0;
}
// the EDID detailed timing nearest the measured rate, so the reported refresh is the panel's exact one
// <bootvram> The panel's preferred mode (the first detailed timing of the EDID) in XeGfxAGDC's NVRMTiming layout: tag,
// pixel clock in Hz, h visible/sync start/sync end/total, v the same, refresh in mHz, h and v sync polarity. Without it
// AGDC answers CoreDisplay's link query (0x921) with upstream's 2560x1600 165 Hz panel. ThinkBook: 3072x1920 60.000 Hz.
#define XIFB_TIMING_TAG 0x4b350001u
static bool xifbEdidTiming(const uint8_t *e, int len, uint32_t t[13])
{
    if (len < 128) return false;
    const uint8_t *d = e + 54;
    uint32_t clk = (d[0] | d[1] << 8) * 10000u; if (!clk) return false;
    uint32_t ha = d[2] | (d[4] & 0xf0) << 4, hb = d[3] | (d[4] & 0x0f) << 8, va = d[5] | (d[7] & 0xf0) << 4, vb = d[6] | (d[7] & 0x0f) << 8;
    uint32_t hso = d[8] | (d[11] & 0xc0) << 2, hsw = d[9] | (d[11] & 0x30) << 4;
    uint32_t vso = (d[10] >> 4) | (d[11] & 0x0c) << 2, vsw = (d[10] & 15) | (d[11] & 3) << 4;
    if (!ha || !va || !hb || !vb || !hsw || !vsw) return false;
    uint64_t tot = (uint64_t)(ha + hb) * (va + vb);
    t[0] = XIFB_TIMING_TAG; t[1] = clk;
    t[2] = ha; t[3] = ha + hso; t[4] = ha + hso + hsw; t[5] = ha + hb;
    t[6] = va; t[7] = va + vso; t[8] = va + vso + vsw; t[9] = va + vb;
    t[10] = (uint32_t)((uint64_t)clk * 1000 / tot); t[11] = (d[17] >> 1) & 1; t[12] = (d[17] >> 2) & 1;
    return true;
}
static uint32_t xifbSnapHz(const uint8_t *e, int len, uint32_t meas)
{
    uint32_t best = meas, bd = ~0u;
    auto dtd = [&](const uint8_t *d) {
        uint32_t clk = (d[0] | d[1] << 8) * 10000u; if (!clk) return;
        uint32_t ht = d[2] | (d[4] & 0xf0) << 4, hb = d[3] | (d[4] & 0x0f) << 8;
        uint32_t vt = d[5] | (d[7] & 0xf0) << 4, vb = d[6] | (d[7] & 0x0f) << 8;
        uint64_t tot = (uint64_t)(ht + hb) * (vt + vb); if (!tot) return;
        uint32_t hz = (uint32_t)(((uint64_t)clk << 16) / tot), d0 = hz > meas ? hz - meas : meas - hz;
        if (d0 < bd) { bd = d0; best = hz; }
    };
    for (int i = 0; i < 4; i++) dtd(e + 54 + 18 * i);
    if (len >= 256 && e[128] == 0x02) for (int o = e[130]; o >= 4 && o + 18 <= 127; o += 18) dtd(e + 128 + o);
    return (meas && bd > meas / 50) ? meas : best;    // more than 2 % off: keep the measurement
}

// <bootvram> The GOP framebuffer on the ThinkBook lies in stolen memory (phys 0x7a800000), outside both GFX0 BARs, so
// IONDRVFramebuffer on GFX0 finds no VRAM and never comes online: WindowServer's "Creating FB 1 of 1" for the Intel GPU
// fails ("Failed to open connection to framebuffer") and it falls back to UEFIGraphicsFB's .Display_boot, which has no
// GPU above it, so IOPresentment stays off (2026-11-02 21:54). The console range as GFX0 device memory lets the boot
// NDRV find it there. Appended last, so every BAR keeps its index. Boot-arg -xeintelfbnovram skips it.
// true when GFX0 holds the console range afterwards
static bool xifbAddConsoleRange(IOService *pci)
{
    int off = 0;
    if (PE_parse_boot_argn("-xeintelfbnovram", &off, sizeof off)) { XIFBLOG("boot vram: off by boot-arg"); return false; }
    PE_Video v; bzero(&v, sizeof v);
    if (IOService::getPlatform()->getConsoleInfo(&v) != kIOReturnSuccess || !v.v_baseAddr || !v.v_rowBytes || !v.v_height) {
        XIFBLOG("boot vram: no console info"); return false;
    }
    uint64_t base = v.v_baseAddr, len = v.v_length ? v.v_length : (uint64_t)v.v_rowBytes * v.v_height;
    OSArray *mem = pci->getDeviceMemory(); unsigned n = mem ? mem->getCount() : 0;
    for (unsigned i = 0; i < n; i++) {
        IOMemoryDescriptor *m = OSDynamicCast(IOMemoryDescriptor, mem->getObject(i)); if (!m) continue;
        uint64_t a = m->getPhysicalAddress();
        if (base >= a && base + len <= a + m->getLength()) { XIFBLOG("boot vram: console %#llx already in device memory %u", base, i); return true; }
    }
    IODeviceMemory *c = IODeviceMemory::withRange((IOPhysicalAddress)base, (IOPhysicalLength)len); if (!c) return false;
    OSArray *nm = mem ? OSArray::withArray(mem, n + 1) : OSArray::withCapacity(1);
    bool added = nm && nm->setObject(c);
    if (added) {
        pci->setDeviceMemory(nm);
        XIFBLOG("boot vram: console %lux%lu rowbytes %lu at phys %#llx (%llu B) added as device memory %u", v.v_width,
                v.v_height, v.v_rowBytes, base, len, n);
    }
    OSSafeReleaseNULL(nm); c->release();
    return added;
}


#include "gu605.h"
// GU605: the brightness slider and keys through AppleBacklightDisplay. IODisplay calls doIntegerSet with the framebuffer
// lock held, so the DPCD write runs from a thread call (as NullMoth's NMIntelBacklight does for the PWM).
class XeGfxIntelBacklight : public IODisplayParameterHandler {
    OSDeclareDefaultStructors(XeGfxIntelBacklight)
    XifbBacklight *fBl = nullptr;
    thread_call_t fApply = nullptr;
    volatile SInt32 fPending = -1;
    SInt32 fLevel = 100;
    static void applyFire(thread_call_param_t p, thread_call_param_t) {
        XeGfxIntelBacklight *me = (XeGfxIntelBacklight *)p;
        SInt32 v;
        do { v = me->fPending; } while (!OSCompareAndSwap((UInt32)v, (UInt32)-1, (volatile UInt32 *)&me->fPending));
        if (v < 0) return;
        if (xifbBacklightSet(me->fBl, (uint32_t)v)) me->fLevel = v;
        else XIFBLOG("gu605 backlight: setting %d%% did not reach the panel", (int)v);
    }
public:
    static XeGfxIntelBacklight *with(XifbBacklight *bl) {
        XeGfxIntelBacklight *h = OSTypeAlloc(XeGfxIntelBacklight);
        if (!h || !h->init()) { OSSafeReleaseNULL(h); return nullptr; }
        h->fBl = bl; h->fLevel = bl->startPercent < 5 ? 5 : bl->startPercent;
        h->fApply = thread_call_allocate(applyFire, h);
        if (!h->fApply) { h->release(); return nullptr; }
        return h;
    }
    bool setDisplay(IODisplay *display) override {
        if (!display) return false;
        OSObject *old = display->copyProperty(gIODisplayParametersKey);
        OSDictionary *cur = OSDynamicCast(OSDictionary, old);
        OSDictionary *params = cur ? OSDictionary::withDictionary(cur) : OSDictionary::withCapacity(2);
        OSSafeReleaseNULL(old);
        if (!params) return false;
        IODisplay::addParameter(params, gIODisplayBrightnessKey, 0, 100);
        IODisplay::setParameter(params, gIODisplayBrightnessKey, fLevel);
        IODisplay::addParameter(params, gIODisplayLinearBrightnessKey, 0, 100);
        IODisplay::setParameter(params, gIODisplayLinearBrightnessKey, fLevel);
        display->setProperty(gIODisplayParametersKey, params);
        params->release();
        XIFBLOG("gu605 backlight: brightness parameters on %s at %d%%", display->getName(), (int)fLevel);
        return true;
    }
    bool doIntegerSet(OSDictionary *, const OSSymbol *name, UInt32 value) override {
        if (name != gIODisplayBrightnessKey && name != gIODisplayLinearBrightnessKey) return false;
        fPending = value > 100 ? 100 : (SInt32)value;
        thread_call_enter(fApply);
        return true;
    }
    bool doDataSet(const OSSymbol *, OSData *) override { return false; }
    bool doUpdate() override { return true; }
    void free() override {
        if (fApply) { thread_call_cancel(fApply); while (!thread_call_free(fApply)) { thread_call_cancel(fApply); IOSleep(1); } fApply = nullptr; }
        IODisplayParameterHandler::free();
    }
};
OSDefineMetaClassAndStructors(XeGfxIntelBacklight, IODisplayParameterHandler)


// Hardware cursor on pipe A's cursor plane. Without one IOFramebuffer draws a software cursor, every move damages a
// rect around it, and WindowServer recomposites blurred window backdrops there (the I-119 square).
// The GOP maps a 16 MB GGTT block at 0xff000000 for its 16,384,000-byte framebuffer; the 96 pages after it lie in
// i915's reserved top of GGTT, so pointing 64 of them at our own pages leaves everything else alone.
#define XIFB_CUR_DIM   256u
#define XIFB_CUR_GGTT  0xfffa0000u
#define XIFB_GSM       0x800000u                     // GGTT PTEs in BAR0
#define XIFB_CUR_CTL   0x70080
#define XIFB_CUR_BASE  0x70084
#define XIFB_CUR_POS   0x70088
#define XIFB_CUR_WM0   0x70140
#define XIFB_CUR_SAGV  0x70158
#define XIFB_CUR_BUF   0x7017c
#define XIFB_P1_BUF    0x7027c
#define XIFB_P1_SURF   0x7019c

class XeGfxIntelFramebuffer : public IONDRVFramebuffer {
    OSDeclareDefaultStructors(XeGfxIntelFramebuffer)
    uint8_t fEdid[256]; int fEdidLen = 0; uint32_t fHz16 = 0;
    bool fGu605 = false; XifbBacklight fBl; XeGfxIntelBacklight *fBacklight = nullptr;   // GU605 (7D55) only
    IOMemoryMap *fMmio = nullptr; volatile uint32_t *fR = nullptr;
    IOBufferMemoryDescriptor *fCurMem = nullptr; IOMemoryMap *fCurMap = nullptr; uint32_t *fCurKva = nullptr;
    uint64_t fCurPte[XIFB_CUR_DIM * XIFB_CUR_DIM * 4 / 4096] = {};
    IOSimpleLock *fCurLock = nullptr; bool fCurReady = false, fCurDdb = false, fCurOn = false;
    uint32_t rd(uint32_t o) { return fR[o / 4]; }
    void wr(uint32_t o, uint32_t v) { fR[o / 4] = v; }
    void waitFrame() { uint32_t a = rd(0x70040); for (int i = 0; i < 50 && rd(0x70040) == a; i++) IODelay(500); }
    // our PTEs, rewritten when anything else changed them; PAT index 2 (uncached) like i915 gives scanout on MTL
    void curPtes() {
        volatile uint64_t *g = (volatile uint64_t *)((volatile uint8_t *)fR + XIFB_GSM) + (XIFB_CUR_GGTT >> 12);
        bool chg = false;
        for (unsigned i = 0; i < sizeof fCurPte / sizeof fCurPte[0]; i++) if (g[i] != fCurPte[i]) { g[i] = fCurPte[i]; chg = true; }
        if (chg) { wr(0x101008, 1); (void)rd(0x101008); }
    }
    bool curSetup() {
        int off = 0;
        if (PE_parse_boot_argn("-xeintelfbnohwc", &off, sizeof off) || !fR) return false;
        // [gu605-mac] WAS: the GOP's plane at GGTT 0xff000000 only. On GU605 Eco boots the GOP put its 16,384,000-byte
        // framebuffer at 0xf8000000 (2026-11-05 11:11, 11:25), so the cursor stayed in software and, once WindowServer
        // ran on the Intel GPU with zero-copy flips, left trails in the menu bar. What matters is that the GOP's block
        // ends at or below our 64 cursor pages at 0xfffa0000 (i915's reserved top of GGTT, above GuC's range).
        const uint32_t gopSurf = rd(XIFB_P1_SURF) & 0xfffff000u, curEnd = XIFB_CUR_GGTT + XIFB_CUR_DIM * XIFB_CUR_DIM * 4;
        const bool gopClear = gopSurf + 16384000u <= XIFB_CUR_GGTT || gopSurf >= curEnd;
        if (gopSurf != 0xff000000u) {
            volatile uint64_t *g = (volatile uint64_t *)((volatile uint8_t *)fR + XIFB_GSM) + (XIFB_CUR_GGTT >> 12);
            XIFBLOG("hw cursor: GOP plane at GGTT %08x (%s our cursor pages); PTE at %08x now %016llx", gopSurf,
                    gopClear ? "clear of" : "OVERLAPS", XIFB_CUR_GGTT, (unsigned long long)g[0]);
        }
        if (rd(XIFB_CUR_CTL) || !gopClear || rd(XIFB_P1_BUF) != 0x07ff0000u) {
            XIFBLOG("hw cursor off: display not as the GOP left it (cur %08x surf %08x buf %08x)", rd(XIFB_CUR_CTL),
                    rd(XIFB_P1_SURF), rd(XIFB_P1_BUF));
            return false;
        }
        const size_t bytes = XIFB_CUR_DIM * XIFB_CUR_DIM * 4;
        fCurMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut, bytes, 0x7ffffff000ull);
        if (!fCurMem || fCurMem->prepare() != kIOReturnSuccess) { XIFBLOG("hw cursor off: no memory"); return false; }
        fCurMap = fCurMem->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
        if (!fCurMap) { XIFBLOG("hw cursor off: no mapping"); return false; }
        fCurKva = (uint32_t *)fCurMap->getVirtualAddress();
        for (unsigned i = 0; i < sizeof fCurPte / sizeof fCurPte[0]; i++) {
            IOByteCount len = 0; addr64_t pa = fCurMem->getPhysicalSegment((IOByteCount)i * 4096, &len, kIOMemoryMapperNone);
            if (!pa || (pa & 0xfff)) { XIFBLOG("hw cursor off: page %u at %llx", i, (unsigned long long)pa); return false; }
            fCurPte[i] = pa | 1 | (1ull << 53);
        }
        bzero(fCurKva, bytes); __asm__ __volatile__("sfence" ::: "memory");
        fCurLock = IOSimpleLockAlloc();
        fCurReady = fCurLock != nullptr;
        XIFBLOG("hw cursor ready: %ux%u ARGB at GGTT %08x", XIFB_CUR_DIM, XIFB_CUR_DIM, XIFB_CUR_GGTT);
        return fCurReady;
    }
    // the GOP gave plane 1 all 2048 DBUF blocks; the cursor takes the last 32 (i915's single-pipe cursor share)
    void curDdb() {
        if (fCurDdb) return;
        wr(XIFB_P1_BUF, 0x07df0000u); wr(XIFB_P1_SURF, rd(XIFB_P1_SURF)); waitFrame(); waitFrame();
        wr(XIFB_CUR_BUF, 0x07ff07e0u);
        wr(XIFB_CUR_WM0, (1u << 31) | (3u << 14) | 8u);
        wr(XIFB_CUR_SAGV, (1u << 31) | (6u << 14) | 16u);
        fCurDdb = true;
    }
    void curRegs(SInt32 x, SInt32 y, bool on) {
        uint32_t pos = (x < 0 ? (1u << 15) | (uint32_t)(-x & 0xfff) : (uint32_t)(x & 0xfff)) |
                       (y < 0 ? (1u << 31) | ((uint32_t)(-y & 0xfff) << 16) : ((uint32_t)(y & 0xfff) << 16));
        wr(XIFB_CUR_POS, pos);
        wr(XIFB_CUR_CTL, on ? (1u << 14) | 0x23u : 0);  // trickle feed off, 256x256 ARGB (0x27 is 64x64)
        wr(XIFB_CUR_BASE, XIFB_CUR_GGTT);                // arms CTL, POS, WM and BUF_CFG
    }
    SInt32 fCurX = 0, fCurY = 0; bool fCurVis = false, fCurImg = false;
public:
    IOReturn getAttribute(IOSelect attr, uintptr_t *value) override {
        if (attr == kIOHardwareCursorAttribute) { if (value) *value = fCurReady; return kIOReturnSuccess; }
        return IONDRVFramebuffer::getAttribute(attr, value);
    }
    IOReturn setCursorImage(void *img) override {
        if (!fCurReady) return IONDRVFramebuffer::setCursorImage(img);
        const size_t bytes = XIFB_CUR_DIM * XIFB_CUR_DIM * 4;
        uint32_t *st = (uint32_t *)IOMalloc(bytes); if (!st) return kIOReturnNoMemory;
        bzero(st, bytes);
        IOHardwareCursorDescriptor d; bzero(&d, sizeof d);
        d.majorVersion = kHardwareCursorDescriptorMajorVersion; d.minorVersion = kHardwareCursorDescriptorMinorVersion;
        d.width = XIFB_CUR_DIM; d.height = XIFB_CUR_DIM; d.bitDepth = 32; d.supportedSpecialEncodings = kTransparentEncodedPixel;
        IOHardwareCursorInfo ci; bzero(&ci, sizeof ci);
        ci.majorVersion = kHardwareCursorInfoMajorVersion; ci.minorVersion = kHardwareCursorInfoMinorVersion;
        ci.hardwareCursorData = (UInt8 *)st;
        bool ok = convertCursorImage(img, &d, &ci) && ci.cursorWidth && ci.cursorHeight &&
                  ci.cursorWidth <= XIFB_CUR_DIM && ci.cursorHeight <= XIFB_CUR_DIM;
        if (ok) {
            // premultiplied ARGB, rows of 256 pixels
            for (unsigned y = 0; y < XIFB_CUR_DIM; y++)
                for (unsigned x = 0; x < XIFB_CUR_DIM; x++) {
                    uint32_t p = (y < ci.cursorHeight && x < ci.cursorWidth) ? st[y * ci.cursorWidth + x] : 0, a = p >> 24;
                    uint32_t r = (((p >> 16) & 255) * a + 127) / 255, g = (((p >> 8) & 255) * a + 127) / 255, b = ((p & 255) * a + 127) / 255;
                    fCurKva[y * XIFB_CUR_DIM + x] = (a << 24) | (r << 16) | (g << 8) | b;
                }
            __asm__ __volatile__("sfence" ::: "memory");
        }
        IOFree(st, bytes);
        static unsigned n; if (++n <= 4) XIFBLOG("hw cursor image %u: %s %ux%u", n, ok ? "ok" : "declined", (unsigned)ci.cursorWidth, (unsigned)ci.cursorHeight);
        if (!ok) return kIOReturnUnsupported;
        curDdb();   // once: waits two frames, so here (thread context) and not in setCursorState
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(fCurLock);
        curPtes(); fCurImg = true; if (fCurDdb) curRegs(fCurX, fCurY, fCurVis);
        IOSimpleLockUnlockEnableInterrupt(fCurLock, is);
        return kIOReturnSuccess;
    }
    IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) override {
        if (!fCurReady) return IONDRVFramebuffer::setCursorState(x, y, visible);
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(fCurLock);
        fCurX = x; fCurY = y; fCurVis = visible;
        if (fCurDdb) curRegs(x, y, visible && fCurImg);
        IOSimpleLockUnlockEnableInterrupt(fCurLock, is);
        static unsigned n; if (++n <= 3) XIFBLOG("hw cursor state %u: %d,%d %s", n, (int)x, (int)y, visible ? "shown" : "hidden");
        return kIOReturnSuccess;
    }
    IOService *probe(IOService *provider, SInt32 *score) override {
        int off = 0;
        if (PE_parse_boot_argn("-xeintelfboff", &off, sizeof off)) { XIFBLOG("off by boot-arg"); return nullptr; }
        if (xifbGuardTripped()) { XIFBLOG("boot guard tripped (two starts without WindowServer): declining"); return nullptr; }
        // GU605: only when the iGPU drives the panel (Standard/Eco); in Ultimate the panel and console are NVIDIA's
        if (xifbIsGu605(provider) && !xifbGu605OwnsPanel(provider)) return nullptr;
        return IONDRVFramebuffer::probe(provider, score);
    }
    // [gu605-mac] XeGfxIntelFB-Start in ioreg: how far start() got, so a refused start is visible without the kernel
    // log. IONDRVFramebuffer::start() only drives the device when IOKit matched it BY NAME (IONameMatched set, from
    // the personality's IONameMatch "display"); otherwise it spawns IONDRVDevice nubs for display children in the
    // device tree and returns false on purpose. The GU605 personality had no IONameMatch, so every start "FAILED" and
    // Apple's IONDRVFramebuffer (IONameMatch display, score 20000) took IGPU as .Display_boot (2026-11-05 10:53 Eco).
    void startStep(const char *step) { setProperty("XeGfxIntelFB-Start", step); }
    bool start(IOService *provider) override {
        fGu605 = xifbIsGu605(provider);
        startStep("begin");
        if (!getProperty(gIONameMatchedKey))
            XIFBLOG("start: not matched by name (no IONameMatched): IONDRVFramebuffer::start will only make "
                    "IONDRVDevice nubs and return false - the personality needs IONameMatch \"display\"");
        xifbGuardCount();
        // System Profiler names the GPU from the PCI device's "model"; without it About This Mac shows a bare "Display"
        if (!provider->getProperty("model")) {
            IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
            const char *model = xifbModelName(pci ? pci->configRead16(kIOPCIConfigDeviceID) : 0);
            if (OSData *m = OSData::withBytes(model, (unsigned)strlen(model) + 1)) { provider->setProperty("model", m); m->release(); }
        }
        // System Profiler shows VRAM,totalMB of an Intel PCI device as "VRAM (Dynamic, Max)", else the 15 MB GOP
        // framebuffer. Same size ANV gives its heap: 3/4 of RAM above 4 GB, half below. xeintelfbvram=<MB> overrides.
        if (!provider->getProperty("VRAM,totalMB")) {
            uint32_t mb = 0;
            if (!PE_parse_boot_argn("xeintelfbvram", &mb, sizeof mb)) {
                uint64_t ram = 0; size_t len = sizeof ram;
                if (!sysctlbyname("hw.memsize", &ram, &len, nullptr, 0) && ram)
                    mb = (uint32_t)((ram <= (4ull << 30) ? ram / 2 : ram / 4 * 3) >> 20);
            }
            if (mb) { provider->setProperty("VRAM,totalMB", (unsigned long long)mb, 32); XIFBLOG("VRAM,totalMB %u", mb); }
        }
        // the guard is for boots that hang: still running 120 s after start clears it. IOGraphics keeps the
        // WindowServer-active attribute and WindowServer's connection skips newUserClient here (N-282 to N-284)
        static thread_call_t tc;
        if (!tc) tc = thread_call_allocate([](thread_call_param_t, thread_call_param_t) { xifbGuardClear(); }, nullptr);
        if (tc) { uint64_t dl; clock_interval_to_deadline(120, kSecondScale, &dl); thread_call_enter_delayed(tc, dl); }
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            int off = 0;
            if (!PE_parse_boot_argn("-xeintelfbnoedid", &off, sizeof off))
                if (IOMemoryMap *m = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)) {
                    gRegs = (volatile uint32_t *)m->getVirtualAddress();
                    if (xifbReadEdid(fEdid, &fEdidLen))
                        XIFBLOG("EDID %d B, maker %02x%02x product %02x%02x, %ux%u mm", fEdidLen, fEdid[8], fEdid[9], fEdid[11],
                                fEdid[10], fEdid[21] * 10, fEdid[22] * 10);
                    else { fEdidLen = 0; XIFBLOG("EDID: AUX A read failed, the display stays unnamed"); }
                    uint32_t meas = xifbMeasureHz();
                    fHz16 = fEdidLen ? xifbSnapHz(fEdid, fEdidLen, meas) : meas;
                    XIFBLOG("refresh measured %u.%02u Hz, reported %u.%02u Hz", meas >> 16, ((meas & 0xffff) * 100) >> 16,
                            fHz16 >> 16, ((fHz16 & 0xffff) * 100) >> 16);
                    gRegs = nullptr; fMmio = m; fR = (volatile uint32_t *)m->getVirtualAddress();
                    // GU605: AUX A stays reachable for the DPCD backlight (serialised by gXifbAuxLock)
                    if (fGu605) {
                        if (!gXifbAuxLock) gXifbAuxLock = IOLockAlloc();
                        gRegs = fR; xifbBacklightProbe(&fBl, fEdid, fEdidLen);
                        xifbGu605LinkReport(this); xifbGu605Modes(this, fEdid, fEdidLen);   // the 240 Hz question
                        setProperty("GU605MeasuredHz16", meas, 32);
                    }
                }
        }
        startStep("edid/link done");
        if (!curSetup()) fCurReady = false;
        // samwu07's bootvram (console range, IOFBDependentID/Index, NVRMTiming): 7D55 only on this branch
        bool vram = fGu605 && OSDynamicCast(IOPCIDevice, provider) && xifbAddConsoleRange(provider);
        // IOFramebuffer::open looks for a child named "backlight" (AppleBacklightDisplay), so it is attached first
        if (fGu605 && fBl.kind != XIFB_BL_NONE && (fBacklight = XeGfxIntelBacklight::with(&fBl))) {
            fBacklight->setName("backlight");
            if (fBacklight->attach(this)) fBacklight->registerService(kIOServiceSynchronous);
            else { OSSafeReleaseNULL(fBacklight); XIFBLOG("gu605 backlight: handler attach failed"); }
        }
        startStep(getProperty(gIONameMatchedKey) ? "IONDRVFramebuffer::start (name matched)"
                                                  : "IONDRVFramebuffer::start (NOT name matched: will return false)");
        bool ok = IONDRVFramebuffer::start(provider);
        startStep(ok ? "started" : (getProperty(gIONameMatchedKey) ? "FAILED in IONDRVFramebuffer::start (name matched)"
                                                                   : "FAILED: not matched by name (IONameMatch display missing)"));
        if (!ok && fBacklight) { fBacklight->detach(this); OSSafeReleaseNULL(fBacklight); }
        // the name is still the class name here; it becomes ".Display_boot" when the boot NDRV opens it
        XIFBLOG("IONDRVFramebuffer start %s, name %s", ok ? "ok" : "FAILED", getName());
        // a GPU's own framebuffer driver names its GPU and its slot; IONDRVFramebuffer does not, and XeGfxAGDC left
        // GFX0's framebuffer out without it (23:02 "has no IOFBDependentIndex number"). Only with the console here:
        // a framebuffer without it never comes online and must not be reported.
        if (ok && vram && !getProperty("IOFBDependentIndex")) {
            setProperty("IOFBDependentID", provider->getRegistryEntryID(), 64);
            setProperty("IOFBDependentIndex", 0ULL, 32);
            XIFBLOG("IOFBDependentID %#llx, IOFBDependentIndex 0", provider->getRegistryEntryID());
        }
        uint32_t t[13];
        if (ok && vram && fEdidLen && !getProperty("NVRMTiming") && xifbEdidTiming(fEdid, fEdidLen, t))
            if (OSData *d = OSData::withBytes(t, sizeof t)) {
                setProperty("NVRMTiming", d); d->release();
                XIFBLOG("NVRMTiming %ux%u, pixel clock %u Hz, totals %ux%u, %u.%03u Hz", t[2], t[6], t[1], t[5], t[9], t[10] / 1000, t[10] % 1000);
            }
        return ok;
    }
    IODeviceMemory *findVRAM() override {
        IODeviceMemory *m = IONDRVFramebuffer::findVRAM();
        static unsigned n; if (++n <= 2) XIFBLOG("findVRAM %u: %s", n, m ? "found" : "none");
        return m;
    }
    // <vbl> The panel's refresh as IOFramebuffer's VBL interrupt. The boot NDRV has none, so IOFramebuffer ran its 30 Hz
    // stand-in (IOFBWaitCursorPeriod 33333333) and WindowServer composited on that beat: 2026-11-03 16:3x, fps-gaps while a
    // window was dragged: 324 of 398 frame gaps 30-34 ms, 7 at 15-19 ms -- the desktop capped at 30 frames/s with the GPU,
    // the present path and WindowServer's waits all ruled out. Here a thread call follows pipe A's frame
    // counter (0x70040, the one xifbMeasureHz reads) and calls IOFramebuffer's handler when it advances; a counter that
    // stops (pipe off) is replaced by the measured period, so a registered handler is never left without a beat.
    // Boot-arg -xeintelfbnovbl keeps the boot NDRV's answer (no VBL).
    IOFBInterruptProc fVblProc = nullptr; OSObject *fVblTarget = nullptr; void *fVblRef = nullptr;
    volatile bool fVblEnabled = false, fVblStop = false; thread_call_t fVblCall = nullptr;
    uint32_t fVblCount = 0; uint64_t fVblLastAbs = 0, fVblPeriodAbs = 0, fVblPollAbs = 0, fVblEarlyAbs = 0;
    uint64_t fVblFired = 0, fVblSynthetic = 0; int fVblToken = 0;
    static void vblThunk(thread_call_param_t me, thread_call_param_t) { ((XeGfxIntelFramebuffer *)me)->vblTick(); }
    void vblTick() {
        if (fVblStop) return;
        const uint64_t now = mach_absolute_time();
        const uint32_t c = rd(0x70040);
        bool fire = false;
        if (c != fVblCount) { fVblCount = c; fVblLastAbs = now; fire = true; }
        else if (now - fVblLastAbs > 2 * fVblPeriodAbs) { fVblLastAbs = now; fire = true; fVblSynthetic++; }
        if (fire && fVblEnabled && fVblProc) {
            fVblProc(fVblTarget, fVblRef);
            if (++fVblFired == 600) XIFBLOG("VBL: 600 refreshes delivered (%llu from the period, the frame counter stopped)", fVblSynthetic);
        }
        // shortly before the next refresh, then every 250 us until the counter moves
        const uint64_t next = fVblLastAbs + fVblPeriodAbs - fVblEarlyAbs;
        thread_call_enter_delayed(fVblCall, next > now + fVblPollAbs ? next : now + fVblPollAbs);
    }
public:
    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **iref) override {
        int off = 0;
        if (!fGu605 || type != kIOFBVBLInterruptType || !fR || !fHz16 || PE_parse_boot_argn("-xeintelfbnovbl", &off, sizeof off))
            return IONDRVFramebuffer::registerForInterruptType(type, proc, target, ref, iref);
        if (fVblProc) return kIOReturnBusy;
        // thread_call_allocate_with_priority and thread_call_cancel_wait are com.apple.kpi.private: a third-party kext
        // does not link against them (2026-11-03 16:41, kmutil "could not find a kext which exports this symbol")
        if (!fVblCall) fVblCall = thread_call_allocate(vblThunk, this);
        if (!fVblCall) return IONDRVFramebuffer::registerForInterruptType(type, proc, target, ref, iref);
        const uint64_t periodNs = (65536ull * 1000000000ull) / fHz16;
        nanoseconds_to_absolutetime(periodNs, &fVblPeriodAbs);
        nanoseconds_to_absolutetime(250000, &fVblPollAbs);
        nanoseconds_to_absolutetime(1000000, &fVblEarlyAbs);
        fVblProc = proc; fVblTarget = target; fVblRef = ref; fVblEnabled = true; fVblStop = false;
        fVblCount = rd(0x70040); fVblLastAbs = mach_absolute_time();
        thread_call_enter_delayed(fVblCall, fVblLastAbs + fVblPollAbs);
        if (iref) *iref = &fVblToken;
        XIFBLOG("VBL: from pipe A's frame counter, %u.%02u Hz (period %llu us)", fHz16 >> 16, ((fHz16 & 0xffff) * 100) >> 16, periodNs / 1000);
        return kIOReturnSuccess;
    }
    IOReturn unregisterInterrupt(void *iref) override {
        if (iref != &fVblToken) return IONDRVFramebuffer::unregisterInterrupt(iref);
        fVblStop = true; fVblEnabled = false;   // a tick already running sees fVblStop first and does not re-arm
        if (fVblCall) thread_call_cancel(fVblCall);
        fVblProc = nullptr; fVblTarget = nullptr; fVblRef = nullptr;
        XIFBLOG("VBL: handler removed after %llu refreshes", fVblFired);
        return kIOReturnSuccess;
    }
    IOReturn setInterruptState(void *iref, UInt32 state) override {
        if (iref != &fVblToken) return IONDRVFramebuffer::setInterruptState(iref, state);
        fVblEnabled = state != kDisabledInterruptState;
        return kIOReturnSuccess;
    }
    bool hasDDCConnect(IOIndex ci) override { return fEdidLen ? true : IONDRVFramebuffer::hasDDCConnect(ci); }
    IOReturn getDDCBlock(IOIndex ci, UInt32 block, IOSelect type, IOOptionBits opt, UInt8 *data, IOByteCount *len) override {
        if (!fEdidLen) return IONDRVFramebuffer::getDDCBlock(ci, block, type, opt, data, len);
        if (!data || !len || block < 1 || (int)(block * 128) > fEdidLen) return kIOReturnUnsupported;
        IOByteCount n = *len < 128 ? *len : 128; bcopy(fEdid + (block - 1) * 128, data, n); *len = n;
        return kIOReturnSuccess;
    }
    IOReturn getAppleSense(IOIndex ci, UInt32 *senseType, UInt32 *primary, UInt32 *extended, UInt32 *displayType) override {
        if (!fBacklight || ci != 0) return IONDRVFramebuffer::getAppleSense(ci, senseType, primary, extended, displayType);
        if (senseType) *senseType = 0;
        if (primary) *primary = 0;
        if (extended) *extended = 0;
        if (displayType) *displayType = kGenericLCD;   // AppleBacklightDisplay::probe takes an LCD connection only
        return kIOReturnSuccess;
    }
    IOReturn getAttributeForConnection(IOIndex ci, IOSelect attr, uintptr_t *value) override {
        if (fGu605 && ci == 0 && attr == kConnectionFlags && value) { *value = kIOConnectionBuiltIn; return kIOReturnSuccess; }
        if (fEdidLen && attr == kConnectionSupportsHLDDCSense) return kIOReturnSuccess;
        return IONDRVFramebuffer::getAttributeForConnection(ci, attr, value);
    }
    // the panel's size and refresh, and ValidForHiRes so the 2x sizes reach Displays settings (I-059 on NVRMFB)
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override {
        IOReturn r = IONDRVFramebuffer::getInformationForDisplayMode(mode, info);
        if (r != kIOReturnSuccess || !info) return r;
        if (fHz16) info->refreshRate = fHz16;
        if (fEdidLen && fEdid[21] && fEdid[22]) { info->imageWidth = fEdid[21] * 10; info->imageHeight = fEdid[22] * 10; }
        info->flags |= kDisplayModeValidForHiResFlag;
        return r;
    }
};

OSDefineMetaClassAndStructors(XeGfxIntelFramebuffer, IONDRVFramebuffer)
