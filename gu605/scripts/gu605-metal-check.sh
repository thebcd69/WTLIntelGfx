#!/bin/bash
# gu605-metal-check.sh REPORTDIR: after stage 7, the Intel Metal device in a test process only (mtlprobe, with the
# XeMTLDriver plugin loaded into that process alone): an MSL compute kernel and a draw checked on the CPU, then the
# plugin's GPU-pass probe on CoreDisplay's own precompiled shaders (the ones WindowServer crashed on before the AIR
# reader existed, samwu07 2026-11-02). Both pass -> $G/gpupass-ok, which gu605-desktop.sh requires.
set -uo pipefail
. "$(dirname "$0")/gu605-common.sh"
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 2; }
R=${1:-$L/metal-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$R"
say() { echo "$(ts) metal: $*" | tee -a "$R/metal.log"; }
MP=$G/tools/mtlprobe; PL=$XB/Contents/MacOS/XeMTLDriver; FLAG=$GB/nvmtl-gpupass-probe
[ -x "$MP" ] || { say "no $MP"; exit 2; }
[ -f "$PL" ] || { say "no $PL"; exit 2; }
[ "$(ioreg -r -c NMXAccel -d 1 | grep -c NMXAccel)" -gt 0 ] || { say "no NMXAccel: stage 7 has not run"; exit 2; }
[ -f "$XU/libvulkan_intel.dylib" ] || { say "no Mesa ANV in $XU"; exit 2; }
run() {   # run <name> [env...]: one mtlprobe with a private log and an empty translation cache
  local n=$1 c rc; shift
  c=$(mktemp -d /tmp/xemtl-probe.XXXXXX); mkdir -p "$c/aircache"
  env "$@" NVMTL_LOG_PATH="$c/nvmtl.log" NVMTL_AIRCACHE="$c/aircache" NM_TEST_PLUGIN="$PL" "$MP" > "$R/mtlprobe-$n.txt" 2>&1
  rc=$?
  cp "$c/nvmtl.log" "$R/nvmtl-$n.log" 2>/dev/null; cp /tmp/nvmtl.log "$R/nvmtl-tmp-$n.log" 2>/dev/null
  rm -rf "$c"; return $rc
}
say "compute + draw"
run basic; rc=$?
say "mtlprobe exit $rc ($(tail -1 "$R/mtlprobe-basic.txt"))"
[ $rc = 0 ] || { say "FAIL: Metal on the Intel GPU did not verify"; rm -f "$G/gpupass-ok"; exit 1; }
say "GPU-pass probe on CoreDisplay's shaders"
touch "$FLAG"; run gpupass; rc=$?; rm -f "$FLAG"
n=$(grep -c 'gpupass probe: .*PIPELINE BUILT' "$R/nvmtl-gpupass.log" "$R/nvmtl-tmp-gpupass.log" 2>/dev/null | awk -F: '{s += $NF} END {print s + 0}')
bad=$(grep -E -c 'gpupass probe: .*(FAILED|NO |no |cannot)|translation produced nothing' "$R/nvmtl-gpupass.log" 2>/dev/null)
say "GPU-pass: exit $rc, $n pipelines built, ${bad:-0} failures"
if [ "$n" -ge 3 ] && [ "${bad:-0}" = 0 ]; then printf '%s: compute, draw and 3 CoreDisplay pipelines built\n' "$(date)" > "$G/gpupass-ok"; say "PASS"; exit 0; fi
rm -f "$G/gpupass-ok"; say "FAIL: CoreDisplay's shaders do not build: the desktop stays on the firmware framebuffer"; exit 1
