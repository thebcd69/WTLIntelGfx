#!/bin/bash
# build_xgfbmove.sh: XgFbMove.kext (GU605 branch), the boot framebuffer's GGTT move out of i915's GuC slot, from
# kexts/XgFbMove (samwu07, BSD-2-Clause) -> $OUTROOT/XgFbMove/XgFbMove.kext. Loaded by hand (no personality), before XeGfx.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
SDK=${SDKROOT:-$(xcrun --sdk macosx --show-sdk-path)}
OUT=${OUTROOT:-$HOME/src/XeGfx-build/kexts}/XgFbMove
K="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[ -d "$K" ] || { echo "STOP: no Kernel.framework headers in $SDK"; exit 1; }
SRC="$REPO/kexts/XgFbMove"
rm -rf "$OUT/XgFbMove.kext"; mkdir -p "$OUT/XgFbMove.kext/Contents/MacOS"
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -mkernel -nostdinc -isysroot "$SDK" -I"$K" -I"$SRC" \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common \
  -O2 -Wall -Werror -Wno-unused-parameter -Wno-unused-function -g0 -c "$SRC/xgfbmove.cpp" -o "$OUT/xgfbmove.o"
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
  "$OUT/xgfbmove.o" -o "$OUT/XgFbMove.kext/Contents/MacOS/XgFbMove"
cp "$SRC/Info.plist" "$OUT/XgFbMove.kext/Contents/Info.plist"
codesign -s - -f "$OUT/XgFbMove.kext" >/dev/null 2>&1 || true
echo "XgFbMove $(shasum -a 256 "$OUT/XgFbMove.kext/Contents/MacOS/XgFbMove" | cut -c1-12) -> $OUT/XgFbMove.kext"
