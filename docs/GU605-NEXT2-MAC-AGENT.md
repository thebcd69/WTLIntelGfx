# GU605: 240 fps, the left USB-C monitor, Intel DDC (one plan, three phases)

**For:** the Claude session on the owner's GU605MY, working on branch **gu605-mac**.
**From:** the build agent, 2026-11-09.
**Package:** prerelease **`gu605-8df4a5b`**, built from **gu605-display `8df4a5b`**. It is the one package with everything:
- the frame-rate work;
- the i915 display path;
- Intel DDC.

Check its BUILDINFO.txt. The fps-only build from gu605-mtl is `gu605-d4b6fb9` (same plugin and XeGfxAccel).

**Inputs read:**
- `docs/GU605-MAC-VIBRANCY-20261109.md`
- `ws-nvmtl-vibrancy-20261109.log`
- `fps-drag-20261109.txt`
- `gu605-report-20261109-141235.txt`
- the NVIDIA DDC report: thebcd69/nvidia-macos-driver, `ddc-location`, `docs/DDC-RAPOR-20261105.md`

English for you. The **Sahip için** blocks are for the owner, in Turkish.

## 0. Rules (the owner's, unchanged)

**Files and branches**
- **Nothing is deleted.** Back up by renaming or moving (`X` → `X.before-<date>`), never `rm`.
- Push only to **gu605-mac**. No force-push. Never push to main, gu605-mtl, gu605-display or gu605-ddc.

**Disks, firmware, power**
- Never touch the Windows SSD or the boot order.
- Never add NVRAM boot entries.
- Never raise power limits.
- Do **not** edit the owner's OpenCore config. Phase 2 swaps one kext folder on the EFI and changes no config key.
- **EFI writes only with the owner's explicit OK, at the office, with him at the PC.**
  - Rename the old folder first.
  - Check `shasum -a 256` before and after.

**WindowServer**
- **Never restart WindowServer under a live session** (2026-11-05 13:11: black screen).
  - A WindowServer restart is: the owner logs out → console is `root` → `sudo launchctl kickstart -k system/com.apple.WindowServer` → the owner logs in.
  - Leave at least 5 minutes between restarts.
  - Before each one, check that `/Library/GPUBundles/xemtl-allow.txt` still starts with `!WindowServer`.
- **Phase 1 needs no logout for its A/B.** Every fps switch is live:
  - kernel: a `sysctl`;
  - plugin: `/Library/GPUBundles/xemtl/live.conf`, which WindowServer re-reads once a second.

**Running scripts**
- Long diagnostics run as a **plain detached job**: `sudo nohup bash … > /tmp/x.out 2>&1 &`.
- **Not `launchctl submit`**: it relaunches the job when it exits (2026-11-09).

**Kill switches: no boot-args** (the OpenCore config rewrites them). Use these instead:

| switch | what it turns off |
|---|---|
| `gu605.conf` `DISPLAY=adopt` | display path |
| `gu605.conf` `DDC=0`, or at once `sudo tools/xeddcprobe enable 0` | DDC |
| `gu605.conf` `FLIP_ASYNC=0`, or at once `sysctl debug.nvaccel_xeflip_async=0` | asynchronous flip |
| file `/Library/XeGfx/gu605/xeflip-async-off` | asynchronous flip, later boots |
| file `/Library/XeGfx/gu605/xeflip-off` | flip, later boots |
| file `/Library/XeGfx/gu605/kms-failed` | display path, later boots |
| file `/Library/XeGfx/gu605/autostart-off` | every automatic run |
| G-Helper **Ultimate** | everything: the iGPU is off and NullMoth runs as before |

## 1. What was found (frame rate)

`fps-drag`: panel and VBL at 240 Hz, flips/s 44-68, copied 0. WindowServer committed ~97 command buffers/s with:
- commit→GPU 13-26 ms;
- GPU 4.6-10.7 ms.

Causes found in the code, each with its own switch (all **off by default**):

