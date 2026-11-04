#!/bin/bash
# build_gu605_accel.sh: XeGfxAccel for macOS 15 Sequoia (GU605 branch): build_xegfxaccel.sh's command line without
# -DNM_TAHOE (the macOS 26 vtable forwarders, as samwu07's 4-kexts.sh does), with samwu07's zero-copy flip, gpufb and
# WindowServer-marks patches already in the source, and the GU605 Info.plist whose MetalPluginName is XeMTLDriver
# (build_gu605_plugin.sh), so NullMoth's NVMTLDriver is never loaded for the Intel GPU.
#   OGKM (open-gpu-kernel-modules 615.78.08), OUTROOT -> $OUTROOT/XeGfxAccel/15/XeGfxAccel.kext
set -uo pipefail
. "$(dirname "$0")/kext_flags.sh"
SRC="$REPO/kexts/XeGfxAccel"; name=XeGfxAccel; out="$OUTROOT/$name/15"
rm -rf "$out"; mkdir -p "$out/$name.kext/Contents/MacOS"
$CXX "${KFLAGS[@]}" -mmacosx-version-min=15.0 -I"$SRC" "${RMINC[@]}" "${KMSINC[@]}" "${RMDEFS[@]}" -DNMX_ACCEL -DNMX_XEGFX \
    -I"$SRC/accel" -I"$SRC/accel/iofam" -I"$SRC/accel/re" \
    -c "$SRC/accel/nvrm-accel.cpp" -o "$out/accel.o" 2>"$out/accel.err" || { grep -A3 error "$out/accel.err" | head -40; exit 1; }
$CXX "${LDFLAGS[@]}" -mmacosx-version-min=15.0 "$out/accel.o" -o "$out/$name.kext/Contents/MacOS/$name" 2>"$out/accelld.err" || { head -20 "$out/accelld.err"; exit 1; }
cp "$SRC/accel/xe/Info-gu605.plist" "$out/$name.kext/Contents/Info.plist"
codesign -s - -f "$out/$name.kext" >/dev/null 2>&1 || true
strings "$out/$name.kext/Contents/MacOS/$name" | grep -c 'nvaccel_xeflip' | xargs echo "xeflip strings:"
printf '15  '; kext_report "$out/$name.kext/Contents/MacOS/$name"
