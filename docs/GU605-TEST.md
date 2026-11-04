# GU605 test kartı (tek sayfa)

Ön koşul: EFI paketi yazıldı (`proposed/`), macOS'ta bir kez `sudo bash scripts/gu605-install.sh` çalıştı ve Mac
yeniden başlatıldı. Her testten sonra **tek komut**:

```
sudo bash /Library/XeGfx/gu605/gu605-collect.sh
```

Rapor: Masaüstünde `gu605-report-<tarih>.txt` (+ `.tar.gz`) ve EFI bölümünde `GU605/report.txt` (Windows'tan okunur).
Her açılışın özeti ayrıca `EFI/GU605/last-boot.txt`'e kendiliğinden yazılır.

## Test 1 — Ultimate (1 açılış, 2 dk): hiçbir şey bozulmadı mı?
1. Windows → G-Helper **Ultimate** → yeniden başlat → KIOXIA'dan macOS.
2. Giriş yap. Bak: dahili panel 240 Hz, parlaklık tuşları/kaydırıcı çalışıyor (NullMoth), harici monitör sağ portta.
3. `gu605-collect.sh` çalıştır. Beklenen: `mode: ultimate … nothing to do`.

## Test 2 — Eco (asıl test, 1 açılış, ~10 dk)
1. Windows → G-Helper **Eco** → harici monitörü **SOL** USB-C'ye tak (açık kalsın) → yeniden başlat → macOS.
2. Giriş ekranı gelince **giriş yapma, ~3-4 dk bekle.** Ekran bir kez kararıp giriş ekranı geri gelirse: masaüstü artık
   Intel GPU'da (beklenen). Harici monitörün siyah kalması bu aşamada **normal** (Aşama 3).
3. Giriş yap, 5 dk normal kullan: pencere sürükle, Safari'de bir sayfa, **parlaklık tuşları ve kaydırıcı**.
4. `gu605-collect.sh` çalıştır.
- Ekran 2 dk'dan uzun siyah/donuk kalırsa: güç tuşuna basılı tut → tekrar aç (otomatik koşu kendini kapatır, masaüstü
  yavaş ama gelir) → giriş yap → `gu605-collect.sh`.

## Test 3 — Standard (isteğe bağlı, 1 açılış, 2 dk)
1. G-Helper **Standard** → macOS → giriş yap (masaüstü dahili panelde, hızlandırmasız olabilir).
2. `gu605-collect.sh`.

## Test 4 — Eco + i915 ekran yolu (yalnız `gu605-display` paketi, Test 2'den SONRA)
1. `sudo nano /Library/XeGfx/gu605/gu605.conf` → `DISPLAY=kms`, `ECO_LEVEL=gt` (önce masaüstü olmadan), kaydet.
2. Eco'da yeniden başlat, giriş ekranında ~3 dk bekle. Ekran **aynı kalmalı**; kısa bir kararma olabilir.
   Panel 240 Hz'e kendiliğinden geçer (`PANEL_HZ=240`).
3. Giriş yap → `sudo bash /Library/XeGfx/gu605/gu605-display.sh dump` (eDP modları, ölçülen Hz).
4. Parlaklık: `sudo bash /Library/XeGfx/gu605/gu605-display.sh bl 300`, sonra klavye tuşları.
5. Harici: monitörü **sol** USB-C'ye tak → ~2 sn içinde görüntü monitöre geçmeli (panel kapanır). Olmazsa:
   `sudo bash /Library/XeGfx/gu605/gu605-display.sh external`. Çıkar → panel geri gelmeli
   (olmazsa `... gu605-display.sh internal`).
6. `gu605-collect.sh`. Siyah ekran kalırsa güç tuşuyla kapat/aç: sonraki açılış kendiliğinden eski yola döner
   (`kms-failed`), ya da seçicide `-xegfxnokms`.
7. Hepsi iyiyse `ECO_LEVEL=metal`, sonra `desktop` ile tekrarla.

## Ne bakılacak (kısaca not al)
- Panel görüntü verdi mi, kaç Hz görünüyor (Sistem Ayarları › Ekranlar), parlaklık değişti mi.
- Pencereler akıcı mı, renk/bulanıklık hatası var mı (bilinen: kenar çubuğu bulanıklığı saydam olabilir;
  Erişilebilirlik › Ekran › Saydamlığı azalt düzeltir).
- Harici monitör sol portta: "sinyal yok" mu, OpenCore seçicisi/Apple logosu sırasında görüntü verdi mi?

## Acil
G-Helper → Ultimate her zaman bugünkü kuruluma döner. OpenCore seçicide boot-arg: `-xegfxnoauto` (koşucu kapalı),
`-xegfxoff` (XeGfx boşta), `-xeintelfboff` (Intel framebuffer kapalı), `-xegfxnokms` (i915 ekran yolu kapalı).
