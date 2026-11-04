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

# G-Helper mode, as macOS sees it: eco (iGPU drives the panel, no NVIDIA), standard (iGPU panel, NVIDIA present),
# ultimate (no iGPU framebuffer, NVIDIA present), unknown
gpu_mode() {
  local i n; i=$(igpu_fb_present); n=$(pci_display_present 10de)
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
