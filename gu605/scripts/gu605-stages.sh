#!/bin/bash
# gu605-stages.sh TARGET [REPORTDIR]: XgFbMove (if needed), load XeGfx idle, then its stages one at a time up to TARGET
# (1..7), saving the driver log, the per-stage results and the probe dictionaries after EVERY stage, synced, so a hang
# or panic in the next stage leaves everything before it on disk. Stops at the first failure and never re-runs a
# stage that failed this boot (the kext refuses that too on 7D55). Adapted from samwu07's run-xegfx-stages.sh.
#   1 shim self-test   2 read-only GPU probe (+ GU605 display/Type-C dump)   3 newbus/DMA   4 drm + i915 (display off)
#   5 one request per engine   6 GPU writes, CPU reads   7 hand the GPU to XeGfxAccel (Metal)
set -uo pipefail
. "$(dirname "$0")/gu605-common.sh"
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 2; }
TARGET=${1:-2}; case "$TARGET" in [1-7]) ;; *) echo "TARGET must be 1..7" >&2; exit 2 ;; esac
OUT=${2:-$L/stages-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$OUT"
say() { echo "$(ts) $*" | tee -a "$OUT/stages.log"; }
snap() {
  echo "per-stage result s0..s7: $(results)" > "$OUT/$1-results.txt"
  sysctl -b debug.xegfx_log > "$OUT/$1-xegfx-log.txt" 2>&1
  ioreg -l -w0 -r -c XeGfxResident > "$OUT/$1-ioreg-XeGfxResident.txt" 2>&1
  sync
}
if has_bootarg -xegfxoff; then say "boot-arg -xegfxoff: XeGfx stays idle, no stages"; exit 3; fi
[ "$TARGET" -lt 7 ] || [ -d /Library/Extensions/XeGfxAccel.kext ] || { say "stage 7 needs XeGfxAccel.kext in /Library/Extensions"; exit 2; }

# 1. the boot framebuffer out of i915's GuC slot, before XeGfx exists in this boot (WTL issue #13)
if [ -d /Library/Extensions/XgFbMove.kext ] && ! has_bootarg -xgfbmoveoff; then
  if loaded 'org\.watchthelight\.XeGfx'; then say "XeGfx already loaded: XgFbMove not run (it only moves before XeGfx)"
  elif ! loaded 'org\.local\.XgFbMove'; then
    kmutil load -b org.local.XgFbMove > "$OUT/xgfbmove-load.txt" 2>&1 || say "XgFbMove load failed: $(tail -2 "$OUT/xgfbmove-load.txt")"
  fi
  ioreg -l -w0 -r -c IOPCIDevice | grep -o '"xgfbmove-result" = "[^"]*"' | head -1 > "$OUT/xgfbmove.txt"
  say "XgFbMove: $(cat "$OUT/xgfbmove.txt")"
fi
# 2. XeGfx itself: in the boot kernel collection through OpenCore Kernel>Add, idle until loaded here
if ! loaded 'org\.watchthelight\.XeGfx'; then
  say "loading XeGfx (idle)"
  kmutil load -b org.watchthelight.XeGfx > "$OUT/kmutil-load.txt" 2>&1 || { say "XeGfx load failed: $(tail -3 "$OUT/kmutil-load.txt")"; exit 1; }
fi
sysctl -w debug.xegfx_wdog=60 > /dev/null 2>&1 || true
# gu605.conf XEGFX_ENV=name=value[ name=value...]: extra pairs for debug.xegfx_env, added once before stage 1 fetches the
# tunables (e.g. compat.linuxkpi.drm_debug=4: i915's KMS debug messages in debug.xegfx_log)
xe=$(conf XEGFX_ENV ""); if [ -n "$xe" ] && [ "$(sysctl -n debug.xegfx_stage 2>/dev/null)" = 0 ]; then
  env=$(sysctl -n debug.xegfx_env); case " $env " in *" $xe "*) ;; *) sysctl -w debug.xegfx_env="$env $xe" > /dev/null; say "env (XEGFX_ENV): $(sysctl -n debug.xegfx_env)" ;; esac; fi
snap stage0
read -r -a R <<< "$(results)"
[ "${#R[@]}" = 8 ] || { say "cannot read debug.xegfx_results"; exit 1; }
for i in 1 2 3 4 5 6 7; do
  [ "${R[$i]}" -ge 0 ] || { say "stage $i already FAILED this boot (${R[$i]}): restart first"; exit 1; }
