#!/bin/bash
# gu605-fps-diag.sh [SECONDS] [REPORTDIR]: the Intel desktop's frame rate problem offline, nothing on screen changed.
# Needs stage 7 (NMXAccel) this boot; works with the desktop on the Intel GPU or not. Run it as a plain detached job
# (sudo nohup bash gu605-fps-diag.sh > /tmp/fps-diag.out 2>&1 &), not with launchctl submit: that relaunches it.
#   fpsprobe (tools/fpsprobe) in a test process with the Intel plugin, once per A/B setting (SECONDS per loop, default 5):
#     base | NVMTL_XE_RO_ONCE=1 | NVMTL_XE_HOSTCOPY_AHEAD=1 | NVMTL_XE_RO_ONCE=1 NVMTL_XE_HOSTCOPY_AHEAD=1
#     | NVMTL_XE_WORKER_AHEAD=1 | NVMTL_XE_EMPTY_TAIL=1 | NVMTL_XE_FPS=1 (all four) | NVMTL_XE_FPS=1 NVMTL_ASYNC_TEX_UPLOAD=1
#   each prints frames/s and where a frame's time goes (commit->submit, submit->GPU, GPU, end->done), and the plugin's
#   own "submit (5 s)" summary (host copies, read-only copies skipped, parts held, vkQueueSubmit cost)
# Output: REPORTDIR/summary.txt, one directory per setting.
set -uo pipefail
. "$(dirname "$0")/gu605-common.sh"
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 2; }
N=${1:-5}; R=${2:-$L/fpsdiag-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$R"; S=$R/summary.txt
say() { echo "$*" | tee -a "$S"; }
FP=$G/tools/fpsprobe; PL=$XB/Contents/MacOS/XeMTLDriver
[ -x "$FP" ] || { say "no $FP (package tools/fpsprobe, or build it: see userland/tools/fpsprobe.m)"; exit 2; }
[ "$(ioreg -r -c NMXAccel -d 1 | grep -c NMXAccel)" -gt 0 ] || { say "no NMXAccel: stage 7 has not run this boot"; exit 2; }
say "== fpsprobe A/B ($(date)), $N s per loop"
run() {   # run <tag> [env...]
  local t=$1 c; shift
  c=$(mktemp -d /tmp/xemtl-fps.XXXXXX); mkdir -p "$c/aircache"
  env "$@" NVMTL_ENABLE=1 NVMTL_XE_FRAME_DIAG=1 NVMTL_LOG_PATH="$c/nvmtl.log" NVMTL_AIRCACHE="$c/aircache" NM_TEST_PLUGIN="$PL" \
    "$FP" "$N" > "$R/$t.txt" 2>&1
  local rc=$?
  cp "$c/nvmtl.log" "$R/$t-nvmtl.log" 2>/dev/null; rm -rf "$c"
  say "-- $t (exit $rc)"
  grep -E '^(FPS |  commit->GPU|FPSPROBE|device|setup|no )' "$R/$t.txt" | tee -a "$S" > /dev/null
  grep -E 'submit \(5 s\)|host copy:|fps knobs|preset' "$R/$t-nvmtl.log" 2>/dev/null | sed -E 's/^pid [0-9]+ NVMTL: /   plugin: /' | tail -4 | tee -a "$S" > /dev/null
}
run base
run ro-once NVMTL_XE_RO_ONCE=1
run hostcopy-ahead NVMTL_XE_HOSTCOPY_AHEAD=1
run ro-once+ahead NVMTL_XE_RO_ONCE=1 NVMTL_XE_HOSTCOPY_AHEAD=1
run worker-ahead NVMTL_XE_WORKER_AHEAD=1
run empty-tail NVMTL_XE_EMPTY_TAIL=1
run fps-preset NVMTL_XE_FPS=1
run fps-preset+async-upload NVMTL_XE_FPS=1 NVMTL_ASYNC_TEX_UPLOAD=1
say "== verdict per setting (paced frames/s, commit->GPU start; base is expected to FAIL, fps-preset to PASS)"
grep -H '^FPSPROBE' "$R"/*.txt 2>/dev/null | sed -E "s#^$R/##; s#\.txt:FPSPROBE verdict: # #" | tee -a "$S" > /dev/null
efi_copy "$S" fps-diag.txt
echo "report: $R (summary.txt)"
