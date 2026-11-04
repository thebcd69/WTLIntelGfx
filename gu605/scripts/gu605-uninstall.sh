#!/bin/bash
# gu605-uninstall.sh: takes the GU605 Intel stack out again. Nothing is deleted: every file is moved to
# /Library/XeGfx/gu605-removed-<date>/, then the auxiliary collection is rebuilt without our kexts (NullMoth's stay).
# XeGfx.kext in the EFI is undone separately (docs/GU605-KURULUM.md, rollback).
#   sudo bash /Library/XeGfx/gu605/gu605-uninstall.sh
set -euo pipefail
PATH=/usr/bin:/bin:/usr/sbin:/sbin
die() { echo "gu605-uninstall: STOP: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run with sudo"
EXT=/Library/Extensions; GB=/Library/GPUBundles; KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
RM=/Library/XeGfx/gu605-removed-$(date +%Y%m%d-%H%M%S); mkdir -p "$RM"
OURS="org.watchthelight.XeGfxIntelFB org.watchthelight.XeGfxAccel org.watchthelight.XeGfxAGDC org.local.XgFbMove"
OLD=$(kmutil inspect -a x86_64 -A "$KC" 2>/dev/null | awk -F'\t' '$1 ~ /^[a-z]+\.[A-Za-z0-9.-]+$/ && $1 !~ /^com\.apple\./ {print $1}' | sort -u || true)
KEEP=""; for b in $OLD; do case " $OURS " in *" $b "*) ;; *) KEEP="$KEEP $b";; esac; done
cp -p "$KC" "$RM/" 2>/dev/null || true
for k in XeGfxIntelFB XeGfxAccel XeGfxAGDC XgFbMove; do [ -d "$EXT/$k.kext" ] && mv "$EXT/$k.kext" "$RM/"; done
[ -f /Library/LaunchDaemons/org.thebcd69.gu605.auto.plist ] && mv /Library/LaunchDaemons/org.thebcd69.gu605.auto.plist "$RM/"
[ -d "$GB/XeMTLDriver.bundle" ] && mv "$GB/XeMTLDriver.bundle" "$RM/"
[ -d "$GB/xemtl" ] && mv "$GB/xemtl" "$RM/"
[ -f "$GB/xemtl-allow.txt" ] && mv "$GB/xemtl-allow.txt" "$RM/"
K=/System/Library/Kernels/kernel; KARG=(--allow-missing-kdk); [ -f "$K" ] && KARG+=(--kernel "$K")
BIDS=(); for b in $KEEP; do BIDS+=(-b "$b"); done
NEW=$KC.gu605-new; rm -f "$NEW"
if [ ${#BIDS[@]} -gt 0 ]; then
  kmutil create -n aux --volume-root / "${KARG[@]}" -B /System/Library/KernelCollections/BootKernelExtensions.kc \
    -S /System/Library/KernelCollections/SystemKernelExtensions.kc --repository "$EXT" "${BIDS[@]}" -A "$NEW" -z > "$RM/kmutil.log" 2>&1 \
    || die "kmutil failed (log $RM/kmutil.log); files are in $RM, the old collection is still in place"
  INS=$(kmutil inspect -a x86_64 -A "$NEW" 2>/dev/null)
  for b in $KEEP; do printf '%s\n' "$INS" | grep -E "^$b[[:space:]]" > /dev/null || die "$b would be missing (old collection kept)"; done
  mv -f "$KC" "$RM/AuxiliaryKernelExtensions.kc.before"; mv -f "$NEW" "$KC"
fi
echo "removed to $RM (scripts and logs stay in /Library/XeGfx/gu605). Restart to apply."
