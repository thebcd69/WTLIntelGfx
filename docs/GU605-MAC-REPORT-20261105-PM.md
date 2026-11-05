# GU605 Eco: Mac-side report, 2026-11-05 afternoon (branch `gu605-mac`)

From: the Mac-side session, working on the owner's GU605MY over the device bridge.
For: the coordinator / build agent.
Follows `GU605-NEXT-MAC-AGENT.md` sections 1 and 2. Section 3 (240 Hz, USB-C) was not started; it needs an EFI write.

## Summary
- **Intel desktop: works.**
  - Metal on: system_profiler shows "Metal: Supported", the cursor is a hardware cursor and smooth, the wallpaper is back.
  - Zero-copy flip confirmed every time; `iop_fail` stays 0.
- **Vibrancy bug: still there.** No `nvrm610.conf` knob changes it. The offline `blurprobe` passes every CA/CI/MPS/Metal test.
  - **Symptom 1:** translucent materials are fully see-through and unblurred. In System Settings the Finder window and its text show sharply through the sidebar.
  - **Symptom 2:** on the lock screen the clock, date and "password required" text sit on **solid black rectangles**.
  - **Symptom 3:** translucent areas flicker while a window is dragged.
- **Desktop-step flow:** fixed 4 bugs (below). The logout path now works on macOS 15.

## Commits on gu605-mac (after 5286960)
| commit | what |
|---|---|
| 8949a20 | merge origin/gu605-mtl (blurprobe, blur diag, fps, automatic-login desktop step) |
| 4393c01 | gu605-blur-diag: `NVMTL_ENABLE=1`. Without it, every blurprobe run ended "no Intel Metal device": the plugin hands a device only to allow-listed names (and mtlprobe/nvmtltest) |
| d7c04e5 | gu605-desktop: refuse only when an NVAccel **instance** exists. Before, it refused when the kext was merely loaded. After login the NVAccel kext was loaded with no instance; NullMoth registers `debug.nvaccel*` only in `NVAccel::start()` |
| a7d331a | gu605-desktop: `kern.boottime` parse. The greedy `.*sec =` matched `usec = M`, giving "logged in 1793154461s after boot", so automatic login was never recognised. A negative login time (clock step after the Dock started) is clamped to 0 |
| c5dc1dc | gu605-auto: **macOS 15 keeps WindowServer across a logout** (logged out at 13:05, still pid 157). In logout mode the runner now restarts WindowServer itself once the console is back at the login window (`root`/`loginwindow`) |
| 35961a5 | gu605-auto: `killall WindowServer` was ignored (pid unchanged); falls back to `launchctl kickstart -k system/com.apple.WindowServer` |

Verified at 13:22, 13:32 and 13:42:
- log line: "logged out (console root): restarting WindowServer"
- log line: "logout seen: 157 -> 2024"
- then "confirmed: 4069 frames … flip confirmed: 2434 flipped, 0 copied"

`gu605.conf` on the Mac: `ECO_LEVEL=desktop`, `DESKTOP_LOGGED_IN=logout` (see "Do not" below), DESKTOP_* keys added. The owner has automatic login ON.
Script backups:
- `/Library/XeGfx/gu605-scripts-before-20261105-124512`
- `/Library/XeGfx/gu605-scripts-before-20261105-131903`

## Section 1: vibrancy
### blurprobe (offline, test process, Intel plugin, NMXAccel)
Run as the user with `NVMTL_ENABLE=1 NM_TEST_PLUGIN=…XeMTLDriver`.

In `base`, every test passes except the two CA tests. Those fail at setup, `renderer 0` (`CARenderer rendererWithMTLTexture:` returned nil) **only when run after the MPS/CI tests in the same process**:
```
metal.rtt_sample PASS 0/40 off · metal.genmips PASS · metal.render_mip PASS · metal.compute_tg PASS
ci.gaussian PASS · mps.gaussian PASS · ca.layerblur FAIL setup: renderer 0 · ca.backdrop FAIL setup: renderer 0
```
Each CA test alone (`BLURPROBE_ONLY=ca.backdrop` / `ca.layerblur`) passes under every setting tried:
```
[base]                 ca.backdrop PASS 30 blurred, 0 unblurred (see-through), 0 in between; detail 27.2 -> 1.2, saturation 95.4 -> 244.4
[base]                 ca.layerblur PASS 30 blurred, 0 unblurred, detail 27.2 -> 0.0
[NVMTL_NO_FBFETCH=1]   both PASS (as WindowServer)
[INTEL_DEBUG=noccs]    both PASS
[INTEL_DEBUG=nofc]     both PASS
```
→ By the doc's table: "everything passes → WindowServer-only (drawable/IOSurface path)".

Side bug: running `ca.*` after `mps`/`ci` in one process makes `CARenderer` nil. That is blurprobe ordering, not the WindowServer bug.

### WindowServer A/B (one knob per WindowServer start, through `/Library/GPUBundles/xemtl/nvrm610.conf`)
| conf | WindowServer log | owner |
|---|---|---|
| `INTEL_DEBUG=noccs` | `conf: 1 knob(s) read` | same: see-through, black boxes, flicker |
| `NVMTL_FBFETCH_WS=1` | `framebuffer fetch ON - render passes declare their colour attachments as input attachments` | same |
| `NVMTL_FBFETCH_WS=1`, `INTEL_DEBUG=noccs,nofc`, `NVMTL_NO_DIRTYONLY=1`, `NVMTL_NO_PSO_SKIP=1` | `conf: 4 knob(s) read`, fetch ON | same |

The conf is now moved aside (`nvrm610.conf.allknobs-134439`). The next WindowServer start runs with defaults.