| # | cause | evidence | switch |
|---|---|---|---|
| 1 | The swap was **synchronous on WindowServer's thread**: `performTransaction` → `nvXeFlip` first waits for the composite's GPU work (`nvSurfWaitReady`, commit→done ≈ 18-38 ms), then polls the panel's latch with `IOSleep(1)` steps (`xeflip_flip_us` 3337-3423 of a 4167 µs frame). So WindowServer could not composite the next frame meanwhile: roughly 1/(commit→done + latch) ≈ 50-60 fps. NVIDIA's path (NullMoth `nvAccelIopAsyncRun`) does both on a thread call and completes through `signalTransactionInterrupt`. | `xeflip_flip_us`, 55 shown/s | kernel `debug.nvaccel_xeflip_async=1` (live). The latch wait then follows the scanline (25 µs steps near vblank). |
| 2 | A **66 MB read-only wallpaper copied at every submission**. "host-pointer import refused … 0x17dab9000 (+66355200) is not all writable memory; that buffer is COPIED — resynced at every submission" (3840x2160 RGBA16, bpr 30720). Every frame that draws wallpaper memcpy'd 66 MB on the submit path, under the command-buffer class lock that WindowServer's commit also takes. WindowServer's `cpu_resource` report: 56% CPU. | WindowServer log, `cpu_resource.diag` | `NVMTL_XE_RO_ONCE=1` |
| 3 | **Every part that touches a host copy was held out of submit-ahead** (`hasHostCopies`). The worker then submitted it itself and waited for its fence before anything later in the queue could follow. The submit census showed AdvancePart=1081 of 4096 submissions. | "submit census … nvmtlAdvancePart=1081" | `NVMTL_XE_HOSTCOPY_AHEAD=1`: hold only when earlier GPU work may write that copy |
| 4 | Even unheld, a part the worker submits itself blocks the queue until its fence. NullMoth 2.7.1 publishes the submit first and kicks the ahead lane. | 2.7.1 `NVMTLEventSync.m` | `NVMTL_XE_WORKER_AHEAD=1` (2.7.1's code) |
| 5 | An empty last part with nothing to signal blocked the ahead lane (2.7.1 retires it there). | 2.7.1 | `NVMTL_XE_EMPTY_TAIL=1` (2.7.1's code) |
| 6 | `replaceRegion` submits and waits on the calling thread (175 of 4096 submissions). | "nvmtl_vk_image_write_region_level_layer=175" | existing `NVMTL_ASYNC_TEX_UPLOAD=1` (one logout, optional) |

Other items in the log:
- **Not the cause.** The barrier preset's cost is one by-region barrier per self-sampled draw. `NVMTL_XE_SELF_SAMPLE=off` also passes offline and is a later A/B if needed.
- **Watch.** "fc cache: miss fixed_frag_lph_cph … 2201.8 ms" (a 2.2 s shader compile in WindowServer) explains single long stalls. So do the 41 `surface_gpu_timeouts` (100 ms). These are not the steady 55 fps.

`NVMTL_XE_FPS=1` in `nvrm610.conf` sets the four plugin switches (2-5) in one line.

The vibrancy fix is now the **default** (self-sample barrier, GENERAL sampling, IOSurface match, tiled pad). The owner's `NVMTL_XE_VIBRANCY=1` line stays and is harmless.

**New measurements** (all in the package):
- WindowServer's plugin log now writes a second 5-s line, `WindowServer submit (5 s)`. It shows:
  - commit→first `vkQueueSubmit`;
  - `vkQueueSubmit` cost;
  - host-copy MB and ms;
  - read-only MB skipped;
  - parts held;
  - the worker's own submits;
  - the live switch values.
- `gu605-fps.sh` now also shows:
  - async flips/s and dropped frames;
  - how long WindowServer's thread was held per present (`xeflip_txn_us`, `_max`);
  - the latch wait;
  - the plugin's 5-s lines from the run.
- `tools/fpsprobe` is a WindowServer-shaped frame loop: three 2560x1600 IOSurfaces, a read-only 66 MB wallpaper, a read-only constant buffer, 60 window draws, the backdrop mip chain, and a `replaceRegion` per frame.
  - It runs paced at 240 Hz and unpaced.
  - Verdict: paced ≥ 228 frames/s and commit→GPU start < 2 ms.
  - `gu605-fps-diag.sh` runs it once per switch.

**Targets:**
- `gu605-fps.sh` while dragging: **shown/s ≥ 200 average, best ≥ 235**, copied 0;
- `WindowServer frames`: commit->GPU **< 2 ms** average;
- `WindowServer submit`: commit->first submit **< 1 ms**;
- `xeflip_txn_us_max` **< 1000 µs**.

## 2. Install (phase 1: no EFI)

1. Download `gu605-8df4a5b.zip` into a new empty folder. Check `shasum -a 256 gu605-8df4a5b.zip` = `04ac416bc62d8509b12faece60cd4367eb2ed80b74f84e6d49c3727be49f1d1b`.
2. Check SHA256SUMS: `shasum -a 256 -c SHA256SUMS`.
3. Check the BUILDINFO branch/commit (gu605-display `8df4a5b`).
4. `strings Library/GPUBundles/XeMTLDriver.bundle/Contents/MacOS/XeMTLDriver | grep -c -E 'NVMTL_XE_RO_ONCE|live.conf|WindowServer submit'` must be ≥ 3.
5. `sudo bash scripts/gu605-install.sh`. It:
   - backs everything up to `/Library/XeGfx/gu605-backup-<date>/` (moves, no deletion);
   - **AIR reader:** it must copy NullMoth's `air-runtime` ("AIR reader: copied from NullMoth's NVMTLDriver.bundle"). This package was built on a macOS 26 runner, and its own `userland/air-runtime/libLLVM.dylib` has minos 26.0, so it cannot load on 15.8.1. If the installer says "from the package", stop and copy NullMoth's by hand. Everything else was checked: the plugin and tools are minos 15.0, `libvulkan_intel` 14.4, libdrm 11.0, and the translator is bundled;
   - installs the four aux-collection kexts (**XeGfxAccel has the asynchronous flip**), the plugin bundle (carrying NullMoth's `air-runtime` over), the scripts, and the tools `fpsprobe` and `xeddcprobe`;
   - rebuilds the aux collection, keeping NullMoth's kexts;
   - keeps the existing `gu605.conf`.

   XeGfxIntelFB in this package has the display path and DDC. With `DISPLAY=adopt` (the owner's current conf) and the old XeGfx.kext on the EFI, the display path stays inert. The DDC bus is published but answers "no device" while the panel has the picture.
6. Keep `/Library/GPUBundles/xemtl/nvrm610.conf` as it is (`NVMTL_XE_VIBRANCY=1`).
7. Restart in **Eco**. The automatic login and `DESKTOP_LOGGED_IN=logout` bring WindowServer to the Intel GPU at the first logout, as on 2026-11-09.
8. Check after login:
   - `sysctl debug.nvaccel_xeflip_async` exists (value 0);
   - WindowServer's plugin log (`ls -t /tmp/nvmtl-WindowServer-*.log | head -1`) has:
     - `vk: self-sampled targets: BARRIER - reads see the draws before them (NVMTL_XE_SELF_SAMPLE=barrier)` (the preset line still sets it);
     - `fps knobs at start: RO_ONCE 0, HOSTCOPY_AHEAD 0, WORKER_AHEAD 0, EMPTY_TAIL 0 (/Library/GPUBundles/xemtl/live.conf overrides them while this process runs)`;
     - `host copy: 66355200 bytes at 0x… are read-only caller memory (NVMTL_XE_RO_ONCE=1 copies them only when they change)`. This shows cause 2 is present.
   - Vibrancy is still fixed. If not, stop and send the log: the default changed from the preset only in name.

## 3. Phase 1: 240 fps on the internal panel (one boot, no logout for the A/B)

All steps run with the owner dragging a window in circles while `sudo bash /Library/XeGfx/gu605/gu605-fps.sh 20` runs. Save each output as `fps-<step>.txt`.

### 3.0 Offline first

Run `sudo nohup bash /Library/XeGfx/gu605/gu605-fps-diag.sh 5 > /tmp/fpsdiag.out 2>&1 &`. It takes about 2 minutes and nothing on screen changes. Run it **after** the desktop step (a Metal client before it blocks arming, 2026-11-05).
- Expected: `base` FAILs (paced < 228 frames/s or commit->GPU ≥ 2 ms). Its plugin line shows `host copies` of hundreds of MB per 5 s and `parts held` > 0.
- `ro-once` shows `read-only skipped` ≈ the same MB, and `host copies` near 0.
- `fps-preset` is the best row.
- If `base` already passes, fpsprobe does not reproduce WindowServer's case. Continue with 3.1-3.4 anyway: WindowServer decides.

### 3.1 Baseline

`gu605-fps.sh 20` while dragging. Expected: as on 2026-11-09 (~55 shown/s). The new lines show `WindowServer's thread held … us at most` in the thousands, and a `WindowServer submit (5 s)` line with large `host copies`.

### 3.2 Asynchronous flip (kernel, live)

1. Run `sudo sysctl -w debug.nvaccel_xeflip_async=1`.
2. Check the kernel log: `log show --last 2m --predicate 'process == "kernel" AND eventMessage CONTAINS "xeflip"'` has `xeflip: asynchronous flips ON (debug.nvaccel_xeflip_async=1): WindowServer's thread no longer waits for the GPU or the latch`.
3. Run `fps.sh 20`. Expected:
   - `async/s` ≈ `flips/s`, `drop/s` small;
   - `held … at most` < 1000 µs;
   - `latch … us (async)` < 4200;
   - shown/s up. The size depends on cause 2.

**Stop if:** screen frozen or black, a garbage frame that stays, or `xeflip: 3 asynchronous flips failed in a row` (it turns itself off).
**Undo:** `sudo sysctl -w debug.nvaccel_xeflip_async=0`, at once. Note it and continue with 3.3 without it.

### 3.3 Plugin switches, live, one at a time

Write `/Library/GPUBundles/xemtl/live.conf` (root, 644). Give the full set each time, adding one line per step:

| step | live.conf | expected in WindowServer's log within ~5 s |
|---|---|---|
| a | `NVMTL_XE_RO_ONCE=1` | `live: NVMTL_XE_RO_ONCE 0 -> 1 (live.conf)`; next `WindowServer submit (5 s)`: `read-only skipped` ≈ the old `host copies` MB, `host copies` near 0, `commit->first submit` down |
| b | + `NVMTL_XE_HOSTCOPY_AHEAD=1` | `live: NVMTL_XE_HOSTCOPY_AHEAD 0 -> 1`; `parts held for the worker` → ~0, `worker's own submits` down |
| c | + `NVMTL_XE_WORKER_AHEAD=1` | `live: NVMTL_XE_WORKER_AHEAD 0 -> 1`; commit->GPU down further if c was a factor |
| d | + `NVMTL_XE_EMPTY_TAIL=1` | `live: NVMTL_XE_EMPTY_TAIL 0 -> 1` |

After each step, run `fps.sh 20` while dragging. Also check:
- step a: text and wallpaper look right. Change the wallpaper once: the new one must appear (it is a new buffer). Glyphs in Terminal and Safari must be complete.
- step b: no stale or missing pieces in windows, Dock or menu bar.

**Stop if:** missing or stale content, a wallpaper that does not change, a WindowServer crash or beachball.
**Undo:** `sudo mv /Library/GPUBundles/xemtl/live.conf /Library/GPUBundles/xemtl/live.conf.off-<time>` (not deleted). The log then says `live: … -> 0 (start value, not in live.conf)`. Or set that one line to `=0`.

**Note:** live.conf only exists to make the A/B logout-free. It is read only in WindowServer (fpsprobe runs keep their own environment).

### 3.4 Make it permanent (one logout)

1. Add `NVMTL_XE_FPS=1` to `nvrm610.conf` for the switches that helped. If one hurt, list the other three one by one instead.
2. Add `FLIP_ASYNC=1` to `/Library/XeGfx/gu605/gu605.conf` if 3.2 helped.
3. Rename `live.conf` → `live.conf.off-<time>`.
4. One WindowServer restart, logout way (section 0).
5. Expected in the log:
   - `conf: NVMTL_XE_FPS=1 preset: read-only copies once 1, host-copy parts ahead 1, worker submits ahead 1, empty tail 1`;
   - `fps knobs at start: RO_ONCE 1, HOSTCOPY_AHEAD 1, WORKER_AHEAD 1, EMPTY_TAIL 1 …`.
6. The asynchronous flip stays on until reboot. From the next boot, `gu605-auto.sh` turns it on after `flip confirmed` and logs `flip: asynchronous ON`, then after a minute `asynchronous flip confirmed: … flips, … dropped, WindowServer held … us at most`. If WindowServer restarted or failures grew in that minute, it writes `asynchronous flip OFF for later boots` and the file mark `xeflip-async-off`.
7. Run `fps.sh 20` while dragging one last time.

### 3.5 Optional: asynchronous texture uploads (one logout)

Only if the fpsprobe row `fps-preset+async-upload` beat `fps-preset`, and the target is not met:
1. Add `NVMTL_ASYNC_TEX_UPLOAD=1` to `nvrm610.conf` and restart WindowServer the logout way.
2. Expected: `vk: async texture upload ON (NVMTL_ASYNC_TEX_UPLOAD)`.

**Stop/undo:** remove the line (rename the conf to a copy and write it without it), logout.

### Phase 1 result to send

- `fps-*.txt`, `/tmp/fpsdiag.out` and the `fpsdiag-*/summary.txt` folder;
- WindowServer's plugin log;
- the `WindowServer frames/submit` lines per step;
- the owner's verdict ("NVIDIA kadar akıcı mı?").

**If the target is missed**, the deciding lines are:
- `WindowServer submit`: `vkQueueSubmit … ms` (high = ANV/i915 execbuffer cost) against `commit->first submit`;
- `WindowServer frames`: `GPU` (high = GPU-bound: GPU time > 4.1 ms per composite);
- `held … at most`.

> **Sahip için (aşama 1):** Bu aşamada EFI'ye dokunulmuyor; yalnızca bir yeniden başlatma, sonra testler canlı
> yapılıyor (oturum kapatmadan). Her adımda pencereyi 20 saniye daire çizerek sürükleyin. Lütfen her adımdan sonra
> söyleyin: (1) sürükleme 60 gibi mi yoksa NVIDIA kadar akıcı mı? (2) eksik yazı, eski görüntü, yanıp sönme var mı?
> (3) Masaüstü resmini bir kez değiştirin: yenisi geldi mi? Bir sorun görürseniz Claude o ayarı hemen geri alır.

## 4. Phase 2: the left USB-C PG27AQDM as its own display

This phase needs the **new XeGfx.kext on the KIOXIA EFI** (i915's display code, `xg_kms.c`) **plus `DISPLAY=kms`**. It needs the owner's explicit OK, at the office.

### 4.1 Before

- Phase 1 done. Note its final `gu605.conf` and `nvrm610.conf`.
- `sudo bash /Library/XeGfx/gu605/gu605-collect.sh` for a "before" report.

> **Sahip için:** Bu aşamada KIOXIA EFI'deki tek bir klasör (XeGfx.kext) değişecek; eskisi silinmeden yeniden
> adlandırılacak. OpenCore ayarlarına (config.plist) dokunulmuyor. Onay veriyor musunuz? (evet / hayır) Ofiste ve
> bilgisayarın başında olduğunuzda yapılacak. Açılışta harici monitör **takılı olmasın**; girişten sonra sol USB-C'ye
> takacağız.

### 4.2 EFI write (with OK)

1. Mount the KIOXIA EFI the way the earlier sessions did.
2. `shasum -a 256 <EFI>/EFI/OC/Kexts/XeGfx.kext/Contents/MacOS/XeGfx` → record.
3. `mv <EFI>/EFI/OC/Kexts/XeGfx.kext <EFI>/EFI/OC/Kexts-backup/XeGfx.kext.before-<date>`. Keep it **outside** `EFI/OC/Kexts`, so OpenCore cannot pick up two bundles. Create `Kexts-backup` if missing.
4. `ditto <pkg>/EFI/OC/Kexts/XeGfx.kext <EFI>/EFI/OC/Kexts/XeGfx.kext`.
5. `shasum` the new binary and compare it to the package's SHA256SUMS line.
6. Unmount. Write both hashes into the report.
7. Set `DISPLAY=kms` in `/Library/XeGfx/gu605/gu605.conf`. Keep `PANEL_HZ=240`.

### 4.3 Boot (monitor unplugged), Eco

The runner's stage 4, in `logs/boot-*/summary.txt` and `stages/stages.log`, must show:
- `display: i915 KMS requested (preflight ok); env: … xegfx.kms=1`
- `stage 4 passed`
- `display: i915 owns the display`
- either `display: eDP-1 240 …` (test then commit), or nothing more if the panel was already at 240

Kernel: `XeGfxIntelFB: display path: i915 owns the display; boot mode 0x… 2560x1600 …`, then `display path: hardware cursor off …`. The cursor is now software (WindowServer composites it). **Watch for a cursor trail.**

**Stop if:**
- `display: DISPLAY=kms, but …: adopt path`: send the line; nothing changed, the machine is as in phase 1.
- `stage 4 FAILED`: the runner writes `kms-failed` and later boots use the adopt path by themselves.
- A black panel at boot: power button. The next boot sees `gu605-pending=1`, does nothing, and writes `autostart-off`. Then rollback R2.

After login (desktop on Intel after the logout step), run `sudo bash /Library/XeGfx/gu605/gu605-display.sh dump`. It must show `connector eDP-1 … status connected` with an active CRTC at ~240 Hz.

### 4.4 Plug the PG27AQDM into the LEFT USB-C (after login)

Expected within ~2 s:
- kernel `display path: DP-<n> has the picture (<k> modes, 2560x1440, EDID 256 B); the panel is off`;
- `display path: connect interrupt #<m> (monitor connected), delivered`;
- the panel goes dark and the monitor shows the desktop. WindowServer re-reads modes (System Settings → Displays: PG27AQDM).

Then:
- `gu605-display.sh dump`: a `DP-<n>` CRTC at 2560x1440 ~240 Hz.
- `ioreg -r -w0 -c XeGfxIntelFramebuffer -d1 | grep XeGfxKmsConnection` → `DP-<n>`.
- `gu605-fps.sh 20` while dragging on the monitor.

**Stop if:** the monitor stays black for 30 s, or a frozen picture.
1. Unplug. Expected `display path: monitor gone, the panel is back at <mHz>`, with the panel picture.
2. If flips are on and the monitor showed stale or garbage frames, try live `sudo sysctl -w debug.nvaccel_xeflip=0` (copy path) once, then plug in again. i915 owns the planes now; see the design doc's §10 on PLANE_SURF.
3. If nothing comes back, power button, then rollback R2.

Unplug and replug once more: both directions must work. The owner never uses both displays at once; the panel being off while the monitor is in is by design.

> **Sahip için (aşama 2):** Monitörü sol USB-C'ye taktığınızda iç ekran kapanır, görüntü monitöre geçer (aynı anda
> ikisi yok, bu bilerek böyle). Çıkarınca iç ekran geri gelmeli. Söyleyin: monitörde görüntü geldi mi, kaç Hz
> görünüyor (Sistem Ayarları → Ekranlar), fare imleci iz bırakıyor mu, sürükleme akıcı mı?

### R2. Phase 2 rollback

1. `DISPLAY=adopt` in `gu605.conf`. The next boot takes the adopt path, even with the new XeGfx.kext.
2. If the new XeGfx.kext itself is the problem, only with the owner's OK:
   - move `EFI/OC/Kexts/XeGfx.kext` → `Kexts-backup/XeGfx.kext.kms-<date>`;
   - move `Kexts-backup/XeGfx.kext.before-<date>` back to `EFI/OC/Kexts/XeGfx.kext`;
   - check the hash against the one recorded in 4.2.
3. If macOS does not come up in Eco: G-Helper **Ultimate** (iGPU off, NullMoth as before), then step 1. Or the Windows-side agent puts the old folder back from Windows (EFI only, never the Windows SSD's own partitions).

## 5. Phase 3: Intel DDC for BetterDisplay (the monitor on the left USB-C)

Precondition: phase 2's external picture works, and DDC/CI is on in the monitor's OSD.

How the NVIDIA lessons (2026-11-05) are applied in this XeGfxIntelFB:
1. **pendingPowerState.** IOGraphics answers `kIOReturnOffline` unless `pendingPowerState != 0`, and IOGraphicsLib hides that error.
   - XeGfxIntelFB is an IONDRVFramebuffer, which keeps IOFramebuffer's `setPowerState`.
   - It still sets `pendingPowerState = 1` once before start, as NVRMFB 47039af does.
   - `XeGfxDdc` shows the power state.
2. **Unique registry path, IODisplayLocation.**
   - One XeGfxIntelFramebuffer under GFX0 drives whichever display has the picture (one at a time), so its path is unique.
   - The external connection reports "not built-in" (`kConnectionFlags`), so IODisplay matches AppleDisplay, not AppleBacklightDisplay.
   - `xeddcprobe` checks both: every display's IODisplayLocation resolves to itself, and to a framebuffer with an I2C bus.
3. **No boot-arg switch.** `XeGfxDdcEnable`:
   - `sudo tools/xeddcprobe enable 0|1`, at once;
   - `gu605.conf` `DDC=` at every boot.

Steps (monitor plugged, picture on it):
1. `ioreg -r -w0 -c XeGfxIntelFramebuffer -d1 | grep -E 'XeGfxDdc|IOFBI2CInterface'`
   → `XeGfxDdc = "bus published; active while an external monitor has the picture | on, power state <n>…"` and `IOFBI2CInterfaceIDs = (0)`.
2. `/Library/XeGfx/gu605/tools/xeddcprobe`. Expected:
   - `framebuffer XeGfxIntelFramebuffer … I2C bus 0 present`;
   - `display AppleDisplay on XeGfxIntelFramebuffer … resolves to this display (unique); that framebuffer's I2C bus present`;
   - `VCP 0x10 = <cur> / 100`.

   If a reply line starts `51 82 01 10`, that is the request echoed back (IO did not run): go to step 3.
3. `xeddcprobe diag`. Expected `open 0x0`, `acquire 0x0`, `io 0x0 result 0x0 replyBytes 11: 6e 88 02 00 10 …`, `release 0x0`.
   - `io 0xe00002d7 (kIOReturnOffline …)` = the power-state lesson: send `XeGfxDdc` and stop.
   - `io 0x0` with an error in `XeGfxDdc` (`error 0x… at send/reply (i915 <errno>)`): -60 = AUX timeout, -5 = NAK. Check DDC/CI in the OSD.
4. `xeddcprobe 10 30` → the monitor visibly dims (the owner confirms). Then `xeddcprobe 10 50` → back.
5. BetterDisplay (4.3.7): the PG27AQDM's brightness slider in hardware (DDC) mode. The owner moves it, and `XeGfxDdc`'s `n … ok …` counters grow.

**Stop if:**
- an `io` other than 0;
- `resolves to ANOTHER display`;
- BetterDisplay offers software dimming only;
- anything odd on the monitor.

**Undo:** `sudo /Library/XeGfx/gu605/tools/xeddcprobe enable 0` (at once), and `DDC=0` in gu605.conf for later boots. Nothing else changes.

> **Sahip için (aşama 3):** BetterDisplay'de PG27AQDM'in parlaklığını oynatın. Monitörün **kendi** parlaklığı
> değişmeli (NVIDIA'daki gibi). Söyleyin: değişti mi, gecikme var mı?

## R. Overall rollback

| what | how |
|---|---|
| plugin switches | rename `live.conf`; remove the `NVMTL_XE_FPS` line from a copy of `nvrm610.conf` and put that copy in place; one logout |
| asynchronous flip | `sudo sysctl -w debug.nvaccel_xeflip_async=0`, and `FLIP_ASYNC=0`, or the file `xeflip-async-off` |
| everything from this package | the kexts and bundle in `/Library/XeGfx/gu605-backup-<date>/` (`*.replaced`, `AuxiliaryKernelExtensions.kc.before`): move them back, `kcditto`, restart. The previous package's `gu605-install.sh` also works |
| display path | R2 |
| DDC | `xeddcprobe enable 0`, `DDC=0` |
| all of it | G-Helper Ultimate |

## 6. Send back (commit to gu605-mac, `docs/gu605-mac-logs/`)

- `GU605-MAC-NEXT2-<date>.md`: the per-step results, the owner's words, the hashes from 4.2.
- the `fps-*.txt` files, `fpsdiag` `summary.txt`, WindowServer's plugin log;
- `gu605-collect.sh` reports (after phase 1 and after phase 2);
- the `gu605-display.sh dump` outputs, `xeddcprobe` and `diag` outputs, the `XeGfxDdc` line;
- any `cpu_resource.diag` of WindowServer.