done
DONE=$(sysctl -n debug.xegfx_stage)
for s in $(seq 1 "$TARGET"); do
  [ "$s" -le "$DONE" ] && { say "stage $s: passed earlier this boot"; continue; }
  if [ "$s" = 4 ]; then
    # display path (docs/GU605-DISPLAY-DESIGN.md): DISPLAY=kms asks the kext to let i915 own the display; the kext
    # still refuses without stage 2's takeover preflight, with -xegfxnokms, or on anything but 7D55
    KMS=0
    if [ "$(conf DISPLAY adopt)" = kms ]; then
      pre=$(sed -nE 's/.*"kms-preflight"="([^"]*)".*/\1/p' "$OUT/stage2-ioreg-XeGfxResident.txt" "$OUT/stage0-ioreg-XeGfxResident.txt" 2>/dev/null | head -1)
      if has_bootarg -xegfxnokms; then say "display: DISPLAY=kms, but boot-arg -xegfxnokms: adopt path"
      elif [ -e "$G/kms-failed" ]; then say "display: DISPLAY=kms, but $G/kms-failed exists (an earlier KMS stage 4 failed): adopt path; sudo rm it to retry"
      elif [ "$pre" != ok ]; then say "display: DISPLAY=kms, but the takeover preflight says: ${pre:-not recorded}: adopt path"
      else
        KMS=1; env=$(sysctl -n debug.xegfx_env)
        case "$env" in *xegfx.kms=1*) ;; *) sysctl -w debug.xegfx_env="$env xegfx.kms=1" > /dev/null ;; esac
        say "display: i915 KMS requested (preflight ok); env: $(sysctl -n debug.xegfx_env)"
      fi
    fi
    # the GuC slot check, from stage 2's probe: never let i915 load the GuC under the panel's pixels
    # (KMS: i915 rebinds the surface at GGTT 0 itself before it reserves the GuC slot)
    surf=""   # set -u: with KMS=1 nothing below set it, and "$surf" stopped the run before stage 4 (2026-11-09)
    [ "$KMS" = 1 ] || surf=$(sed -nE 's/.*"plane-surf-1-a"=([0-9]+).*/\1/p' "$OUT/stage3-ioreg-XeGfxResident.txt" "$OUT/stage2-ioreg-XeGfxResident.txt" "$OUT/stage0-ioreg-XeGfxResident.txt" 2>/dev/null | head -1)
    if [ -n "$surf" ]; then surf=$((surf & 0xFFFFF000))
      printf 'boot surface GGTT 0x%08X\n' "$surf" | tee -a "$OUT/stages.log"
      if [ "$surf" -ge $((0xFEE00000)) ] && [ "$surf" -lt $((0xFF000000)) ]; then
        say "STOP before stage 4: the boot surface starts inside the GuC slot (XgFbMove did not move it)"; break; fi
    fi
  fi
  say "stage $s"
  sync
  sysctl -w debug.xegfx_stage="$s" > "$OUT/stage$s-sysctl.txt" 2>&1
  rc=$?
  snap "stage$s"
  DONE=$(sysctl -n debug.xegfx_stage)
  read -r -a R <<< "$(results)"
  if [ $rc -ne 0 ] || [ "$DONE" -lt "$s" ]; then
    say "stage $s FAILED: result ${R[$s]:-?}"; tail -30 "$OUT/stage$s-xegfx-log.txt" >> "$OUT/stages.log"
    if [ "$s" = 4 ] && [ "${KMS:-0}" = 1 ]; then
      touch "$G/kms-failed"; sysctl -n debug.xegfx_kms > "$OUT/display-kms.txt" 2>&1
      say "display: stage 4 failed with i915 owning the display: later boots use the adopt path ($G/kms-failed)"; fi
    exit 1
  fi
  say "stage $s passed"
  if [ "$s" = 4 ]; then
    ioreg -l -w0 -r -c XeGfxResident -d 1 2>/dev/null | grep -oE '"XeGfx(Kms|Post)[A-Za-z]*" = [^,]*(,[^"]*)?' > "$OUT/stage4-kms-ioreg.txt"
    if sysctl -n debug.xegfx_kms > "$OUT/display-kms.txt" 2>&1 && grep -q '^connector' "$OUT/display-kms.txt"; then
      say "display: i915 owns the display"; grep -E '^(xegfx-kms|crtc|connector)|active crtc|backlight' "$OUT/display-kms.txt" | tee -a "$OUT/stages.log" > /dev/null
      hz=$(conf PANEL_HZ 240)
      if [ -n "$hz" ] && [ "$hz" != 0 ]; then
        edp=$(sed -nE 's/^connector (eDP-[0-9]+) .*status connected.*/\1/p' "$OUT/display-kms.txt" | head -1)
        if [ -n "$edp" ] && grep -A200 "^connector $edp " "$OUT/display-kms.txt" | grep -q "active crtc"; then
          sysctl -w debug.xegfx_kms_set="$edp $hz test" > /dev/null 2>&1; say "display: $(sysctl -n debug.xegfx_kms_set)"
          if sysctl -w debug.xegfx_kms_set="$edp $hz" > /dev/null 2>&1; then say "display: $(sysctl -n debug.xegfx_kms_set)"
          else say "display: switch to $hz Hz refused: $(sysctl -n debug.xegfx_kms_set)"; fi
          sysctl -n debug.xegfx_kms > "$OUT/display-kms-after.txt" 2>&1
          grep -E '^crtc|active crtc' "$OUT/display-kms-after.txt" | tee -a "$OUT/stages.log" > /dev/null
        fi
      fi
    else
      say "display: adopt path ($(head -1 "$OUT/display-kms.txt"))"
    fi
  fi
  [ "$s" = 2 ] && { probe_decode "$OUT/stage2-ioreg-XeGfxResident.txt" > "$OUT/display-decoded.txt" 2>&1; cat "$OUT/display-decoded.txt" >> "$OUT/stages.log"; }
done
[ "$TARGET" = 7 ] && { ioreg -l -w0 -r -c NMXAccel > "$OUT/ioreg-NMXAccel.txt" 2>&1
  [ "$(grep -c NMXAccel "$OUT/ioreg-NMXAccel.txt")" -gt 0 ] && say "XeGfxAccel attached (NMXAccel)" || { say "no NMXAccel after stage 7"; exit 1; }; }
[ "$(sysctl -n debug.xegfx_stage)" -ge "$TARGET" ] || exit 1
exit 0
