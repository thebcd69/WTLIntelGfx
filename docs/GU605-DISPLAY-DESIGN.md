# GU605 — i915 ekran yolu (KMS) tasarımı

Dal: `gu605-display` (gu605-mtl üzerine). gu605-mtl'deki "firmware'in açtığı pipe'ı benimse" (adopt) yolu
değişmeden kalır ve bu yolun **otomatik geri dönüşüdür**.

## 0. Hedefler ve kesin kurallar

Sahibin sırası:

1. **Eco: dahili eDP 2560x1600 @ 240 Hz** (link HBR3 x4, gerekirse DSC — Linux ne seçerse; mod VBT/DisplayID'den).
   PSR / panel replay ilk sürümde kapalı.
2. **Eco: sol USB-C → PG27AQDM 2560x1440 @ 240 Hz** (DP alt mode, Type-C port sahipliği, HPD, iki yönde hotplug).
3. **Bu yoldan yerel parlaklık.**
4. **Ultimate tamamen etkisiz**; KMS yolu başarısız olursa adopt yolu otomatik devreye girer (yedeksiz siyah ekran yok).

Ekranlar **aynı anda kullanılmıyor**: dahili panel tek başına 240 Hz, harici monitör (kapak kapalı / dahili kapalı)
tek başına 240 Hz. Yani CDCLK / DBuf / bant genişliği aynı anda yalnız **bir** 240 Hz pipe'ı karşılamak zorunda;
hotplug'da temiz geçiş yeterli. İki ekranın birlikte çalışması isteğe bağlı (nice-to-have), bu tasarımın
ilk sürümünde yok (§8 D6).

## 1. i915 ekran kodu WTL'de neden kapalıydı

* `compat.linuxkpi.i915_xegfx_no_display=1` (xkpi/src/i915/intel_display_device.c): `disable_display` tek başına
  bağlantıları yine kuruyordu (Legion N-223, HDCP stub'ında panik). Tunable i915'i kendi "ekran yok" yoluna sokar.
* Ekran kodu kapalıyken i915 firmware görüntüsünü sahiplenmez; bu yüzden XeGfx iki yama ekledi:
  `xg_stolen_start` (stolen ayırıcı boot FB'nin **arkasından** başlar) ve aşama 4'teki
  `xg_i915_reserve_bootfb` (boot yüzeyinin GGTT aralığı rezerve edilir). İkisi de i915'in kendi devralma kodu
  (`intel_plane_initial.c`) ile çakışır: MTL'de i915 boot FB'yi `i915_gem_object_create_region_at(stolen, phys)`
  ile stolen içinde sahiplenir ve GGTT 0'a yeniden bağlar (GuC-top sorununu i915 kendisi böyle çözer).
  KMS kipinde bu iki yama **devre dışı** kalmalı.
* DMC (mtl_dmc.bin) gömülü değildi; GuC tek firmware'di.
* XeGfxAccel'in sıfır-kopya flip'i (xeflip) PLANE_SURF'e doğrudan yazar; i915 bunu bilmez. PSR/FBC açıkken bu
  donmuş/eski görüntü demektir → KMS kipinde **PSR, panel replay, FBC kapalı** ve DC durumları ilk sürümde kapalı.

## 2. Mimari referans: NullMoth (yalnız tasarım)

NullMoth'un NVKMS köprüsü (`kexts/NVRMFB/fb/nvrm-fb.cpp`, `kexts/NVRM/os-nvkms-xnu.cpp`) **yalnızca mimari referans**
olarak incelendi: ekran başına doğrulanmış mod listesi, macOS'un listelemediği her doğrulanmış yenileme hızının
ayrı bir mod olarak eklenmesi, `switchMode` = önce test sonra commit, boot modunun devri, hotplug'da connect
kesmesinin bir kez ateşlenmesi, "backlight" adlı parametre işleyicisi. NullMoth **PolyForm-Noncommercial**
lisanslıdır; kodu BSD-2 WTL ağacına **kopyalanmadı**. Buradaki her satır Linux i915/drm (MIT) API'leri üzerine
bağımsız yazıldı.

## 3. Katmanlar

```
 macOS WindowServer
   │  IOFramebuffer modları / setDisplayMode / connect kesmesi / "backlight"
 XeGfxIntelFB  (IONDRVFramebuffer alt sınıfı, boot FB aperture'ı — mevcut)
   │  callPlatformFunction("xgKms", op, …)            ← kextler arası bağ yok, sembol çağrısı
 XeGfx.kext  XeGfxResident  ─ sysctl debug.xegfx_kms* (test/teşhis için aynı işlevler)
   │
 xkpi/src/i915/xg_kms.c  (i915 içinde, C)  — durum dökümü, mod listesi, atomic commit, backlight, hotplug istemcisi
   │
 Linux i915 display (drm-kmod): intel_display_driver, intel_dp/ddi, intel_tc, intel_cx0_phy (C10/C20),
                                intel_dmc, intel_vdsc, intel_backlight, intel_bios (VBT), intel_hotplug
```

### 3.1 KMS kararı (XeGfx.kext, aşama 4'ten önce)

KMS yalnız şu durumların **hepsinde** açılır; aksi halde adopt yolu (bugünkü davranış):

* aygıt 8086:7D55 (GU605) — diğer her şey için tunable zorla 1 kalır,
* `debug.xegfx_env` içinde `xegfx.kms=1` (script `DISPLAY=kms` iken yazar),
* boot-arg `-xegfxnokms` **yok**,
* **ön kontrol (preflight)** geçti — aşama 2 okuması, i915'in devralmasının başarılı olacağı koşullar:
  pipe A açık ve kare sayacı ilerliyor, plane 1 açık, lineer, XRGB8888, stride = genişlik×4,
  GGTT girdisinde LM biti (`GEN12_GGTT_PTE_LM`, i915 bunu ister), fiziksel adres stolen (DSM) içinde.
  Biri bile tutmazsa sebep `XeGfxDisplay/kms-preflight` özelliğine yazılır ve adopt yolu kullanılır —
  **aynı açılışta**, çünkü aşama 4 henüz çalışmadı.

KMS kipinde: `i915_xegfx_no_display=0`, `i915_disable_display=0`, `i915_enable_psr=0`, `i915_enable_fbc=0`,
`i915_enable_dc=0` (sonra açılabilir), `xg_stolen_start` → 0, `xg_i915_reserve_bootfb` atlanır,
aşama 4 sonrası kontrol "aynı GGTT girdisi" yerine "yeni PLANE_SURF'ün girdisi **aynı fiziksel sayfayı**
gösteriyor + kare sayacı ilerliyor" olur. MTL'de BAR2 (LMEMBAR) stolen'ı doğrusal gösterdiği için macOS'un
boot FB aperture'ı (BAR2 + 8 MiB + ofset) i915 GGTT 0'a yeniden bağladıktan sonra da geçerli kalır.

### 3.2 Köprü: xg_kms.c

* **Durum dökümü** (`sysctl debug.xegfx_kms`): her bağlantı (eDP-1, DP-1..4): durum, CRTC, encoder/port,
  TC modu (dp-alt / tbt / legacy / disconnected) ve TC lane sayısı, EDID var mı, i915'in **doğruladığı** tüm
  modlar (w, h, pclk, htotal, vtotal, Hz×1000, preferred), DPCD max link hızı/lane, DSC desteği, aktif
  CRTC'nin port_clock / lane_count / pipe_bpp / DSC / output format, backlight (present, level, min, max,
  enabled, tür).
* **Mod commit** (`xg_kms_set(bağlantı, mod indeksi)`): `drm_atomic_helper_set_config` ile, kilitler
  `DRM_MODESET_LOCK_ALL_BEGIN/END` ve deadlock backoff ile. Primary plane FB:
  dahili panel aynı raster → i915'in devraldığı boot FB'nin kendisi; farklı raster (2560x1440) → **aynı GEM
  nesnesi** üzerine `intel_framebuffer_create` ile yeni FB (aynı pitch 10240). Hedef dışındaki her CRTC kapatılır
  (tek ekran kuralı). Commit başarısızsa eski durum kalır, hata kodu döner (atomic: ya hep ya hiç).
* **Önce test, sonra commit**: `DRM_MODE_ATOMIC_TEST_ONLY` eşdeğeri (`drm_atomic_check_only`) ile önce denenir.
* **Backlight**: `intel_backlight_set_acpi(conn_state, level, max)` — i915 VBT'ye göre PWM ya da DPCD (eDP
  AUX) seçer; seviye/min/max döküme yazılır. macOS 0..65535 → i915 aralığı; tabanda güvenli minimum (%5) korunur.
* **Hotplug istemcisi**: `drm_client_init(..., "xegfx", funcs)` + `drm_client_register`; `hotplug` geri çağrısı
  bağlantı durumlarını okur, bir sayaç artırır ve XeGfxIntelFB'yi (kayıtlı geri çağrı) haberdar eder.
  fbdev istemcisi yok (`NO_FBDEV`), yani i915 kendiliğinden hiçbir şeyi değiştirmez.

### 3.3 XeGfxIntelFB (macOS tarafı)

* Mod listesi: boot raster'ında i915'in doğruladığı her yenileme hızı ayrı mod (ID `0x10000 | i915 mod no << 8`),
  boot modu da kalır. `getInformationForDisplayMode` gerçek Hz'i bildirir.
* `setDisplayMode` → `xgKms set` → başarılıysa mod ID ve VBL zamanlaması güncellenir; başarısızsa
  `kIOReturnUnsupportedMode` ve panel eski modda kalır.
* Varsayılan: KMS hazır olduğunda `PANEL_HZ` (varsayılan 240) için **bir kez** otomatik geçiş — macOS kendi
  tercihini sonra hatırlar.
* Harici monitör (tek ekran): hotplug'da DP bağlandı + monitör doğrulanmış mod sundu → aynı aperture
  (16 MiB boot FB; 2560x1440x4 = 14,7 MiB sığar) üzerinde 2560x1440@en yüksek Hz commit, eDP kapanır, bağlantı
  bayrakları "harici" olur, EDID harici monitörün EDID'i olur ve connect kesmesi ateşlenir (IODisplay yeniden
  kurulur). Çıkarılınca tersi. Boot FB'ye sığmayan modlar (ör. 4K) listelenmez.
