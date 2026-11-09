# GU605 — harici monitörde DDC/CI (Intel iGPU yolu, branch `gu605-ddc`)

Amaç: Standart/Eko modda sol USB-C/TB4 portundaki ASUS PG27AQDM'in **kendi** parlaklığını (VCP 0x10), kontrastını
(0x12), girişini (0x60) macOS'tan DDC/CI ile ayarlamak; ddcctl, MonitorControl, BetterDisplay (DDC modu) ve
ghelper-mac kaydırıcısı Intel Mac'teki gibi çalışsın.

Taban: `origin/gu605-display` (i915 KMS yolu). Bu dal `gu605-display`, `gu605-mtl`, `main`'e yazmaz; onlardan
merge ile güncellenir. Öncelik: DDC en son iş — önce görüntü, Metal ve USB-C; bu dal onları bekletmez.

## Nasıl çalışır
```
ddcctl / MonitorControl / ghelper-mac
  └ IOFBCopyI2CInterfaceForBus(fb, 0) → IOI2CSendRequest
      └ IOI2CInterfaceUserClient::extIO → IOFramebufferI2CInterface::startIO (fbLock)
          └ XeGfxIntelFramebuffer::doI2CRequest            kexts/XeGfxIntelFB/ddc.inc
              └ callPlatformFunction("xgKms", op 6)         shim/src/xnu/xg_kext.cpp (gKmsLock)
                  └ xg_kms_i2c(bağlayıcı, istek)            xkpi/src/i915/xg_kms.c
                      └ i2c_transfer(connector->ddc)        DP/USB-C: intel_dp->aux.ddc (drm_dp_i2c_xfer, I2C-over-AUX)
                                                            HDMI: GMBUS adaptörü
```
- `start()` (IONDRVFramebuffer::start'tan önce) `IOFBI2CInterfaceInfo` ile tek bir bus yayınlar; IOGraphics bundan
  `IOI2CInterface` nub'ını WindowServer çerçeve tamponunu açtığında (`IOFramebuffer::open`) kurar.
- İstek yalnız **harici monitör görüntüyü aldığında** (`fExternal`, kms.inc'in tek-ekran devri) işlenir; dahili
  panelde (eDP) DDC/CI yok, parlaklığı backlight handler'ı yönetir → `kIOReturnNoDevice`.
- AUX gücü, Type-C port kilidi, DEFER yeniden denemeleri, 16 baytlık AUX parçaları, MOT biti: hepsi i915'in kendi
  kodu (`intel_dp_aux_xfer`, `drm_dp_i2c_xfer`). Bus kilidi adaptörün `lock_ops`'u (i915'in kendi EDID okumalarıyla
  çakışmaz).
- DDC/CI isteği iki aktarım: yazma (≤64 bayt) → ≥40 ms bekleme (`minReplyDelay` daha uzunsa o, en çok 500 ms) →
  okuma (≤256 bayt). EDID okuması (0x50, alt adresli) tek aktarım: 1 baytlık ofset + okuma, arada repeated start.
- `replyBytes` okunan bayt sayısıdır; DDC/CI yanıtında gömülü uzunluk + 3.

## Güvenlik sınırları (iki katmanda: ddc.inc ve xg_kms_i2c)
| Kural | Değer |
|---|---|
| Yazılabilen adres | yalnız 7-bit 0x37 (8-bit 0x6E) |
| Okunabilen adres | 0x37, 0x50 (0x50'ye en çok 1 bayt ofset yazılır, okumayla birlikte) |
| Boyut | gönderim ≤ 64 bayt (alt adres dahil), yanıt ≤ 256 bayt |
| Süre | yanıt beklemesi ≤ 500 ms; AUX/GMBUS zaman aşımları i915'inki |
| Hedef | yalnız bağlı harici bağlayıcı; eDP/LVDS asla |
| Hata | isteğe hata kodu, veri taşınmaz (`replyBytes` = 0) |
| Kapatma | `gu605.conf` `DDC=0` (her açılışta) veya hemen `sudo tools/xeddcprobe enable 0` (XeGfxDdcEnable özelliği; boot-arg gerekmez); ayrıca boot-arg `-xeintelfbnoddc` (bus yayınlanmaz), `-xeintelfbnokms` |

## Durum
| Kalem | Durum |
|---|---|
| `xg_kms_i2c` + `xgKms` op 6 | yazıldı; `xg_kms.c` harness bayraklarıyla (drm-kmod c16de0f, FreeBSD d2145e9) Linux clang'da hatasız/uyarısız derlendi |
| XeGfxIntelFB `ddc.inc` (bus + `doI2CRequest`) | yazıldı; MacKernelSDK ile `-Wall -Werror` sözdizimi denetimi geçti |
| CI | `gu605-ddc quick` (macos-14): XeGfxIntelFB Apple clang `-Werror` ile derlenip bağlandı (edd39f5, başarılı). `gu605.yml` tetikleyicisine `gu605-ddc` eklendi; ama intel işi `gu605-display`'de de olduğu gibi firmware indirmesinde (HTTP 404) düşüyor, arm işi macos-15 kuyruğunda bekliyor — XeGfx.kext (xg_kms.c) henüz CI'da derlenmedi, yalnız yerelde |
| Donanım testi | yok — önce sol USB-C'de görüntü gerekli (`GU605-DISPLAY-DESIGN.md` §9 adım 5) |

## Test (görüntü harici monitördeyken)
1. Monitör menüsünde DDC/CI açık olsun.
2. `ioreg -r -w0 -c XeGfxIntelFramebuffer -d1 | grep -E 'XeGfxDdc|IOFBI2CInterface'`
   → `bus published…`, `IOFBI2CInterfaceIDs = (0)`.
3. `ddcprobe` (thebcd69/nvidia-macos-driver `tools/ddcprobe.c`, aynı API) veya `ddcctl -d 1 -b ?`:
   VCP 0x10 okunur; `ddcprobe 0 10 50` parlaklığı 50 yapar.
4. Hata olursa `XeGfxDdc` satırı aşamayı (`send`/`reply`) ve i915 hata kodunu (ör. -60 ETIMEDOUT: AUX yanıt yok,
   -5 EIO: NAK/eksik) gösterir; kernel günlüğünde `XeGfxIntelFB: ddc:` satırları.

## Bilinen sınırlar / riskler
- DP-MST (hub/daisy-chain) bağlayıcıları `DP-x-y` adıyla ayrı; tek-ekran devri yalnız `DP-1..4` arar, MST'de DDC
  denenmez.
- IONDRVFramebuffer kendi `IOFBI2CInterfaceInfo`'sunu yazarsa (GOP üzerinde beklenmiyor) bizimkinin üstüne yazabilir;
  `XeGfxDdc` satırı ve `IOFBI2CInterfaceIDs` ile görülür.
- `doI2CRequest` IOGraphics'in `fbLock`'u altında çalışır; 40 ms bekleme o süre boyunca aynı çerçeve tamponundaki
  mod değişimini bekletir (Apple'ın kendi sürücülerinde de böyle).
- Bağlayıcı adı `fConnName` monitör değişince kms.inc tarafından güncellenir; istek sırasında monitör çıkarılırsa i915
  `-ENODEV` döner, istek hata alır.

## Lisans
Yeni kod BSD-2 (WTL'nin lisansı). NullMoth NVRMFB DDC dalı (PolyForm-NC) yalnız tasarım referansı; kodu kopyalanmadı.

## NVIDIA DDC'den alınan dersler (2026-11-09, `ddc-location` / DDC-RAPOR-20261105)
* **pendingPowerState**: IOGraphics, `pendingPowerState == 0` iken her isteğe `kIOReturnOffline` döner ve
  IOGraphicsLib bunu gizler. XeGfxIntelFB (IONDRVFramebuffer) IOFramebuffer'ın `setPowerState`'ini korur, yine de
  bus yayınlanınca `IONDRVFramebuffer::start`'tan önce bir kez 1 yapılır (NVRMFB 47039af gibi); `XeGfxDdc` satırı
  güç durumunu gösterir.
* **IODisplayLocation**: BetterDisplay framebuffer'ı bu yoldan bulur. Burada tek XeGfxIntelFramebuffer (GFX0 altında)
  hangi ekran görüntüdeyse onu sürer: yol tek. Harici bağlantı "yerleşik değil" bildirir (AppleDisplay).
  `tools/xeddcprobe` her ekranın yolunun kendine ve bus'lı bir framebuffer'a çözüldüğünü denetler; `xeddcprobe diag`
  gerçek IOReturn kodlarını gösterir.

