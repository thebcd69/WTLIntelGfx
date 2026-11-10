#!/bin/bash
# gu605-auto.sh: run by launchd at every boot (org.thebcd69.gu605.auto). Decides from what macOS sees which G-Helper
# mode this is and does as much as gu605.conf allows, logging everything, never twice after a failure:
#   ultimate (panel on NVIDIA): nothing but a short read-only note; the iGPU driver stays off, NullMoth keeps everything
#   eco      (iGPU only):       ECO_LEVEL      (default metal)
#   standard (iGPU + NVIDIA):   STANDARD_LEVEL (default probe: read-only)
# Levels: off | probe (stages 1-2, read-only) | gt (to 6: GuC, engines) | metal (to 7 + Metal test in a test process)
#         | desktop (metal + WindowServer on the Intel GPU, at the login window only)
# Safety: the pending mark (nvram gu605-pending=1 and the file $G/pending-run) is set before the first stage that can hang and cleared when the run is confirmed; a
# boot that finds it still set (hang, panic, power button) does nothing and writes $G/autostart-off. Off switches:
# boot-arg -xegfxnoauto (this boot), -xegfxoff (kext idle), file $G/autostart-off, Safe Mode.
set -uo pipefail
. "$(dirname "$0")/gu605-common.sh"
R=$L/boot-$(date +%Y%m%d-%H%M%S); mkdir -p "$R"; S=$R/summary.txt
# [gu605-mac] the pending mark is kept in a file as well: on this machine an nvram write is not on the flash before a
# panic or the power button (2026-11-09 17:06: panic in stage 4 with gu605-pending=1, next boot read 0 and ran again)
PF=$G/pending-run
# sync(2) only schedules the writes on macOS: F_FULLFSYNC (51) on the file and its folder puts them on the flash
fullsync() { /usr/bin/python3 -c 'import os,fcntl,sys
for p in sys.argv[1:]:
    fd=os.open(p,os.O_RDONLY); fcntl.fcntl(fd,51); os.close(fd)' "$@" 2>/dev/null || sync; }
pend() { nvram gu605-pending="$1" 2>/dev/null; printf '%s %s\n' "$1" "$R" > "$PF.tmp" && mv -f "$PF.tmp" "$PF"; fullsync "$PF" "$G"; }
pend_is_set() { [ "$(nvram gu605-pending 2>/dev/null | cut -f2)" = 1 ] || [ "$(cut -d' ' -f1 "$PF" 2>/dev/null)" = 1 ]; }
say() { echo "$(ts) $*" | tee -a "$S"; }
# [gu605-mac] with i915 owning the display (DISPLAY=kms) the machine froze 1-3 min AFTER a finished run (2026-11-10
# 12:35, 12:38: 'metal done', then a frozen panel at full brightness): the mark stays set for 5 minutes after the run
kms_hold() {
  grep -q 'display: i915 owns the display' "$R/stages/stages.log" 2>/dev/null || return 0
  pend 1; say "kms: the pending mark stays set for 5 minutes (a freeze in that time turns automatic runs off)"
  # in the foreground: launchd ends the job's whole process group when this script exits (no AbandonProcessGroup)
  sleep 300; pend 0; say "kms: 5 minutes up with i915 owning the display, pending mark cleared"
}
finish() { kms_hold; say "end: $*"; { echo; echo "stage results s0..s7: $(results)"; } >> "$S"; cp "$S" "$L/last-summary.txt"; efi_copy "$S" last-boot.txt; sync; exit 0; }
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
say "mode: $MODE (XeGfxIntelFramebuffer $(igpu_fb_present), iGPU drives the panel $(igpu_drives_panel), NVRM live $(nvidia_live), NVIDIA PCI node $(pci_display_present 10de))"
say "XeGfxIntelFB start: $(ioreg -r -c XeGfxIntelFramebuffer -d 1 -l -w0 2>/dev/null | awk -F'= ' '/"XeGfxIntelFB-Start"/ {print $2; exit}') | $(log show --last boot --style compact --predicate 'process == "kernel" AND eventMessage CONTAINS "IONDRVFramebuffer start"' 2>/dev/null | grep -o 'IONDRVFramebuffer start.*' | tail -1)"
{ echo "== kexts"; kmutil showloaded --list-only 2>/dev/null | grep -E 'watchthelight|org\.local|nullmoth|thebcd69|NVPrimarySelector'
  echo "== XeGfxIntelFramebuffer"; ioreg -l -w0 -r -c XeGfxIntelFramebuffer -d 2 2>/dev/null | grep -E 'XeGfxIntelFramebuffer|IOFBDependent|NVRMTiming|display0|IOFBCurrentPixelCount|backlight' | head -20
  # WhateverGreen renames GFX0 to IGPU once disable-gpu is gone: look for both
  echo "== iGPU"; for n in GFX0 IGPU; do ioreg -l -w0 -r -n "$n" -d 1 2>/dev/null | grep -E '\+-o|"device-id"|"class-code"|"model"|xgfbmove|VRAM|"IOName"' | head -12; done
  echo "== kernel (XeGfxIntelFB, XgFbMove)"; log show --last boot --style compact --predicate 'process == "kernel" AND (eventMessage CONTAINS "XeGfxIntelFB" OR eventMessage CONTAINS "XgFbMove")' 2>/dev/null | tail -60
} > "$R/boot-state.txt" 2>&1
cat "$R/boot-state.txt" >> "$S"
# the iGPU drives the panel but through Apple's IONDRVFramebuffer: XeGfxIntelFB did not start, nothing below can work
if [ "$MODE" = eco ] || [ "$MODE" = standard ]; then
  [ "$(igpu_fb_present)" = 1 ] || finish "$MODE, but XeGfxIntelFB did not start (Apple's IONDRVFramebuffer/.Display_boot drives the panel): see the start line above"
fi
case "$MODE" in
  eco) LEVEL=$(conf ECO_LEVEL metal) ;;
  standard) LEVEL=$(conf STANDARD_LEVEL probe) ;;
  *) finish "nothing to do in $MODE mode (the iGPU driver stays off)" ;;
