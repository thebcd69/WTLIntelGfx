#!/bin/bash
# gu605-auto.sh: run by launchd at every boot (org.thebcd69.gu605.auto). Decides from what macOS sees which G-Helper
# mode this is and does as much as gu605.conf allows, logging everything, never twice after a failure:
#   ultimate (panel on NVIDIA): nothing but a short read-only note; the iGPU driver stays off, NullMoth keeps everything
#   eco      (iGPU only):       ECO_LEVEL      (default desktop)
#   standard (iGPU + NVIDIA):   STANDARD_LEVEL (default probe: read-only)
# Levels: off | probe (stages 1-2, read-only) | gt (to 6: GuC, engines) | metal (to 7 + Metal test in a test process)
#         | desktop (metal + WindowServer on the Intel GPU, at the login window only)
# Safety: nvram gu605-pending=1 is set before the first stage that can hang and cleared when the run is confirmed; a
# boot that finds it still set (hang, panic, power button) does nothing and writes $G/autostart-off. Off switches:
# boot-arg -xegfxnoauto (this boot), -xegfxoff (kext idle), file $G/autostart-off, Safe Mode.
set -uo pipefail
. "$(dirname "$0")/gu605-common.sh"
R=$L/boot-$(date +%Y%m%d-%H%M%S); mkdir -p "$R"; S=$R/summary.txt
say() { echo "$(ts) $*" | tee -a "$S"; }
finish() { say "end: $*"; { echo; echo "stage results s0..s7: $(results)"; } >> "$S"; cp "$S" "$L/last-summary.txt"; efi_copy "$S" last-boot.txt; sync; exit 0; }
# run folders are never deleted here (a few MB each); gu605-collect.sh reports their size

say "GU605 auto, $(sw_vers -productVersion) ($(sw_vers -buildVersion)), boot $(sysctl -n kern.boottime | sed -E 's/.*\} //')"
say "boot-args: $(bootargs)"
[ -e "$G/autostart-off" ] && finish "off ($G/autostart-off exists; remove it to allow automatic runs)"
[ "$(sysctl -n kern.safeboot 2>/dev/null)" = 1 ] && finish "Safe Mode"
has_bootarg -xegfxnoauto && finish "boot-arg -xegfxnoauto"
# WindowServer is up before the panel's framebuffer gets its display (and before anything below may restart it)
for i in $(seq 1 90); do pgrep -x WindowServer > /dev/null && break; sleep 2; done
sleep 5
MODE=$(gpu_mode)
say "mode: $MODE (iGPU framebuffer $(igpu_fb_present), NVIDIA display device $(pci_display_present 10de))"
{ echo "== kexts"; kmutil showloaded --list-only 2>/dev/null | grep -E 'watchthelight|org\.local|nullmoth|thebcd69|NVPrimarySelector'
  echo "== XeGfxIntelFramebuffer"; ioreg -l -w0 -r -c XeGfxIntelFramebuffer -d 2 2>/dev/null | grep -E 'XeGfxIntelFramebuffer|IOFBDependent|NVRMTiming|display0|IOFBCurrentPixelCount|backlight' | head -20
  echo "== iGPU"; ioreg -l -w0 -r -n GFX0 -d 1 2>/dev/null | grep -E '"device-id"|"class-code"|"model"|xgfbmove|VRAM' | head
  echo "== kernel (XeGfxIntelFB, XgFbMove)"; log show --last boot --style compact --predicate 'process == "kernel" AND (eventMessage CONTAINS "XeGfxIntelFB" OR eventMessage CONTAINS "XgFbMove")' 2>/dev/null | tail -60
} > "$R/boot-state.txt" 2>&1
cat "$R/boot-state.txt" >> "$S"
case "$MODE" in
  eco) LEVEL=$(conf ECO_LEVEL desktop) ;;
  standard) LEVEL=$(conf STANDARD_LEVEL probe) ;;
  *) finish "nothing to do in $MODE mode (the iGPU driver stays off)" ;;
esac
has_bootarg -xegfxoff && finish "boot-arg -xegfxoff: XeGfx idle"
say "level: $LEVEL"
[ "$LEVEL" = off ] && finish "level off"
if [ "$(nvram gu605-pending 2>/dev/null | cut -f2)" = 1 ]; then
  nvram gu605-pending=0; touch "$G/autostart-off"
  finish "the previous automatic run was never confirmed (hang, panic or power button): automatic runs are now OFF. Run gu605-collect.sh and send the report; re-enable with: sudo rm $G/autostart-off"
