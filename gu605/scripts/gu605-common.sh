# gu605-common.sh: sourced by every GU605 script. Paths, logging, mode detection, the EFI copy of the newest report.
# ASUS ROG Zephyrus G16 GU605MY, iGPU 8086:7D55, macOS 15 Sequoia. Built on samwu07's ThinkBook scripts (BSD-2).
# No `grep -q` behind a pipe anywhere: with pipefail, grep -q's early exit kills the writer (SIGPIPE) and the test fails.
PATH=/usr/bin:/bin:/usr/sbin:/sbin
G=/Library/XeGfx/gu605                 # installed scripts, config, state
L=$G/logs                              # one folder per run
CONF=$G/gu605.conf
GB=/Library/GPUBundles
XB=$GB/XeMTLDriver.bundle              # the Intel Metal plugin (NullMoth's NVMTLDriver.bundle is never touched)
XU=$GB/xemtl                           # ANV, libdrm, ICD manifest, caches of the Intel plugin
AL=$GB/xemtl-allow.txt                 # the Intel plugin's allow-list (NullMoth's nvmtl-allow.txt stays NullMoth's)
mkdir -p "$L" 2>/dev/null

ts() { date '+%F %T'; }
# conf values: KEY=value lines; unset keys get the defaults below
conf() { local k=$1 d=$2 v; v=$(sed -n "s/^$k=//p" "$CONF" 2>/dev/null | tail -1); echo "${v:-$d}"; }
bootargs() { sysctl -n kern.bootargs 2>/dev/null; }
has_bootarg() { case " $(bootargs) " in *" $1 "*) return 0 ;; esac; return 1; }
loaded() { kmutil showloaded --list-only 2>/dev/null | grep -E "$1 \(" > /dev/null; }

