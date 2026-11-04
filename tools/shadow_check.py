#!/usr/bin/env python3
# Usage: tools/shadow_check.py <kext> [--kc PATH] : fails when the kext defines an external symbol the kernel collection
# also exports. The static link binds the kext's own callers to its own copy, so an XNU-side caller silently gets the
# FreeBSD/Linux function of the same name (I-094: sysctl_register_oid).
import os, re, subprocess, sys
import plistlib  # noqa: E402
args = sys.argv[1:]
if os.environ.get("XEGFX_XNU_EXPORTS") and "--kc" not in args:
    # GU605 branch: xnu's x86_64 export lists instead of the host's kernel collection (Apple-silicon CI)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import xnu_exports  # noqa: E402
    _inf = plistlib.load(open(os.path.join(args[0], "Contents", "Info.plist"), "rb"))
    exe = os.path.join(args[0], "Contents", "MacOS", _inf["CFBundleExecutable"])
    kern = xnu_exports.exported(os.environ["XEGFX_XNU_EXPORTS"])
else:
    sys.path.insert(0, os.path.expanduser("~/src/kcabi"))
    from kcfile import KC  # noqa: E402
    from linkcheck import DEFAULT_KC, exe_of, exported  # noqa: E402
    kcpath = DEFAULT_KC
    if "--kc" in args:
        i = args.index("--kc"); kcpath = args[i + 1]; del args[i:i + 2]
    exe, _ = exe_of(args[0])
    kern = None
text = subprocess.run(["nm", "-gUm", exe], capture_output=True, text=True, check=True).stdout
mine = {m.group(1) for m in re.finditer(r"\) external (\S+)", text)}
if kern is None:
    kern = exported(KC(kcpath))
# the kmod glue every kext defines is expected
allowed = {"_kmod_info", "__realmain", "__antimain", "__kext_apple_cc", "_KextBuild", "__start", "__stop"}
hits = sorted(s for s in mine & set(kern) if s not in allowed)
for s in hits:
    print(f"{s}  (also exported by {kern[s]})")
if hits:
    print(f"shadow check FAILED: {len(hits)} kernel symbols redefined in the kext")
    sys.exit(1)
print(f"shadow check ok ({len(mine)} defined symbols)")
