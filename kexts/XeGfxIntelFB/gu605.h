// gu605.h: ASUS ROG Zephyrus G16 GU605MY (Meteor Lake-H, iGPU 8086:7D55, Samsung ATNA60DL01 OLED eDP) additions to
// XeGfxIntelFB. Everything here is reached only when the provider is device 7D55; other GPUs keep the 0.9.4 behaviour.
//
//  * xifbGu605OwnsPanel(): the iGPU is the panel's scan-out (G-Helper Standard or Eco mode). In Ultimate the MUX puts
//    the panel on the NVIDIA GPU and the boot console lives in NVIDIA's BAR1; then the iGPU has no lit pipe and this
//    framebuffer must not start, or the console range would be added to GFX0 and a second framebuffer would sit on
//    NVIDIA's memory next to NullMoth's. Read-only: BAR0 is mapped only when the firmware left memory decoding on.
//  * Panel brightness over DPCD (the panel is an OLED: no PWM). Intel's HDR backlight interface (DPCD 0x340-0x35x,
//    level in nits) first, then VESA eDP (0x701/0x702 caps, 0x721 mode, 0x722/0x723 level), the order Linux i915's
//    intel_dp_aux_backlight.c uses for a VBT that says "AUX". Nothing is written until the user moves the slider or
//    presses a brightness key; the floor is 5 %. Boot-arg -xeintelfbnobl turns it off, xeintelfbnits=<max> sets the
//    HDR interface's full-scale level (default 512 nits, i915's default without a VBT value).
//
// Register and DPCD numbers: Linux i915 intel_dp_aux_regs.h, intel_dp_aux_backlight.c, drm_dp.h (MIT).
// <wtl> fork: thebcd69/WTLIntelGfx gu605-mtl | BSD-2-Clause, as XeGfxIntelFB </wtl>
#pragma once

#define XIFB_GU605_DEV      0x7d55
#define XIFB_TRANSCONF_A    0x70008
#define XIFB_PLANE_CTL_1A   0x70180
#define XIFB_PLANE_SIZE_1A  0x70190

static bool xifbIsGu605(IOService *provider)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    return pci && pci->configRead16(kIOPCIConfigVendorID) == 0x8086 && pci->configRead16(kIOPCIConfigDeviceID) == XIFB_GU605_DEV;
}

// true when pipe A's transcoder and plane 1 are on and the plane is the boot console's size
static bool xifbGu605OwnsPanel(IOService *provider)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;
    if (!(pci->configRead16(kIOPCIConfigCommand) & kIOPCICommandMemorySpace)) {
        XIFBLOG("gu605: iGPU memory decoding is off (MUX on the NVIDIA GPU?): not the panel's GPU");
        return false;
    }
    IOMemoryMap *m = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    if (!m || m->getLength() < 0x80000) { OSSafeReleaseNULL(m); XIFBLOG("gu605: BAR0 not mappable"); return false; }
    volatile uint32_t *r = (volatile uint32_t *)m->getVirtualAddress();
    const uint32_t conf = r[XIFB_TRANSCONF_A / 4], ctl = r[XIFB_PLANE_CTL_1A / 4], size = r[XIFB_PLANE_SIZE_1A / 4];
    m->release();
    const uint32_t w = (size & 0xffff) + 1, h = ((size >> 16) & 0xffff) + 1;
    PE_Video v; bzero(&v, sizeof v);
    const bool cons = IOService::getPlatform() && IOService::getPlatform()->getConsoleInfo(&v) == kIOReturnSuccess && v.v_baseAddr;
    const bool lit = conf != ~0u && (conf & (1u << 31)) && (ctl & (1u << 31));
    const bool same = cons && v.v_width == w && v.v_height == h;
    XIFBLOG("gu605: pipe A transconf %08x plane %08x %ux%u, console %lux%lu at %#llx -> %s", conf, ctl, w, h,
            cons ? v.v_width : 0, cons ? v.v_height : 0, cons ? (unsigned long long)v.v_baseAddr : 0ull,
            lit && same ? "the iGPU scans out the panel (Standard/Eco)" : "the panel is not on the iGPU (Ultimate): staying off");
    return lit && same;
}

// ---- DPCD over native AUX (AUX A), with the request/reply framing of xifbAux() ----
static IOLock *gXifbAuxLock;
static int xifbDpcdRead(uint32_t addr, uint8_t *buf, int n)
{
    if (!gRegs || n < 1 || n > 16) return -1;
    uint8_t tx[4] = { (uint8_t)(0x90 | ((addr >> 16) & 0xf)), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(n - 1) }, rx[20];
    for (int t = 0; t < 5; t++) {
        int got = xifbAux(tx, 4, rx, n + 1);
        if (got >= 1 && (rx[0] & 0x30) == 0 && got == n + 1) { bcopy(rx + 1, buf, n); return n; }
        IODelay(400);   // DEFER or a short reply: retry as drm_dp_dpcd_access does
    }
    return -1;
}
static int xifbDpcdWrite(uint32_t addr, const uint8_t *buf, int n)
{
    if (!gRegs || n < 1 || n > 16) return -1;
    uint8_t tx[20] = { (uint8_t)(0x80 | ((addr >> 16) & 0xf)), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(n - 1) }, rx[2];
    bcopy(buf, tx + 4, n);
    for (int t = 0; t < 5; t++) {
        int got = xifbAux(tx, 4 + n, rx, 1);
        if (got >= 1 && (rx[0] & 0x30) == 0) return n;
        IODelay(400);
    }
    return -1;
}

