#!/bin/bash
# gu605-desktop.sh REPORTDIR: this boot only, hand WindowServer to the Intel GPU (Eco mode). Every switch is a runtime
# write a restart undoes. Adapted from samwu07's desktop-on.sh (ThinkBook 14+, same iGPU, same macOS):
#   1 stages to 7 (XeGfxAccel attached)  2 AGDC up + K5 link answers  3 register the panel's framebuffer (nvaccelfb=1)
#   4 scan-out pipe (nvaccelfb=3)  4a WindowServer completion marks  4b frame transactions (nvaccel_iop=1)
#   5 crash-loop guard, then WindowServer restarts (only at the login window unless FORCE=1)
# Stops BEFORE the restart at the first failed check: nothing visible changes then.
# [gu605-mac] EARLY=1 (gu605-auto.sh, gu605.conf EARLY_ARM=1): steps 2-4b BEFORE WindowServer's first display start,
# so there is nothing to restart (exit 20). See "early" below for when that is possible.
set -uo pipefail
. "$(dirname "$0")/gu605-common.sh"
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 2; }
R=${1:-$L/desktop-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$R"
say() { echo "$(ts) desktop: $*" | tee -a "$R/desktop.log"; }
die() { say "STOP: $*  (nothing was restarted)"; sync; exit 1; }
# [gu605-mac] diagnostics only, in the background: each log show took 20-50 s here and the three of them delayed the
# restart by ~2.5 minutes (2026-11-10 16:00-16:02). A failed check still reads the log in the foreground (die path).
klog_bg() { local m=$1 f=$2; ( klog "$m" > "$f" 2>&1 ) < /dev/null & }
klog() { log show --last "${1:-2m}" --style compact --predicate 'process == "kernel" AND (eventMessage CONTAINS[c] "nvaccel" OR eventMessage CONTAINS[c] "agdc" OR eventMessage CONTAINS[c] "nmx" OR eventMessage CONTAINS[c] "XeGfx" OR eventMessage CONTAINS[c] "framebuffer")' 2>/dev/null; }

# NullMoth's NVAccel registers the same debug.nvaccel* names as XeGfxAccel: only one of them may run (Eco mode)
# [gu605-mac] an NVAccel INSTANCE, not the kext being loaded: after login the kext was loaded with no instance (GFX0's
# node, ids cached, matched its personality; probe declined) and NullMoth registers debug.nvaccel* only in start()
[ "$(ioreg -r -c NVAccel -d 1 2>/dev/null | grep -c '<class NVAccel')" -gt 0 ] && die "NullMoth's NVAccel is running (Standard/Ultimate mode): the Intel desktop is Eco-only on this branch"
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
has_d0() { [ "$(ioreg -r -c XeGfxIntelFramebuffer -d 2 2>/dev/null | grep -c display0)" -gt 0 ]; }
etime_s() { awk -F'[-:]' '{ n = NF; s = $n + 60 * $(n-1); if (n >= 3) s += 3600 * $(n-2); if (n >= 4) s += 86400 * $(n-3); print s }' <<< "$(ps -o etime= -p "$1" 2>/dev/null | tr -d ' ')"; }
# [gu605-mac] early (2026-11-13). What the first WindowServer of a boot does in Eco, from today's 13:58:52 boot:
#   13:59:18 it starts; 13:59:32 "Setting offline display ... CGXConstructVirtualFramebuffer" and it waits for the GPU;
#   14:00:32 "GPUWranglerWaitForPublish: timed out" (60 s), only THEN "Creating FB 1 of 1": it opens XeGfxIntelFB's
#   framebuffer (display0 appears) and loads XeMTLDriver, which said "'WindowServer' is armed-only and debug.nvaccelfb
#   reads 0 -- no device"; the accelerator's pipe got "scanout gate CLOSED".
# The stages had been at 7 since 13:59:46. The only thing that kept the arming until after that open was the wait for
# display0 just below, which cannot come before the open. With one framebuffer (XeGfxIntelFB's, on the iGPU itself) the
# accelerator's head 4 can be no other, so in that case the arming is done at once: WindowServer's first look at the
# GPU then finds it armed and the gate open, and nothing has to be restarted. AGDC comes last here: if its
# registration is what WindowServer waits for, everything else is in place when the wait ends.
# Anything unexpected -> the usual path below (the same writes again are harmless), and early-failed tells the runner.
EARLY=${EARLY:-0}
if [ "$EARLY" = 1 ]; then
  ws=$(pgrep -x WindowServer | head -1); wse=$([ -n "$ws" ] && etime_s "$ws" || echo 9999)
  nfb=$(ioreg -r -c IOFramebuffer -d 1 2>/dev/null | grep -c '^+-o ')
  for i in $(seq 1 20); do hp=$(sysctl -n debug.nvaccel_heads_published 2>/dev/null); [ $(( ${hp:-0} & 16 )) != 0 ] && break; sleep 0.5; done
  cu=$(stat -f %Su /dev/console 2>/dev/null)
  if has_d0; then say "early: WindowServer has already opened the framebuffer (display0 is there): the usual path"; EARLY=0
  elif [ "$nfb" != 1 ]; then say "early: $nfb framebuffers, not 1: the usual path"; EARLY=0
  elif [ $(( ${hp:-0} & 16 )) = 0 ]; then say "early: the accelerator has no head 4 yet (published 0x$(printf %x "${hp:-0}")): the usual path"; EARLY=0
  elif [ "$wse" -gt "$(conf EARLY_ARM_MAX_WS_S 55)" ]; then say "early: WindowServer $ws is ${wse}s old (its wait for the GPU ends about 74 s after its start): too late, the usual path"; EARLY=0
  else case "$cu" in ""|root|_*) ;; *) say "early: $cu is logged in: the usual path"; EARLY=0 ;; esac; fi