* KMS yoksa (adopt yolu) davranış gu605-mtl ile birebir aynı.

### 3.4 Script'ler

`gu605.conf`: `DISPLAY=adopt|kms` (varsayılan **adopt**; KMS ilk testte elle açılır), `PANEL_HZ=240`,
`EXT_POLICY=switch|ignore`. `gu605-stages.sh` KMS ise env'i yazar, aşama 4'ten sonra `debug.xegfx_kms`
dökümünü `display-kms.txt`'ye kaydeder; `gu605-collect.sh` bunu ve `xegfx_log`'u toplar.

## 4. Dahili eDP 240 Hz ayrıntısı

* 2560x1600@240 ≈ 1043 MHz piksel saati → 8 bpc'de ~25,0 Gbit/s; HBR3 x4 yükü 25,92 Gbit/s → **DSC'siz 8 bpc
  sığar**, 10 bpc için DSC gerekir. i915 `intel_dp_compute_config` bunu kendisi seçer (önce DSC'siz en düşük
  link, sığmazsa DSC; eDP'de "max link" kuralı). Hangisini seçtiği dökümde görünür.
* 240 Hz modu EDID'in DTD'sinde olamaz (DTD tavanı 655,35 MHz) → DisplayID genişlemesi veya VBT'den gelir. i915
  eDP'de `fixed_modes`'u EDID'deki **tüm** modlardan (aynı raster, farklı Hz) kurar; DisplayID tip VII/I
  `drm_edid` tarafından okunur. 240 Hz yalnız VBT'deyse (GOP'un kullandığı) yine `fixed_modes`'a girer.
