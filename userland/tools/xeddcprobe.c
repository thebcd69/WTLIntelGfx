// xeddcprobe: DDC/CI on the Intel framebuffer (XeGfxIntelFramebuffer, gu605-ddc), the checks BetterDisplay depends on,
// and the real I2C return codes IOGraphicsLib hides. GU605 branch of XeGfx.
//   xeddcprobe                   every framebuffer: class, registry path, I2C bus, XeGfxDdc status; every display:
//                                IODisplayLocation, whether that path resolves to this very display (unique), and
//                                whether the framebuffer it resolves to has an I2C bus (what BetterDisplay needs)
//   xeddcprobe diag              one Get VCP 0x10 through the I2C user client directly: acquire / IO / release with
//                                their own IOReturn (kIOReturnOffline 0xe00002d7 = the framebuffer is not "powered")
//   xeddcprobe <vcp>             read a VCP code (hex) on the Intel framebuffer's bus 0
//   xeddcprobe <vcp> <value>     set it (decimal), then read it back
//   sudo xeddcprobe enable 0|1   DDC off / on in XeGfxIntelFB at once (its XeGfxDdcEnable property; no reboot)
// Exit 0 when every check (or the read/set) succeeded. Build (Command Line Tools only):
//   clang -O2 -framework IOKit -framework CoreFoundation xeddcprobe.c -o xeddcprobe
// <wtl> project: xegfx (GU605 branch) | BSD-2-Clause </wtl>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOGraphicsLib.h>
#include <IOKit/i2c/IOI2CInterface.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *kIntel = "XeGfxIntelFramebuffer";

static void prop_str(io_registry_entry_t e, CFStringRef k, char *out, size_t n)
{
    out[0] = 0;
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)v, out, (CFIndex)n, kCFStringEncodingUTF8);
    else if (v && CFGetTypeID(v) == CFDataGetTypeID()) {
        CFIndex l = CFDataGetLength((CFDataRef)v); if (l >= (CFIndex)n) l = (CFIndex)n - 1;
        memcpy(out, CFDataGetBytePtr((CFDataRef)v), (size_t)l); out[l] = 0;
    }
    if (v) CFRelease(v);
}

// the IOFramebuffer above a display service (AppleDisplay / AppleBacklightDisplay -> IODisplayConnect -> framebuffer)
static io_service_t fb_of(io_service_t d)
{
    io_registry_entry_t e = d, p = 0;
    IOObjectRetain(e);
    for (int i = 0; i < 6; i++) {
        if (IORegistryEntryGetParentEntry(e, kIOServicePlane, &p)) { IOObjectRelease(e); return 0; }
        IOObjectRelease(e); e = p;
        if (IOObjectConformsTo(e, "IOFramebuffer")) return e;
    }
    IOObjectRelease(e);
    return 0;
}

