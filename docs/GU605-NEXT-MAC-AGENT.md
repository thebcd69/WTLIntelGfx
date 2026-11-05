# GU605 — next steps for the Mac-side agent (2026-11-05)

For: the Claude session on the owner's GU605MY (macOS 15.8.1, OpenCore, Eco mode), branch **gu605-mac**.
From: the build agent (branches gu605-mtl / gu605-display). English for you; the blocks marked **Sahip için** are for the
owner, in Turkish.

## 0. Rules (unchanged, owner's)

- **Nothing is deleted.** Backups are made by renaming/moving (`X` → `X.before-<date>`), never `rm`.
- **EFI writes only with the owner's explicit OK** in this session, and every copied file is checked with
  `shasum -a 256` against the source after the copy. Never touch the Windows SSD, the boot order or NVRAM boot entries.
  Never edit the OpenCore `config.plist` (all changes in this file avoid it).
- You work and push only on **gu605-mac** (`git push origin gu605-mac`). Never push to `main`, `gu605-mtl`,
  `gu605-display` or `gu605-ddc`, and never force-push. The coordinator merges.
- Commit identity `Claude <noreply@anthropic.com>`, messages end with the `Co-Authored-By:` line as before.
- Safety switches (OpenCore picker, boot-args for one boot): `-xegfxnoauto` (runner does nothing), `-xegfxoff` (XeGfx
  idle), `-xeintelfboff` (Intel framebuffer off), `-xegfxnokms` (display path off). G-Helper **Ultimate** always gives
  back the NullMoth-only setup.

Get the current code first:

```bash
cd ~/<your checkout> && git fetch origin && git checkout gu605-mac
git merge --no-ff origin/gu605-mtl      # blurprobe, blur diag, fps tool, automatic-login desktop step, CI fixes
```

New files on gu605-mtl since your 5286960:
`userland/tools/blurprobe.m`, `gu605/scripts/gu605-blur-diag.sh`, `gu605/scripts/gu605-fps.sh`, and changes in
`gu605-desktop.sh`, `gu605-auto.sh`, `gu605.conf.default`, `gu605-install.sh`, `docs/GU605-TEST.md`. None of them needs a
kext or an EFI change. Copy the scripts in place with your `gu605-mac-swap.sh` (scripts part) or simply:

```bash
sudo mkdir -p /Library/XeGfx/gu605-scripts-before-$(date +%Y%m%d)
sudo cp -p /Library/XeGfx/gu605/gu605-*.sh /Library/XeGfx/gu605-scripts-before-$(date +%Y%m%d)/
sudo cp gu605/scripts/gu605-*.sh /Library/XeGfx/gu605/ && sudo chmod 755 /Library/XeGfx/gu605/gu605-*.sh
```

(`gu605.conf` is never overwritten: add new keys by hand when a step below needs them.)

---

## 1. Backdrop blur ("vibrancy") — see-through, flickers while dragging

### What the code says (read before testing)

The Metal plugin (XeMTLDriver = WTL's fork of NullMoth's NVMTL, NvGfx lineage) was written and tuned on **NVK**, which
ignores Vulkan image layouts and has no colour compression. **ANV does not**: on Meteor Lake, optimal-tiled colour
images get CCS (lossless compression) plus fast-clear state, and the layout given in a barrier/descriptor decides how
the hardware reads them. Places in `plugin/nvmtl_vk.c` that are harmless on NVK and suspicious on ANV:

1. **One layout per image** (`img->layout`, `img_to()` transitions all mips/layers at once). A render pass that renders
   into mip level N declares `initialLayout = TRANSFER_SRC_OPTIMAL` (when it loads) or `UNDEFINED` (when it clears)
   for that level, whatever that level really is in; descriptors for sampling say `SHADER_READ_ONLY_OPTIMAL` while the
   image may be resting in `TRANSFER_SRC_OPTIMAL` or `GENERAL`. Core Animation's blur is exactly a chain of
   render-into-a-smaller-level / sample-it-back passes inside one command buffer.