* Raster aynı kaldığı için macOS'un belleği/pitch'i değişmez; yalnız timing değişir. VBL kaynağı pipe A kare
  sayacı olduğundan VBL kendiliğinden 240 Hz'e çıkar.
* CDCLK: 1043 MHz pclk / 2 piksel/saat ≈ 522 MHz gerekir; MTL tavanı 652,8 MHz → yeter. i915 `intel_cdclk`
  bunu kendisi yükseltir. DBuf/watermark tek pipe için hesaplanır.
* PSR2/panel replay: 240 Hz panelde güç tasarrufu sağlar ama xeflip ile uyumsuz; ilk sürümde kapalı.

## 5. Sol USB-C (Type-C) ayrıntısı

* **DP alt mode girişi i915'in işi değil.** i915 (`intel_tc.c`, xelpdp) yalnız şunu yapar: PICA ISR'de
  "DP-alt HPD" canlı durumu → `XELPDP_PORT_BUF_CTL1` içinde TCSS güç isteği (bit 5) → güç durumu (bit 4) →
  PHY sahipliği (bit 6) → pin ataması (`TCSS_DDI_STATUS` 28:25) ile lane sayısı (C/E = 4, D = 2). Alt mode
  girişini PD denetleyicisi ile PMC/IOM yapar; Linux'ta bu `ucsi_acpi` + `intel_pmc_mux` ile olur **ya da**
  firmware/PD denetleyicisi kendisi yapar.
