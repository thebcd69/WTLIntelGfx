#!/usr/bin/env python3
# xnu_exports.py: the x86_64 macOS kernel's exported KPI symbols from xnu's own export lists (config/*.exports in
# apple-oss-distributions/xnu), as {symbol: bundle}. A stand-in for reading the running x86_64 kernel collection when
# the build host has none (an Apple-silicon CI runner): tools/kpi_split.py and tools/shadow_check.py use it when
# XEGFX_XNU_EXPORTS names an xnu config directory. GU605 branch addition. BSD-2-Clause.
import glob, os

BUNDLE = {"BSDKernel": "com.apple.kpi.bsd", "IOKit": "com.apple.kpi.iokit", "Libkern": "com.apple.kpi.libkern",
          "Mach": "com.apple.kpi.mach", "Unsupported": "com.apple.kpi.unsupported", "Private": "com.apple.kpi.private",
          "MACFramework": "com.apple.kpi.dsep"}


def exported(confdir):
    out = {}
    for f in sorted(glob.glob(os.path.join(confdir, "*.exports"))):
        base = os.path.basename(f)[:-len(".exports")]
        parts = base.split(".")
        name = parts[0]
        # common lists and the x86_64 (and x86_64.MacOSX) ones only; never arm, Kasan/Kcov or Unused
        if name not in BUNDLE or any(p.startswith("arm") for p in parts[1:]) or "Tightbeam" in parts:
            continue
        for line in open(f):
            s = line.strip().split("#", 1)[0].strip()
            if not s or "*" in s:
                continue
            s = s.split(":")[0].strip()        # "sym:alias" lines export sym
            out.setdefault(s, BUNDLE[name])
    return out


if __name__ == "__main__":
    import sys
    e = exported(sys.argv[1])
    print(f"{len(e)} exported symbols")
