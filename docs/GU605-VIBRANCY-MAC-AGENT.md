# GU605 — vibrancy: settle it in one Mac session (2026-11-05 night)

For: the Claude session on the owner's GU605MY, branch **gu605-mac**.
From: the build agent. Code: **gu605-mtl c4fd275 or later**. Prerelease: the newest `gu605-<sha>` built from gu605-mtl,
**92697ce or later**; check its BUILDINFO. c4fd275 itself is `[skip ci]` and only adds the barrier to indirect draws.
Inputs read: `docs/GU605-MAC-REPORT-20261105-PM.md` and both WindowServer plugin logs.
English for you; **Sahip için** blocks are for the owner, in Turkish.

## 0. Rules (owner's, unchanged)

- **Nothing is deleted**: backups by renaming/moving (`X` → `X.before-<date>`), never `rm`.
- **EFI writes only with the owner's explicit OK** plus a `shasum -a 256` check. **This session needs no EFI write, no
  kext and no kernel-collection change**: only the Metal plugin bundle, one conf file, blurprobe and two scripts.
- Push only to **gu605-mac**. No force-push; never main, gu605-mtl, gu605-display or gu605-ddc.
- **Never restart WindowServer under a live session** (13:11 black screen). Each step below is: the owner logs out, the
  console is `root`, then you run `sudo launchctl kickstart -k system/com.apple.WindowServer`, then the owner logs in.
- **At least 5 minutes between WindowServer restarts.** Before each restart, check that
  `/Library/GPUBundles/xemtl-allow.txt` still starts with `!WindowServer`. The crash-loop guard rewrites it to
  `-WindowServer` after more than 2 starts in 10 minutes; if it did, rename the guard's file and restore the line.
- Escape hatches: move the conf aside (step R), G-Helper Ultimate, or the boot-args `-xegfxnoauto`, `-xegfxoff`,
  `-xeintelfboff`.

## 1. What the logs and the code say

| log line (WindowServer) | finding | knob |
|---|---|---|
| `self-sampled target 2560x1600 / 64x64 / 436x200 / 468x228: later passes read a copy taken before the pass` | Core Animation reads its target inside the pass that draws it (after `memoryBarrier(renderTargets)`). The plugin served a **copy taken before the pass**, so a backdrop missed everything drawn earlier in that pass. Lock screen: the pass starts from a clear → **black rectangles**. Dragging: last state → **flicker**. Otherwise **see-through**. NullMoth 2.7.1 has no such copy (it reads the attachment directly) | `NVMTL_XE_SELF_SAMPLE=barrier` |
| (same, Clear passes) | without framebuffer fetch the pass had **no self-dependency and no barrier**: undefined on ANV (render cache not coherent with the sampler) | barrier mode declares the self-dependency always |
| (same, 64x64 etc.) | a read of **another mip level** of the same texture counted as self-sampling and got the level-0-only copy: blur chains (level n from n-1) never blurred | barrier mode checks the level |
| `newTextureWithDescriptor:iosurface 2560x1404` ×8192 | WindowServer re-wraps its IOSurfaces every frame; a read through a *second wrapper* of the attachment's surface was not seen as self-sampling (object compare), so it got no barrier | `NVMTL_XE_SELF_SURF=1` |
| `memoryBarrier...` inside a pass | became a full ALL_COMMANDS barrier, which Vulkan does not permit inside a render pass | barrier mode: the self-dependency's barrier |
| `G11 a Load pass began on an image resting in layout 8` | layouts disagree (PREINITIALIZED vs declared). Sampled descriptors said SHADER_READ_ONLY while attachments live in GENERAL | `NVMTL_XE_SAMPLE_GENERAL=1` |
| `buffer texture: REFUSED, bytesPerRow 176 … copied texture (44x16)` | a one-time copy that goes **stale** when WindowServer rewrites those bytes | `NVMTL_XE_TILED_PAD=1` (stand-in padded to 64 B, refilled every submission) |
| `dummy images ZEROED … reads opaque black` | a one-time init line. The dummy is cleared to 0,0,0,0. Not a cause by itself | — |
| `binding 192 … a band this device's set layout does not have` | a static check, printed at most 8 times; set 3 is separate | — |
| `storage texture 0 is declared … dummy` | `sum_rgba_*` declares slots 0–1 as storage; slot 0 is read only. Harmless | — |
| `VK_ERROR_OUT_OF_POOL_MEMORY` ×19 | retries before a new descriptor pool opens; no "draws are being skipped" | — |
| `encoder: clear-only pass` | normal (Core Animation clears its offscreens) | — |

