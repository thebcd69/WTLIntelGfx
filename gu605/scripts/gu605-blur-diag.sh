#!/bin/bash
# gu605-blur-diag.sh [REPORTDIR]: the backdrop-blur ("vibrancy") problem on the Intel desktop, in one run, nothing on
# screen changed. Needs stage 7 (NMXAccel) this boot; works with the desktop on the Intel GPU or not.
#   1. blurprobe (tools/blurprobe) in a test process with the Intel plugin, once per A/B setting:
#        base | INTEL_DEBUG=noccs | INTEL_DEBUG=nofc | INTEL_DEBUG=noccs,nofc | NVMTL_NO_FBFETCH=1 (as WindowServer)
#        | NVMTL_NO_DIRTYONLY=1 | NVMTL_NO_PSO_SKIP=1 | NVMTL_XE_SELF_SAMPLE=barrier (also as WindowServer)
#        | NVMTL_XE_SELF_SAMPLE=off (as WindowServer) | NVMTL_XE_SAMPLE_GENERAL=1 (as WindowServer)
#        | NVMTL_XE_VIBRANCY=1 (the one-line preset, as WindowServer)
#      each prints PASS/FAIL per GPU path and writes PNGs
#   2. what the plugin inside WindowServer logged about refused/skipped work (log show + /tmp/nvmtl.log)
#   3. the conf file the WindowServer plugin reads, as it is now
# Then: docs in GU605-NEXT-MAC-AGENT.md (which result points where). Output: REPORTDIR/summary.txt.
set -uo pipefail
. "$(dirname "$0")/gu605-common.sh"
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 2; }
R=${1:-$L/blur-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$R"; S=$R/summary.txt
say() { echo "$*" | tee -a "$S"; }
BP=$G/tools/blurprobe; PL=$XB/Contents/MacOS/XeMTLDriver
[ -x "$BP" ] || { say "no $BP (package tools/blurprobe, or build it: see userland/tools/blurprobe.m)"; exit 2; }
[ "$(ioreg -r -c NMXAccel -d 1 | grep -c NMXAccel)" -gt 0 ] || { say "no NMXAccel: stage 7 has not run this boot"; exit 2; }
say "== blurprobe A/B ($(date))"
# NVMTL_ENABLE=1: the plugin hands a device only to allow-listed processes (and mtlprobe/nvmtltest by name); blurprobe
# is neither, so without it every run ended "no Intel Metal device" (Mac, 2026-11-05 12:45)
run() {   # run <tag> [env...]
  local t=$1 c; shift
  c=$(mktemp -d /tmp/xemtl-blur.XXXXXX); mkdir -p "$c/aircache"
  env "$@" NVMTL_ENABLE=1 NVMTL_LOG_PATH="$c/nvmtl.log" NVMTL_AIRCACHE="$c/aircache" NM_TEST_PLUGIN="$PL" "$BP" "$R/$t" > "$R/$t.txt" 2>&1
  local rc=$?
  cp "$c/nvmtl.log" "$R/$t-nvmtl.log" 2>/dev/null; rm -rf "$c"
  say "-- $t (exit $rc)"; grep -E '^(TEST|device|blurprobe)' "$R/$t.txt" | tee -a "$S" > /dev/null
  grep -iE 'refused|skipped|produced nothing|FAILED|dummy|not ready|ceiling' "$R/$t-nvmtl.log" 2>/dev/null | sort | uniq -c | sort -rn | head -15 | sed 's/^/   plugin: /' | tee -a "$S" > /dev/null
}
run base
run noccs INTEL_DEBUG=noccs
run nofc INTEL_DEBUG=nofc
run noccs-nofc INTEL_DEBUG=noccs,nofc
run as-windowserver NVMTL_NO_FBFETCH=1
run no-dirtyonly NVMTL_NO_DIRTYONLY=1
run no-psoskip NVMTL_NO_PSO_SKIP=1
# the self-sampled-target modes (plugin: NVMTL_XE_SELF_SAMPLE), alone and as WindowServer runs (fetch off)
run self-barrier NVMTL_XE_SELF_SAMPLE=barrier
run self-barrier-as-ws NVMTL_XE_SELF_SAMPLE=barrier NVMTL_NO_FBFETCH=1
run self-off-as-ws NVMTL_XE_SELF_SAMPLE=off NVMTL_NO_FBFETCH=1
run sample-general-as-ws NVMTL_XE_SAMPLE_GENERAL=1 NVMTL_NO_FBFETCH=1
run preset-as-ws NVMTL_XE_VIBRANCY=1 NVMTL_NO_FBFETCH=1
say "== verdict: ws.* tests per setting (base must FAIL, preset-as-ws must PASS for the fix to be the right one)"
grep -H -E '^TEST ws\.' "$R"/*.txt 2>/dev/null | sed -E "s#^$R/##; s#\.txt:TEST # #" | tee -a "$S" > /dev/null
say "== WindowServer's plugin, this boot (refused / skipped / failed work)"
{ log show --last boot --style compact --predicate 'process == "WindowServer" AND eventMessage CONTAINS "NVMTL"' 2>/dev/null
  grep -h "NVMTL" /tmp/nvmtl.log /tmp/nvmtl-WindowServer-*.log /private/var/folders/*/*/T/WindowServer/nvmtl.log 2>/dev/null; } > "$R/windowserver-nvmtl.txt"
grep -iE 'refused|skipped|produced nothing|FAILED|dummy|framebuffer fetch|not ready|layout|ceiling|view' "$R/windowserver-nvmtl.txt" \
  | sed -E 's/^.*NVMTL: //' | sort | uniq -c | sort -rn | head -40 | tee -a "$S" > /dev/null
say "== $XU/nvrm610.conf (read by the plugin at WindowServer start)"; cat "$XU/nvrm610.conf" 2>/dev/null | tee -a "$S" > /dev/null || say "(none)"
efi_copy "$S" blur-diag.txt
echo "report: $R (summary.txt, PNGs per setting)"
