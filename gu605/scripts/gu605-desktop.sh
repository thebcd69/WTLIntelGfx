#!/bin/bash
# gu605-desktop.sh REPORTDIR: this boot only, hand WindowServer to the Intel GPU (Eco mode). Every switch is a runtime
# write a restart undoes. Adapted from samwu07's desktop-on.sh (ThinkBook 14+, same iGPU, same macOS):
#   1 stages to 7 (XeGfxAccel attached)  2 AGDC up + K5 link answers  3 register the panel's framebuffer (nvaccelfb=1)
#   4 scan-out pipe (nvaccelfb=3)  4a WindowServer completion marks  4b frame transactions (nvaccel_iop=1)
#   5 crash-loop guard, then WindowServer restarts (only at the login window unless FORCE=1)
# Stops BEFORE the restart at the first failed check: nothing visible changes then.
set -uo pipefail
. "$(dirname "$0")/gu605-common.sh"
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 2; }
R=${1:-$L/desktop-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$R"
say() { echo "$(ts) desktop: $*" | tee -a "$R/desktop.log"; }
die() { say "STOP: $*  (nothing was restarted)"; sync; exit 1; }
klog() { log show --last "${1:-2m}" --style compact --predicate 'process == "kernel" AND (eventMessage CONTAINS[c] "nvaccel" OR eventMessage CONTAINS[c] "agdc" OR eventMessage CONTAINS[c] "nmx" OR eventMessage CONTAINS[c] "XeGfx" OR eventMessage CONTAINS[c] "framebuffer")' 2>/dev/null; }

# NullMoth's NVAccel registers the same debug.nvaccel* names as XeGfxAccel: only one of them may run (Eco mode)
loaded 'com\.nullmoth\.NVAccel' && die "NullMoth's NVAccel is loaded (Standard/Ultimate mode): the Intel desktop is Eco-only on this branch"
[ -d "$XB" ] || die "$XB missing (gu605-install.sh)"
[ -f "$XU/libvulkan_intel.dylib" ] || die "Mesa ANV missing ($XU/libvulkan_intel.dylib): userland not installed"
[ -f "$XB/Contents/MacOS/air-runtime/libLLVM.dylib" ] || [ -x "$XU/air-opt" ] || die "no AIR bitcode reader in $XB (WindowServer would crash on CoreDisplay's shaders)"
[ "$(conf DESKTOP_GPUPASS_OK 0)" = 1 ] || [ -f "$G/gpupass-ok" ] || die "CoreDisplay's shaders have not passed the GPU-pass probe yet (gu605-metal-check.sh)"

# allow-list: WindowServer first, armed-only; "!App" lines the owner added are kept
if [ "$(grep -v '^[[:space:]]*#' "$AL" 2>/dev/null | grep -v '^[[:space:]]*$' | head -1 | tr -d ' \t')" != '!WindowServer' ]; then
  printf '# Intel Metal plugin (XeMTLDriver): processes that get the Intel Metal device.\n# "!name" = only once the accelerator is armed.\n!WindowServer\n' > "$AL"; chmod 644 "$AL"
fi
say "allow-list: $(grep -v '^#' "$AL" | tr '\n' ' ')"

say "1. stages to 7"
bash "$G/gu605-stages.sh" 7 "$R/stages" || die "the stages did not reach 7"
[ "$(sysctl -n debug.nmxaccel_headless 2>/dev/null)" = 0 ] || say "note: debug.nmxaccel_headless is $(sysctl -n debug.nmxaccel_headless 2>/dev/null) (0 = the Intel GPU owns the panel)"
# the panel reaches XeGfxIntelFB's framebuffer (display0) once WindowServer has opened it
for i in $(seq 1 60); do [ "$(ioreg -r -c XeGfxIntelFramebuffer -d 2 | grep -c display0)" -gt 0 ] && break; sleep 2; done
[ "$(ioreg -r -c XeGfxIntelFramebuffer -d 2 | grep -c display0)" -gt 0 ] || die "the panel is not on XeGfxIntelFB's framebuffer (nvram xeintelfb-starts = $(nvram xeintelfb-starts 2>/dev/null | cut -f2))"
nvram xeintelfb-starts=%00 2>/dev/null

say "2. AGDC"
sysctl -w debug.xegfx_agdc=1 > /dev/null || die "debug.xegfx_agdc=1 failed"
for i in $(seq 1 10); do [ "$(sysctl -n debug.xegfx_agdc)" = 1 ] && break; sleep 0.5; done
[ "$(sysctl -n debug.xegfx_agdc)" = 1 ] || { klog 2m > "$R/kernel-agdc.txt"; die "AGDC did not come up ($(sysctl -n debug.xegfx_agdc))"; }
[ "$(conf K5 1)" = 1 ] && { sysctl -w debug.xegfx_agdc_k5=1 > /dev/null || die "debug.xegfx_agdc_k5=1 failed"; }

say "3. register the panel's framebuffer"
sysctl -w debug.nvaccelfb=1 > /dev/null || die "debug.nvaccelfb=1 failed"
sleep 1; klog 1m > "$R/kernel-after-fb1.txt"
say "4. scan-out pipe"
sysctl -w debug.nvaccelfb=3 > /dev/null || die "debug.nvaccelfb=3 failed"
sleep 1; klog 1m > "$R/kernel-after-fb3.txt"
[ "$(sysctl -n debug.nvaccelfb)" = 1 ] || die "the accelerator is not armed (debug.nvaccelfb = $(sysctl -n debug.nvaccelfb))"
if [ "$(conf WSMARK 1)" = 1 ]; then sysctl -w debug.nvaccel_surface_gpu_on=1 > /dev/null 2>&1 || say "no WindowServer marks in this XeGfxAccel"
else sysctl -w debug.nvaccel_surface_gpu_on=0 > /dev/null 2>&1; fi
sysctl -w debug.nvaccel_iop=1 > /dev/null || die "debug.nvaccel_iop=1 failed"
[ "$(sysctl -n debug.nvaccel_iop)" = 1 ] || die "debug.nvaccel_iop did not stay 1"
{ sysctl debug.nvaccelfb debug.nvaccel_iop debug.xegfx_agdc debug.xegfx_agdc_k5 debug.nmxaccel_headless debug.xegfx_stage 2>&1
  ioreg -l -w0 -r -c NMXAccel; ioreg -l -w0 -r -c XeGfxIntelFramebuffer; } > "$R/state-before-restart.txt" 2>&1
klog 10m > "$R/kernel-10m.txt"; sync

cu=$(stat -f %Su /dev/console 2>/dev/null)
case "$cu" in ""|root|_*) ;; *) [ "${FORCE:-0}" = 1 ] || die "$cu is logged in: WindowServer is not restarted under a session (FORCE=1 to do it)";; esac

