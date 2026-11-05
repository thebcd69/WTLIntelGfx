#!/bin/bash
# gu605-fps.sh [SECONDS] [REPORTDIR]: how many frames really reach the screen, per second, for SECONDS (default 20).
# Run it, then drag a window in circles (or play https://www.testufo.com in Safari) until it ends. Read-only.
# Per second:
#   panel   the pipe's own refresh (frame counter; debug.xegfx_kms on the display path, else XeGfxIntelFB's measurement)
#   shown   frames XeGfxAccel put on the Intel panel (debug.nvaccel_iop_ok_h4): what you see change
#   flips   of those, scanned out of WindowServer's surface without a copy (xeflip); copied = CPU copies
# A 240 Hz desktop shows panel ~240 and shown close to 240 while dragging; shown ~60 with panel 240 means
# WindowServer composites at 60 (the mode it believes, or VBL pacing); panel 60 means the mode is still 60 Hz.
set -uo pipefail
. "$(dirname "$0")/gu605-common.sh"
N=${1:-20}; R=${2:-$L/fps-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$R"; O=$R/fps.txt
ctr() { sysctl -n "$1" 2>/dev/null || echo 0; }
fbprop() { ioreg -r -c XeGfxIntelFramebuffer -d 1 -l -w0 2>/dev/null | sed -nE "s/.*\"$1\" = ([0-9]+).*/\\1/p" | head -1; }
panel() {   # measured refresh of the active pipe, Hz
  local k; k=$(sysctl -n debug.xegfx_kms 2>/dev/null | sed -nE 's/^crtc .* active 1 .* measured ([0-9]+) Hz.*/\1/p' | head -1)
  [ -n "$k" ] && { echo "$k"; return; }
  k=$(fbprop GU605MeasuredHz16); [ -n "$k" ] && echo $(( k >> 16 )) || echo "?"
}
# the pipe refresh once before and once after (debug.xegfx_kms probes every connector: not once a second)
P0=$(panel); echo "panel refresh before: $P0 Hz" | tee "$O"
echo "time shown/s flips/s copied/s" | tee -a "$O"
s0=$(ctr debug.nvaccel_iop_ok_h4); f0=$(ctr debug.nvaccel_xeflip_flips); c0=$(ctr debug.nvaccel_xeflip_copied)
tot=0
for i in $(seq 1 "$N"); do
  sleep 1
  s1=$(ctr debug.nvaccel_iop_ok_h4); f1=$(ctr debug.nvaccel_xeflip_flips); c1=$(ctr debug.nvaccel_xeflip_copied)
  ds=$((s1 - s0)); tot=$((tot + ds))
  printf '%s %d %d %d\n' "$(date +%T)" "$ds" $((f1 - f0)) $((c1 - c0)) | tee -a "$O"
  s0=$s1; f0=$f1; c0=$c1
done
best=$(awk 'NR > 2 && $2 ~ /^[0-9]+$/ {if ($2 > m) m = $2} END {print m + 0}' "$O")
echo "panel refresh after: $(panel) Hz" | tee -a "$O"
echo "average $((tot / N)) frames/s shown, best second $best; mode $(fbprop XeGfxKmsMode || true)" | tee -a "$O"
echo "== XeGfxIntelFB's VBL log (the beat WindowServer composites on)"; log show --last 10m --style compact --predicate 'process == "kernel" AND eventMessage CONTAINS "VBL:"' 2>/dev/null | tail -5 | tee -a "$O"
echo "== WindowServer's idea of the mode"; system_profiler SPDisplaysDataType 2>/dev/null | grep -E 'Resolution|Refresh|UI Looks like|Display Type|Main Display' | tee -a "$O"
efi_copy "$O" fps.txt