fi
if [ "$EARLY" = 1 ]; then
  say "early: arming before WindowServer's first display start (WindowServer $ws is ${wse}s old, no display0, heads published 0x$(printf %x "$hp"))"
  why=""
  if [ "$(conf K5 1)" = 1 ]; then sysctl -w debug.xegfx_agdc_k5=1 > /dev/null || why="debug.xegfx_agdc_k5=1 failed"; fi
  [ -n "$why" ] || sysctl -w debug.nvaccelfb=1 > /dev/null || why="debug.nvaccelfb=1 failed"
  [ -n "$why" ] || { sleep 1; sysctl -w debug.nvaccelfb=3 > /dev/null || why="debug.nvaccelfb=3 failed"; }
  [ -n "$why" ] || { sleep 1; [ "$(sysctl -n debug.nvaccelfb)" = 1 ] || why="not armed (debug.nvaccelfb = $(sysctl -n debug.nvaccelfb), heads registered 0x$(printf %x "$(sysctl -n debug.nvaccel_heads_registered 2>/dev/null)"))"; }
  if [ -z "$why" ]; then
    if [ "$(conf WSMARK 1)" = 1 ]; then sysctl -w debug.nvaccel_surface_gpu_on=1 > /dev/null 2>&1 || say "no WindowServer marks in this XeGfxAccel"
    else sysctl -w debug.nvaccel_surface_gpu_on=0 > /dev/null 2>&1; fi
    sysctl -w debug.nvaccel_iop=1 > /dev/null && [ "$(sysctl -n debug.nvaccel_iop)" = 1 ] || why="debug.nvaccel_iop=1 did not stay"
  fi
  # WindowServer opened the framebuffer while this ran: what it found is not known
  late=0; [ -n "$why" ] || { has_d0 && { why="WindowServer opened the framebuffer during the arming"; late=1; }; }
  if [ -z "$why" ]; then
    sysctl -w debug.xegfx_agdc=1 > /dev/null || why="debug.xegfx_agdc=1 failed"
    [ -n "$why" ] || { for i in $(seq 1 10); do [ "$(sysctl -n debug.xegfx_agdc)" = 1 ] && break; sleep 0.5; done
                       [ "$(sysctl -n debug.xegfx_agdc)" = 1 ] || why="AGDC did not come up ($(sysctl -n debug.xegfx_agdc))"; }
  fi
  if [ -z "$why" ]; then
    { sysctl debug.nvaccelfb debug.nvaccel_iop debug.xegfx_agdc debug.xegfx_agdc_k5 debug.nmxaccel_headless debug.xegfx_stage debug.nvaccel_heads_published debug.nvaccel_heads_registered 2>&1
      ioreg -l -w0 -r -c NMXAccel; ioreg -l -w0 -r -c XeGfxIntelFramebuffer; } > "$R/state-early.txt" 2>&1
    klog_bg 3m "$R/kernel-early.txt"
    echo early > "$R/restart-mode"
    say "armed early; nothing restarted: WindowServer $ws finds the Intel GPU when its wait ends"
    sync; exit 20
  fi
  say "early: $why: the usual path from here"
  # a timing miss is not a failure of the method: only a real failure turns early arming off for later boots
  if [ $late = 1 ]; then echo "$why" > "$R/early-late"; else echo "$why" > "$R/early-failed"; fi
  klog_bg 3m "$R/kernel-early-failed.txt"
