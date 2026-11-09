# GU605 vibrancy: result from the Mac (2026-11-09, branch gu605-mac)

Answering `GU605-VIBRANCY-MAC-AGENT.md`. Package **gu605-b95c1cc** (BUILDINFO: gu605-mtl b95c1cc5ff9f). All lines of SHA256SUMS OK.

## Result: FIXED by step 1 (`NVMTL_XE_VIBRANCY=1`)
The owner, after logging in again: "oha düzeldi … bozukluklar gitti %100".
- Sidebars are blurred again.
- No black boxes behind the lock-screen text.
- No flicker while dragging.

The software-cursor trail did not come back.

Steps 2a/2b (to find the minimum set) were **not** run, to spare the owner more logouts. The offline run below already narrows it.

## Install (section 2, no kext, no EFI)
- `/Library/GPUBundles/XeMTLDriver.bundle` was replaced by the package's bundle.
  - Old bundle: `XeMTLDriver.bundle.before-20261109-135443`.
  - `air-runtime` copied over from the old bundle.
  - xattr cleared, root:wheel, ad-hoc `codesign --deep`.
  - `strings … | grep -c NVMTL_XE_VIBRANCY` = 2.
- `blurprobe`, `gu605-blur-diag.sh` and `gu605-collect.sh` installed (`*.before-20261109-135443`).
- Conf now: `/Library/GPUBundles/xemtl/nvrm610.conf` = `NVMTL_XE_VIBRANCY=1`.
- Note: `gu605-collect.sh` also writes `GU605/report.txt` to the EFI partition (its existing behaviour).

## Offline proof (section 3): reproduced and fixed
`docs/gu605-mac-logs/blur-verdict-20261109.txt`:

| setting | ws.selfsample-load | ws.selfsample-clear | ws.backdrop-load | ws.backdrop-clear |
|---|---|---|---|---|
| base | **FAIL** 1/20, 19 pre-pass copy | PASS 20/20 | **FAIL** 0/10, 10 other | PASS 10/10 |
| as-windowserver | FAIL (same) | PASS | FAIL (same) | PASS |
| noccs / nofc / noccs-nofc / no-dirtyonly / no-psoskip | FAIL (same) | PASS | FAIL (same) | PASS |
| sample-general-as-ws | FAIL (same) | PASS | FAIL (same) | PASS |
| **self-barrier**, **self-barrier-as-ws** | PASS 20/20 | PASS | PASS 10/10 | PASS |
| **self-off-as-ws** | PASS | PASS | PASS | PASS |
| **preset-as-ws** | PASS 20/20 | PASS 20/20 | PASS 10/10 | PASS 10/10 |

→ **`NVMTL_XE_SELF_SAMPLE=barrier` alone (or `off`) fixes the tests. `SAMPLE_GENERAL` alone does nothing.** The Load variants are the ones that fail; the Clear variants pass already in base.

Lines from `preset-as-ws-nvmtl.log`:
```
conf: NVMTL_XE_VIBRANCY=1 preset: self-sample barrier, sample layout GENERAL 1, surface match 1, tiled pad 1
vk: self-sampled targets: BARRIER - reads see the draws before them (NVMTL_XE_SELF_SAMPLE=barrier)
vk: sampled images are described in layout GENERAL (NVMTL_XE_SAMPLE_GENERAL=1)
```

Two things about the diag script on this Mac:
1. It ran through `launchctl submit` (to survive the 60 s bridge limit). That **relaunches** the job when it exits; a second run had started before I removed it. Use a plain detached job instead.
2. `/tmp/nvmtl.log` gets an owner (`baha`) that WindowServer can't append to. The new per-process `/tmp/nvmtl-WindowServer-*.log` fixes that.

## WindowServer, step 1
WindowServer was restarted at the login window by the runner (logout path, c5dc1dc/35961a5). Log lines:
- 14:05:04 "logged out (console root): restarting WindowServer"
- "157 -> 3189"
- 14:08:29 "confirmed: 4298 frames"
- 14:09:30 "flip confirmed: 416 flipped, 0 copied"

`xemtl-allow.txt` still `!WindowServer`. `iop_fail` 0.

`/tmp/nvmtl-WindowServer-*.log` (`docs/gu605-mac-logs/ws-nvmtl-vibrancy-20261109.log`):
```
conf: NVMTL_XE_VIBRANCY=1 preset: self-sample barrier, sample layout GENERAL 1, surface match 1, tiled pad 1
vk: self-sampled targets: BARRIER - reads see the draws before them (NVMTL_XE_SELF_SAMPLE=barrier)
vk: sampled images are described in layout GENERAL (NVMTL_XE_SAMPLE_GENERAL=1)
newTiledTextureWithDescriptor: 44x16 bpr 176 -> padded stand-in (pitch 192) resynced at every submission: ok
```
No "self-sampled target by IOSurface …" line appeared during this session.

## Next issue: the UI composites at ~55 fps while the panel and the cursor run at 240
The owner: the cursor is 240 Hz, but the UI feels like 60 fps while dragging; not as smooth as on NVIDIA.

`gu605-fps.sh 20` while dragging a window in circles (`docs/gu605-mac-logs/fps-drag-20261109.txt`):
```
panel refresh before/after: 240 Hz
shown/s = flips/s: 47 57 55 62 44 62 63 56 59 58 56 62 53 49 68 45 46 61 55 47   (copied 0)
average 55 frames/s shown, best second 68
XeGfxIntelFB: VBL: from pipe A's frame counter, 240.11 Hz (period 4164 us)
system_profiler: UI 1280x800 @ 240.00Hz
debug.nvaccel_xeflip_flip_us 3423, xeflip_map_us 6051
```

WindowServer's own frame stats over the same period:
```
WindowServer frames (5 s): 487, avg 18.5 ms (max 172.1) = commit->GPU 13.3 (max 170.3) + GPU 4.6 (max 39.6) + end->done 0.6 ms
WindowServer frames (5 s): 197, avg 38.4 ms (max 80.9) = commit->GPU 26.5 (max 63.5) + GPU 10.7 (max 38.6) + end->done 1.2 ms
```

Reading:
- The panel, the VBL and the flip path are at 240 Hz; nothing is copied.
- The limit is the plugin's **commit→GPU** delay: 13–26 ms average, against GPU time of 4–11 ms. A 4.17 ms frame budget is out of reach while a command buffer waits >10 ms to reach the GPU.
- WindowServer committed ~97 frames/s, but only ~55/s were shown. So frames also miss their flip slot.
- Suspects for the coordinator:
  - the submit path: G-SUB submit-ahead and the prologue submits, where "each WAS its own submit" for managed uploads;
  - ANV queue submission cost;
  - the flip's 3.4 ms per flip plus 6 ms map.

The barrier preset itself adds a self-dependency barrier per self-sampled pass. A/B against `NVMTL_XE_SELF_SAMPLE=off` (also passes offline) is worth one logout if the coordinator wants to rule that out.

## State left on the Mac
- Eco, the Intel desktop, flip on.
- `NVMTL_XE_VIBRANCY=1` in the conf.
- Automatic login on, `DESKTOP_LOGGED_IN=logout`: one logout per Eco boot brings WindowServer to the Intel GPU.
- Ultimate unaffected.
- Collect report: `docs/gu605-mac-logs/gu605-report-20261109-141235.txt`.
