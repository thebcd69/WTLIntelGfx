# GU605MY (Meteor Lake 7D55) çalışma durumu

Dal: `gu605-mtl` (thebcd69/WTLIntelGfx). Taban: upstream `ff64f83` (XeGfx 0.0.47). Sahibin sürümü: macOS 15.8.1,
NullMoth **2.7.1**. Öncelik (sahip): **1) Eco: dahili panel + Metal/WindowServer + parlaklık**, 2) Eco: sol USB-C,
3) bonus: Standard, 4) DDC (ayrı dal). Ayrıntı: `docs/IGPU-FEASIBILITY.md`; kurulum: `docs/GU605-KURULUM.md`;
test kartı: `docs/GU605-TEST.md`; EFI farkı: `docs/GU605-EFI-CHANGES.md`.

## Yapıldı
- samwu07'nin aynı iGPU + aynı macOS'ta çalışan yamaları (bootvram, VBL, zero-copy flip, gpufb, WindowServer işaretleri).
- **XeGfxIntelFB 0.9.5**: 7D55'i yalnız PCI kimliğiyle eşler (WhateverGreen GFX0→IGPU yapar); **Ultimate'te başlamaz**;
  OLED için DPCD AUX parlaklık (Linux i915 ile birebir: Intel HDR / VESA / eDP 1.5 luminance), alt sınır %5.
- **XgFbMove 1.2.1**: yalnız GuC yuvasıyla çakışan yüzeyi taşır; host testleri 37/37.
- **XeGfx**: `-xegfxoff`, 7D55'te başarısız aşama tekrar çalışmaz, GuC yuvası koruması, stage 2'de salt-okunur ekran
  motoru + Type-C dökümü (çözümlü özet: hangi pipe/DDI/TC portu).
- **XeGfxAccel 0.9.6** (Sequoia) + **XeMTLDriver.bundle**: NullMoth'un eklentisi/klasörleri/allow-list'i hiç paylaşılmaz.
- Betikler: açılış koşucusu (mod algılama, "onaylanmadı" koruması), aşamalar, Metal testi + CoreDisplay GPU-pass,
  masaüstü adımı (çökme döngüsü koruması), tek komut rapor (Masaüstü + `EFI/GU605/`), kurulum/kaldırma (kcditto,
  karantina temizliği, hiçbir şey silinmez).
- Yerel doğrulama: harness 422/422 kaynak derleniyor (drm-kmod c16de0f + FreeBSD d2145e9), lisans kapısı temiz;
  değişen tüm kext C++ dosyaları MacKernelSDK ile `-Werror` sözdizimi denetiminden geçiyor.
- Önerilen EFI: `/home/claude/gu605-efi-work/proposed/` (yalnız 2 config değişikliği + XeGfx.kext; `CHANGES.md`).

## Devam ediyor
- CI (`.github/workflows/gu605.yml`): `arm` (macos-15, çapraz derleme), `intel` (macos-15-intel: XeGfx.kext + Mesa
  ANV), `pack` (prerelease). İlk denemede Intel runner hiç alınamadı; Intel işi yine çalışmazsa paket upstream
  v0.0.47 XeGfx.kext'ini (yayın notundaki SHA-256 ile doğrulanmış) kullanır ve Mesa ANV'siz çıkar (koşucu aşama 6'da durur).

## Sıradaki adım
1. CI yeşil → `proposed/EFI/OC/Kexts/XeGfx.kext` doldur, firmware hash'lerini `gu605/firmware.lock`'a sabitle.
2. Sahibin testi: `docs/GU605-TEST.md` (Ultimate 1 açılış, Eco 1 açılış, isteğe bağlı Standard).
3. Rapor sonrası: GOP yerleşimi/TC dökümüne göre Aşama 2 yolu (3a: GOP'un yaktığı harici ekranı devralmak; 3b: i915 display).

## Riskler
- Donanımda denenmedi. I-116 (WTL, açık panik). GU605 GOP yerleşimi ölçülmedi (korumalar var).
- Standard'da NullMoth ile aynı `debug.nvaccel*` adları → masaüstü adımı Standard'da bilerek reddedilir.
- Mesa ANV CI'da derlenemezse Metal/masaüstü adımı kilitli kalır.

## DDC/CI dalı için not (gu605-ddc)
- iGPU panel framebuffer'ı **XeGfxIntelFramebuffer** (IONDRVFramebuffer alt sınıfı), tek bağlantı (index 0, dahili eDP,
  `kIOConnectionBuiltIn`). AUX A: `xifbAux()`, `xifbI2c()`, `xifbDpcdRead/Write()` (kexts/XeGfxIntelFB), parlaklık
  yolu `gXifbAuxLock` ile sıralı — I2C-over-AUX kullanan her yeni kod aynı kilidi almalı.
- Sol USB-C (iGPU TC portu) i915 display kapalıyken sürülmüyor: orada DDC/CI için bağlantı yok (Aşama 2/3'e kadar).
  Sağ port/HDMI NVIDIA'da = NullMoth'un alanı.