esac
# DDC/CI on the external monitor (XeGfxIntelFB, gu605-ddc): gu605.conf DDC=0 switches it off at once, every boot
# (the kext's XeGfxDdcEnable property; no boot-arg needed). A kext without DDC answers "unsupported": nothing happens.
if [ -x "$G/tools/xeddcprobe" ]; then say "ddc: $("$G/tools/xeddcprobe" enable "$(conf DDC 1)" 2>&1 | tail -1)"; fi
has_bootarg -xegfxoff && finish "boot-arg -xegfxoff: XeGfx idle"
say "level: $LEVEL"
[ "$LEVEL" = off ] && finish "level off"
if pend_is_set; then
  prev=$(cut -d' ' -f2- "$PF" 2>/dev/null); pend 0; touch "$G/autostart-off"; say "unconfirmed run: ${prev:-?}"
  finish "the previous automatic run was never confirmed (hang, panic or power button): automatic runs are now OFF. Run gu605-collect.sh and send the report; re-enable with: sudo rm $G/autostart-off"
fi
case "$LEVEL" in probe) T=2 ;; gt) T=6 ;; metal|desktop) T=7 ;; *) finish "unknown level $LEVEL" ;; esac
# stages 1-2 are read-only; from 3 on a hang is possible, so the pending mark goes first
bash "$G/gu605-stages.sh" 2 "$R/stages" || finish "stages 1-2 failed (see stages/)"
{ echo "== display as the firmware left it (stage 2)"; cat "$R/stages/display-decoded.txt" 2>/dev/null; } >> "$S"
[ $T -le 2 ] && finish "probe done"
pend 1
if ! bash "$G/gu605-stages.sh" "$T" "$R/stages"; then pend 0; finish "stages to $T failed (see stages/)"; fi
say "stages to $T passed"
if [ "$LEVEL" = gt ]; then pend 0; finish "gt done"; fi
if [ ! -f "$XU/libvulkan_intel.dylib" ]; then pend 0; finish "no Mesa ANV installed ($XU): Metal not tried"; fi
bash "$G/gu605-metal-check.sh" "$R/metal"; mc=$?
pend 0
[ $mc = 0 ] || finish "Metal check failed (see metal/)"
[ "$LEVEL" = metal ] && finish "metal done"
# desktop: gu605-desktop.sh decides when WindowServer restarts (login window, automatic login left idle, or the next
# logout: exit 10, armed and waiting)
pend 1
bash "$G/gu605-desktop.sh" "$R/desktop"; dr=$?
if [ $dr = 10 ]; then
  # a shutdown while waiting is not a failed run: the pending mark is set again only when WindowServer restarts
  pend 0
  wmin=$(conf DESKTOP_WAIT_MIN 120); old=$(pgrep -x WindowServer | head -1)
  say "desktop armed; waiting up to $wmin min for the next logout (WindowServer $old)"
  # [gu605-mac] macOS 15 keeps WindowServer across a logout (2026-11-05 13:05: logged out, WindowServer still 157), so a
  # logout alone never brought the Intel desktop. When the console goes back to the login window, restart it here.
  for i in $(seq 1 $((wmin * 6))); do now=$(pgrep -x WindowServer | head -1); [ -n "$now" ] && [ "$now" != "$old" ] && break
    cu=$(stat -f %Su /dev/console 2>/dev/null)
    case "$cu" in root|loginwindow|"") say "logged out (console $cu): restarting WindowServer"; killall WindowServer 2>/dev/null; sleep 3
      [ "$(pgrep -x WindowServer | head -1)" != "$old" ] || launchctl kickstart -k system/com.apple.WindowServer; sleep 7 ;; esac
    sleep 10; done
  now=$(pgrep -x WindowServer | head -1)
  [ -n "$now" ] && [ "$now" != "$old" ] || finish "armed, no logout within $wmin min: the desktop stayed on the firmware framebuffer this boot"
  pend 1
  nohup bash "$R/desktop/guard.sh" "$AL" "$R/desktop/guard.log" > /dev/null 2>&1 < /dev/null & disown
  echo "$(date +%T) WindowServer start #1 (pid $now, after logout)" >> "$R/desktop/guard.log"
  say "logout seen: WindowServer $old -> $now"
