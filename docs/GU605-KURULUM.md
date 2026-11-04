# GU605 kurulum (OpenCore + macOS) ve geri alma

Hedef: **tek OpenCore config** ile üç G-Helper modu:
- **Ultimate** (panel NVIDIA'da): NullMoth her şeyi yapar, Intel sürücüleri hiçbir şey yapmaz (bugünkü gibi).
- **Eco** (dGPU kapalı): Intel sürücüleri dahili paneli, Metal'i ve parlaklığı alır; NullMoth boşta kalır.
- **Standard** (ikisi de açık): panel Intel'de; varsayılan olarak yalnız salt-okunur tanılama + yazılımsal masaüstü (bonus).

Config'i sahibin yerine **düzenlemiyoruz**: hazır kopya `/home/claude/gu605-efi-work/proposed/` altında
(`CHANGES.md` her değişikliği ve geri almayı listeler). Bu belge aynı değişiklikleri elle yapmak içindir.

## 1. OpenCore `config.plist` değişiklikleri (yalnız ikisi)

| Yer | Şimdi | Yeni | Neden |
|---|---|---|---|
| `DeviceProperties > Add > PciRoot(0x0)/Pci(0x2,0x0)` | `disable-gpu = true` | **anahtar silinir** (sözlük boş kalırsa o da) | WhateverGreen bu özellikle iGPU'yu gizliyor; hiçbir Intel sürücüsü eşleşemez |
| `Kernel > Add` (listenin sonuna) | — | `XeGfx.kext`, `Contents/MacOS/XeGfx`, `Contents/Info.plist`, `Arch x86_64`, `MinKernel 24.0.0`, `MaxKernel 24.99.99`, `Enabled true` | XeGfx boot koleksiyonunda olmalı (samwu07: aux koleksiyonundan yüklemek Mac'i dondurdu); `kmutil load` edilene kadar **boşta** |

Dosya: `EFI/OC/Kexts/XeGfx.kext` (CI sürümündeki `EFI/OC/Kexts/XeGfx.kext`).

**Değişmeyenler (bilerek):**
- boot-args: NullMoth'unkiler (`nvfb=1 nvaccel=1 nvfbheads=4 …`, `nvrmacpibl=1`) Eco'da NVIDIA olmadığı için etkisiz,
  Ultimate'te gerekli. Yeni zorunlu boot-arg yok.
- `Kernel > Block com.apple.iokit.IONDRVSupport`: OpenCore günlüğü bunun **etkisiz** olduğunu gösteriyor
  ("Prelinked blocker … - Not Found": IONDRVSupport Sequoia'da sistem koleksiyonunda). Ultimate'te NVIDIA'yı NullMoth'un
  `NVRMFBClaim`'i koruyor. Kalabilir; Intel framebuffer'ı (IONDRVFramebuffer alt sınıfı) bundan etkilenmez.
- NVIDIA `DeviceProperties` (`acpi-wake-type`), ACPI tabloları, USB haritası, WhateverGreen: değişmez.

## 2. macOS tarafı

CI sürümünü (`gu605-<sha>.zip`, GitHub prerelease) aç, klasörde:

```
sudo bash scripts/gu605-install.sh
```

Kurar: `/Library/Extensions/{XeGfxIntelFB,XeGfxAccel,XeGfxAGDC,XgFbMove}.kext` (aux koleksiyonu yeniden kurulur,
NullMoth'un kext'leri kimlikleriyle korunur, eski koleksiyon yedeklenir), `/Library/GPUBundles/XeMTLDriver.bundle`
(Intel Metal eklentisi; NullMoth'un `NVMTLDriver.bundle`'ına dokunulmaz, AIR okuyucusu NullMoth'unkinden kopyalanır),
`/Library/GPUBundles/xemtl/` (Mesa ANV, paket içeriyorsa), `/Library/XeGfx/gu605/` (betikler, `gu605.conf`, kayıtlar),
`/Library/LaunchDaemons/org.thebcd69.gu605.auto.plist`. Her şeyin yedeği `/Library/XeGfx/gu605-backup-<tarih>/`.

`/Library/XeGfx/gu605/gu605.conf`: `ECO_LEVEL=metal` (ilk test; sonra `desktop`), `STANDARD_LEVEL=probe` (seviyeler: off, probe, gt, metal,
desktop), `FLIP`, `WSMARK`, `K5`, `EFI_LOG`.

## 3. Acil durum anahtarları (OpenCore seçicide boot-arg eklemeden de çalışır)

| Ne | Etki |
|---|---|
| G-Helper → **Ultimate** | Intel sürücüleri hiç başlamaz; bugünkü NullMoth kurulumu |
| boot-arg `-xegfxnoauto` | açılış koşucusu bu açılışta hiçbir şey yapmaz |
| boot-arg `-xegfxoff` | XeGfx tamamen boşta (aşama çalışamaz) |
| boot-arg `-xeintelfboff` | Intel framebuffer'ı başlamaz |
| boot-arg `-xgfbmoveoff`, `-xegfxnoflip`, `-xeintelfbnobl` | taşıma / flip / parlaklık kapalı |
| boot-arg `-xegfxnokms` | i915 ekran yolu (DISPLAY=kms) bu açılışta kapalı, adopt yolu |
| boot-arg `-xeintelfbnokms`, `-xeintelfbnoext` | framebuffer i915 köprüsünü / harici monitöre geçişi kullanmaz |
| dosya `/Library/XeGfx/gu605/kms-failed` | KMS aşama 4 başarısız olduktan sonra kendiliğinden yazılır: sonraki açılışlar adopt; silmek yeniden dener |
| dosya `/Library/XeGfx/gu605/autostart-off` | otomatik koşu kapalı (onaylanmayan bir koşudan sonra kendiliğinden yazılır) |
| Safe Mode | koşucu hiçbir şey yapmaz |

Bir aşama donarsa: XeGfx'in bekçisi 60 sn'de bilerek panik yapar → yeniden açılış → koşucu "onaylanmadı" görür, kendini
kapatır → masaüstü Intel framebuffer'ında yazılımsal olarak gelir. USB kurtarma diski gerekmez.

## 4. Geri alma

1. macOS: `sudo bash /Library/XeGfx/gu605/gu605-uninstall.sh` (her şey `/Library/XeGfx/gu605-removed-<tarih>/`'e
   taşınır, aux koleksiyonu bizim kext'lerimiz olmadan yeniden kurulur; NullMoth'unkiler kalır).
2. EFI: `config.plist`'i `/home/claude/gu605-efi-work/kioxia-snapshot-20261104-1733/EFI/OC/config.plist` ile değiştir
   (sha256 `edfb6abf…22f`); `EFI/OC/Kexts/XeGfx.kext` kalsa da zararsız (config'te yoksa yüklenmez).
3. Acil (macOS açılmıyor): G-Helper → Ultimate; olmazsa OpenCore seçicide `-xegfxoff -xegfxnoauto -xeintelfboff`.