enum { XIFB_BL_NONE = 0, XIFB_BL_INTEL_HDR = 1, XIFB_BL_VESA = 2 };
struct XifbBacklight { int kind = XIFB_BL_NONE; uint32_t max = 0; bool vesa16 = false; uint8_t hdrCtl = 0; int startPercent = 100; };

// which DPCD backlight interface the panel has, and its level now (as a percentage of the interface's range)
static void xifbBacklightProbe(XifbBacklight *b)
{
    int off = 0; uint8_t c[4] = {};
    if (PE_parse_boot_argn("-xeintelfbnobl", &off, sizeof off)) { XIFBLOG("gu605 backlight: off by boot-arg"); return; }
    if (gXifbAuxLock) IOLockLock(gXifbAuxLock);
    if (xifbDpcdRead(0x340, c, 4) == 4 && (c[1] & 0x10) && (c[2] & 0x01)) {   // BRIGHTNESS_NITS_CAP, SDR_TCON_BRIGHTNESS_AUX_CAP
        uint32_t nits = 512; PE_parse_boot_argn("xeintelfbnits", &nits, sizeof nits);
        b->kind = XIFB_BL_INTEL_HDR; b->max = nits < 50 ? 50 : nits > 2000 ? 2000 : nits;
        uint8_t ctl = 0, lv[2] = {};
        if (xifbDpcdRead(0x344, &ctl, 1) == 1) b->hdrCtl = ctl;
        if ((ctl & 0x80) && xifbDpcdRead(0x354, lv, 2) == 2) {
            uint32_t now = lv[0] | lv[1] << 8; b->startPercent = (int)(now >= b->max ? 100 : now * 100 / b->max);
        }
        XIFBLOG("gu605 backlight: Intel HDR interface (tcon caps %02x %02x %02x %02x, ctl %02x), full scale %u nits, now %d%%",
                c[0], c[1], c[2], c[3], ctl, b->max, b->startPercent);
    } else {
        uint8_t g = 0, a = 0, mode = 0, lv[2] = {};
        if (xifbDpcdRead(0x701, &g, 1) == 1 && xifbDpcdRead(0x702, &a, 1) == 1 && (g & 0x01) && (a & 0x02)) {
            b->kind = XIFB_BL_VESA; b->vesa16 = (a & 0x04) != 0; b->max = b->vesa16 ? 0xffff : 0xff;
            if (xifbDpcdRead(0x721, &mode, 1) == 1 && (mode & 3) == 2 && xifbDpcdRead(0x722, lv, b->vesa16 ? 2 : 1) >= 1) {
                uint32_t now = b->vesa16 ? (uint32_t)(lv[0] << 8 | lv[1]) : lv[0]; b->startPercent = (int)(now * 100 / b->max);
            }
            XIFBLOG("gu605 backlight: VESA eDP interface (caps %02x %02x, mode %02x), %u-bit, now %d%%", g, a, mode,
                    b->vesa16 ? 16 : 8, b->startPercent);
        } else XIFBLOG("gu605 backlight: no DPCD backlight interface (tcon %02x %02x %02x, vesa %02x %02x): no brightness control",
                       c[0], c[1], c[2], g, a);
    }
    if (gXifbAuxLock) IOLockUnlock(gXifbAuxLock);
}

static bool xifbBacklightSet(XifbBacklight *b, uint32_t percent)
{
    if (b->kind == XIFB_BL_NONE) return false;
    if (percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    const uint32_t lvl = (uint32_t)((uint64_t)b->max * percent / 100);
    bool ok = false;
    if (gXifbAuxLock) IOLockLock(gXifbAuxLock);
    if (b->kind == XIFB_BL_INTEL_HDR) {
        uint8_t v[2] = { (uint8_t)lvl, (uint8_t)(lvl >> 8) };
        // AUX brightness on (bit 7), the other control bits as the firmware left them
        if (!(b->hdrCtl & 0x80)) { uint8_t ctl = b->hdrCtl | 0x80; if (xifbDpcdWrite(0x344, &ctl, 1) == 1) b->hdrCtl = ctl; }
        ok = (b->hdrCtl & 0x80) && xifbDpcdWrite(0x354, v, 2) == 2;
    } else {
        uint8_t mode = 0;
        if (xifbDpcdRead(0x721, &mode, 1) == 1 && (mode & 3) != 2) { mode = (uint8_t)((mode & ~3u) | 2); xifbDpcdWrite(0x721, &mode, 1); }
        uint8_t v[2] = { (uint8_t)(b->vesa16 ? lvl >> 8 : lvl), (uint8_t)lvl };
        ok = xifbDpcdWrite(0x722, v, b->vesa16 ? 2 : 1) >= 1;
    }
    if (gXifbAuxLock) IOLockUnlock(gXifbAuxLock);
    return ok;
}