# PCI display devices by vendor: "1" when one exists
pci_display_present() {   # pci_display_present <vendor hex, e.g. 10de>
  ioreg -r -c IOPCIDevice -d 1 -l -w0 2>/dev/null | awk -v v="$1" '
    /^\+-o / { if (ven == v && cls ~ /^0003/) found = 1; ven = ""; cls = "" }
    /"vendor-id" = </ { s = $0; sub(/.*</, "", s); sub(/>.*/, "", s); ven = substr(s, 3, 2) substr(s, 1, 2) }
    /"class-code" = </ { s = $0; sub(/.*</, "", s); sub(/>.*/, "", s); cls = substr(s, 7, 2) substr(s, 5, 2) substr(s, 3, 2) substr(s, 1, 2) }
    END { if (ven == v && cls ~ /^0003/) found = 1; print found ? 1 : 0 }'
}
igpu_fb_present() { [ "$(ioreg -r -c XeGfxIntelFramebuffer -d 1 2>/dev/null | grep -c XeGfxIntelFramebuffer)" -gt 0 ] && echo 1 || echo 0; }
# [gu605-mac] 1 when a framebuffer of ANY class, with a display attached, sits on the Intel display PCI device (8086, class 03): our
# XeGfxIntelFramebuffer, or Apple's IONDRVFramebuffer (.Display_boot) when ours did not start. WAS: only our class
# counted, so an Eco boot where XeGfxIntelFB failed was taken for Ultimate (2026-11-05 10:53).
igpu_drives_panel() {
  [ "$(igpu_fb_present)" = 1 ] && { echo 1; return; }
  local n
  for n in $(ioreg -r -c IOPCIDevice -d 1 -l -w0 2>/dev/null | awk '
      /^\+-o / { if (ven == "8086" && cls ~ /^0003/) print name; name = $2; sub(/@.*/, "", name); ven = ""; cls = "" }
      /"vendor-id" = </ { s = $0; sub(/.*</, "", s); sub(/>.*/, "", s); ven = substr(s, 3, 2) substr(s, 1, 2) }
      /"class-code" = </ { s = $0; sub(/.*</, "", s); sub(/>.*/, "", s); cls = substr(s, 7, 2) substr(s, 5, 2) substr(s, 3, 2) substr(s, 1, 2) }
      END { if (ven == "8086" && cls ~ /^0003/) print name }'); do
    # a framebuffer with a display attached (an idle IONDRVFramebuffer on the iGPU in Ultimate has none)
    ioreg -r -n "$n" -d 5 -w0 2>/dev/null | awk '/<class (IONDRVFramebuffer|XeGfxIntelFramebuffer|NMIntelFB)/ {f=1} /<class (AppleBacklightDisplay|AppleDisplay)/ {d=1} END {exit !(f && d)}' && { echo 1; return; }
  done
  echo 0
}
# [gu605-mac] 1 when NVIDIA's driver is live (NVRM bound to GFX0). WAS: pci_display_present 10de, which also counts GFX0's
# IOPCIDevice node, which stays in ioreg with its cached ids when Eco has cut the dGPU's power (config space ffff:ffff).
nvidia_live() { [ "$(ioreg -r -c NVRM -d 1 2>/dev/null | grep -c '<class NVRM')" -gt 0 ] && echo 1 || echo 0; }

# G-Helper mode, as macOS sees it: eco (iGPU drives the panel, NVIDIA off), standard (iGPU drives the panel, NVIDIA
# live), ultimate (NVIDIA live, iGPU not on the panel), unknown
gpu_mode() {
  local i n; i=$(igpu_drives_panel); n=$(nvidia_live)
  if [ "$i" = 1 ] && [ "$n" = 0 ]; then echo eco
  elif [ "$i" = 1 ]; then echo standard
  elif [ "$n" = 1 ]; then echo ultimate
  else echo unknown; fi
}

# results of the XeGfx stages: 8 int32, 0 not run, 1 pass, <0 failure
results() { sysctl -b debug.xegfx_results 2>/dev/null | od -An -v -t d4 | tr -s ' \n' ' ' | sed 's/^ //'; }

# the EFI partition of the disk macOS booted from, as a disk identifier (diskXsY), or nothing
boot_efi() {
  local store disk
  store=$(diskutil info / 2>/dev/null | awk -F': *' '/APFS Physical Store/ {print $2; exit}')
  [ -n "$store" ] || store=$(diskutil info / 2>/dev/null | awk -F': *' '/Part of Whole/ {print $2; exit}')
  disk=$(echo "$store" | sed -E 's/s[0-9]+$//')
  [ -n "$disk" ] || return 0
  diskutil list "$disk" 2>/dev/null | awk '$2 == "EFI" { print $NF; exit }'
}
# efi_copy <file> [name]: copies a small report to <EFI>/GU605/ so it can be read from Windows (FAT32) too.
# Off with EFI_LOG=0 in gu605.conf. Mounts the EFI only for the copy if it was not mounted.
efi_copy() {
  [ "$(conf EFI_LOG 1)" = 1 ] || return 0
  local f=$1 name=${2:-$(basename "$1")} id mp was=1
  [ -f "$f" ] || return 0
  [ "$(stat -f %z "$f")" -lt 4194304 ] || { echo "efi_copy: $f larger than 4 MiB, not copied"; return 0; }
  id=$(boot_efi); [ -n "$id" ] || return 0
  mp=$(diskutil info "$id" 2>/dev/null | awk -F': *' '/Mount Point/ {print $2; exit}')
  if [ -z "$mp" ]; then was=0; diskutil mount "$id" > /dev/null 2>&1 || return 0
    mp=$(diskutil info "$id" 2>/dev/null | awk -F': *' '/Mount Point/ {print $2; exit}'); fi
  if [ -n "$mp" ] && [ -d "$mp/EFI" ]; then
    # fixed names, the one before kept as .prev: the EFI never fills up and nothing else there is touched
    mkdir -p "$mp/GU605" && cp "$f" "$mp/GU605/$name.tmp" || true
    [ -f "$mp/GU605/$name" ] && mv -f "$mp/GU605/$name" "$mp/GU605/$name.prev"
    [ -f "$mp/GU605/$name.tmp" ] && mv -f "$mp/GU605/$name.tmp" "$mp/GU605/$name"
  fi
  [ $was = 1 ] || diskutil unmount "$id" > /dev/null 2>&1
  sync
}

# probe_decode <ioreg XeGfxResident dump>: the GU605 stage 2 display dump in words (which pipes the firmware lit,
# which DDIs are on, which Type-C port sees a monitor in DP-alt or TBT mode)
probe_decode() {
  local f=$1 k v
  pk() { sed -nE "s/.*\"$1\"=([0-9]+).*/\1/p" "$f" | head -1; }
  [ -n "$(pk transconf-A)" ] || { echo "no GU605 display dump in $f"; return 0; }
  for t in A B C D; do
    v=$(pk "transconf-$t"); p=$(pk "plane-ctl-1-$t"); s=$(pk "plane-size-1-$t"); u=$(pk "plane-surf-1-$t"); d=$(pk "trans-ddi-func-ctl-$t")
    [ -n "$v" ] || continue
    printf 'pipe %s: transcoder %s, plane %s, %dx%d, surface GGTT 0x%08X, DDI func 0x%08X (DDI select %d: 1=A eDP, 2=B, 4-7=TC1-TC4)\n' "$t" \
      "$([ $((v & 0x80000000)) -ne 0 ] && echo on || echo off)" "$([ $((p & 0x80000000)) -ne 0 ] && echo on || echo off)" \
      $(( (s & 0xFFFF) + 1 )) $(( ((s >> 16) & 0xFFFF) + 1 )) $((u & 0xFFFFF000)) "$d" $(( (d >> 27) & 0xF ))
  done
  # the 240 Hz question: timings, measured refresh, pixel clock, bpc, lanes, DSC, VRR, PSR for each lit transcoder
  for t in A B C D; do
    v=$(pk "transconf-$t"); [ -n "$v" ] && [ $((v & 0x80000000)) -ne 0 ] || continue
    h=$(pk "htotal-$t"); vt=$(pk "vtotal-$t"); m=$(pk "refresh-mhz-$t"); d=$(pk "trans-ddi-func-ctl-$t")
    ha=$(( (h & 0xFFFF) + 1 )); ht=$(( (h >> 16) + 1 )); va=$(( (vt & 0xFFFF) + 1 )); vtt=$(( (vt >> 16) + 1 ))
    m=${m:-0}; pclk=$(( ht * vtt * m / 1000000 ))   # kHz
    bpc=$(( (d >> 20) & 7 )); case $bpc in 0) bpc=8;; 1) bpc=10;; 2) bpc=6;; 3) bpc=12;; *) bpc="?$bpc";; esac
    printf 'pipe %s mode: %dx%d active, %dx%d total, %d.%03d Hz measured, pixel clock ~%d.%03d MHz, %s bpc, DDI port width x%d, mode select %d\n' \
      "$t" $ha $va $ht $vtt $((m / 1000)) $((m % 1000)) $((pclk / 1000)) $((pclk % 1000)) "$bpc" $(( ((d >> 1) & 7) + 1 )) $(( (d >> 24) & 7 ))
    printf '  VRR ctl 0x%08X vmin %s vmax %s flipline %s\n' "$(pk "vrr-ctl-$t")" "$(pk "vrr-vmin-$t")" "$(pk "vrr-vmax-$t")" "$(pk "vrr-flipline-$t")"
  done
  v=$(pk psr-ctl-A); [ -n "$v" ] && printf 'eDP PSR ctl 0x%08X (%s), PSR2 ctl 0x%08X (%s)\n' "$v" "$([ $((v & 0x80000000)) -ne 0 ] && echo on || echo off)" \
    "$(pk psr2-ctl-A)" "$([ $(( $(pk psr2-ctl-A) & 0x80000000 )) -ne 0 ] && echo on || echo off)"
  v=$(pk dss-ctl2-pipe-A); [ -n "$v" ] && printf 'DSC pipe A: DSS_CTL2 0x%08X (VDSC0 %s, VDSC1 %s), DSS_CTL1 0x%08X; pipe B DSS_CTL2 0x%08X; eDP DSS_CTL2 0x%08X\n' \
    "$v" "$([ $((v & 0x80000000)) -ne 0 ] && echo on || echo off)" "$([ $((v & 0x8000)) -ne 0 ] && echo on || echo off)" \
    "$(pk dss-ctl1-pipe-A)" "$(pk dss-ctl2-pipe-B)" "$(pk dss-ctl2-edp)"
  for k in a b tc1 tc2 tc3 tc4; do v=$(pk "ddi-buf-ctl-$k"); [ -n "$v" ] && printf 'DDI %s buffer %s (0x%08X)\n' "$k" "$([ $((v & 0x80000000)) -ne 0 ] && echo ON || echo off)" "$v"; done
  for n in 1 2 3 4; do
    v=$(pk "tcss-ddi-status-tc$n"); b=$(pk "port-buf-ctl1-tc$n"); [ -n "$v" ] || continue
    printf 'TC%d: HPD DP-alt %d, HPD TBT %d, ready %d, pin assignment %d; PHY ownership %d, TCSS power req %d state %d (status 0x%08X buf 0x%08X)\n' \
      "$n" $((v & 1)) $(((v >> 1) & 1)) $(((v >> 2) & 1)) $(((v >> 25) & 0xF)) $(((b >> 6) & 1)) $(((b >> 5) & 1)) $(((b >> 4) & 1)) "$v" "$b"
  done
  v=$(pk blc-pwm-ctl); [ -n "$v" ] && printf 'south PWM backlight: ctl 0x%08X freq %s duty %s\n' "$v" "$(pk blc-pwm-freq)" "$(pk blc-pwm-duty)"
  return 0
}
