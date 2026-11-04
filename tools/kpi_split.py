#!/usr/bin/env python3
# Splits out/undefined.txt into symbols the macOS kernel already exports (with the bundle that exports them) and the
# ones XeGfx must implement itself. Uses Bash's kcabi reader on the boot kernel collection.
import os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if os.environ.get("XEGFX_XNU_EXPORTS"):
    # GU605 branch: no x86_64 kernel collection on this host (Apple-silicon CI): xnu's own x86_64 export lists
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import xnu_exports
    exp = xnu_exports.exported(os.environ["XEGFX_XNU_EXPORTS"])
    # names on tools/xnu-allow.txt were checked against a real x86_64 collection (the kernel's own symbol table
    # exports some, strstr for one, that no KPI list names): they stay bound to the kernel here too
    for l in open(os.path.join(ROOT, "tools/xnu-allow.txt")):
        if l.strip():
            exp.setdefault("_" + l.strip(), "com.apple.kernel (tools/xnu-allow.txt)")
    print(f"kpi_split: exports from xnu export lists in {os.environ['XEGFX_XNU_EXPORTS']} ({len(exp)} symbols)")
else:
    sys.path.insert(0, os.path.expanduser("~/src/kcabi"))
    import kcfile, linkcheck
    kc = kcfile.KC("/System/Library/KernelCollections/BootKernelExtensions.kc")
    exp = linkcheck.exported(kc)
# XNU exports some names FreeBSD also uses with another ABI (eventhandler_*, selrecord, ...): a name binds to XNU
# only when tools/xnu-allow.txt lists it as checked
allow = {l.strip() for l in open(os.path.join(ROOT, "tools/xnu-allow.txt")) if l.strip()}
have, todo, clash = [], [], []
for line in open(os.path.join(ROOT, "out/undefined.txt")):
    n, sym = line.split()
    if sym in exp and sym[1:] in allow:
        have.append((int(n), sym, exp[sym]))
    elif sym in exp and not sym[1:].startswith("xg_"):
        clash.append((int(n), sym, exp[sym]))
    else:
        todo.append((int(n), sym, ""))
open(os.path.join(ROOT, "out/kpi-clash.txt"), "w").write("".join(f"{s} {o}\n" for _, s, o in clash))
if clash:
    print("ABI CHECK NEEDED (exported by XNU, not on tools/xnu-allow.txt): " + " ".join(s for _, s, _ in clash))
open(os.path.join(ROOT, "out/kpi-have.txt"), "w").write("".join(f"{s} {o}\n" for _, s, o in have))
open(os.path.join(ROOT, "out/kpi-todo.txt"), "w").write("".join(f"{n} {s}\n" for n, s, _ in todo))
print(f"exported by macOS: {len(have)}   to implement: {len(todo)}")