* GU605 DSDT'sinde **UCSI (USBC000/PNP0CA0) ve PMC mux (INTC105C…) aygıtı yok**; TCSS/TXHC harici SSDT'de, DSDT'de
  "[TCSS] 3rd PD PS_ON" izleri var → üçüncü taraf PD denetleyicisi, firmware yönetimli mux. Bu, alt mode
  girişini PD/EC'nin işletim sisteminden bağımsız yaptığını düşündürüyor (Windows'ta da ek sürücü gerekmiyor).
  **Doğrulama**: aşama 2 TC dökümü (gu605-mtl'de var) monitör takılıyken `tcss-ddi-status-tcN` bit 0 (HPD alt)
  ve pin atamasını gösterir. Bit 0 hiç gelmezse alt mode'a giriş yok demektir → bu hedef PD/EC tarafında kilitli
  (risk §10).
* Bant: 1440p240 ≈ 938 MHz → ~22,5 Gbit/s; HBR3 x4 (25,92) DSC'siz yeter; 2 lane (pin D) ise DSC gerekir
  (PG27AQDM DSC destekliyorsa i915 seçer). USB-C→DP kablosu pin C (4 lane) kullanır.
* Hotplug: PICA HPD kesmesi → `intel_hotplug` → `drm_kms_helper_hotplug_event` → bizim `drm_client` hotplug
  geri çağrısı. Kesme gelmezse i915'in kendi poll'u ve köprünün 2 sn'lik yoklaması yedek.

## 6. Parlaklık

KMS kipinde i915'in `intel_backlight`'ı paneli VBT'ye göre sürer (PWM ya da DPCD/eDP AUX; GU605'in paneli OLED,
bu yüzden büyük olasılıkla DPCD — tür dökümde görünür). XeGfxIntelBacklight KMS hazırsa `xgKms backlight` kullanır, değilse
gu605-mtl'deki DPCD yolunu. Kaydedilen seviye ve %5 alt sınır korunur. Ultimate'te bu sürücüler hiç yüklenmez
(NullMoth `nvrmacpibl=1` devam).

## 7. Güvenlik ve geri dönüş zinciri