fi
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
sleep 1; klog_bg 1m "$R/kernel-after-fb1.txt"
say "4. scan-out pipe"
sysctl -w debug.nvaccelfb=3 > /dev/null || die "debug.nvaccelfb=3 failed"
sleep 1; klog_bg 1m "$R/kernel-after-fb3.txt"
[ "$(sysctl -n debug.nvaccelfb)" = 1 ] || die "the accelerator is not armed (debug.nvaccelfb = $(sysctl -n debug.nvaccelfb))"
if [ "$(conf WSMARK 1)" = 1 ]; then sysctl -w debug.nvaccel_surface_gpu_on=1 > /dev/null 2>&1 || say "no WindowServer marks in this XeGfxAccel"
else sysctl -w debug.nvaccel_surface_gpu_on=0 > /dev/null 2>&1; fi
sysctl -w debug.nvaccel_iop=1 > /dev/null || die "debug.nvaccel_iop=1 failed"
[ "$(sysctl -n debug.nvaccel_iop)" = 1 ] || die "debug.nvaccel_iop did not stay 1"
{ sysctl debug.nvaccelfb debug.nvaccel_iop debug.xegfx_agdc debug.xegfx_agdc_k5 debug.nmxaccel_headless debug.xegfx_stage 2>&1
  ioreg -l -w0 -r -c NMXAccel; ioreg -l -w0 -r -c XeGfxIntelFramebuffer; } > "$R/state-before-restart.txt" 2>&1
klog_bg 10m "$R/kernel-10m.txt"; sync

# [gu605-mac] kern.boottime reads "{ sec = N, usec = M } date": the old sed's greedy .*sec matched "usec" and took M as
# the boot time, so every login looked 56 years after boot and an automatic login was never recognised (2026-11-05 13:03).
# When to restart WindowServer (the screen goes dark once and the session ends):
#   nobody logged in (login window)            -> now
#   FORCE=1                                    -> now
#   logged in, DESKTOP_LOGGED_IN=auto (default): the session is an automatic login (started within DESKTOP_AUTOLOGIN_S,
#     default 150 s, of boot) and nobody touched keyboard or trackpad for DESKTOP_IDLE_S (default 15 s, waited for up to
#     DESKTOP_IDLE_WAIT_S, default 180 s) -> a DESKTOP_NOTICE_S (default 10 s) notice, then now unless there was input
#     meanwhile; otherwise -> at the next logout
#   DESKTOP_LOGGED_IN=logout                   -> at the next logout (armed now, nothing restarted; exit 10)
#   DESKTOP_LOGGED_IN=skip                     -> stop (the old behaviour)
idle_s() { ioreg -r -c IOHIDSystem -d 1 2>/dev/null | awk '/"HIDIdleTime"/ {print int($NF / 1000000000); exit}'; }
notify() { local u=$1 msg=$2 uid; uid=$(id -u "$u" 2>/dev/null) || return 0
  launchctl asuser "$uid" sudo -u "$u" osascript -e "display notification \"$msg\" with title \"GU605\"" > /dev/null 2>&1 || true; }