elif [ $dr != 0 ]; then pend 0; finish "desktop step stopped before the restart (see desktop/)"; fi
say "WindowServer on the Intel GPU; confirming in 3 minutes"
sleep 180
frames=$(sysctl -n debug.nvaccel_iop_ok_h4 2>/dev/null); frames=${frames:-0}
starts=$(grep -c 'WindowServer start #' "$R/desktop/guard.log" 2>/dev/null); starts=${starts:-0}
if [ "$frames" -gt 0 ] && [ "$starts" -le 1 ]; then
  pend 0
  say "confirmed: $frames frames on the panel, $starts WindowServer restart(s)"
  # zero-copy flip (samwu07: 602 flips, 0 copies), only on a confirmed desktop, off again on any trouble in a minute
  if [ "$(conf FLIP 1)" = 1 ] && [ ! -e "$G/xeflip-off" ] && ! has_bootarg -xegfxnoflip && sysctl -n debug.nvaccel_xeflip_ggtt > /dev/null 2>&1; then
    ws=$(pgrep -x WindowServer | head -1); f0=$(sysctl -n debug.nvaccel_iop_fail 2>/dev/null)
    sysctl -w debug.nvaccel_xeflip=1 > /dev/null 2>&1; say "flip: $(sysctl -n debug.nvaccel_xeflip_ggtt 2>/dev/null | head -1)"
    sleep 60
    ws2=$(pgrep -x WindowServer | head -1); f1=$(sysctl -n debug.nvaccel_iop_fail 2>/dev/null)
    if [ "$ws2" != "$ws" ] || [ "${f1:-0}" != "${f0:-0}" ]; then
      sysctl -w debug.nvaccel_xeflip=0 > /dev/null 2>&1; touch "$G/xeflip-off"; say "flip OFF for later boots (WindowServer $ws -> $ws2, failures $f0 -> $f1)"
    else
      say "flip confirmed: $(sysctl -n debug.nvaccel_xeflip_flips 2>/dev/null) flipped, $(sysctl -n debug.nvaccel_xeflip_copied 2>/dev/null) copied"
      # asynchronous flip (FLIP_ASYNC=1, gu605.conf): WindowServer's thread no longer waits for the GPU or the latch; the
      # same one-minute guard, and the file mark xeflip-async-off keeps it off for later boots
      if [ "$(conf FLIP_ASYNC 0)" = 1 ] && [ ! -e "$G/xeflip-async-off" ] && sysctl -n debug.nvaccel_xeflip_async > /dev/null 2>&1; then
        a0=$(sysctl -n debug.nvaccel_xeflip_async_flips 2>/dev/null); f0=$(sysctl -n debug.nvaccel_iop_fail 2>/dev/null)
        sysctl -w debug.nvaccel_xeflip_async=1 > /dev/null 2>&1; say "flip: asynchronous ON"
        sleep 60
        ws3=$(pgrep -x WindowServer | head -1); f1=$(sysctl -n debug.nvaccel_iop_fail 2>/dev/null); a1=$(sysctl -n debug.nvaccel_xeflip_async_flips 2>/dev/null)
        on=$(sysctl -n debug.nvaccel_xeflip_async 2>/dev/null)
        if [ "$ws3" != "$ws" ] || [ "${f1:-0}" != "${f0:-0}" ] || [ "${on:-0}" != 1 ]; then
          sysctl -w debug.nvaccel_xeflip_async=0 > /dev/null 2>&1; touch "$G/xeflip-async-off"
          say "asynchronous flip OFF for later boots (WindowServer $ws -> $ws3, failures $f0 -> $f1, still on: ${on:-?})"
        else say "asynchronous flip confirmed: ${a0:-0} -> ${a1:-0} flips, $(sysctl -n debug.nvaccel_xeflip_async_drops 2>/dev/null) dropped, WindowServer held $(sysctl -n debug.nvaccel_xeflip_txn_us_max 2>/dev/null) us at most"; fi
      fi
    fi
  fi
  sysctl debug.nvaccel_iop_ok_h4 debug.nvaccel_iop_fail debug.nvaccel_pipe_perform 2>/dev/null >> "$S"
  finish "desktop on the Intel GPU"
fi
pend 0; touch "$G/autostart-off"
finish "desktop NOT confirmed ($frames frames, $starts WindowServer starts): automatic runs OFF; run gu605-collect.sh"
