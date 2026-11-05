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

## Test 2 — Eco (asıl test, 1 açılış, ~10 dk) — seviye `metal` (varsayılan)
1. Windows → G-Helper **Eco** → harici monitörü **SOL** USB-C'ye tak (açık kalsın) → yeniden başlat → macOS.
2. Giriş ekranı gelince **~3 dk bekle**, sonra giriş yap. Bu seviyede masaüstü **yeniden başlamaz**: görüntü firmware
   çerçeve tamponunda (XeGfxIntelFB) kalır; arka planda XeGfx aşama 1→7'yi koşar ve Metal'i yalnız bir test
   sürecinde dener (WindowServer'a dokunmaz). Harici monitörün siyah kalması bu aşamada **normal**.
3. 5 dk normal kullan: pencere sürükle, Safari'de bir sayfa, **parlaklık tuşları ve kaydırıcı**.
4. `gu605-collect.sh` çalıştır. Beklenen özet (`EFI/GU605/last-boot.txt`): `stages to 7 passed`, `metal done`.
- Ekran donarsa ya da beyaz **panik yazısı** çıkarsa (debug=0x100 kendiliğinden yeniden başlatmaz): güç tuşuna
  basılı tut → aç. Sonraki açılışta otomatik koşu kendini kapatır (`autostart-off`), masaüstü yine gelir → giriş yap →
  `gu605-collect.sh`.
- Test boyunca **uyku yok**: kapağı kapatma, Uyku menüsünü kullanma (i915'in uyku/uyanma desteği yok; uyanışta
  ekran gelmeyebilir).
- Masaüstü adımı (`ECO_LEVEL=desktop`) otomatik girişle de çalışır: giriş yapılmışsa ve 60 sn dokunulmadıysa
  20 sn önce bildirim gelir, sonra ekran bir kez kararır ve oturum kapanır (tekrar giriş yap). Bildirim sırasında
  klavyeye/izleme dörtgenine dokunursan iptal olur; masaüstü bir sonraki **Çıkış**'ta (Apple menüsü) Intel'e geçer.
- Masaüstü adımı (WindowServer Intel GPU'da) sonraki test: `gu605.conf`'ta `ECO_LEVEL=desktop`.

## Test 3 — Standard (isteğe bağlı, 1 açılış, 2 dk)
1. G-Helper **Standard** → macOS → giriş yap (masaüstü dahili panelde, hızlandırmasız olabilir).
2. `gu605-collect.sh`.

## Ne bakılacak (kısaca not al)
- Panel görüntü verdi mi, kaç Hz görünüyor (Sistem Ayarları › Ekranlar), parlaklık değişti mi.
- Pencereler akıcı mı, renk/bulanıklık hatası var mı (bilinen: kenar çubuğu bulanıklığı saydam olabilir;
  Erişilebilirlik › Ekran › Saydamlığı azalt düzeltir).
- Harici monitör sol portta: "sinyal yok" mu, OpenCore seçicisi/Apple logosu sırasında görüntü verdi mi?

## Acil
G-Helper → Ultimate her zaman bugünkü kuruluma döner. OpenCore seçicide boot-arg: `-xegfxnoauto` (koşucu kapalı),
`-xegfxoff` (XeGfx boşta), `-xeintelfboff` (Intel framebuffer kapalı).
