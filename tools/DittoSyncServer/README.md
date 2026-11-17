# Ditto bulut senkronu — API sözleşmesi ve test sunucusu

Ditto, veritabanını tek bir şifreli JSON paketi olarak bir REST adresinde saklar. Her bilgisayar paketi indirir, kendi veritabanıyla birleştirir ve gerekirse geri yükler. Sunucu paketi açamaz; yalnız saklar ve sürümünü (ETag) korur.

Ayarlar: Seçenekler → **Veritabanı** sekmesi → **Bulut senkronu**.

## 1. API

Ditto'ya girilen **API adresi** paketin tam adresidir (ör. `https://sunucu/ditto/alperen`). Aynı adresi kullanan bilgisayarlar aynı paketi paylaşır.

| İstek | Başlıklar | Yanıt |
|---|---|---|
| `GET <adres>` | `Authorization: Bearer <API anahtarı>` | `200` + paket + `ETag` başlığı · `404` = bulutta henüz paket yok |
| `PUT <adres>` | `Authorization`, `Content-Type: application/json`, ilk yüklemede `If-None-Match: *`, sonrakilerde `If-Match: <son GET'teki ETag>` | `200`/`201`/`204` + yeni `ETag` · `412` (veya `409`) = paket bu arada başka bir bilgisayarca değişti |

Sunucunun uyması gerekenler:

- `GET` yanıtında `ETag` zorunludur. Ditto ETag görmezse senkron yapmaz.
- `PUT`'ta `If-Match` son sürümle eşleşmiyorsa paketi yazmadan `412` döner. Ditto bu durumda paketi yeniden indirir, birleştirir ve tekrar dener (en çok 3 kez).
- Yanlış anahtarda `401`, paket çok büyükse `413` döner. Ditto bu kodları kullanıcıya Türkçe açıklar.
- Paket büyüyebilir (metin geçmişinde birkaç MB, resimler açıksa daha fazla). İstek boyutu sınırı buna göre ayarlanır.

## 2. Paket

Buluttaki gövde (zarf):

```json
{
  "format": "ditto-sync", "version": 1,
  "cipher": "AES-256-GCM", "kdf": "PBKDF2-SHA256", "iterations": 210000,
  "salt": "<base64, 16 bayt>", "nonce": "<base64, 12 bayt>", "tag": "<base64, 16 bayt>",
  "data": "<base64, şifreli içerik>"
}
```

Anahtar, **Şifreleme parolası**ndan (UTF-8) PBKDF2-SHA256 ile 32 bayt olarak üretilir. Parola tüm bilgisayarlarda aynı olmalıdır; kaybolursa buluttaki veri açılamaz.

Şifresi çözülmüş içerik:

```json
{
  "format": "ditto-sync-data", "version": 1,
  "updatedAt": 1763380000, "updatedBy": "BILGISAYAR-ADI",
  "clips": [
    {
      "id": "<32 hex>", "modified": 1763380000, "group": false, "parent": "<grubun id'si veya boş>",
      "date": 1763370000, "lastPaste": 1763375000,
      "desc": "açıklama", "quickPaste": "", "neverDelete": false, "sticky": false,
      "contentHash": "<sha256>",
      "formats": [
        { "name": "CF_UNICODETEXT", "text": "kopyalanan metin" },
        { "name": "Rich Text Format", "base64": "..." }
      ]
    }
  ],
  "deleted": [ { "id": "<32 hex>", "at": 1763380000 } ]
}
```

Zamanlar Unix saniyesidir (UTC).

## 3. Birleştirme kuralları

- Her klibin kalıcı bir `id`'si vardır. Yerel eşleşme (Ditto klip numarası ↔ `id`) ve son senkron durumu veritabanındaki `DittoSync` tablosunda tutulur.
- Bir klipte en son değişiklik kazanır (`modified`). Değişiklik sayılanlar: metin/içerik, açıklama, Quick Paste metni, grup, "otomatik silme", sabitleme. Sıra ve kısayollar senkronlanmaz.
- Bilgisayarların saati doğru olmalıdır; kural zaman damgasına dayanır.
- Yeni bir klibin metni buluttaki bir klibinkiyle aynıysa ikisi tek klip sayılır (ilk senkronda çift kayıt oluşmaz).
- **Silmeler:** "Burada silinen klipleri diğer bilgisayarlardan da sil" kapalıyken silinen klip o bilgisayara geri inmez, bulutta ve diğer bilgisayarlarda kalır. Açıkken silme pakete `deleted` kaydı olarak girer ve daha eski sürümleri her yerden siler. Bu kayıtlar 180 gün tutulur. Not: Açıkken Ditto'nun "en fazla N klip" / "X gün sonra sil" temizliği de diğer bilgisayarlara yansır.
- Gönderilen biçimler: varsayılan yalnız metin (`CF_UNICODETEXT`). "Resim, RTF ve HTML biçimlerini de gönder" açıkken `CF_TEXT`, `Rich Text Format`, `HTML Format`, `CF_DIB`, `PNG` de gider. "En büyük klip (KB)" sınırını aşan biçim gönderilmez.

## 4. Test sunucusu

Bağımlılığı yoktur (Node.js 18+).

```bash
set DITTO_SYNC_KEYS=test-anahtari
node server.js --port 8787 --data ./data
```

Ditto'da API adresi: `http://127.0.0.1:8787/ditto/test`, API anahtarı: `test-anahtari`.

Sunucu her adres için paketi `data` klasöründe bir dosyada tutar; `If-Match`/`If-None-Match` denetimini ve ETag'i yukarıdaki sözleşmeye göre yapar. Yalnız deneme içindir: HTTPS, kullanıcı yönetimi ve yedekleme yoktur.
