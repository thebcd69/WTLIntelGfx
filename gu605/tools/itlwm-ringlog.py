#!/usr/bin/env python3
# itlwm-ringlog.py <itlwm checkout>: GU605 Wi-Fi diagnosis build of laobamac/itlwm's AirportItlwm.
# The driver logs with kprintf (serial only). On this machine the lines of a failed start are gone from the 128 KB
# kernel message buffer before anything in user space can read it (2026-11-13: dmesg 31 s after boot began at the
# very end of the failed attach). So: XYLog also writes into a 768 KB buffer inside the kext that keeps the FIRST
# lines (the taskq chatter is left out), readable at any time with `sysctl -b debug.itlwm_log`. IOLog as well.
import sys, re, pathlib
root = pathlib.Path(sys.argv[1])
t = root / 'itl80211/linux/types.h'; s = t.read_text()
old = '''#define XYLog(fmt, x...)\\
do\\
{\\
kprintf("%s: " fmt, "itlwm", ##x);\\
}while(0)'''
new = '''#ifdef __cplusplus
extern "C" {
#endif
void itlwm_ringlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#ifdef __cplusplus
}
#endif
#define XYLog(fmt, x...)\\
do\\
{\\
itlwm_ringlog("%s: " fmt, "itlwm", ##x);\\
}while(0)'''
assert old in s, 'XYLog macro not found'
t.write_text(s.replace(old, new))
w = root / 'AirportItlwm/IOPCIEDeviceWrapper.cpp'; s = w.read_text()
impl = r'''
// ---- GU605 diagnosis: the driver's first log lines, kept inside the kext (sysctl -b debug.itlwm_log)
#include <sys/sysctl.h>
#include <stdarg.h>
#include <libkern/OSAtomic.h>
static char gItlRing[768 * 1024];
static volatile SInt32 gItlRingLen;
extern "C" void itlwm_ringlog(const char *fmt, ...)
{
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
    // (no strnstr: it is not exported to kexts; kmutil refused a kext that used it, 2026-11-13)
    for (int i = 0; i + 5 <= n; i++) if (line[i] == 't' && !memcmp(line + i, "taskq", 5)) return;
    IOLog("%s", line);
    SInt32 off = OSAddAtomic(n, &gItlRingLen);
    if (off >= 0 && off + n <= (SInt32)sizeof(gItlRing)) memcpy(gItlRing + off, line, n);
}
static int itlwm_ring_sysctl SYSCTL_HANDLER_ARGS
{
    SInt32 len = gItlRingLen;
    if (len > (SInt32)sizeof(gItlRing)) len = sizeof(gItlRing);
    return SYSCTL_OUT(req, gItlRing, (size_t)len);
}
SYSCTL_PROC(_debug, OID_AUTO, itlwm_log, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, 0, 0, itlwm_ring_sysctl, "S", "AirportItlwm: the first log lines");
static bool gItlSysctl;
static void itlwm_ring_register()
{
    if (!gItlSysctl) { gItlSysctl = true; sysctl_register_oid(&sysctl__debug_itlwm_log); }
}
'''
anchor = '#define super IOService\n'
assert anchor in s
s = s.replace(anchor, impl + anchor, 1)
old = '''probe(IOService *provider, SInt32 *score)
{
    XYLog("%s\\n", __PRETTY_FUNCTION__);'''
assert old in s, 'probe not found'
s = s.replace(old, '''probe(IOService *provider, SInt32 *score)
{
    itlwm_ring_register();
    XYLog("%s\\n", __PRETTY_FUNCTION__);''', 1)
w.write_text(s)
print('ringlog patch applied')
