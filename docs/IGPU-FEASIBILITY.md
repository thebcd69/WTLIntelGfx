# GU605MY: Intel Arc iGPU (8086:7D55) ile macOS — fizibilite

Durum: 2026-11-04 (ikinci sürüm). Makine: ASUS ROG Zephyrus G16 GU605MY (BIOS 329), Core Ultra 9 185H (Meteor
Lake-H), iGPU `8086:7D55` alt sistem `1043:3778` `PciRoot(0x0)/Pci(0x2,0x0)` (`\_SB.PC00.GFX0`), dGPU RTX 4090
Laptop `10DE:2757` (**NullMoth 2.7.1**), macOS Sequoia 15.8.1, panel Samsung ATNA60DL01-0 OLED 2560x1600 240 Hz eDP.
Donanımda denenmeden; kaynak kod, aynı iGPU'daki test raporları, sahibinin config/DSDT/OpenCore günlükleriyle yazıldı.

Sahibin öncelik sırası: **1) Eco: dahili panel + Metal/WindowServer + yerel parlaklık**, 2) Eco: **sol USB-C**
harici monitör, 3) bonus: Standard (NullMoth ile birlikte), 4) DDC (ayrı, en son). Tek OpenCore config ile Ultimate
(NullMoth) bozulmadan kalmalı.

## 1. Kısa karar