# crash-loop guard, detached: >2 WindowServer starts in 10 minutes turn the Intel device off for WindowServer
cat > "$R/guard.sh" <<'EOF'
#!/bin/bash
AL=$1; G=$2; C=$(dirname "$G")/counters.log; last=$(pgrep -x WindowServer | head -1); starts=0
echo "$(date +%T) guard up, WindowServer pid $last" >> "$G"
echo "time validate perform complete iop_ok iop_ok_h4 iop_fail iop_empty k5_link k5_caps" > "$C"
for i in $(seq 1 600); do
  [ $((i % 5)) = 1 ] && { printf '%s ' "$(date +%T)"; sysctl -n debug.nvaccel_pipe_validate debug.nvaccel_pipe_perform debug.nvaccel_pipe_complete \
    debug.nvaccel_iop_ok debug.nvaccel_iop_ok_h4 debug.nvaccel_iop_fail debug.nvaccel_iop_empty debug.xegfx_agdc_k5_link debug.xegfx_agdc_k5_caps 2>/dev/null | tr '\n' ' '; echo; } >> "$C"
  sleep 1; now=$(pgrep -x WindowServer | head -1)
  if [ -n "$now" ] && [ "$now" != "$last" ]; then starts=$((starts + 1)); echo "$(date +%T) WindowServer start #$starts (pid $now)" >> "$G"; last=$now; fi
  if [ $starts -gt 2 ]; then
    printf '# disarmed by the crash-loop guard %s\n-WindowServer\n' "$(date)" > "$AL"
    echo "$(date +%T) CRASH LOOP: allow-list set to -WindowServer, the next WindowServer runs without the Intel GPU" >> "$G"; sync; exit 0
  fi
done
echo "$(date +%T) guard done: $starts WindowServer start(s) in 10 minutes" >> "$G"; sync
EOF
nohup bash "$R/guard.sh" "$AL" "$R/guard.log" > /dev/null 2>&1 < /dev/null &
disown
say "5. WindowServer restart (the screen goes dark once, then the login window)"
sync; sleep 3
old=$(pgrep -x WindowServer | head -1)
killall WindowServer
sleep 5
[ "$(pgrep -x WindowServer | head -1)" != "$old" ] || launchctl kickstart -k system/com.apple.WindowServer
say "WindowServer restarted (pid $(pgrep -x WindowServer | head -1))"
exit 0