All new knobs are **off by default**: the bundle without a conf behaves exactly as today. The one-line preset
`NVMTL_XE_VIBRANCY=1` turns on `SELF_SAMPLE=barrier`, `SAMPLE_GENERAL=1`, `SELF_SURF=1` and `TILED_PAD=1`. A knob
written on its own line wins over the preset.

## 2. Install (no kext, no EFI)

```bash
P=~/Downloads/gu605-<sha>            # the unzipped prerelease
GB=/Library/GPUBundles; D=$(date +%Y%m%d-%H%M%S)
grep -E 'branch|commit' $P/BUILDINFO.txt          # gu605-mtl, 92697ce or later
sudo mv $GB/XeMTLDriver.bundle $GB/XeMTLDriver.bundle.before-$D
sudo ditto $P/Library/GPUBundles/XeMTLDriver.bundle $GB/XeMTLDriver.bundle
sudo ditto $GB/XeMTLDriver.bundle.before-$D/Contents/MacOS/air-runtime $GB/XeMTLDriver.bundle/Contents/MacOS/air-runtime
sudo xattr -cr $GB/XeMTLDriver.bundle; sudo chown -R root:wheel $GB/XeMTLDriver.bundle; sudo chmod -R a+rX,go-w $GB/XeMTLDriver.bundle
sudo codesign -s - -f --deep $GB/XeMTLDriver.bundle
sudo install -m 755 $P/tools/blurprobe /Library/XeGfx/gu605/tools/blurprobe
for f in gu605-blur-diag.sh gu605-collect.sh; do sudo cp -p /Library/XeGfx/gu605/$f /Library/XeGfx/gu605/$f.before-$D; sudo install -m 755 $P/scripts/$f /Library/XeGfx/gu605/$f; done
strings $GB/XeMTLDriver.bundle/Contents/MacOS/XeMTLDriver | grep -c NVMTL_XE_VIBRANCY      # >= 1: the new plugin
```

If no prerelease newer than 92697ce exists yet, wait for it rather than building locally (the bundle needs the
translator and Vulkan-Headers).

## 3. Offline proof first (no WindowServer restart)

Run this with the Intel desktop up this boot, after the desktop step.

```bash
sudo bash /Library/XeGfx/gu605/gu605-blur-diag.sh
sed -n '/== verdict/,$p' /Library/XeGfx/gu605/logs/blur-*/summary.txt | tail -40
```

blurprobe now has WindowServer-shaped tests:
- **`ws.selfsample-load/-clear`**: an IOSurface target in one command buffer. Picture A, then picture B, then a
  render-target barrier, then a draw that samples the target. It must read B.
- **`ws.backdrop-load/-clear`**: the backdrop chain. A, then B drawn into the IOSurface target. B is copied down into
  a 4-level texture, each level rendered from the level above (Load and Clear variants). Level 3 is then saturated
  ×2 over the target. The result must be B, blurred and saturated, not A and not black.