| Hedef | Kanıt (başka makinede) | GU605 beklentisi | Güven |
|---|---|---|---|
| 1a. Eco: dahili eDP'de görüntü | samwu07 ThinkBook 14+ (aynı 7D55, aynı 15.8.1) | **Olası**: XeGfxIntelFB firmware'in yaktığı modu devralır | yüksek |
| 1b. Eco: iGPU'da Metal + WindowServer | samwu07: ~58-60 fps, açılışta otomatik; WTL ARL 7D67: saatlerce | **Olası** (aynı yığın + yamalar) | orta-yüksek |
| 1c. Eco: OLED parlaklık (kaydırıcı/tuşlar) | WTL/samwu: yok; VedCloud: PWM | DPCD AUX ile **olası** (OLED'de PWM yok) | orta |
| Ultimate aynı config ile bozulmaz | — | **Evet**: Intel framebuffer panel iGPU'da değilse başlamaz, koşucu hiçbir şey yapmaz | yüksek |
| 2. Eco: sol USB-C harici (DP alt mode) | hiçbir i915-on-XNU projesinde yok | **Bugün yok**; iki yol (§8) | düşük |
| 3. Standard (NullMoth ile birlikte) | denenmedi | dahili panel yazılımsal masaüstü: olası; Intel hızlandırma: ad çakışmaları nedeniyle şimdilik kapalı | düşük |
| Panel 240 Hz | hiçbir yerde | firmware'in yaktığı hız (büyük olasılıkla 60 Hz; ölçülür) | — |

## 2. Temel aldığımız işler

- **WTLIntelGfx / XeGfx 0.0.47** (BSD-2 çekirdek; PolyForm NC Metal katmanı): Linux i915, FreeBSD drm-kmod +
  LinuxKPI ile XNU'da; GuC `mtl_guc_70.bin` (70.53.0) gönderimi, Mesa ANV, NullMoth türevi Metal eklentisi ve
  hızlandırıcı (XeGfxAccel = NMXAccel), XeGfxAGDC, XeGfxIntelFB. i915 display kodu **kapalı**: panel firmware'in
  yaktığı modda kalır, WindowServer karesi kopyalanır ya da GGTT üzerinden sıfır-kopya gösterilir.
- **samwu07/xegfx-thinkbook14plus-mtl** (BSD-2 + PolyForm yamalar) — **en yakın örnek**, aynı iGPU ve macOS:
  GuC bandı (WTL #13) → `XgFbMove`; `xegfx.nvram_trace=0`; CoreDisplay için AIR bitcode okuyucu; IONDRV boot
  framebuffer'ına konsol aralığı (bootvram), `IOFBDependentID/Index`, EDID zamanlaması, AGDC K5; pipe A kare
  sayacından VBL (30 → 60 fps); Intel sıfır-kopya flip; LaunchDaemon + "onaylanmadı" koruması. Bilinen: 60 Hz,
  saydam vibrancy, bazı widget köşeleri. Bu daldaki yığın bu yamaları olduğu gibi içerir.
- **VedCloud MTL** (Latitude 5350, 7D45, GPL): Metal/masaüstü (takılıyor), PWM parlaklık. Kod alınmadı.
- **NullMoth 2.7.1**: NVAccel "headless" (panel başka GPU'daysa NVIDIA yalnız uygulamalara Metal), `NVRMFBClaim`,
  NMIntelFB (Intel paneli NVIDIA'nın 5. başı, deneysel, CPU kopyası), eklentide "işlem başına tek GPU" kuralı.
- **NootedBlue / WhateverXe / NootedGreen**: Apple'ın Intel sürücüleri; Xe-LPG'ye (MTL) ulaşmıyor.

## 3. Meteor Lake'e (display 14, Xe-LPG 12.70) özgü noktalar

| Konu | Durum |
|---|---|
| GT | Xe-LPG 12.70; execlist yok, GuC submission zorunlu (`enable_guc=3`). |
| GuC | `mtl_guc_70.bin` kext'e gömülü (aux KC'de `OSKextRequestResource` çalışmıyor). CI linux-firmware'den indirir, SHA-256 kilidi `gu605/firmware.lock`. |
| HuC/GSC | MTL HuC'u GSC doğrular, GSC proxy MEI ister (macOS'ta yok). Yalnız video çözme: gerekmiyor. |
| DMC | `mtl_dmc.bin` yalnız i915 display'i sürerken (Aşama 3); CI indirip doğrular, gömülmez. |
| Ekran | Xe_LPD+, port A/B C10 PHY (eDP), TC1-TC4 C20 PHY (Type-C). i915 display kapalıyken PHY'ye dokunulmaz. |
| Stolen | `Wa_22018444074`: BAR üzerinden stolen'a CPU erişimi asabilir → konsol adresi kullanılır. |
| GGTT | GOP yüzeyi GuC yuvası `[0xFEE00000, 0xFF000000)` ile çakışırsa band. 2560x1600 GOP'u (WTL Legion) `0xFF000000`'de: taşımaya gerek yok; XgFbMove yalnız çakışmada taşır, stage 4 çakışmada reddeder. |

## 4. Mimari (Eco öncelikli)

- **Framebuffer: XeGfxIntelFB 0.9.5** (IONDRVFramebuffer alt sınıfı + samwu07 bootvram/VBL). OpenCore günlüğü
  `Kernel>Block IONDRVSupport`'un **etkisiz** olduğunu gösteriyor ("Prelinked blocker … Not Found": IONDRVSupport
  Sequoia'da sistem koleksiyonunda) → IONDRV tabanlı yol GU605'te de kullanılabilir. 7D55 için yalnız PCI kimliğiyle
  eşleşir (WhateverGreen, `disable-gpu` kalkınca GFX0'ı IGPU yapar) ve **yalnız pipe A yanıyor ve konsol boyutu
  tutuyorsa** başlar (Ultimate'te başlamaz).
- **Hızlandırıcı: XeGfxAccel 0.9.6** (Sequoia derlemesi + samwu07 flip/gpufb/işaret yamaları), yalnız 7D55,
  yalnız stage 7 sonrası (`nmxgpu-claimed`).
- **Metal eklentisi: XeMTLDriver.bundle** (WTL eklentisi + samwu07 yaması) — NullMoth'un `NVMTLDriver.bundle`,
  `/Library/GPUBundles/nvmtl/` ve `nvmtl-allow.txt` dosyalarından **tamamen ayrı** (`xemtl/`, `xemtl-allow.txt`,
  çevirici paket içinde). NullMoth güncellemesi Intel yığınını silemez; Intel yığını NullMoth'a dokunmaz.
- **Açılış otomasyonu:** LaunchDaemon `gu605-auto.sh` modu kendisi bulur (Intel framebuffer var mı, NVIDIA ekran
  aygıtı var mı): Ultimate → hiçbir şey; Eco → `ECO_LEVEL=desktop`; Standard → `probe` (salt okunur).
  XeGfx OpenCore `Kernel>Add` ile boot koleksiyonunda (samwu07: aux'tan yüklemek dondurdu), kendi başına eşleşmez.

## 5. Eco'da NullMoth

Eco'da dGPU PCI'dan kalkar (ACPI `ATKD.WMNB 0x00090020` → `CUMA=1`, `RP12.PXSX` eject). NullMoth 2.7.1'in
`NVRM/NVAccel/NVRMFB` kişilikleri `10DE` ister → yüklenmez. Her zaman yüklenenler (`NVRMAGDC` ve
`NVPrimarySelector` IOResources yükleyicileri) boşta kalır; adları (`debug.nvrmfb_agdc*`) XeGfxAGDC'ninkilerle
(`debug.xegfx_agdc*`) çakışmaz. NullMoth boot-arg'ları (`nvfb=1 nvaccel=1 nvfbheads=4 …`) ve NVIDIA
`DeviceProperties` Eco'da etkisiz. NullMoth'un açılış koruması (iki başarısız başlatmada kapanma) NVRM hiç
başlamadığı için sayaç artırmaz. Sonuç: **tek config yeterli**; yalnız `disable-gpu` kalkar ve XeGfx eklenir.

## 6. Ultimate'te dokunulmazlık

- Intel framebuffer `probe()`: pipe A kapalı / konsol NVIDIA BAR1'de → reddeder; ikinci framebuffer, ikinci
  parlaklık aygıtı yok. NullMoth'un `nvrmacpibl=1` ACPI parlaklığı aynen.
- XeGfx boşta, koşucu "ultimate: nothing to do". XeGfxAccel `nmxgpu-claimed` olmadan eşleşmez.
- Sol USB-C Ultimate'te de iGPU'ya bağlıysa orada görüntü yok (bugün de yok); kabul edilebilir.

## 7. Parlaklık (OLED, Eco/Standard)

DSDT'de GFX0 yalnız `_ADR`; `_BCM` Intel IGD SSDT'sinde (OpRegion `BCLP/ASLC`, sürücünün ASLE işlemesi gerekir:
yok). OLED → DPCD AUX. Uygulama Linux i915 `intel_dp_aux_backlight.c` ve `drm_dp_helper.c` ile birebir: Intel HDR
arayüzü (tcon sürümü ≥1, eDP ≤1.4b, EDID'de HDR statik metaveri: aralık oradan, yoksa 512 nit; AUX etkin bit 4 @
0x344, seviye 0x354, aralık 0x34A), değilse VESA (AUX seviye / eDP 1.5 parlaklık mili-nit @ 0x734). Probe salt
okunur; Intel kaynak OUI'si yalnız VESA yoksa yazılır. İlk kaydırıcı/tuş hareketinde DPCD kontrolü açılır; alt sınır
%5 (NullMoth gibi). Boot-arg: `-xeintelfbnobl`, `xeintelfbbl=intel|vesa`, `xeintelfbnits=<max>`.

## 8. Sol USB-C (Eco, Aşama 2)

Sahip: sol port Standard ve Eco'da Windows'ta iGPU'dan görüntü veriyor (ASUS: TB4 portu) → TCSS TC portlarından
biri (TC1-TC4). Hangisi olduğu stage 2'nin yeni salt-okunur dökümüyle belirlenir (`tcss-ddi-status-tcN` HPD/pin,
`port-buf-ctl1-tcN`, `ddi-buf-ctl-tcN`, tüm transcoder/plane'ler). İki yol:

- **3a — GOP'un yaktığı harici ekranı devralmak (ucuz):** Eco'da monitör takılıyken GOP harici ekranı da yakıyorsa
  (döküm: `transconf-B` + `plane-ctl-1-B` açık, bir `ddi-buf-ctl-tcN` açık) i915 display'e gerek yok: pipe B'yi
  kendi yüzeyine (xeflip gibi GGTT yuvası) yönlendiren ikinci bir framebuffer, GOP'un seçtiği modda (büyük
  olasılıkla 60 Hz) genişletilmiş masaüstü verir. Tahmini: günler.
- **3b — i915 display (tam):** `xegfx_no_display=0`, eDP'yi "fastboot" ile devralma, DMC gömme, TC PHY sahipliği/HPD
  (`intel_tc.c` xelpdp yolu display MMIO ile — PMC IPC gerekmez), HBR3 link eğitimi (1440p240 ≈ 23,5 Gbit/s, DSC'siz
  sığar), DRM KMS → IOFramebuffer köprüsü (bağla/çıkar, modlar, flip). Ek koşul: **DP alt mode girişi** — Linux'ta
  `intel_pmc_mux` (UCSI olayıyla PMC'ye IPC) yapar; macOS'ta yok. EC/PD kendisi giriyorsa (BIOS/OpenCore ekranı
  harici monitörde de görünüyorsa) gerekmez. Tahmini: haftalar.
- macOS USB/TB: MTL TCSS xHCI Apple'ın XHCI sürücüsüyle çalışır, şerit seçimine karışmaz; Apple'ın Thunderbolt
  sürücüsü MTL'nin entegre TBT'sini tanımaz → TB/USB4 dock üzerinden DP tünelleme yok, doğrudan USB-C→DP (alt mode).
  USBToolBox haritası ve kapalı XhciPortLimit sorun değil.

## 9. Riskler

- **I-116** (WTL, açık): saatlerce yoğun disk okumasında CPU rendezvous paniği; MTL'de de beklenmeli.
- GOP yerleşimi GU605'te ölçülmedi (stage 2 ölçer; korumalar var).
- WhateverGreen artık iGPU'yu görür; bilinmeyen MTL için Apple framebuffer'ı eşleşmez, ad değiştirme dışında etkisi
  beklenmez (ilk açılış günlüğünde kontrol).
- Userland (Mesa ANV) CI'da MacPorts LLVM 20 ile derleniyor; olmazsa koşucu aşama 6'da durur.
- Standard'da NullMoth ve XeGfxAccel aynı `debug.nvaccel*` adlarını kaydeder → masaüstü adımı Standard'da reddedilir.
- Wi-Fi: samwu07 AirportItlwm 2.4.0'da (latitude_mld dalı) `iwx_rm_sta_cmd` taşması buldu (Wi-Fi bağlandıktan
  1-5 dk sonra panik). Grafikle ilgisiz; sahibin itlwm fork'u aynı kodu içeriyorsa aynı düzeltme gerekir.

## 10. Test planı (en az el emeği; ayrıntı `docs/GU605-TEST.md`)

| Açılış | Mod | Otomatik olan | Geçti |
|---|---|---|---|
| 1 | Ultimate | hiçbir şey (koşucu not düşer) | NullMoth 240 Hz + parlaklık aynen |
| 2 | Eco (monitör sol USB-C'de) | stage 1-2 (+TC dökümü) → XgFbMove gerekirse → 3-6 → 7 + Metal testi + CoreDisplay GPU-pass → giriş ekranında WindowServer Intel'e → 3 dk onay → flip | panelde kareler, ≤1 WindowServer yeniden başlatma, parlaklık |
| (2b) | Eco | 2. açılış donarsa: koşucu kendini kapatır, yazılımsal masaüstü | rapor |
| 3 | Standard (isteğe bağlı) | stage 1-2 | yazılımsal masaüstü dahili panelde |

Her açılışta özet `EFI/GU605/last-boot.txt`'e (Windows'tan okunur); `gu605-collect.sh` tek dosya rapor.

## Kaynaklar

- WTLIntelGfx: https://github.com/watchthelight-labs/WTLIntelGfx ; issue #13: https://github.com/watchthelight-labs/WTLIntelGfx/issues/13
- samwu07: https://github.com/samwu07/xegfx-thinkbook14plus-mtl (+ releases/tag/backup-20261104)
- VedCloud: https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics
- NullMoth: https://github.com/nullmoth/nvidia-macos-driver (2.7.1; NMIntelFB, NVRM/accel, NVRMAGDC, plugin)
- Linux (Kasım 2026): drivers/gpu/drm/i915/display/{intel_dp_aux_backlight.c, intel_cx0_phy_regs.h, intel_tc.c,
  intel_dp.c}, drivers/gpu/drm/display/drm_dp_helper.c, include/drm/display/drm_dp.h, drivers/gpu/drm/drm_edid.c
- linux-firmware i915/ (LICENSE.i915)
- Sahibin dosyaları: KIOXIA EFI anlık görüntüsü (config, DSDT, OpenCore günlükleri 2026-11-04)