1. **Ultimate**: XeGfx hiçbir aşamayı çalıştırmaz (iGPU'da pipe yok → `gpu_mode` ultimate) — değişmedi.
2. **Ön kontrol** başarısız → aynı açılışta adopt yolu.
3. **Aşama 4 KMS sonrası kontrol** başarısız (plane kapalı / sayaç durdu / farklı sayfa) → `kms-failed` işaretlenir,
   script sonraki açılış için `DISPLAY=adopt`'a düşer (`/Library/XeGfx/gu605/kms-failed`), masaüstü seviyesi
   çalışmaz.
4. **Mod commit** başarısız → eski mod kalır (atomic).
5. **Panik / donma** → mevcut `gu605-pending` NVRAM bekçisi: sonraki açılışta otomatik çalıştırma kapanır.
6. Boot-arg'lar: `-xegfxnokms` (KMS asla), `-xegfxoff` (XeGfx hiç), `-xegfxnoauto` (script hiç).

## 8. Uygulama adımları

| Adım | İçerik | Durum |
|---|---|---|
| D1 | KMS kararı + ön kontrol + overlay atlamaları + DMC gömme + KMS sonrası kontrol | bu dalda |
| D2 | xg_kms.c: durum dökümü, mod commit, backlight, hotplug istemcisi; sysctl'ler | bu dalda |
| D3 | Script'ler: DISPLAY=kms, PANEL_HZ, döküm toplama, kms-failed geri dönüşü | bu dalda |
| D4 | XeGfxIntelFB: KMS mod listesi, setDisplayMode, KMS backlight | bu dalda |
| D5 | Harici: hotplug → raster değişimi + connect kesmesi + EDID/bağlantı bayrakları | deneysel |
| D6 | (isteğe bağlı) iki ekran birlikte: ikinci framebuffer + stolen'da ikinci yüzey | yok |

## 9. Test planı (sahip için kısa: GU605-TEST.md)

1. Önce gu605-mtl testi (Eco, `ECO_LEVEL=metal`) — KMS'siz taban çizgisi.
2. Eco + `DISPLAY=kms`, `ECO_LEVEL=gt`: açılış → script KMS ile aşama 4'ü çalıştırır → ekran **aynı kalmalı**.
   Topla: `sudo bash /Library/XeGfx/gu605/gu605-collect.sh`. Bakılacaklar: `display-kms.txt` (eDP modları
   arasında 240 Hz var mı, link/DSC), `kms-preflight`, `XeGfxPost`.
3. `sudo sysctl debug.xegfx_kms_set="eDP-1 240"` → panel 240 Hz'e geçmeli (`GU605MeasuredHz16`, döküm).
   Geri: `... "eDP-1 60"`.
4. Parlaklık: `sudo sysctl debug.xegfx_kms_bl=300` (‰), sonra klavye tuşları.
5. Harici: kapak açık, sol USB-C'ye PG27AQDM → döküm `DP-x connected`, TC modu `dp-alt`, lane 4;
   `sudo sysctl debug.xegfx_kms_set="DP-1 2560x1440@240"` (dahili kapanır). Çıkar → `"eDP-1 240"`.
6. Sonra `ECO_LEVEL=metal` / `desktop` ile aynı adımlar (WindowServer çalışırken).

## 10. Riskler

* i915 ekran kodu XNU üzerinde ilk kez tam çalışacak: daha önce görülmemiş eksik LinuxKPI parçaları (ACPI _DSM
  yok → OpRegion/ACPI backlight yerine native; i2c doğrudan `master_xfer`), kesme teslimi (AUX/HPD/vblank).
  AUX kesmesi gelmezse i915 zaman aşımından sonra durumu yine okur (yavaş ama çalışır); vblank kesmesi gelmezse
  commit'ler 50–100 ms bekleyip uyarı basar.
* i915 devralma sırasında tam modeset'e karar verirse kısa bir kararma olur (aynı FB ile geri gelir).
* xeflip PLANE_SURF'e doğrudan yazar; i915 commit'i PLANE_SURF'ü kendi FB'sine geri çeker → mod değişiminde
  bir kare eski görüntü. Commit öncesi xeflip'in "boot yüzeyine dön" durumu (D4).
* DP alt mode PD/EC tarafından girilmiyorsa (bit 0 hiç yok) hedef 2 ancak UCSI/PMC IPC ile mümkün — ayrı iş.
* 240 Hz modu ne EDID'de ne VBT'de değilse i915 listelemez; o zaman elle timing (DisplayID'den) eklemek gerekir.
* DMC yüklenemezse i915 DC durumlarını kapatır (zaten kapalı), ekran çalışır.
* İki ekran aynı anda: kapsam dışı (tek boot FB aperture'ı).

## 11. Lisans

Bu daldaki yeni dosyalar WTL'nin lisansıyla (BSD-2) yazıldı; değiştirilen drm-kmod dosyaları kendi lisanslarında
(MIT) kalır. NullMoth (PolyForm-Noncommercial) yalnız tasarım referansıdır, kodundan satır alınmadı.