| setting | expected |
|---|---|
| `base` (today's default, framebuffer fetch on in a test process) | `ws.selfsample-load` and/or `ws.backdrop-load` **FAIL**, frames "from the old picture A" / "pass's starting pixels" |
| `as-windowserver` (fetch off) | also FAIL; Clear variants may be "other" |
| `preset-as-ws` (`NVMTL_XE_VIBRANCY=1`, fetch off) | **all `ws.*` PASS** (20/20, 10/10) |

Before step 4, confirm in that run's `preset-as-ws-nvmtl.log`:
`conf: NVMTL_XE_VIBRANCY=1 preset: self-sample barrier, …`, `vk: self-sampled targets: BARRIER - reads see the draws
before them`, and `vk: sampled images are described in layout GENERAL`.

- If **base passes too**: the tests do not reproduce WindowServer. Go to step 4 anyway; it is still the best guess.
- If **preset-as-ws fails**: stop. Send `summary.txt`, the `ws.*` PNGs and `preset-as-ws-nvmtl.log`.

## 4. A/B in WindowServer — ordered, one logout per step

Each step:
1. Write the conf: `C=/Library/GPUBundles/xemtl/nvrm610.conf; [ -f $C ] && sudo mv $C $C.before-$(date +%H%M%S)`,
   then `printf '%s\n' <lines> | sudo tee $C`.
2. **Sahip için:** Apple menüsü › **Çıkış Yap**.
3. At the login window (`stat -f %Su /dev/console` = `root`): `sudo launchctl kickstart -k system/com.apple.WindowServer`.
4. **Sahip için:** giriş yap.
5. Owner check and the agent log check below.
6. Wait 5 minutes before the next step.

**Sahip için, her adımdan sonra 3 soru:**
1. Sistem Ayarları'nı aç: kenar çubuğu **bulanık ve renkli** mi, arkadaki pencere net görünüyor mu?
2. Control-Command-Q ile ekranı kilitle: saat/tarih yazılarının arkasında **siyah kutu** var mı? (Şifreyle aç.)
3. Bir pencereyi Dock'un ve kenar çubuğunun arkasında sürükle: **titreme** var mı?

Agent check: `L=$(ls -t /tmp/nvmtl-WindowServer-*.log /tmp/nvmtl.log 2>/dev/null | head -1); grep -E 'conf:|self-sampled|sampled images|padded stand-in|IOSurface' $L | tail -20`,
and `sysctl debug.nvaccel_iop_ok_h4 debug.nvaccel_iop_fail` (rising, 0).

| step | conf lines | expected new log lines | if fixed | if not |
|---|---|---|---|---|
| **1** | `NVMTL_XE_VIBRANCY=1` | `conf: … knob(s) read`, `NVMTL_XE_VIBRANCY=1 preset: self-sample barrier, sample layout GENERAL 1, surface match 1, tiled pad 1`, `self-sampled targets: BARRIER`, `sampled images … GENERAL`, maybe `self-sampled target by IOSurface …`, `padded stand-in (pitch 192) … ok` | go to step 2 (find the minimum) | step 3 |
| **2a** | `NVMTL_XE_SELF_SAMPLE=barrier` `NVMTL_XE_SAMPLE_GENERAL=0` `NVMTL_XE_SELF_SURF=0` | `BARRIER`, `layout SHADER_READ_ONLY_OPTIMAL` | barrier alone is enough; report | 2b |
| **2b** | `NVMTL_XE_VIBRANCY=1` `NVMTL_XE_TILED_PAD=0` | as step 1, no `padded stand-in` | the tiled pad is not needed; report | the pad is needed; report |
| **3** | `NVMTL_XE_VIBRANCY=1` `INTEL_DEBUG=noccs,nofc` | as step 1 | aux state matters together with barrier; report | step 4 |
| **4** | `NVMTL_XE_VIBRANCY=1` `NVMTL_FBFETCH_WS=1` | as step 1 + `framebuffer fetch ON` | fetch matters too; report | step 5 |
| **5** | `NVMTL_XE_SELF_SAMPLE=off` | `self-sampled targets: OFF` (NullMoth-like) | report | report everything; stop |

Stop at the first step the owner calls fixed, apart from steps 2a/2b, which only narrow down a fix. Afterwards keep the
best conf in place (or step R), and run `sudo bash /Library/XeGfx/gu605/gu605-collect.sh` once.

Also tell us if a **software-cursor square trail** appears. The snapshot was originally added for that trail; the
hardware cursor is on now, so it should not appear.

## R. Rollback (any time)

- Conf: `sudo mv /Library/GPUBundles/xemtl/nvrm610.conf /Library/GPUBundles/xemtl/nvrm610.conf.tried-$(date +%H%M%S)`,
  then logout and the WindowServer restart at the login window. The defaults are today's behaviour.
- Bundle: `sudo mv $GB/XeMTLDriver.bundle $GB/XeMTLDriver.bundle.new-$D && sudo mv $GB/XeMTLDriver.bundle.before-$D $GB/XeMTLDriver.bundle`,
  then logout and restart.
- If the screen stays black after a restart at the login window: hold the power button. The next boot uses the same
  bundle and conf, so boot with `-xegfxnoauto` once and do the conf rollback from the firmware-framebuffer desktop.

## 5. Send back

- The `== verdict` block from step 3, plus the `ws.*` PNGs of `base` and `preset-as-ws`.
- Per step: the conf lines, the owner's three answers, and the grepped log lines.
- The `gu605-collect.sh` report of the final state, and any gu605-mac commit hashes.
- If step 1 fixed it, the coordinator will make the minimal set the default for Intel on gu605-mtl.
