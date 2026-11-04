# GU605MY: Intel Arc iGPU (8086:7D55) ile macOS — fizibilite

Durum tarihi: 2026-11-04. Makine: ASUS ROG Zephyrus G16 GU605MY (BIOS 329), Core Ultra 9 185H (Meteor Lake-H),
iGPU `8086:7D55` alt sistem `1043:3778` `PciRoot(0x0)/Pci(0x2,0x0)` (`\_SB.PC00.GFX0`), dGPU RTX 4090 Laptop
`10DE:2757` (NullMoth 2.6.6), macOS Sequoia 15.8.1, panel Samsung ATNA60DL01-0 OLED 2560x1600 240 Hz eDP.
Bu belge donanımda denenmeden, kaynak kod, yayınlanmış test raporları ve sahibinin config/DSDT dosyalarından yazıldı.
Güven düzeyleri buna göre verilmiştir.

## 1. Kısa karar

| Hedef (sahibin önceliği) | Bugün başkasının makinesinde | GU605'te beklenti | Güven |
|---|---|---|---|
| 1a. Standard modda dahili eDP'de görüntü (firmware'in yaktığı mod) | MTL 7D55 ThinkBook'ta çalışıyor (samwu07) | **Olası.** NMIntelFB firmware modunu devralır; IONDRVSupport engeli kalabilir | orta-yüksek |
| 1b. Eco modda dahili eDP | aynı (ThinkBook'ta dGPU yok = Eco eşdeğeri) | **Olası**, Standard ile aynı yol | orta-yüksek |
| 2. iGPU'da Metal + WindowServer kompozisyonu | ARL 7D67 (WTL, saatlerce), MTL 7D55 (samwu07, ~60 fps, açılışta otomatik) | **Olası**, ama NullMoth ile aynı anda çalışması (Standard) kimse tarafından denenmedi | orta (Eco), düşük-orta (Standard) |
| 3. Sol USB-C (DP alt mode) harici monitör, iGPU üzerinden | Hiçbir XeGfx/i915-on-XNU projesinde yok | **Bugün yok.** i915 display kodunun açılması + yeni bir KMS köprüsü gerekir | düşük (haftalar-aylar) |
| Ultimate modu aynı OpenCore config ile bozulmadan | — | **Olası**: sürücüler iGPU'da panel yoksa hiç başlamaz | yüksek |
| eDP parlaklığı (OLED) iGPU üzerinden | WTL: yok; VedCloud: PWM ile var | DPCD AUX ile **olası** (OLED'de PWM yok) | orta |
| Panel 240 Hz | hiçbir yerde (XeGfx mode-set yapmaz) | firmware hangi hızı yaktıysa o (ölçülür) | — |

## 2. Elimizdeki temel (prior art)

- **WTLIntelGfx / XeGfx 0.0.47** (BSD-2 çekirdek + PolyForm NC Metal katmanı): Linux i915'i FreeBSD drm-kmod +
  LinuxKPI üzerinden XNU'ya taşır. GuC (`mtl_guc_70.bin`, 70.53.0) ile gönderim, ANV Vulkan, Metal eklentisi
  (NullMoth fork'u), XeGfxAccel (NMXAccel fork'u), XeGfxAGDC, NMIntelFB/XeGfxIntelFB. i915 display kodu **kapalı**
  (`i915_xegfx_no_display=1`): panel firmware'in yaktığı modda kalır, WindowServer karesi ya kopyalanır ya da
  (ArrowWhatever / samwu07 yamaları) GGTT üzerinden sıfır-kopya flip yapılır. Kaynak: WTL README, docs/HOW-IT-WORKS.md.
- **samwu07/xegfx-thinkbook14plus-mtl** (BSD-2 + PolyForm yamalar), **aynı iGPU 7D55, aynı macOS 15.8.1**:
  - GOP framebuffer'ı GGTT `0xFE800000`'de `GUC_GGTT_TOP` (`0xFEE00000`) sınırını kesiyor → stage 4 `-28` ya da
    panelde GuC görüntüsü bandı (WTL issue #13). Çözüm: `XgFbMove.kext` XeGfx'ten önce framebuffer'ın GGTT
    girişlerini `0x80000000`'e kopyalayıp `PLANE_SURF`'ü vblank'te yeniden yönlendiriyor.
  - `xegfx.nvram_trace=0` şart (aksi halde girişte donma).
  - WindowServer çökme döngüsü: CoreDisplay `default.metallib` için AIR bitcode okuyucu gerekti (WTL eklentisinde;
    NullMoth 2.6.6 eklentisi NVIDIA'da bunu zaten çözüyor).
  - AGDC/IOPresentment: `debug.xegfx_agdc_k5=1`, `debug.nvaccel_iop=1`, EDID zamanlaması, `IOFBDependentID/Index`.
  - VBL kaynağı olmadan WindowServer 30 Hz'de kalıyor; pipe A kare sayacından VBL ile ~58-60 fps.
  - Açılışta otomatik başlatma: LaunchDaemon + NVRAM "onaylanmadı" bayrağı (çökme döngüsünü önler).
  - Bilinen: panel 60 Hz (firmware'in modu), bulanıklık (vibrancy) saydam, bazı widget köşeleri hatalı.
- **VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics** (Dell Latitude 5350, `7D45`, 15.8.1): i915 türevi GPL çekirdek,
  Metal/Vulkan doğrulanmış, masaüstü "çalışıyor ama takılıyor", PWM parlaklık çalışıyor; GuC 70.53.0, HuC 8.5.4,
  DMC 2.23, GSC 102.1.15.1926 yüklüyor. XeGfx'ten bağımsız, GPL — kod alınmadı, sadece ders.
- **NullMoth 2.6.6**: `NMIntelFB` (Intel panelini NVIDIA'nın 5. başı olarak yayınlar, NVIDIA karesi CPU ile Intel
  panele kopyalanır; `debug.nvaccel_igpu=1` ile, varsayılan kapalı, "deneysel"); NVAccel "headless" modu (panel başka
  GPU'daysa NVIDIA yalnız uygulamalara Metal verir); `NVRMFBClaim`; eklenti tek-işlem-tek-GPU kuralı
  (`nvmtl_vk_set_accelerator`: ilk hızlandırıcı kazanır).
- **NootedBlue (ChefKiss)**: Apple'ın kendi Intel sürücüleriyle eski nesiller için Lilu eklentisi; Xe-LPG (MTL) için
  bir yol değil (doğrulanmadı, düşük öncelik). **WhateverXe / NootedGreen**: Apple TGL sürücüsü; MTL'ye ulaşmıyor.

## 3. MTL'ye (display 14, Xe-LPG 12.70) özgü farklar

| Konu | Durum |
|---|---|
| GT | Xe-LPG 12.70 (7D55 ve ARL-S 7D67 i915'te aynı MTL aile kodu). Execlist yok, GuC submission zorunlu (`enable_guc=3`). |
| GuC | `mtl_guc_70.bin` (her 70.x; i915 en az 70.12.1 ister). Kext ikilisine gömülü (aux KC'de `OSKextRequestResource` çalışmıyor). |
| HuC / GSC | MTL'de HuC, GSC (`mtl_gsc_1.bin`) üzerinden doğrulanır, GSC proxy için MEI gerekir: macOS'ta yok. Sadece medya (video çözme) için; Metal/görüntü için **gereksiz**. Gömülmedi. |
| DMC | `mtl_dmc.bin`: sadece i915 display'i sürerken (DC5/DC6 güç durumları). Display kapalı olduğu sürece gereksiz; Aşama 3 (harici USB-C) için gerekecek. CI indirip doğruluyor, gömülmesi Aşama 3'e bırakıldı. |
| Ekran motoru | Xe_LPD+ (display 14), PICA/C10 (port A/B, eDP) ve C20 (TC1-TC4) PHY. i915 display kapalıyken PHY'ye dokunulmaz. |
| Çalınmış bellek | `Wa_22018444074`: BAR üzerinden stolen'a CPU erişimi makineyi asabilir → konsol adresi kullanılır (NMIntelFB zaten öyle). |
| GGTT yerleşimi | GOP framebuffer'ı GGTT tepesine yakın; `[0xFEE00000, 4 GiB)` GuC'a ayrılmış → **XgFbMove** gerekebilir (GU605'te 2560x1600x4 = 15,6 MiB; adres stage 2 ile ölçülecek). |

## 4. Mimari seçimi: Standard modda kim kompoze eder?

Standard modda (Hybrid/Optimus) dahili panel iGPU'ya, HDMI NVIDIA'ya, sol USB-C iGPU'ya bağlı. İki yol var:

**Yol A — "NVIDIA kompoze eder, Intel panel gösterir" (NullMoth'un kendi deneysel yolu):** NMIntelFB paneli
NVAccel'in 4. başı olarak yayınlar, `debug.nvaccel_igpu=1` + `debug.nvaccelfb=3` + WindowServer yeniden başlatma.
Artı: tek GPU'da WindowServer (HDMI + dahili panel aynı yerde), i915 gerekmez. Eksi: her kare NVIDIA VRAM'den
(BAR1, önbelleksiz okuma) Intel'e **CPU ile kelime kelime kopyalanır** (2560x1600 = 16 MB/kare) → muhtemelen
15-40 fps ve bir çekirdek yükü; dGPU hiç uyumaz (pil). Eco'da çalışmaz.

**Yol B — "Intel kompoze eder, NVIDIA uygulamalara ikinci Metal cihazı" (samwu07/WTL yolu + NullMoth headless):**
XeGfx + XeGfxAccel + NMIntelFB(baş 4) + XeGfxAGDC; NullMoth "headless" kalır (panelin sahibi NMIntelFB görünür).
Artı: Eco ile aynı yığın, zero-copy flip ile ~60 fps (samwu07), dGPU boştayken uyuyabilir. Eksi: NullMoth ile aynı
anda çalışma kimse tarafından denenmedi; çakışmalar (aşağıda) çözülmeli; HDMI'deki NVIDIA monitörü WindowServer'ın
ikinci bir Metal cihazına ihtiyaç duyar (eklenti işlem başına tek GPU) → Standard + HDMI harici şimdilik desteklenmez.

**Seçim:** Ana yol **B** (Eco'yu da çözer, sahibin 2. önceliği Metal'i iGPU'da ister). Yol A, Standard'da ilk
görüntü testi ve yedek olarak belgelendi (NullMoth sysctl'leriyle, kod değişikliği gerektirmez).

### Yol B'de NullMoth ile çakışmalar ve bu daldaki çözümler

| Çakışma | Etki | Çözüm (dal `gu605-mtl`) |
|---|---|---|
| XeGfxAccel, NVAccel ile aynı `debug.nvaccel*` sysctl adlarını kaydeder ("Hybrid'de NVIDIA bus'ta değil" varsayımı) | Standard'da aynı adlı iki OID, betikler yanlış sürücüyü okur | GU605 derlemesi (`-DXEGFX_GU605`) adları `debug.xeaccel*` olarak kaydeder |
| NMIntelFB `IOFBDependentID` = NVRM'nin değeri | Intel paneli NVIDIA başlarıyla aynı denetleyici grubuna düşer | 7D55'te ve XeGfx yolunda ayrı ID (`XGFX`) |
| Eklenti: tek işlem tek GPU, ilk gelen kazanır | Uygulama rastgele Intel ya da NVIDIA alır | GU605 politikası: WindowServer → Intel (panel Intel'deyse), diğer işlemler → NVIDIA (NVAccel varsa); Eco'da allow-list |
| WTL eklentisi NullMoth 2.6.2 tabanlı | NullMoth 2.6.6 çekirdeğiyle NVIDIA yolunu geriletebilir | 2.6.6 eklentisine WTL'nin Intel değişiklikleri 3-yollu birleştirildi (`gu605/plugin`) |
| `com.apple.iokit.IONDRVSupport` Block (Ultimate için) | XeGfxIntelFB (IONDRV alt sınıfı) yüklenemez | Framebuffer olarak IONDRV'ye bağımlı olmayan **NMIntelFB** kullanılır; XeGfxAGDC NMIntelFB'yi de tanır |

## 5. Ultimate modu korumak (aynı OpenCore config)

- `disable-gpu` **kaldırılmalı** (WhateverGreen bu özellikle iGPU'yu gizler; hiçbir iGPU sürücüsü eşleşemez).
  Ultimate'te etkisi: iGPU PCI'da görünür (MUX dGPU'dayken iGPU'nun bus'ta kalıp kalmadığı stage 2 ile
  doğrulanacak; bazı laptoplarda sınıf `0380` olur ya da tamamen kaybolur). Panel NVIDIA'da olduğu için:
  - NMIntelFB: hiçbir pipe yanmıyor / konsol iGPU'da değil → `start()` reddeder, hiçbir şey yayınlamaz.
  - XeGfx: personality yok, kendiliğinden yüklenmez; otomatik başlatıcı (LaunchDaemon) "panel iGPU'da değil"
    görünce hiçbir şey yapmaz (NMIntelFB yoksa → Ultimate).
  - WhateverGreen iGPU'yu görür; 7D55 için Apple framebuffer'ı eşleşmez. Risk düşük, ilk açılışta log kontrol edilir.
- `Kernel > Block IONDRVSupport` **kalır**: Ultimate'te NVRMFB'nin 0. indeksini korur; Standard/Eco'da panel
  NMIntelFB'de olur. Yan etki: NMIntelFB başarısız olursa Standard/Eco'da **hiç framebuffer yok** (siyah ekran,
  sistem ayakta). Kurtarma: G-Helper ile Ultimate'e dönmek (Windows'tan) veya `-nmintelfboff`... ama o da siyah
  verir; asıl kurtarma Ultimate + yedek EFI USB'si (GU605-KURULUM.md).

## 6. Eco modu

Eco'da (G-Helper `dgpu_disable`, ACPI `ATKD WMNB 0x00090020` → `CUMA=1`, `PXSX` eject) NVIDIA PCI'dan
kalkar: NullMoth hiç eşleşmez, `nvfb`/`nvaccel` boot-arg'ları etkisiz. Tek GPU iGPU'dur: samwu07 ThinkBook'un
birebir durumu. Yol B gerekir; XeGfx başarısız olursa NMIntelFB üzerinde yazılımsal WindowServer kalır (Sequoia
bunu yapabilir, yavaş). Not: Eco'da `debug.nvaccel*` adları boştur ama GU605 derlemesi yine `xeaccel*` kullanır
(betikler tek yol bilir).

## 7. Parlaklık (OLED)

DSDT'de GFX0 yalnız `_ADR` içeriyor; `_BCM/_DOD` bir SSDT'de (verilmedi). NVIDIA tarafı `_BCM`'i `BCLP/ASLC`
OpRegion posta kutusuna yazıyor (sürücünün ASLE kesmesini işlemesi gerekir). Intel tarafı büyük olasılıkla aynı
OpRegion yolunu kullanır → sürücüsüz etkisiz. ATNA60DL01 bir **OLED**: parlaklık PWM değil, **DPCD AUX** ile
(Intel HDR arayüzü `0x340-0x35x`, nits; ya da VESA `0x720-0x723`). NMIntelFB'ye (7D55'e kapılı) AUX A üzerinden
yetenek okuma ve yalnız kullanıcı kaydırıcıyı oynattığında yazma eklendi (`IODisplayParameterHandler`).
Firmware parlaklığı açılışta değiştirilmez. Alt sınır %5 (NullMoth ile aynı; 0 bazı panellerde ekranı tamamen
kapatır). Linux i915 bu panel için VBT'deki "backlight control type" alanına bakar (`aux` ise önce Intel HDR, sonra
VESA dener; `enable_dpcd_backlight` parametresi); VBT okunmadığından burada sıra aynı, otomatik.

**Ultimate'te dokunulmaz:** panel NVIDIA'dayken NMIntelFB `start()`'ta reddeder (iGPU'da yanan pipe/konsol yok),
dolayısıyla ikinci bir `IODisplayParameterHandler` kaydedilmez; NullMoth'un `nvrmacpibl=1` ACPI parlaklığı aynen
çalışır.

## 8. Aşama 3: sol USB-C (DP alt mode) harici monitör

Bugün **hiçbir** i915-on-XNU projesi harici ekran sürmüyor; tümü i915 display'i kapalı tutup firmware'in yaktığı
eDP'yi devralıyor. Sol port (Windows'ta iGPU'ya bağlı, ASUS'a göre TB4 portu; DSDT/VBT'den teyit gerek — stage 2'ye
eklenen TC durum okuması hangi TC portunun HPD verdiğini gösterecek) için gerekenler:

1. **i915 display'in açılması** (`xegfx_no_display=0`) ve eDP'nin "fastboot" ile devralınması (aksi halde panel
   yeniden mode-set edilir). DMC (`mtl_dmc.bin`) gömülmeli. Riskler: i915 display init'i tüm pipe/PHY/power-well
   durumunu okur, AUX/HPD kesmeleri, `Wa_22018444074`.
2. **DP alt mode girişi**: Linux'ta MTL'de bunu `intel_pmc_mux` (UCSI'den gelen olayla PMC IPC üzerinden IOM'a
   komut) yapar; Windows'ta Intel/ASUS PD yazılımı. Bazı laptoplarda EC/PD denetleyici kendi başına yapar.
   **Ucuz test:** monitör sol porta takılıyken OpenCore seçicisi/BIOS logosu harici ekranda da görünüyor mu ya da
   monitör "sinyal var" diyor mu? Görünüyorsa alt mode OS'suz giriliyor demektir (iyi haber). Görünmüyorsa macOS'ta
   bir UCSI + PMC mux sürücüsü yazmak gerekir (ayrı proje).
3. **TC PHY sahipliği / HPD**: i915'in `intel_tc.c` (xelpdp) yolu bunu display MMIO üzerinden yapar
   (`XELPDP_PORT_BUF_CTL1` TC_PHY_OWNERSHIP, `TCSS_DDI_STATUS` READY/HPD); i915 display açılırsa hazır gelir.
4. **Link eğitimi** HBR3 (2560x1440@240 ≈ 23,5 Gbit/s ham, 4 şerit HBR3 yeterli; DSC gerekmez): i915 yapar.
5. **macOS köprüsü**: i915'in DRM KMS çıktısını bir `IOFramebuffer`'a bağlayan yeni kext (bağlantı/çıkarma →
   online/offline, mod listesi EDID'den, sayfa çevirme). XeGfxAccel'in birden fazla Intel başı desteklemesi.
6. **macOS USB/TB yığını**: MTL TCSS xHCI (`8086:7EC0`) Apple'ın XHCI sürücüsüyle çalışır; DP alt mode şeritleri
   PD/IOM tarafında ayrılır, xHCI'nin buna karışmaz. Apple'ın Thunderbolt NHI sürücüsü MTL'nin entegre TBT'sini
   tanımaz (eşleşme yok) → DP tunnelling (TBT/USB4 dock) **yok**; doğrudan USB-C→DP kablo (alt mode) tek seçenek.
   USBToolBox haritası sol portun SS şeritlerini etkilemez; XhciPortLimit kapalı kalabilir.

**Değerlendirme:** Aşama 3, Aşama 1-2 sağlam çalıştıktan sonra ayrı bir proje (tahmini haftalar). Bu dalda yalnız
salt-okunur TC tanılaması (stage 2, `xegfx.tcprobe=1`) ve DMC'nin CI'da doğrulanması var. O zamana kadar harici
monitör **sağ portta NullMoth ile** (Ultimate/Standard) kullanılmaya devam eder; Standard + Yol B'de NVIDIA
başındaki harici ekran WindowServer için ikinci Metal cihazı gerektirdiğinden desteklenmez (bkz. §4).

## 9. Riskler

- **I-116** (WTL, açık): saatler süren yoğun disk okumasında CPU rendezvous panik. MTL'de de beklenmeli.
- GuC bandı / GGTT çakışması (XgFbMove ile; ilk stage 2 ölçümü şart).
- Girişte donma: `xegfx.nvram_trace=0` (varsayılan) dışında NVRAM izi kullanma; NMIntelFB'nin her adımda
  NVRAM `sync()` yapması samwu07'de açılışı durdurmuş olabilir → 7D55'te varsayılan kapalı (`nmintelfbtrace=1`).
- Standard'da iki GPU sürücüsü aynı anda: test edilmemiş kombinasyon (NullMoth headless + XeGfxAccel).
- NullMoth güncellemesi (2.7.x) eklenti birleştirmesini geçersiz kılar: GU605 eklentisi yalnız 2.6.6 ile.
- OLED'de AUX yazmaları: yanlış arayüz → parlaklık değişmez (zararsız); yazma yalnız kaydırıcıda.
- Panel firmware hızında (büyük olasılıkla 60 Hz, ölçülecek). 240 Hz için mode-set = Aşama 3 ile aynı iş.

## 10. Aşamalı test planı

| Aşama | Mod | Ne | Geçti sayılır | Geri dönüş |
|---|---|---|---|---|
| 0 | Ultimate | `disable-gpu` kaldır, yeni kext'ler OpenCore'da, hiçbir şey çalıştırma | Normal açılış, NullMoth 240 Hz, `ioreg` iGPU'yu görüyor | eski config |
| 1a | Ultimate | `kmutil load` XeGfx, stage 1-2 (salt okunur) | `XeGfxProbe` sonuç 1; iGPU sınıfı/BAR'lar raporlanır | yeniden başlat |
| 1b | Standard | sadece açılış: NMIntelFB paneli devralıyor mu (yazılımsal masaüstü) | Masaüstü dahili panelde, `NMIntelFBPipe` var | Ultimate'e dön |
| 1c | Standard | stage 2 → `boot-fb` GGTT adresi; gerekirse XgFbMove; stage 3-6 | GuC çalışıyor, motorlar istekleri bitiriyor | yeniden başlat |
| 1d | Standard | stage 7 + Metal (mtlprobe), WindowServer **değil** | Intel Metal cihazı test aracında doğru çiziyor | yeniden başlat |
| 1e | Standard | `gu605-desktop-on.sh`: WindowServer Intel'de, NullMoth headless | Panelde kareler, 10 dk çökme yok | koruma kendiliğinden kapatır / yeniden başlat |
| 1f | Standard | otomatik başlatıcı açık | 3 açılış üst üste onaylı | `-xegfxnoauto` |
| 2 | Eco | 1b-1f aynısı, NVIDIA yokken | aynı | Standard'a dön |
| 3 | Standard/Eco | (gelecek) i915 display, sol USB-C | — | — |

## Kaynaklar

- WTLIntelGfx: https://github.com/watchthelight-labs/WTLIntelGfx (README, docs/HOW-IT-WORKS.md, CHANGELOG)
- WTL issue #13 (MTL 7D55 GuC bandı): https://github.com/watchthelight-labs/WTLIntelGfx/issues/13
- samwu07 ThinkBook 14+ MTL: https://github.com/samwu07/xegfx-thinkbook14plus-mtl ve
  https://github.com/samwu07/xegfx-thinkbook14plus-mtl/releases/tag/backup-20261104
- VedCloud MTL: https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics
- NullMoth: https://github.com/nullmoth/nvidia-macos-driver (docs/AUTO-PRIMARY-GPU.md, docs/MULTI-GPU.md,
  kexts/NMIntelFB, kexts/NVRM/accel/nvrm-accel.cpp, plugin/nvmtl_vk.c)
- Linux i915 (MIT başlıklar): `intel_dp_aux_backlight.c`, `intel_tc.c`, `intel_uc_fw.c`, `intel_dmc.c`,
  `intel_dp_aux_regs.h`; linux-firmware `i915/` (LICENSE.i915)
- Sahibin dosyaları: `config.plist` (DeviceProperties, Kernel>Block IONDRVSupport), `DSDT.aml`
  (`ATKD.WMNB` 0x00090016 → `DGPM` MUX, 0x00090020 → `CUMA` Eco, `RP12.PXSX` `_BCM` → `BCLP/ASLC`)
