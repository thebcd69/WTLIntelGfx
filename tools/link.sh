#!/bin/bash
# Links out/obj (from harness/score.py --obj) + the XNU side + generated stubs into out/XeGfx.kext and runs kcabi's
# linkcheck on it.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${SDKROOT:-/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk}
K="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
O="$ROOT/out/link"; rm -rf "$O"; mkdir -p "$O"
python3 "$ROOT/tools/kpi_split.py"
[ -s "$ROOT/out/kpi-clash.txt" ] && { echo "STOP: ABI clash, see out/kpi-clash.txt"; exit 1; }
python3 "$ROOT/tools/stubs.py"
xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -mkernel -ffreestanding -nostdinc -O2 -c \
  "$ROOT/shim/src/stubs/xg_stubs.c" -o "$O/xg_stubs.o"
for cpp in "$ROOT"/shim/src/xnu/*.cpp; do
  xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -mkernel -nostdinc -isysroot "$SDK" -I"$K" \
    -I"$ROOT/shim/include" -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions \
    -O2 -Wall -Wno-inconsistent-missing-override -Wno-deprecated-declarations -c "$cpp" -o "$O/$(basename "$cpp").o"
done
# firmware linked into the binary, so no daemon round trip is needed (empty table without linux-firmware)
FW=${XEGFX_FW:-$HOME/src/xe-src/linux-firmware}
FWS="mtl_guc_70.bin mtl_dmc.bin"   # DMC: GU605 display path (i915 owns the display)
{ printf '.section __DATA,__const\n.p2align 3\n.globl _xg_fw_embedded\n_xg_fw_embedded:\n'
  i=0; for f in $FWS; do [ -f "$FW/$f" ] || continue
    printf '.quad n%d, d%d, e%d - d%d\n' $i $i $i $i; i=$((i+1)); done
  printf '.quad 0, 0, 0\n'
  i=0; for f in $FWS; do [ -f "$FW/$f" ] || continue
    printf 'n%d: .asciz "%s"\n.p2align 4\nd%d: .incbin "%s"\ne%d:\n' $i "$f" $i "$FW/$f" $i; i=$((i+1)); done
} > "$O/xg_fwblob.S"
xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -c "$O/xg_fwblob.S" -o "$O/xg_fwblob.o"
B="$ROOT/out/XeGfx.kext/Contents"; rm -rf "$ROOT/out/XeGfx.kext"; mkdir -p "$B/MacOS"
find "$ROOT/out/obj" -name '*.o' > "$O/objs.txt"
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
  -Xlinker -filelist -Xlinker "$O/objs.txt" "$O/xg_stubs.o" "$O/xg_fwblob.o" "$O"/*.cpp.o -o "$B/MacOS/XeGfx" 2> "$O/ld.txt" || {
  grep -E 'Undefined|duplicate|error' "$O/ld.txt" | head -40; exit 1; }
cp "$ROOT/kext/Info.plist" "$B/Info.plist"
# GuC only for now: HuC and GSC need MEI (none on macOS), DMC only matters once i915 owns the display.
# Blobs come from linux-firmware (LICENSE.i915, redistributable) and are not in this repo.
if [ -f "$FW/mtl_guc_70.bin" ]; then
  mkdir -p "$B/Resources"
  cp "$FW/mtl_guc_70.bin" "$FW/LICENSE.i915" "$B/Resources/"
  [ -f "$FW/mtl_dmc.bin" ] && cp "$FW/mtl_dmc.bin" "$B/Resources/"
fi
codesign -s - -f "$ROOT/out/XeGfx.kext" >/dev/null 2>&1 || true
echo "XeGfx.kext $(stat -f %z "$B/MacOS/XeGfx") B"
if [ -n "${XEGFX_XNU_EXPORTS:-}" ]; then
  # GU605 branch, Apple-silicon host: no x86_64 collection to check against; the link check against the declared KPI
  # libraries happens when OpenCore or kmutil links the kext on the Mac
  echo "linkcheck/kpi_check skipped: no x86_64 kernel collection on this host (xnu export lists used for kpi_split/shadow)"
else
python3 ~/src/kcabi/linkcheck.py "$ROOT/out/XeGfx.kext" | tail -25
# every undefined symbol must resolve from a declared, public KPI library (kmutil enforces this when it builds a
# collection; linkcheck only knows what the kernel exports)
python3 "$ROOT/tools/kpi_check.py" "$ROOT/out/XeGfx.kext" || exit 1
fi
# a kernel name defined inside the kext would capture the XNU-side callers (I-094)
python3 "$ROOT/tools/shadow_check.py" "$ROOT/out/XeGfx.kext" || exit 1
# and against every other macOS build's collection we have (~/abi-src/<build>/kc, see kcabi)
for kc in "$HOME"/abi-src/*/kc/BootKernelExtensions.kc; do
  [ -f "$kc" ] || continue
  echo -n "$(basename "$(dirname "$(dirname "$kc")")"): "; python3 ~/src/kcabi/linkcheck.py "$ROOT/out/XeGfx.kext" --kc "$kc" | tail -1
  python3 "$ROOT/tools/shadow_check.py" "$ROOT/out/XeGfx.kext" --kc "$kc" | tail -1 | grep -q "ok" || {
    python3 "$ROOT/tools/shadow_check.py" "$ROOT/out/XeGfx.kext" --kc "$kc"; exit 1; }
done
# the userspace probe the guard runs after the stages; needs libdrm's uapi headers (XEGFX_DRM_INC)
INC=${XEGFX_DRM_INC:-$HOME/src/xe-userland/drm/include/drm}
if [ -f "$INC/i915_drm.h" ]; then
  xcrun clang -O2 -Wall -Wextra -Werror -mmacosx-version-min=11.0 -I "$INC" -o "$ROOT/out/xgprobe" "$ROOT/tools/xgprobe.c" || exit 1
  echo "xgprobe $(stat -f %z "$ROOT/out/xgprobe") B"
fi