2. **Fast clears**: passes with `MTLLoadActionClear` → Vulkan `LOAD_OP_CLEAR`; ANV may fast-clear (only the aux
   surface is written). A later sample in a layout/aux state that does not resolve it reads undefined pixels —
   undefined = transparent (see-through) or stale (flicker).
3. **Framebuffer fetch is off in WindowServer** (`nvmtl_fbfetch_on`: `NVMTL_FBFETCH_WS=1` opts in): Core Animation's
   uber fragment shader reads `[[color(0..2)]]`; with fetch off those reads return a zero dummy image. On NVIDIA
   (NullMoth) blur works with the same default, so this is not the first suspect, but vibrancy's blend modes read the
   destination, so it stays on the list.
4. Translator: WTL's `translator/` is older than NullMoth 2.7.1's (NullMoth added inline-sampler lowering, sampler
   arrays, short/punned access chains, …). A refused shader is skipped, not drawn — see-through. The plugin log says
   so ("refused", "skipped", "translation produced nothing").

Most likely, in order: (1)/(2) ANV aux/layout state on CA's intermediate textures, then (4) a refused blur/saturate
shader, then (3). The flicker-only-while-dragging symptom fits (1)/(2) best (undefined contents change every frame
while the backdrop is re-rendered; when nothing moves, CA reuses its cached backdrop).

### Step 1a — build blurprobe (Command Line Tools only)

```bash
cd ~/<checkout>
clang -fobjc-arc -O1 -arch x86_64 -mmacosx-version-min=15.0 -framework Foundation -framework Metal \
  -framework QuartzCore -framework CoreImage -framework MetalPerformanceShaders -framework ImageIO \
  -framework CoreGraphics -framework IOKit userland/tools/blurprobe.m -o /tmp/blurprobe
sudo install -m 755 /tmp/blurprobe /Library/XeGfx/gu605/tools/blurprobe
```

