#!/bin/bash
# gu605-install.sh: installs a gu605-<sha> package (unzipped folder) on the Mac. Run from that folder:
#   sudo bash scripts/gu605-install.sh            (XEGFX_MODE=uninstall: see gu605-uninstall.sh)
# What goes where (XeGfx.kext itself is NOT installed here: it goes to the EFI, OpenCore Kernel>Add, see
# docs/GU605-KURULUM.md):
#   /Library/Extensions/{XeGfxIntelFB,XeGfxAccel,XeGfxAGDC,XgFbMove}.kext   -> auxiliary kernel collection, rebuilt
#   /Library/GPUBundles/XeMTLDriver.bundle   the Intel Metal plugin (NullMoth's NVMTLDriver.bundle is not touched)
#   /Library/GPUBundles/xemtl/               Mesa ANV + libdrm, when the package has userland/
#   /Library/XeGfx/gu605/                    scripts, tools/mtlprobe, gu605.conf (kept if present), logs/
#   /Library/LaunchDaemons/org.thebcd69.gu605.auto.plist   the boot-time runner (from the next boot on)
# Everything replaced is copied to /Library/XeGfx/gu605-backup-<date>/ first, the old collection too; nothing is
# deleted. The new collection must keep every third-party kext of the old one (NullMoth's), or the old one stays.
set -euo pipefail
PATH=/usr/bin:/bin:/usr/sbin:/sbin
die() { echo "gu605-install: STOP: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run with sudo"
P=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$P/SHA256SUMS" ] || die "run it from an unzipped gu605-<sha> package (no SHA256SUMS in $P)"
(cd "$P" && shasum -a 256 -c SHA256SUMS --quiet) || die "package checksum mismatch: download it again"
# a downloaded zip leaves com.apple.quarantine on every file: WindowServer and kmutil must not see it
xattr -cr "$P" 2>/dev/null || true
G=/Library/XeGfx/gu605; EXT=/Library/Extensions; GB=/Library/GPUBundles; KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
BK=/Library/XeGfx/gu605-backup-$(date +%Y%m%d-%H%M%S); mkdir -p "$BK" "$G/logs" "$G/tools"
KEXTS="XeGfxIntelFB XeGfxAccel XeGfxAGDC XgFbMove"
echo "backup -> $BK"
[ -f "$KC" ] && cp -p "$KC" "$BK/"
for k in $KEXTS; do [ -d "$EXT/$k.kext" ] && ditto "$EXT/$k.kext" "$BK/$k.kext"; done
[ -d "$GB/XeMTLDriver.bundle" ] && ditto "$GB/XeMTLDriver.bundle" "$BK/XeMTLDriver.bundle"
[ -d "$GB/xemtl" ] && ditto "$GB/xemtl" "$BK/xemtl"
[ -d "$G" ] && ditto --norsrc "$G" "$BK/gu605-scripts" 2>/dev/null || true
# NullMoth's NMIntelFB (built separately, not part of its installer) matches 8086:7D55 at a higher score than
# XeGfxIntelFB and would take the panel in Eco: this test needs XeGfxIntelFB. Nothing of NullMoth's is moved here.
[ ! -d "$EXT/NMIntelFB.kext" ] || [ "${GU605_ALLOW_NMINTELFB:-0}" = 1 ] || die "$EXT/NMIntelFB.kext is installed: it would drive the Intel panel instead of XeGfxIntelFB. Move it out of $EXT yourself (it is not part of NullMoth's installer), or run with GU605_ALLOW_NMINTELFB=1"
# what the current collection holds, as text (kmutil's column layout is not relied on: IDs are looked up in it)
OLDINS=$(kmutil inspect -a x86_64 -A "$KC" 2>/dev/null || true)
[ ! -f "$KC" ] || [ -n "$OLDINS" ] || die "cannot inspect the current $KC: nothing changed"
# every kext in $EXT that is in the current collection must be in the new one (NullMoth's above all)
# a whole bundle ID in kmutil's text, not a prefix of a longer one (com.nullmoth.NVRM vs com.nullmoth.NVRMFB)
has_id() { printf '%s\n' "$1" | grep -Eq "(^|[^A-Za-z0-9._-])$(printf '%s' "$2" | sed 's/[.]/[.]/g')([^A-Za-z0-9._-]|\$)"; }
KEEP=""
for x in "$EXT"/*.kext; do
  [ -f "$x/Contents/Info.plist" ] || continue
  id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$x/Contents/Info.plist" 2>/dev/null) || continue
  if has_id "$OLDINS" "$id"; then KEEP="$KEEP $id"; fi
done
NMIDS=$(printf '%s\n' "$OLDINS" | grep -oE '(com\.nullmoth\.[A-Za-z0-9]+|org\.hackintosh\.experimental\.NVPrimarySelector)' | sort -u | tr '\n' ' ' || true)
echo "kexts of $EXT in the current collection: $KEEP"
echo "NullMoth in the current collection: ${NMIDS:-none}"

for k in $KEXTS; do
  rm -rf "$EXT/$k.kext.new"; ditto "$P/Library/Extensions/$k.kext" "$EXT/$k.kext.new"
  chown -R root:wheel "$EXT/$k.kext.new"; chmod -R go-w "$EXT/$k.kext.new"
  [ -d "$EXT/$k.kext" ] && mv "$EXT/$k.kext" "$BK/$k.kext.replaced"
  mv "$EXT/$k.kext.new" "$EXT/$k.kext"
done
rm -rf "$GB/XeMTLDriver.bundle.new"; ditto "$P/Library/GPUBundles/XeMTLDriver.bundle" "$GB/XeMTLDriver.bundle.new"
# the AIR bitcode reader CoreDisplay's precompiled shaders need: NullMoth's own (same plugin lineage), else the package's
NMR=$GB/NVMTLDriver.bundle/Contents/MacOS/air-runtime
if [ -f "$NMR/libLLVM.dylib" ]; then ditto "$NMR" "$GB/XeMTLDriver.bundle.new/Contents/MacOS/air-runtime"; echo "AIR reader: copied from NullMoth's NVMTLDriver.bundle"
elif [ -d "$P/userland/air-runtime" ]; then ditto "$P/userland/air-runtime" "$GB/XeMTLDriver.bundle.new/Contents/MacOS/air-runtime"; echo "AIR reader: from the package"
else echo "note: no AIR bitcode reader available: the desktop step stays locked (Metal tests still run)"; fi
chown -R root:wheel "$GB/XeMTLDriver.bundle.new"; chmod -R a+rX,go-w "$GB/XeMTLDriver.bundle.new"
codesign -s - -f --deep "$GB/XeMTLDriver.bundle.new" > /dev/null 2>&1 || true
[ -d "$GB/XeMTLDriver.bundle" ] && mv "$GB/XeMTLDriver.bundle" "$BK/XeMTLDriver.bundle.replaced"
mv "$GB/XeMTLDriver.bundle.new" "$GB/XeMTLDriver.bundle"
if [ -f "$P/userland/libvulkan_intel.dylib" ] && [ -f "$P/userland/libdrm.2.dylib" ]; then
  X=$GB/xemtl; mkdir -p "$X"
  for f in libdrm.2.dylib libvulkan_intel.dylib; do cp "$P/userland/$f" "$X/$f.new"; done
  install_name_tool -id "$X/libdrm.2.dylib" "$X/libdrm.2.dylib.new" 2>/dev/null || true
  old=$(otool -L "$X/libvulkan_intel.dylib.new" | awk '/libdrm\.2\.dylib/ {print $1; exit}')
  install_name_tool -id "$X/libvulkan_intel.dylib" ${old:+-change "$old" "$X/libdrm.2.dylib"} "$X/libvulkan_intel.dylib.new" 2>/dev/null || true
  codesign -s - -f "$X/libdrm.2.dylib.new" "$X/libvulkan_intel.dylib.new" > /dev/null 2>&1 || true
  mv -f "$X/libdrm.2.dylib.new" "$X/libdrm.2.dylib"; mv -f "$X/libvulkan_intel.dylib.new" "$X/libvulkan_intel.dylib"
  printf '{\n    "file_format_version": "1.0.1",\n    "ICD": {\n        "library_path": "%s",\n        "api_version": "1.4.363",\n        "library_arch": "64"\n    }\n}\n' "$X/libvulkan_intel.dylib" > "$X/intel_icd.json"
  chown -R root:wheel "$X"; chmod -R a+rX,go-w "$X"; echo "Mesa ANV -> $X"
else echo "note: the package has no userland/ (Mesa ANV): automatic runs stop after stage 6"; fi
[ -f "$GB/xemtl-allow.txt" ] || { printf '# Intel Metal plugin (XeMTLDriver): processes that get the Intel Metal device.\n!WindowServer\n' > "$GB/xemtl-allow.txt"; chmod 644 "$GB/xemtl-allow.txt"; }

cp "$P"/scripts/gu605-*.sh "$G/"; chmod 755 "$G"/gu605-*.sh
[ -f "$P/tools/mtlprobe" ] && install -m 755 "$P/tools/mtlprobe" "$G/tools/mtlprobe"
[ -f "$P/tools/blurprobe" ] && install -m 755 "$P/tools/blurprobe" "$G/tools/blurprobe"
[ -f "$P/tools/fpsprobe" ] && install -m 755 "$P/tools/fpsprobe" "$G/tools/fpsprobe"
[ -f "$G/gu605.conf" ] || cp "$P/scripts/gu605.conf.default" "$G/gu605.conf"
cp "$P/BUILDINFO.txt" "$G/VERSION" 2>/dev/null || true
chown -R root:wheel "$G"
install -m 644 -o root -g wheel "$P/launchd/org.thebcd69.gu605.auto.plist" /Library/LaunchDaemons/org.thebcd69.gu605.auto.plist

echo "rebuilding the auxiliary kernel collection (keeps every kext in $EXT)"
K=/System/Library/Kernels/kernel; KARG=(--allow-missing-kdk); [ -f "$K" ] && KARG+=(--kernel "$K")
NEW=$KC.gu605-new; rm -f "$NEW"
# as NullMoth's installer (proven on this machine): the whole of $EXT, owned by root (macOS skips other owners)
kmutil create -n aux --volume-root / "${KARG[@]}" -B /System/Library/KernelCollections/BootKernelExtensions.kc \
  -S /System/Library/KernelCollections/SystemKernelExtensions.kc --repository "$EXT" -A "$NEW" -z > "$BK/kmutil.log" 2>&1 \
  || { tail -20 "$BK/kmutil.log"; die "kmutil failed (log $BK/kmutil.log; a kext in $EXT that names a missing dependency stops it); the old collection is still in place, but the new files are installed: restore from $BK or fix and rerun"; }
[ -s "$NEW" ] || die "kmutil produced no collection; the old one is still in place"
INS=$(kmutil inspect -a x86_64 -A "$NEW" 2>/dev/null) || die "cannot inspect the new collection"
for b in $KEEP $NMIDS org.watchthelight.XeGfxIntelFB org.watchthelight.XeGfxAccel org.watchthelight.XeGfxAGDC org.local.XgFbMove; do
  has_id "$INS" "$b" || die "$b would be missing from the new collection (old one kept; $NEW left for inspection)"
done
echo "new collection: every earlier kext kept (${NMIDS:-no NullMoth}) plus XeGfxIntelFB, XeGfxAccel, XeGfxAGDC, XgFbMove"
mv -f "$KC" "$BK/AuxiliaryKernelExtensions.kc.before" 2>/dev/null || true
mv -f "$NEW" "$KC"
# the collection the next boot reads is the Preboot copy: kcditto puts the new one there (samwu07's installer does too)
kcditto > "$BK/kcditto.log" 2>&1 || echo "note: kcditto failed (log $BK/kcditto.log)"
echo "done. Backup: $BK. Restart to apply. Then: docs/GU605-TEST.md"
