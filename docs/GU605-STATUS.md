# GU605MY (Meteor Lake 7D55) çalışma durumu

Dal: `gu605-mtl` (thebcd69/WTLIntelGfx). Taban: upstream `ff64f83` (XeGfx 0.0.47). Sahibin sürümü: macOS 15.8.1,
NullMoth **2.7.1**. Öncelik sırası (sahip, 2026-11-04): **1) Eco: dahili panel + Metal/WindowServer + parlaklık**,
2) Eco: sol USB-C harici monitör, 3) bonus: Standard (NullMoth ile birlikte), 4) DDC (ayrı dal, en son).

## Yapıldı
- Fizibilite: `docs/IGPU-FEASIBILITY.md`.
- samwu07'nin aynı iGPU + aynı macOS'ta çalışan yamaları alındı (bootvram, VBL, zero-copy flip, gpufb, WindowServer
  işaretleri) — ThinkBook 14+ 7D55'te ~60 fps Intel masaüstü.
- XeGfxIntelFB 0.9.5: 7D55 eşleşmesi; **Ultimate'te başlamaz** (pipe A yanmıyor/konsol boyutu tutmuyor → probe reddeder);
  OLED için **DPCD AUX parlaklık** (Intel HDR nits arayüzü, yoksa VESA), alt sınır %5, yalnız kaydırıcı/tuşta yazar.
- XgFbMove 1.2.1: framebuffer'ı yalnız GuC yuvasıyla [0xFEE00000, 0xFF000000) çakışıyorsa taşır (2560x1600 GOP
  0xFF000000'de kalır); host testleri 37/37.
- XeGfx: `-xegfxoff`, 7D55'te başarısız aşama tekrar çalışmaz, GuC yuvası koruması, stage 2'de salt-okunur ekran motoru
  + Type-C (TCSS) dökümü (sol portun TC numarasını verir).
- XeGfxAccel 0.9.6 (Sequoia derlemesi, `NM_TAHOE` yok), eklenti adı **XeMTLDriver** → NullMoth'un `NVMTLDriver.bundle`,
  `nvmtl/` klasörü ve `nvmtl-allow.txt` dosyasına **hiç dokunulmaz** (Intel: `xemtl/`, `xemtl-allow.txt`).
- Betikler (`gu605/scripts`): açılış koşucusu (LaunchDaemon, mod algılama, "onaylanmadı" koruması), aşama koşucusu,
  Metal testi, masaüstü adımı (çökme döngüsü koruması), tek komutla rapor (`gu605-collect.sh`, Masaüstü + EFI),
  kurulum/kaldırma (hiçbir şey silinmez, yedekler).
- CI: `.github/workflows/gu605.yml` (macos-15-intel), prerelease `gu605-<sha>`.

## Devam ediyor
- İlk CI derlemesi (XeGfx çekirdeği bağımlılıkları: drm-kmod c16de0f, freebsd d2145e9, kcabi, OGKM 615.78.08).
- Mesa ANV + libdrm (userland) CI işi: henüz yok → paket şimdilik Metal/masaüstü adımına kadar gidemez (aşama 6'da durur).

## Sıradaki adım
1. CI'yi yeşile çek; firmware hash'lerini `gu605/firmware.lock`'a sabitle.
2. Userland CI işi (MacPorts LLVM 20 + SPIRV-LLVM-Translator + mesa_clc, `userland/BUILD.md`).
3. `/home/claude/gu605-efi-work/proposed/` EFI paketi + `docs/GU605-KURULUM.md` + `docs/GU605-TEST.md`.

## Riskler
- Donanımda hiçbir şey denenmedi. I-116 (WTL, açık panik), 7D55'te XeGfxIntelFB/IONDRV yolu samwu07'de çalıştı ama
  GU605'in GOP yerleşimi ölçülmedi (stage 2 ölçecek).
- Standard modda NullMoth ile aynı `debug.nvaccel*` adları → masaüstü adımı Standard'da bilerek reddedilir.

## DDC/CI dalı için not (gu605-ddc)
- iGPU'daki panel framebuffer'ı **XeGfxIntelFramebuffer** (IONDRVFramebuffer alt sınıfı), tek bağlantı (index 0,
  dahili eDP, `kIOConnectionBuiltIn`). AUX A erişimi `xifbAux()` / `xifbDpcdRead/Write()` (kexts/XeGfxIntelFB),
  `gXifbAuxLock` ile sıralı — I2C-over-AUX `xifbI2c()` aynı kilidi kullanmalı.
- Sol USB-C (iGPU TC portu) i915 display kapalıyken **sürülmüyor**: orada DDC/CI için framebuffer/bağlantı yok
  (Aşama 3'e kadar). Sağ port/HDMI NVIDIA'da = NullMoth'un alanı.