If it does not compile (it was written without a macOS SDK at hand; the CI build of 51aa7bf compiles it too — check
that run's `arm` log), fix it on gu605-mac and push.

### Step 1b — run the diagnostic (nothing on screen changes; needs stage 7 this boot)

```bash
sudo bash /Library/XeGfx/gu605/gu605-blur-diag.sh
cat /Library/XeGfx/gu605/logs/blur-*/summary.txt | tail -120
```

It runs blurprobe in a test process with the Intel plugin under 7 settings (`base`, `INTEL_DEBUG=noccs`,
`INTEL_DEBUG=nofc`, both, `NVMTL_NO_FBFETCH=1` = as in WindowServer, `NVMTL_NO_DIRTYONLY=1`, `NVMTL_NO_PSO_SKIP=1`),
writes PNGs per test and setting, and collects what WindowServer's plugin logged this boot.

### Step 1c — read it

| blurprobe result | means | next |
|---|---|---|
| `ca.backdrop` FAIL in `base` with "unblurred (see-through)" frames | reproduced offline: iterate here, no WindowServer restarts needed | go by the rows below |
| `metal.rtt_sample` FAIL / some of 40 frames off | render→sample in one command buffer is broken on ANV | look at which setting fixes it |
| a test FAILs in `base` but PASSes with `noccs` | CCS/aux state vs layouts (suspect 1) | step 1d with `INTEL_DEBUG=noccs`; then the plugin fix below |
| PASSes with `nofc` only | fast-clear (suspect 2) | step 1d with `INTEL_DEBUG=nofc` |
| `metal.render_mip` or `metal.genmips` FAIL in every setting | per-level layout/rendering into levels (suspect 1, not aux) | plugin fix: per-level layouts or GENERAL (below) |
| `ci.gaussian`/`mps.gaussian` FAIL, plugin log has "refused"/"skipped" | translator/shader (suspect 4) | send the `*-nvmtl.log` lines; candidate: port NullMoth 2.7.1 translator |
| `ca.backdrop` PASS in `base`, FAIL in `as-windowserver` | framebuffer fetch matters (suspect 3) | step 1d with `NVMTL_FBFETCH_WS=1` |
| everything PASSes | WindowServer-only (drawable/IOSurface path) | step 1d with each knob anyway, in that order |

### Step 1d — confirm in WindowServer (one knob at a time)

The WindowServer plugin reads `/Library/GPUBundles/xemtl/nvrm610.conf` at its start (keys `NVMTL_*`, `INTEL_DEBUG`).

```bash
[ -f /Library/GPUBundles/xemtl/nvrm610.conf ] && sudo mv /Library/GPUBundles/xemtl/nvrm610.conf /Library/GPUBundles/xemtl/nvrm610.conf.before-$(date +%H%M%S)
echo 'INTEL_DEBUG=noccs' | sudo tee /Library/GPUBundles/xemtl/nvrm610.conf     # or nofc, or NVMTL_FBFETCH_WS=1
```

**Sahip için:** Apple menüsü › **Çıkış Yap**, tekrar giriş yap (masaüstü Intel GPU'da yeniden başlar). Sistem
Ayarları'nı aç: kenar çubuğunun arkası **bulanık ve renkli** mi? Bir pencereyi kenar çubuğunun arkasında sürükle:
**titreme** var mı? Sonra tek komut: `sudo bash /Library/XeGfx/gu605/gu605-collect.sh`.

- At most one logout every 5 minutes: the crash-loop guard disarms the Intel desktop after more than 2 WindowServer
  starts in 10 minutes (that is the safety net, not a fault).
- Rollback: `sudo mv /Library/GPUBundles/xemtl/nvrm610.conf /Library/GPUBundles/xemtl/nvrm610.conf.tried-$(date +%H%M%S)`, log out/in.
- `noccs` costs memory bandwidth (some power, a little speed); fine as a workaround while the real fix is made.

### Fix candidates (implement on gu605-mac after 1c/1d point at one)

- **Aux/layout (most likely):** an Intel-only knob in `plugin/nvmtl_vk.c`, default on for vendor 0x8086 after it is
  proven: every colour image lives in `VK_IMAGE_LAYOUT_GENERAL` (render pass initial/final, descriptors, `img_to`),
  so no layout can disagree with another. ANV keeps CCS in GENERAL on Xe-LPG but resolves fast clears for it. Second
  option: per-level layout tracking (`img->layout` → array per mip). Gate: `NVMTL_XE_GENERAL=0` turns it off.
- **Fast clear:** for 0x8086, `LOAD_OP_CLEAR` → `LOAD_OP_DONT_CARE` + `vkCmdClearAttachments` (no fast clear), or
  keep `INTEL_DEBUG=nofc` in the conf.
- **Translator:** NullMoth 2.7.1's `translator/` and `plugin/` may now be copied (owner's decision; PolyForm
  Noncommercial: keep NullMoth's notice and the `Required Notice:` line in every copied file, mark it PolyForm-NC, list
  it in NOTICE). Port only what the logs ask for.
- Prove every fix with blurprobe first (all PASS), then step 1d.

---

## 2. Desktop step with automatic login (new on gu605-mtl)

`gu605-desktop.sh` no longer stops when someone is logged in. `gu605.conf` (add if missing):

```
DESKTOP_LOGGED_IN=auto     # auto | logout | skip
DESKTOP_AUTOLOGIN_S=150    # a session that started within 150 s of boot is an automatic login
DESKTOP_IDLE_S=60          # and nobody touched keyboard/trackpad for 60 s
DESKTOP_WAIT_MIN=120       # logout mode: how long the runner waits for the next logout
```

- Automatic login + 60 s idle → a notification ("20 sn sonra ekran bir kez kararacak…"), 20 s, then the WindowServer
  restart (the session ends; the owner logs in again). Input during the 20 s cancels it.
- Otherwise (or `logout`): everything is armed, nothing restarts; the next logout brings the desktop to the Intel GPU.
  The runner clears the pending mark while it waits (a shutdown in between is not a failed run), sets it again when
  WindowServer restarts, starts the crash-loop guard, and confirms after 3 minutes as before.

Test: `ECO_LEVEL=desktop`, the owner turns automatic login on (System Settings › Users & Groups), reboot, hands off.
Expected in `EFI/GU605/last-boot.txt`: `... logged in Ns after boot, session ..., idle ...`, `automatic login, no
input: restarting now`, then `confirmed: N frames`. Second run: touch the trackpad during the notice → `waiting for the
next logout`, then log out → `logout seen`, `confirmed`. Check the summaries; nothing else is needed.

**Sahip için:** Bu test için otomatik girişi aç (Sistem Ayarları › Kullanıcılar ve Gruplar › Otomatik giriş). Açılışta
hiçbir şeye dokunma; bildirim gelirse bekle, ekran bir kez kararır, tekrar giriş yap. İkinci denemede bildirim gelince
izleme dörtgenine dokun, sonra Apple menüsü › Çıkış Yap. Bitince `gu605-collect.sh`.

---

## 3. Next hardware step for the display path (gu605-display)

### 3a. Is the internal panel already at 240 Hz, and does WindowServer use it? (gu605-mtl, no new kext)

`system_profiler` showed 2560x1600 @ 240.00 Hz: the GOP already lights the panel at 240 Hz, and XeGfxIntelFB reports
the measured rate. What is not known is how many frames reach the panel per second. With the Intel desktop up:

```bash
sudo bash /Library/XeGfx/gu605/gu605-fps.sh 20      # then drag a window in circles for 20 s
```

**Sahip için:** Komutu çalıştır, hemen bir pencereyi 20 saniye daireler çizerek sürükle (ya da Safari'de
https://www.testufo.com). Bitince çıktıyı bırak.

Read: `panel refresh before/after` (the pipe's own rate) and `shown/s` per second (frames XeGfxAccel put on the panel),
`flips/s` / `copied/s`. Panel 240 and shown ~240 while dragging = done for the internal panel. Panel 240 but shown ~60:
WindowServer paces at 60 (check XeGfxIntelFB's `VBL:` lines in the output: the VBL period must be ~4.17 ms; the
gu605-display XeGfxIntelFB follows the real refresh, gu605-mtl's assumes the boot measurement).

### 3b. PG27AQDM on the left USB-C as its own display at 2560x1440 @ 240 Hz (gu605-display package)

The GOP finding matters: with the monitor plugged in at boot, the GOP picks a **2560x1440 clone** on both pipes, so the
boot framebuffer is 2560x1440. The display path keeps scanning that one framebuffer (one display at a time), so after
a clone boot the panel cannot go back to 2560x1600 (refused, logged). **Boot without the monitor**, plug it in after
login.

What gu605-display adds: Linux i915's own display code (KMS) on 7D55 when `DISPLAY=kms`, behind a stage-2 takeover
check; the panel switched to `PANEL_HZ` (240) by i915; on hotplug the monitor gets the picture at its preferred raster
and fastest validated refresh (panel off); unplugged, the panel comes back. Design: `docs/GU605-DISPLAY-DESIGN.md`.

Package: the CI prerelease of branch gu605-display at the commit after `a7e4860` (title says "gu605-display"). It
changes **XeGfx.kext (EFI)**, XeGfxIntelFB (AuxKC) and the scripts.

1. **EFI (owner's OK needed):** mount the KIOXIA EFI, rename `EFI/OC/Kexts/XeGfx.kext` to
   `EFI/OC/Kexts/XeGfx.kext.mtl-<date>` (OpenCore loads only `XeGfx.kext`), copy the package's `EFI/OC/Kexts/XeGfx.kext`
   there, verify: `cd <pkg>/EFI/OC/Kexts && find XeGfx.kext -type f -exec shasum -a 256 {} \; | sort` must equal the
   same command in the EFI. No config.plist change.
2. **XeGfxIntelFB + scripts:** your `gu605-mac-swap.sh install <pkg>/Library/Extensions/XeGfxIntelFB.kext <checkout of gu605-display>`.
3. `gu605.conf`: `DISPLAY=kms`, `PANEL_HZ=240`, `ECO_LEVEL=gt` for the first boot.
4. Boot in Eco, **monitor unplugged**. Check:
   ```bash
   sudo bash /Library/XeGfx/gu605/gu605-display.sh dump | head -80
   grep -E 'display:|kms' /Library/XeGfx/gu605/logs/boot-*/stages/stages.log | tail -20
   ```
   Expect `display: i915 owns the display`, `connector eDP-1 ... connected`, a 2560x1600 mode near 240 Hz, `crtc ...
   measured ~240 Hz`, `committed eDP-1 2560x1600 240.xxx Hz` (or already at 240).
   If instead `display: adopt path (... why ...)`: the stage-2 check refused (reason printed) — report it, stop.
5. **Sahip için:** monitörü **sol** USB-C'ye tak (PG27AQDM açık). ~2 sn içinde görüntü monitöre geçmeli, dahili ekran
   kapanmalı. 1 dk bekle, sonra kabloyu çıkar: dahili ekran geri gelmeli. Olmazsa hiçbir şeye basma, bana söyle.
6. Check: `gu605-display.sh dump`: `connector DP-n ... connected`, `tc dp-alt max-lanes 4`, `active crtc ... 2560x1440
   ~240 Hz ... lanes 4`, `measured ~240 Hz`. XeGfxIntelFB log: `display path: DP-n has the picture`. If the hotplug did
   nothing: `sudo bash /Library/XeGfx/gu605/gu605-display.sh external` (and `internal` to come back).
   If `DP-n` never shows `connected` and `tc` stays `disconnected`: the PD controller did not enter DP alt mode
   without the OS — report `display-decoded.txt` (TC lines) and stop there.
7. Then `ECO_LEVEL=desktop` (desktop step above) and `gu605-fps.sh` on the monitor and on the panel.
8. Optional, report only: one boot **with** the monitor plugged in (clone boot): expect the external to work and
   `eDP-1 240` to be refused ("does not fit the boot framebuffer 2560x1440").

Kill switches / rollback for 3b: `-xegfxnokms` (one boot), `DISPLAY=adopt` in gu605.conf (permanent), the automatic
`/Library/XeGfx/gu605/kms-failed` mark after a failed KMS stage 4 (rename it to retry), `-xeintelfbnoext` (no monitor
switch), `-xeintelfbnokms` (framebuffer ignores i915). Full rollback: rename the EFI `XeGfx.kext.mtl-<date>` back
(owner's OK), `gu605-mac-swap.sh rollback <backup dir>`.

Known limits on gu605-display: the hardware cursor is turned off while i915 owns the display (software cursor; the
gu605-mac hardware cursor would fight i915's plane/DBUF programming); no sleep (do not close the lid / sleep during these
tests); both screens at once is not supported.

---

## 4. Send back (one message to the coordinator)

- Section 1: `blur-*/summary.txt`, the PNGs of the failing tests (`ca.backdrop-00.png` etc.), the result of each
  WindowServer A/B (looks right / flickers / see-through).
- Section 2: the two `last-boot.txt` summaries.
- Section 3: `fps.txt` (3a), `gu605-display.sh dump` before/after plugging, `stages.log`, the XeGfxIntelFB
  `display path:` log lines, and `gu605-collect.sh`'s report.
- Any fix you committed on gu605-mac (commit hashes).