fi
case "$LEVEL" in probe) T=2 ;; gt) T=6 ;; metal|desktop) T=7 ;; *) finish "unknown level $LEVEL" ;; esac
# stages 1-2 are read-only; from 3 on a hang is possible, so the pending mark goes first
bash "$G/gu605-stages.sh" 2 "$R/stages" || finish "stages 1-2 failed (see stages/)"
[ $T -le 2 ] && finish "probe done"
nvram gu605-pending=1; sync
if ! bash "$G/gu605-stages.sh" "$T" "$R/stages"; then nvram gu605-pending=0; finish "stages to $T failed (see stages/)"; fi
say "stages to $T passed"
if [ "$LEVEL" = gt ]; then nvram gu605-pending=0; finish "gt done"; fi
if [ ! -f "$XU/libvulkan_intel.dylib" ]; then nvram gu605-pending=0; finish "no Mesa ANV installed ($XU): Metal not tried"; fi
bash "$G/gu605-metal-check.sh" "$R/metal"; mc=$?
nvram gu605-pending=0
[ $mc = 0 ] || finish "Metal check failed (see metal/)"
[ "$LEVEL" = metal ] && finish "metal done"
# desktop: only at the login window (it restarts WindowServer)
cu=$(stat -f %Su /dev/console 2>/dev/null)
case "$cu" in ""|root|_*) ;; *) finish "$cu logged in before the desktop step: skipped this boot (log out, or run sudo $G/gu605-desktop.sh with FORCE=1)";; esac
nvram gu605-pending=1; sync
if ! bash "$G/gu605-desktop.sh" "$R/desktop"; then nvram gu605-pending=0; finish "desktop step stopped before the restart (see desktop/)"; fi
say "WindowServer on the Intel GPU; confirming in 3 minutes"
sleep 180
frames=$(sysctl -n debug.nvaccel_iop_ok_h4 2>/dev/null); frames=${frames:-0}
starts=$(grep -c 'WindowServer start #' "$R/desktop/guard.log" 2>/dev/null); starts=${starts:-0}
if [ "$frames" -gt 0 ] && [ "$starts" -le 1 ]; then
  nvram gu605-pending=0
  say "confirmed: $frames frames on the panel, $starts WindowServer restart(s)"
  # zero-copy flip (samwu07: 602 flips, 0 copies), only on a confirmed desktop, off again on any trouble in a minute
  if [ "$(conf FLIP 1)" = 1 ] && [ ! -e "$G/xeflip-off" ] && ! has_bootarg -xegfxnoflip && sysctl -n debug.nvaccel_xeflip_ggtt > /dev/null 2>&1; then
    ws=$(pgrep -x WindowServer | head -1); f0=$(sysctl -n debug.nvaccel_iop_fail 2>/dev/null)
    sysctl -w debug.nvaccel_xeflip=1 > /dev/null 2>&1; say "flip: $(sysctl -n debug.nvaccel_xeflip_ggtt 2>/dev/null | head -1)"
    sleep 60
    ws2=$(pgrep -x WindowServer | head -1); f1=$(sysctl -n debug.nvaccel_iop_fail 2>/dev/null)
    if [ "$ws2" != "$ws" ] || [ "${f1:-0}" != "${f0:-0}" ]; then
      sysctl -w debug.nvaccel_xeflip=0 > /dev/null 2>&1; touch "$G/xeflip-off"; say "flip OFF for later boots (WindowServer $ws -> $ws2, failures $f0 -> $f1)"
    else say "flip confirmed: $(sysctl -n debug.nvaccel_xeflip_flips 2>/dev/null) flipped, $(sysctl -n debug.nvaccel_xeflip_copied 2>/dev/null) copied"; fi
  fi
  sysctl debug.nvaccel_iop_ok_h4 debug.nvaccel_iop_fail debug.nvaccel_pipe_perform 2>/dev/null >> "$S"
  finish "desktop on the Intel GPU"
fi
nvram gu605-pending=0; touch "$G/autostart-off"
finish "desktop NOT confirmed ($frames frames, $starts WindowServer starts): automatic runs OFF; run gu605-collect.sh"