MODE=now
cu=$(stat -f %Su /dev/console 2>/dev/null)
case "$cu" in ""|root|_*) ;; *)
  if [ "${FORCE:-0}" = 1 ]; then say "$cu is logged in; FORCE=1: restarting anyway"
  else
    pol=$(conf DESKTOP_LOGGED_IN auto)
    case "$pol" in
      skip) die "$cu is logged in and DESKTOP_LOGGED_IN=skip" ;;
      logout) MODE=logout ;;
      *) MODE=logout
         dock=$(pgrep -u "$cu" -x Dock | head -1); up=$(( $(date +%s) - $(sysctl -n kern.boottime | sed -E 's/^[{] sec = ([0-9]+),.*/\1/') ))
         age=$([ -n "$dock" ] && etime_s "$dock" || echo "$up"); login_at=$(( up - ${age:-$up} )); idle=$(idle_s)
         # a clock step after the Dock started (Windows leaves local time in the RTC; NTP moves it by hours) makes the Dock
         # look older than the boot: count that session as started at boot
         [ "$login_at" -ge 0 ] || login_at=0
         say "$cu logged in ${login_at}s after boot, session ${age}s old, idle ${idle:-?}s"
         # [gu605-mac] WAS: one look at the idle time (60 s needed), so a desktop ready 1-2 minutes after boot almost
         # never qualified. Now: for up to DESKTOP_IDLE_WAIT_S (default 180) wait for DESKTOP_IDLE_S (default 15) without
         # input; then a DESKTOP_NOTICE_S (default 10) notice. Input during the notice, or a session busy for the whole
         # wait -> the next logout, as before. A logout meanwhile (login window) -> now.
         if [ "$login_at" -le "$(conf DESKTOP_AUTOLOGIN_S 150)" ]; then
           need=$(conf DESKTOP_IDLE_S 15); nsec=$(conf DESKTOP_NOTICE_S 10); wmax=$(conf DESKTOP_IDLE_WAIT_S 180); waited=0
           while [ "${idle:-0}" -lt "$need" ] && [ "$waited" -lt "$wmax" ]; do
             case "$(stat -f %Su /dev/console 2>/dev/null)" in ""|root|_*) break ;; esac
             sleep 2; waited=$((waited + 2)); idle=$(idle_s)
           done
           case "$(stat -f %Su /dev/console 2>/dev/null)" in
             ""|root|_*) MODE=now; say "logged out while waiting: restarting now" ;;
             *) if [ "${idle:-0}" -ge "$need" ]; then
                  say "automatic login, no input for ${idle}s (after ${waited}s): ${nsec}s notice"
                  [ "$nsec" -gt 0 ] && notify "$cu" "${nsec} sn sonra ekran bir kez kararacak ve oturum kapanacak (masaüstü Intel GPU'ya geçiyor). Klavye/izleme dörtgenine dokunursan iptal olur ve çıkış yaptığında olur."
                  sleep "$nsec"; idle2=$(idle_s); [ "$nsec" -gt 0 ] || idle2=0
                  if [ "${idle2:-0}" -ge "$nsec" ]; then MODE=now; say "automatic login, no input: restarting now"
                  else say "input during the notice (idle ${idle2:-?}s): waiting for the next logout instead"; fi
                else say "input for ${waited}s: waiting for the next logout instead"; fi ;;
           esac
         fi ;;
    esac
  fi ;;
esac

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
if [ "$MODE" = logout ]; then
  echo logout > "$R/restart-mode"
  notify "$cu" "Intel masaüstü hazır: bir sonraki oturum kapatmada (Apple menüsü > Çıkış) devreye girer."
  say "armed; WindowServer NOT restarted: the next logout brings the desktop to the Intel GPU (the runner waits for it)"
  sync; exit 10
fi
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