### What WindowServer's plugin log says
The plugin cannot write `/tmp/nvmtl.log`: it is owned by `baha` because a user process created it first. So WindowServer logs to `/private/var/folders/zz/zyxvpxvq6csfxvn_n00000b000002r/T/WindowServer/nvmtl.log` (root only). Copies:
- `ws-nvmtl-T.log`: pid 2024, defaults + noccs
- `ws3.log`: pids 2024, 3362 (fetch ON), 4699 (all knobs)

Per WindowServer start, nothing is refused or skipped and the translator produced every shader. The relevant lines:
```
vk: framebuffer fetch off in WindowServer (NVMTL_FBFETCH_WS=1 opts in)                       (default)
vk: framebuffer fetch is off and a pipeline reads [[color(n)]] - those reads see the dummy image, not an unbound descriptor
vk: dummy images ZEROED ... a declared-but-unbound texture now reads opaque black
vk: fragment module declares descriptor binding 192 — [[color(n)]] framebuffer fetch ..., a band this device's set layout does not have; built   (x8, also with fetch ON)
vk: framebuffer fetch draw of MTLPrimitiveType 1 is drawn whole - overlapping primitives in it can read stale colour   (fetch ON)
vk: G11 a Load pass began on an image resting in layout 8 (declared TRANSFER_SRC)
self-sampled target 2560x1600 fmt 80: later passes read a copy taken before the pass
self-sampled target 64x64 fmt 80 / 468x228 / 436x200: later passes read a copy taken before the pass
newTextureWithDescriptor:iosurface:... (2560x1404 fmt 80)   [logged 8192 times]   <- a new texture for a 2560x1404 IOSurface every frame
nvk-reason: W anv_descriptor_set.c:1200: VK_ERROR_OUT_OF_POOL_MEMORY   (x19; then "descriptor pool 2 opened"; no CEILING / "draws are being skipped")
buffer texture: REFUSED, bytesPerRow 176 is not a multiple of 32 B -> copied texture (44x16)
encoder: clear-only pass (0.00 0.00 0.00 0.00)   (many)
```

### Reading (for whoever fixes it in `plugin/`)
1. **Black rectangles on the lock screen ≈ the "dummy image reads opaque black" signature.**
   - A backdrop/vibrancy source is bound as the zeroed dummy, or reads one.
   - With fetch off, `[[color(n)]]` reads are the dummy. With fetch on they should not be, but the picture did not change.
   - So the dummy most likely comes from a **sampled** texture slot rather than fetch.
   - First suspect: the **backdrop capture texture**. That is likely the 2560x1404 IOSurface re-wrapped every frame, or the "self-sampled 2560x1600 copy taken before the pass".
   - The backdrop must see what was drawn *earlier in the same pass/command buffer*. A copy taken **before** the pass misses everything drawn into the target in that pass. During a drag that content changes every frame, which fits the flicker.
   - That may be fine on NVK only because of ordering/timing.
2. **`G11 Load pass on layout 8 (declared TRANSFER_SRC)`**: the per-image layout disagreement from suspect (1) in the doc. It is visible, but `noccs`/`nofc` did not change the picture, so aux/fast-clear state is not the cause.
3. The `OUT_OF_POOL_MEMORY` lines look like the expected retry noise of `nvmtl_snapshot_set_md` walking full pools before it opens a new one (no CEILING line). Worth a counter, but not the first suspect.
4. `blurprobe`'s CA tests render into a managed texture. Proposed new test in blurprobe: render the backdrop into an **IOSurface-backed** target that also holds content drawn earlier in the same command buffer, as WindowServer does. If that fails offline, it can be iterated without WindowServer restarts.

Owner workaround meanwhile: System Settings → Accessibility → Display → **Reduce transparency**. Materials become opaque, so the black boxes, the see-through and the flicker should disappear (not tried yet).

## Section 2: desktop step with automatic login
- `auto` mode never fired, because of the boottime parse bug (fixed in a7d331a, installed). After the fix it has not had a clean automatic-login boot yet.
- **Do not restart WindowServer under a live session on this machine.** At 13:11 `launchctl kickstart -k` with the owner logged in gave a **black screen**: the new WindowServer showed only 3 frames, the old session's loginwindow and Dock stayed, and the owner had to hold the power button.
  - At 11:49 a `FORCE=1` restart under a session did work. That time `killall` ended the session first.
  - So `auto` mode (the restart after 60 s idle under the automatic-login session) is risky until it is proven to end the session first, e.g. with `launchctl bootout gui/<uid>` or loginwindow's own logout before the WindowServer restart.
  - For now the Mac is set to `DESKTOP_LOGGED_IN=logout`: the owner logs out once per Eco boot, and the runner (c5dc1dc/35961a5) restarts WindowServer at the login window. That path worked 3 out of 3 times.
- Also noted:
  - `launchctl kickstart -k system/org.thebcd69.gu605.auto` during the runner's 3-minute confirm leaves `gu605-pending=1`. The next run then turns automatic runs OFF. Wait for the `end:` line before re-running.
  - Arming fails if a Metal client (blurprobe) ran before step 3 in that boot: "found_framebuffer(.Display_boot index 4) — count went 1 -> 1 … Stopping", `debug.nvaccelfb` stays 0, "not armed". Run blur tests **after** the desktop step.

## State left on the Mac
- Eco boots: XeGfxIntelFB 5286960 (hw cursor), stages 1–7. The desktop step arms after login. One logout brings WindowServer to the Intel GPU (3 min confirm, flip).
- `nvrm610.conf` moved aside (defaults). Automatic login ON.
- Ultimate is unaffected.
