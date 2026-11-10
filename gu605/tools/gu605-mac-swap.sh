#!/bin/bash
# gu605-mac-swap.sh: put a locally built XeGfxIntelFB.kext (whole bundle: Info.plist changes too) and the gu605 scripts
# of this checkout in place, without the rest of a release. Branch gu605-mac, for Mac-side test builds.
#   sudo bash gu605/tools/gu605-mac-swap.sh install <XeGfxIntelFB.kext> [<repo checkout for gu605/scripts>]
#   sudo bash gu605/tools/gu605-mac-swap.sh rollback <backup dir>
# Order: test collection with the new kext -> backup by MOVE (nothing deleted) -> install -> live collection -> kcditto.
# Every bundle ID the current collection holds (NullMoth's included) must still be in the new one.
set -euo pipefail
EXT=/Library/Extensions; G=/Library/XeGfx/gu605; KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
KB=/System/Library/KernelCollections/BootKernelExtensions.kc; KS=/System/Library/KernelCollections/SystemKernelExtensions.kc
K=/System/Library/Kernels/kernel; KARG=(--allow-missing-kdk); [ -f "$K" ] && KARG+=(--kernel "$K")
die() { echo "STOP: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run with sudo"
WANT="com.nullmoth.NVRM com.nullmoth.NVRMFB com.nullmoth.NVAccel com.nullmoth.NVRMAGDC org.watchthelight.XeGfxIntelFB org.watchthelight.XeGfxAccel org.watchthelight.XeGfxAGDC org.local.XgFbMove"
build_kc() {  # <repository> <output kc>
  kmutil create -n aux --volume-root / "${KARG[@]}" -B "$KB" -S "$KS" --repository "$1" -A "$2" -z > "$2.log" 2>&1 \
    || { tail -20 "$2.log"; die "kmutil create failed ($2.log)"; }
  [ -s "$2" ] || die "kmutil produced no collection"
  local ins id; ins=$(kmutil inspect -a x86_64 -A "$2" 2>/dev/null) || die "cannot inspect $2"
  # a here-string, not printf | grep -q: with pipefail, grep -q leaving at the first match sends printf SIGPIPE and the
  # check failed on a bundle ID that WAS in the collection (2026-11-10 15:55, com.nullmoth.NVRMAGDC)
  for id in $WANT; do grep -Eq "(^|[^A-Za-z0-9.])${id//./\\.}([^A-Za-z0-9.]|$)" <<< "$ins" || { tail -20 "$2.log"; die "$id missing from $2"; }; done
}
live_kc() {
  local T; T=$(mktemp -d /var/tmp/gu605-mac.XXXX)
  build_kc "$EXT" "$T/new.kc"
  mv -f "$KC" "$1/AuxiliaryKernelExtensions.kc.before" 2>/dev/null || true
  mv -f "$T/new.kc" "$KC"
  kcditto > "$1/kcditto.log" 2>&1 || echo "note: kcditto failed (log $1/kcditto.log)"
}
case "${1:-}" in
install)
  NEW=${2:-}; REPO=${3:-}
  [ -f "$NEW/Contents/MacOS/XeGfxIntelFB" ] && plutil -lint "$NEW/Contents/Info.plist" >/dev/null || die "no XeGfxIntelFB.kext at $NEW"
  [ -d "$EXT/XeGfxIntelFB.kext" ] || die "no $EXT/XeGfxIntelFB.kext (install the gu605 release first)"
  BK=/Library/XeGfx/gu605-mac-backup-$(date +%Y%m%d-%H%M%S); mkdir -p "$BK"
  T=$(mktemp -d /var/tmp/gu605-mac.XXXX); mkdir -p "$T/repo"
  echo "1. test collection with the new XeGfxIntelFB"
  for x in "$EXT"/*.kext; do [ "$(basename "$x")" = XeGfxIntelFB.kext ] || ditto "$x" "$T/repo/$(basename "$x")"; done
  ditto "$NEW" "$T/repo/XeGfxIntelFB.kext"; xattr -cr "$T/repo/XeGfxIntelFB.kext"
  chown -R root:wheel "$T/repo"; chmod -R go-w "$T/repo"
  build_kc "$T/repo" "$T/test.kc"
  echo "2. back up -> $BK"
  ditto "$EXT/XeGfxIntelFB.kext" "$BK/XeGfxIntelFB.kext"; cp -p "$KC" "$BK/AuxiliaryKernelExtensions.kc"
  [ -z "$REPO" ] || ditto --norsrc "$G" "$BK/gu605-scripts"
  shasum -a 256 "$EXT/XeGfxIntelFB.kext/Contents/MacOS/XeGfxIntelFB" "$EXT/XeGfxIntelFB.kext/Contents/Info.plist" \
    "$NEW/Contents/MacOS/XeGfxIntelFB" "$NEW/Contents/Info.plist" > "$BK/SHA256"
  echo "3. install"
  mv "$EXT/XeGfxIntelFB.kext" "$BK/XeGfxIntelFB.kext.replaced"
  ditto "$T/repo/XeGfxIntelFB.kext" "$EXT/XeGfxIntelFB.kext"; chown -R root:wheel "$EXT/XeGfxIntelFB.kext"; chmod -R 755 "$EXT/XeGfxIntelFB.kext"
  if [ -n "$REPO" ]; then
    for f in "$REPO"/gu605/scripts/gu605-*.sh; do cp "$f" "$G/$(basename "$f")"; done
    chmod 755 "$G"/gu605-*.sh; echo "   gu605 scripts from $REPO (gu605.conf kept)"
  fi
  echo "4. live collection + kcditto"
  live_kc "$BK"
  echo "OK. Backup: $BK   Undo: sudo bash $0 rollback $BK   Then restart."
  ;;
rollback)
  BK=${2:-}; [ -d "$BK/XeGfxIntelFB.kext" ] || die "no XeGfxIntelFB.kext in backup $BK"
  [ ! -d "$EXT/XeGfxIntelFB.kext" ] || mv "$EXT/XeGfxIntelFB.kext" "$BK/XeGfxIntelFB.kext.test-$(date +%H%M%S)"
  ditto "$BK/XeGfxIntelFB.kext" "$EXT/XeGfxIntelFB.kext"; chown -R root:wheel "$EXT/XeGfxIntelFB.kext"; chmod -R 755 "$EXT/XeGfxIntelFB.kext"
  [ ! -d "$BK/gu605-scripts" ] || { for f in "$BK"/gu605-scripts/gu605-*.sh; do cp "$f" "$G/"; done; }
  live_kc "$BK"
  echo "OK: the previous XeGfxIntelFB is back. Restart."
  ;;
*) die "usage: $0 install <XeGfxIntelFB.kext> [<repo>] | rollback <backup dir>" ;;
esac