static IOReturn ddc_xfer(io_service_t fb, UInt8 *send, int nsend, UInt8 *reply, int nreply)
{
    io_service_t bus = 0;
    IOReturn r = IOFBCopyI2CInterfaceForBus(fb, 0, &bus);
    if (r) return r;
    IOI2CConnectRef c = NULL;
    r = IOI2CInterfaceOpen(bus, kNilOptions, &c);
    IOObjectRelease(bus);
    if (r) return r;
    IOI2CRequest q; memset(&q, 0, sizeof q);
    q.sendTransactionType = kIOI2CSimpleTransactionType; q.sendAddress = 0x6E;
    q.sendBuffer = (vm_address_t)send; q.sendBytes = (UInt32)nsend;
    if (nreply) {
        q.replyTransactionType = kIOI2CDDCciReplyTransactionType; q.replyAddress = 0x6F;
        q.replyBuffer = (vm_address_t)reply; q.replyBytes = (UInt32)nreply;
        q.minReplyDelay = 50 * 1000 * 1000;   // ns (absolute time units on x86)
    }
    r = IOI2CSendRequest(c, kNilOptions, &q);
    IOI2CInterfaceClose(c, kNilOptions);
    return r ? r : q.result;
}
static int get_vcp(io_service_t fb, UInt8 vcp, int *cur, int *max)
{
    UInt8 s[5] = { 0x51, 0x82, 0x01, vcp, 0 };
    s[4] = 0x6E ^ s[0] ^ s[1] ^ s[2] ^ s[3];
    for (int tries = 0; tries < 3; tries++) {
        UInt8 r[11]; memset(r, 0, sizeof r);
        IOReturn e = ddc_xfer(fb, s, 5, r, sizeof r);
        if (e) { printf("  read VCP 0x%02x: IOReturn 0x%x\n", vcp, e); usleep(100000); continue; }
        UInt8 chk = 0x50; for (int i = 0; i < 10; i++) chk ^= r[i];
        printf("  reply:"); for (int i = 0; i < 11; i++) printf(" %02x", r[i]);
        printf("%s\n", chk == r[10] ? "" : "  (checksum mismatch)");
        // IOGraphicsLib returns success with the request's own bytes when the I2C IO failed: r[0] 0x51 is that echo
        if (r[0] == 0x51) printf("  that is the request echoed back: the IO did not run (see: xeddcprobe diag)\n");
        if (r[2] == 0x02 && r[4] == vcp && r[3] == 0) { *max = (r[6] << 8) | r[7]; *cur = (r[8] << 8) | r[9]; return 0; }
        usleep(100000);
    }
    return -1;
}
static int set_vcp(io_service_t fb, UInt8 vcp, int v)
{
    UInt8 s[7] = { 0x51, 0x84, 0x03, vcp, (UInt8)(v >> 8), (UInt8)v, 0 };
    s[6] = 0x6E ^ s[0] ^ s[1] ^ s[2] ^ s[3] ^ s[4] ^ s[5];
    IOReturn e = ddc_xfer(fb, s, 7, NULL, 0);
    if (e) printf("  set VCP 0x%02x: IOReturn 0x%x\n", vcp, e);
    return e ? -1 : 0;
}

