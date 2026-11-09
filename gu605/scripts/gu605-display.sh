#!/bin/bash
# gu605-display.sh: the display path (DISPLAY=kms) by hand. Needs stage 4 passed with i915 owning the display.
#   sudo bash /Library/XeGfx/gu605/gu605-display.sh dump               every connector, mode, link, DSC, backlight
#   sudo bash /Library/XeGfx/gu605/gu605-display.sh internal [HZ]      the panel alone (default: PANEL_HZ, else 240)
#   sudo bash /Library/XeGfx/gu605/gu605-display.sh external [WxH[@HZ]] the left USB-C monitor alone (default: fastest
#                                                                    mode at its preferred raster); the panel goes off
#   sudo bash /Library/XeGfx/gu605/gu605-display.sh bl PERMILLE         panel brightness 20..1000
# Every change is checked by i915 first (test), then committed; a refused one leaves the screen as it was. Output goes
# to the terminal and to $L/display-<time>.txt.
set -uo pipefail
. "$(dirname "$0")/gu605-common.sh"
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 2; }
mkdir -p "$L"; O=$L/display-$(date +%Y%m%d-%H%M%S).txt
say() { echo "$*" | tee -a "$O"; }
dump() { sysctl -n debug.xegfx_kms > "$O.dump" 2>&1; cat "$O.dump" >> "$O"; }
setm() {
  sysctl -w debug.xegfx_kms_set="$1 test" > /dev/null 2>&1; say "test:   $(sysctl -n debug.xegfx_kms_set)"
  if sysctl -w debug.xegfx_kms_set="$1" > /dev/null 2>&1; then say "commit: $(sysctl -n debug.xegfx_kms_set)"
  else say "REFUSED: $(sysctl -n debug.xegfx_kms_set)"; return 1; fi
  sleep 1; dump; grep -E '^crtc|active crtc' "$O.dump" | tee -a "$O"
}
case "${1:-dump}" in
  dump) dump; cat "$O.dump" ;;
  internal)
    dump; c=$(sed -nE 's/^connector (eDP-[0-9]+) .*/\1/p' "$O.dump" | head -1)
    [ -n "$c" ] || { say "no eDP connector: $(head -1 "$O.dump")"; exit 1; }
    hz=${2:-$(conf PANEL_HZ 240)}; setm "$c ${hz:-240}" ;;
  external)
    dump; c=$(sed -nE 's/^connector (DP-[0-9]+) .*status connected.*/\1/p' "$O.dump" | head -1)
    [ -n "$c" ] || { say "no DisplayPort monitor connected (left USB-C)"; grep -E '^connector|  encoder' "$O.dump" | tee -a "$O"; exit 1; }
    m=${2:-}; [ -n "$m" ] || m=$(grep -A200 "^connector $c " "$O.dump" | sed -nE 's/^  mode [0-9]+ ([0-9]+x[0-9]+) .* preferred$/\1/p' | head -1)
    setm "$c $m" ;;
  bl) [ -n "${2:-}" ] || { echo "bl PERMILLE (20..1000)"; exit 2; }
    sysctl -w debug.xegfx_kms_bl="$2" > /dev/null 2>&1; say "$(sysctl -n debug.xegfx_kms_set)" ;;
  *) sed -n 2,8p "$0"; exit 2 ;;
esac
