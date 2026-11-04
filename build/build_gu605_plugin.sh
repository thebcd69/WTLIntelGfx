#!/bin/bash
# build_gu605_plugin.sh: the Intel-only Metal plugin bundle for the GU605 branch, XeMTLDriver.bundle, which
# XeGfxAccel-gu605 names as its MetalPluginName. Same sources and flags as build_plugin.sh (WTL's plugin with samwu07's
# WindowServer-marks patch), plus:
#   - its own allow-list, /Library/GPUBundles/xemtl-allow.txt (NullMoth's nvmtl-allow.txt stays NullMoth's)
#   - every /Library/GPUBundles/nvmtl/ path (ANV, libdrm, ICD manifest, caches, nvrm610.conf) moved to
#     /Library/GPUBundles/xemtl/ in a copy of the sources: NullMoth's installer replaces nvmtl wholesale on every
#     update, and the Intel stack must neither be wiped by it nor overwrite NullMoth's files
#   - the translator (libnvmtl_translate.dylib) inside the bundle, next to the binary, where the plugin looks first, so
#     NullMoth's own translator in /Library/GPUBundles/nvmtl is never used by (or replaced for) the Intel GPU
# NullMoth's NVMTLDriver.bundle is not touched. Output: $XEGFX_OUT/plugin-gu605/XeMTLDriver.bundle
#   XEGFX_DEPS (Vulkan-Headers), XEGFX_OUT, RELEASE=1 as build_plugin.sh; XLATE=<dylib> to bundle a translator.
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${XEGFX_DEPS:-$HOME/src/deps}
O=${XEGFX_OUT:-$HOME/src/XeGfx-build}/plugin-gu605
B=$O/XeMTLDriver.bundle
rm -rf "$O"; mkdir -p "$B/Contents/MacOS" "$O"
RDEF=""; [ -n "${RELEASE:-}" ] && RDEF="-DNVMTL_RELEASE=1"
rm -rf "$O/src"; mkdir -p "$O/src"; cp -R "$R/plugin/." "$O/src/"
grep -rl '/Library/GPUBundles/nvmtl/' "$O/src" | while read -r f; do sed -i '' 's#/Library/GPUBundles/nvmtl/#/Library/GPUBundles/xemtl/#g' "$f"; done
left=$( (grep -rn '/Library/GPUBundles/nvmtl/' "$O/src" || true) | wc -l | tr -d ' ')   # grep finds none: exit 1 under pipefail
[ "$left" = 0 ] || { echo "STOP: $left nvmtl paths left"; exit 6; }
cd "$O/src"
xcrun clang -arch x86_64 -bundle -fobjc-arc -O1 $RDEF -DNVMTL_ALLOW_LIST='"/Library/GPUBundles/xemtl-allow.txt"' \
  -Wno-protocol -Wno-incomplete-implementation -mmacosx-version-min=15.0 -I"$DEPS/Vulkan-Headers/include" \
  -o "$B/Contents/MacOS/XeMTLDriver" NVMTLDevice.m \
  -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation 2>"$O/XeMTLDriver.err" \
  || { echo "compile failed"; grep -A2 ': error:' "$O/XeMTLDriver.err" | head -40; exit 5; }
cat > "$B/Contents/Info.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>XeMTLDriver</string>
	<key>CFBundleIdentifier</key><string>org.thebcd69.gu605.XeMTLDriver</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>XeMTLDriver</string>
	<key>CFBundlePackageType</key><string>BNDL</string>
	<key>CFBundleShortVersionString</key><string>0.1</string>
	<key>CFBundleVersion</key><string>0.1</string>
	<key>NSPrincipalClass</key><string>NVMTLDevice</string>
</dict>
</plist>
PL
if [ -n "${XLATE:-}" ] && [ -f "$XLATE" ]; then cp "$XLATE" "$B/Contents/MacOS/libnvmtl_translate.dylib"; fi
codesign -f -s - "$B" 2>/dev/null || true
nm -gU "$B/Contents/MacOS/XeMTLDriver" | grep -c 'OBJC_CLASS_\$_NVMTLDevice' | xargs echo "NVMTLDevice class:"
echo "XeMTLDriver $(git -C "$R" rev-parse --short HEAD) sha $(shasum -a256 "$B/Contents/MacOS/XeMTLDriver" | cut -c1-12) warnings $(grep -c ': warning:' "$O/XeMTLDriver.err" || true) translator $([ -f "$B/Contents/MacOS/libnvmtl_translate.dylib" ] && echo bundled || echo none)"