// IOI2CInterfaceUserClient's own methods (IOGraphics IOI2CInterface.cpp): 0 acquire, 1 release, 2 IO with
// struct IOI2CBuffer { IOI2CRequest request; UInt8 inlineBuffer[1024]; } in and out
struct xe_i2c_buffer { IOI2CRequest request; UInt8 inlineBuffer[1024]; };
static int diag(io_service_t fb)
{
    io_service_t bus = 0;
    IOReturn r = IOFBCopyI2CInterfaceForBus(fb, 0, &bus);
    printf("bus 0: %s (0x%x)\n", r ? "absent" : "present", r);
    if (r) return 1;
    io_connect_t c = 0;
    r = IOServiceOpen(bus, mach_task_self(), 0, &c);
    IOObjectRelease(bus);
    printf("open 0x%x\n", r);
    if (r) return 1;
    r = IOConnectCallMethod(c, 0, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    printf("acquire 0x%x\n", r);
    struct xe_i2c_buffer in, out; memset(&in, 0, sizeof in);
    UInt8 s[5] = { 0x51, 0x82, 0x01, 0x10, 0 }; s[4] = 0x6E ^ s[0] ^ s[1] ^ s[2] ^ s[3];
    in.request.sendTransactionType = kIOI2CSimpleTransactionType; in.request.sendAddress = 0x6E; in.request.sendBytes = 5;
    in.request.replyTransactionType = kIOI2CDDCciReplyTransactionType; in.request.replyAddress = 0x6F; in.request.replyBytes = 11;
    in.request.minReplyDelay = 50 * 1000 * 1000;
    memcpy(in.inlineBuffer, s, sizeof s);
    size_t osz = sizeof out;
    IOReturn io = IOConnectCallMethod(c, 2, NULL, 0, &in, sizeof in, NULL, NULL, &out, &osz);
    printf("io 0x%x%s result 0x%x replyBytes %u:", io, io == (IOReturn)0xe00002d7 ? " (kIOReturnOffline: IOGraphics thinks the framebuffer is off)" : "",
           out.request.result, (unsigned)out.request.replyBytes);
    for (unsigned i = 0; !io && i < out.request.replyBytes && i < 16; i++) printf(" %02x", out.inlineBuffer[i]);
    printf("\n");
    r = IOConnectCallMethod(c, 1, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    printf("release 0x%x\n", r);
    IOServiceClose(c);
    return io || out.request.result ? 1 : 0;
}

int main(int argc, char **argv)
{
    io_iterator_t it; io_service_t fb, intel = 0; int bad = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOFramebuffer"), &it)) return 1;
    const int list = argc < 2;
    while ((fb = IOIteratorNext(it))) {
        io_name_t cls; IOObjectGetClass(fb, cls);
        io_string_t path; if (IORegistryEntryGetPath(fb, kIOServicePlane, path)) strcpy(path, "?");
        io_service_t bus = 0; IOReturn r = IOFBCopyI2CInterfaceForBus(fb, 0, &bus);
        if (bus) IOObjectRelease(bus);
        if (list) {
            char st[256]; prop_str(fb, CFSTR("XeGfxDdc"), st, sizeof st);
            printf("framebuffer %s\n  path %s\n  I2C bus 0 %s%s%s\n", cls, path, r ? "absent" : "present", st[0] ? "\n  XeGfxDdc " : "", st);
        }
        if (!strcmp(cls, kIntel) && !intel) { intel = fb; IOObjectRetain(intel); }
        IOObjectRelease(fb);
    }
    IOObjectRelease(it);
    if (list) {
        // every display: its IODisplayLocation must resolve to itself, and its framebuffer must be the one with the bus
        if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IODisplay"), &it)) return 1;
        io_service_t d; int n = 0;
        while ((d = IOIteratorNext(it))) {
            char loc[512]; prop_str(d, CFSTR(kIODisplayLocationKey), loc, sizeof loc);
            io_name_t cls; IOObjectGetClass(d, cls);
            io_registry_entry_t res = loc[0] ? IORegistryEntryFromPath(kIOMainPortDefault, loc) : 0;
            const int self = res && IOObjectIsEqualTo(res, d);
            io_service_t f = fb_of(d), rf = res ? fb_of(res) : 0;
            io_service_t bus = 0; const IOReturn br = rf ? IOFBCopyI2CInterfaceForBus(rf, 0, &bus) : kIOReturnNoDevice;
            if (bus) IOObjectRelease(bus);
            io_name_t fcls = "?"; if (f) IOObjectGetClass(f, fcls);
            printf("display %s on %s\n  IODisplayLocation %s\n  resolves to %s; that framebuffer's I2C bus %s\n", cls, fcls, loc[0] ? loc : "(none)",
                   self ? "this display (unique)" : res ? "ANOTHER display (not unique: BetterDisplay would ask the wrong framebuffer)" : "nothing",
                   br ? "absent" : "present");
            if (!self) bad++;
            if (res) IOObjectRelease(res);
            if (rf) IOObjectRelease(rf);
            if (f) IOObjectRelease(f);
            IOObjectRelease(d); n++;
        }
        IOObjectRelease(it);
        if (!n) printf("no IODisplay\n");
        if (!intel) { printf("no %s\n", kIntel); return 4; }
        int cur = -1, max = -1;
        printf("%s: VCP 0x10 (brightness)\n", kIntel);
        if (get_vcp(intel, 0x10, &cur, &max) == 0) printf("  VCP 0x10 = %d / %d\n", cur, max); else { printf("  no valid reply\n"); bad++; }
        IOObjectRelease(intel);
        return bad ? 1 : 0;
    }
    if (!intel) { printf("no %s\n", kIntel); return 4; }
    int rc = 0;
    if (!strcmp(argv[1], "diag")) rc = diag(intel);
    else if (!strcmp(argv[1], "enable") && argc > 2) {
        const IOReturn r = IORegistryEntrySetCFProperty(intel, CFSTR("XeGfxDdcEnable"), atoi(argv[2]) ? kCFBooleanTrue : kCFBooleanFalse);
        char st[256]; prop_str(intel, CFSTR("XeGfxDdc"), st, sizeof st);
        printf("XeGfxDdcEnable %s: 0x%x%s
XeGfxDdc %s
", atoi(argv[2]) ? "1" : "0", r, r == (IOReturn)0xe00002c1 ? " (not root)" : "", st);
        rc = r ? 1 : 0;
    }
    else {
        const UInt8 vcp = (UInt8)strtol(argv[1], NULL, 16);
        int cur = -1, max = -1;
        if (argc > 2) { if (set_vcp(intel, vcp, atoi(argv[2]))) rc = 2; usleep(200000); }
        if (get_vcp(intel, vcp, &cur, &max) == 0) printf("VCP 0x%02x = %d / %d\n", vcp, cur, max); else { printf("VCP 0x%02x: no valid reply\n", vcp); rc = rc ? rc : 3; }
    }
    IOObjectRelease(intel);
    return rc;
}
