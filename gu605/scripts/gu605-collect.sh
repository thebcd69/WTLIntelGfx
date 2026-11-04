#!/bin/bash
# gu605-collect.sh: ONE command after any GU605 test boot. Writes everything into one text file and one archive on the
# Desktop of the logged-in user, and copies the text file to <EFI>/GU605/report.txt (readable from Windows).
#   sudo bash /Library/XeGfx/gu605/gu605-collect.sh
set -uo pipefail
. "$(dirname "$0")/gu605-common.sh"
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 2; }
U=${SUDO_USER:-$(stat -f %Su /dev/console)}; UH=$(eval echo "~$U"); D=$UH/Desktop; mkdir -p "$D"
N=gu605-report-$(date +%Y%m%d-%H%M%S); F=$D/$N.txt; T=$(mktemp -d /tmp/gu605-collect.XXXXXX)
sec() { echo; echo "===== $* ====="; }
{
  echo "GU605 report $(date)"
  sec system; sw_vers; sysctl -n machdep.cpu.brand_string kern.bootargs kern.boottime; csrutil status 2>&1
  sec mode; echo "mode $(gpu_mode)  iGPU-framebuffer $(igpu_fb_present)  NVIDIA $(pci_display_present 10de)"
  sec "installed"; ls -la /Library/Extensions | grep -E 'XeGfx|XgFbMove|NVRM|NVAccel|NVPrimary|NMIntelFB'; ls -la "$GB" "$XU" 2>&1 | head -40
  cat "$G/VERSION" 2>/dev/null; cat "$CONF" 2>/dev/null; ls -la "$G" 2>&1
  sec "loaded kexts"; kmutil showloaded --list-only 2>/dev/null | grep -v com.apple
  sec "nvram"; for v in gu605-pending xeintelfb-starts nmintelfb-starts xegfx-step xegfx-step-prev xegfx-accel; do echo "$v: $(nvram "$v" 2>/dev/null | cut -f2-)"; done
  sec "newest automatic run"; cat "$L/last-summary.txt" 2>/dev/null
  sec "sysctl debug.xegfx/nvaccel/nmxaccel"; sysctl -a 2>/dev/null | grep -E '^debug\.(xegfx|nvaccel|nmxaccel|nmintelfb|nvrmfb_agdc)' | grep -v -E 'xegfx_log|xegfx_results'
  echo "xegfx_results s0..s7: $(results)"
  sec "XeGfx driver log"; sysctl -b debug.xegfx_log 2>/dev/null | tail -400
  sec "ioreg XeGfxResident (probe, GT, exec, store)"; ioreg -l -w0 -r -c XeGfxResident 2>/dev/null | grep -E 'XeGfx(Probe|Gt|Exec|Store|Post|Stage)'
  sec "ioreg framebuffers"; ioreg -l -w0 -r -c IOFramebuffer -d 3 2>/dev/null | grep -E '\+-o|IOFBDependent|NVRMTiming|IOFBCurrentPixel|IOFBConfig|display0|"model"|backlight|IODisplayParameters' | head -80
  sec "240 Hz: panel link, DSC, EDID modes (XeGfxIntelFB)"
  ioreg -l -w0 -r -c XeGfxIntelFramebuffer -d 1 2>/dev/null | grep -E '"GU605(Modes|LinkMbpsPerLane|LinkLanes|DSCEnabled|MeasuredHz16|DPCD|EDID)"'
  log show --last boot --style compact --predicate 'process == "kernel" AND eventMessage CONTAINS "gu605"' 2>/dev/null | tail -40
  for f in "$L"/boot-*/stages/display-decoded.txt; do [ -f "$f" ] && { echo "--- $f"; cat "$f"; }; done 2>/dev/null | tail -60
  sec "ioreg NMXAccel"; ioreg -l -w0 -r -c NMXAccel -d 1 2>/dev/null | head -60
  sec "ioreg GPUs"; ioreg -l -w0 -r -c IOPCIDevice -d 1 2>/dev/null | grep -E '\+-o (GFX0|PXSX|PEG|IGPU|GFX)|"device-id"|"vendor-id"|"class-code"|xgfbmove|"model"' | head -40
  sec "displays"; system_profiler SPDisplaysDataType 2>/dev/null
  sec "kernel log, this boot (driver lines)"
  log show --last boot --style compact --predicate 'process == "kernel" AND (eventMessage CONTAINS[c] "xegfx" OR eventMessage CONTAINS[c] "XeGfxIntelFB" OR eventMessage CONTAINS "XgFbMove" OR eventMessage CONTAINS[c] "nmx" OR eventMessage CONTAINS[c] "nvaccel" OR eventMessage CONTAINS[c] "agdc" OR eventMessage CONTAINS "i915" OR eventMessage CONTAINS "[drm" OR eventMessage CONTAINS "NVRM")' 2>/dev/null | tail -3000
  sec "WindowServer, this boot (errors)"; log show --last boot --style compact --predicate 'process == "WindowServer" AND (messageType == error OR messageType == fault)' 2>/dev/null | tail -200
  sec "panic reports (newest 3)"; for p in $(ls -t /Library/Logs/DiagnosticReports/*panic* 2>/dev/null | head -3); do echo "--- $p"; head -80 "$p"; done
  sec "crash reports WindowServer (newest 2)"; for p in $(ls -t /Library/Logs/DiagnosticReports/WindowServer* 2>/dev/null | head -2); do echo "--- $p"; head -60 "$p"; done
  sec "plugin log"; tail -300 /tmp/nvmtl.log 2>/dev/null
  sec "log folder sizes"; du -sh "$L"/* 2>/dev/null | tail -20
} > "$F" 2>&1
cp -R "$L" "$T/logs" 2>/dev/null; cp "$F" "$T/"
cp /Library/Logs/DiagnosticReports/*panic* "$T/" 2>/dev/null
tar -czf "$D/$N.tar.gz" -C "$T" . 2>/dev/null
chown "$U" "$F" "$D/$N.tar.gz" 2>/dev/null
rm -rf "$T"
efi_copy "$F" report.txt
echo "Report: $F"
echo "Archive: $D/$N.tar.gz"
echo "Also on the EFI partition: GU605/report.txt"
