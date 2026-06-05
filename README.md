# BİL 304 - İşletim Sistemleri Dönem Projesi
## OTA (Over-The-Air) Firmware Güncellemesi - Geliştirme İş Parçacığı

**Öğrenci:** 22060338 - Şule Şahan

### 🎥 Proje Sunum Videosu (YouTube)
Projenin Cooja ortamında çalıştırılmasını ve teorik altyapısını açıklayan yüksek çözünürlüklü sunum videoma aşağıdaki bağlantıdan ulaşabilirsiniz:
👉 https://youtu.be/GHRJNH-PQ6Y

---

### 1. Gerçeklenen Yöntemler ve Alınan Önlemler

Projede, iki cihaz (Node 2 Gönderici, Node 1 Alıcı) arasında güvenilir bir veri aktarımı sağlamak amacıyla aşağıdaki yöntemler ve önlemler uygulanmıştır:

#### A. Güvenilir Aktarım (Stop-and-Wait ARQ) ve Zaman Aşımı (Timeout)
Firmware aktarımı sırasında ağda oluşabilecek paket kayıplarını önlemek için **Stop-and-Wait (Dur-ve-Bekle)** protokolü kullanılmıştır. 
Gönderici düğüm bir bloğu gönderdikten sonra alıcıdan `ACK` (Onay) mesajı gelene kadar bekler. Eğer `ACK` mesajı 5 saniye içinde ulaşmazsa (Timeout) veya alıcıdan paket hatalı geldiğine dair `NAK` (Olumsuz Onay) mesajı gelirse, gönderici aynı bloğu tekrar yollar (Maksimum 3 tekrar).

**Alınan Önlem (Kod Parçası):** Protothread yapısında `dest_ipaddr` değişkeninin bekleme esnasında bellekten silinmesini önlemek için `static` anahtar kelimesi kullanılarak adresin kaybolması engellenmiştir.
```c
// udp-client.c içerisindeki zaman aşımı ve tekrar mekanizması
static uip_ipaddr_t dest_ipaddr; // IP adresinin kaybolmaması için static yapıldı
// ...
if(etimer_expired(&timeout_timer)) {
    retries++;
    if(retries <= MAX_RETRIES) {
        LOG_INFO("Timeout block %u, retry %u/%u\n", current_block, retries, MAX_RETRIES);
        send_data_block(current_block, &dest_ipaddr);
    } else {
        LOG_INFO("ERROR: Block %u failed after %u retries\n", current_block, MAX_RETRIES);
        // Hata yönetimi
    }
}
```

#### B. Cooja CFS Kısıtlaması ve Harici Flash Benzetimi (Simulated Flash)
Geliştirme sürecinde Cooja'nın varsayılan CFS (Coffee File System) boyutunun tam olarak 4000 Bayt ile sınırlandırıldığı tespit edilmiştir. 4096 Baytlık firmware dosyamızın 62. bloğunda `Write Error` (disk dolu) hatası alınmıştır.
**Alınan Önlem:** Bu donanımsal engeli aşmak için dosya sistemine yazmak yerine, alıcı düğümün RAM'inde `150 KB` boyutunda statik bir dizi oluşturularak **Harici Flash Simülasyonu** tasarlanmıştır.

```c
// udp-server.c içerisindeki Harici Flash Benzetimi
#define SIM_FLASH_SIZE (150 * 1024)
static uint8_t sim_flash[SIM_FLASH_SIZE];

// Gelen bloğun sanal flash'a yazılması
if(total_bytes_received + pkt->data_len <= SIM_FLASH_SIZE) {
    memcpy(&sim_flash[total_bytes_received], pkt->data, pkt->data_len);
    total_bytes_received += pkt->data_len;
    next_expected_block = pkt->block_num + 1;
}
```

---

### 2. Paket Uzunlukları ve Yapısı
Gönderilecek olan 4096 Baytlık dosya, tek seferde gönderilemeyeceği için **64 Baytlık** veri parçalarına (payload) bölünmüştür.
Her bir UDP paketinin alıcı tarafından doğru yorumlanabilmesi için verinin başına ek başlık (header) bilgileri eklenmiştir. Paket uzunluğu `64 Byte Veri + 9 Byte Header = 73 Byte` olacak şekilde özel bir `struct` olarak tasarlanmıştır.

```c
// Paket (Struct) Yapısı
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;     // 1 Byte: Mesaj türü (DATA, ACK, NAK, COMPLETE)
  uint16_t block_num;    // 2 Byte: Mevcut blok numarası (Örn: 0..63)
  uint16_t total_blocks; // 2 Byte: Toplam blok sayısı
  uint16_t data_len;     // 2 Byte: Gönderilen veri boyutu (Sabit 64)
  uint16_t block_crc16;  // 2 Byte: Sadece bu bloğun CRC16 sağlama toplamı
  uint8_t  data[64];     // 64 Byte: Firmware ham verisi
} ota_packet_t;
```

---

### 3. Kullanılan Hash Algoritmaları (Hata Denetimi)

Ağ üzerinden aktarılan verilerin bütünlüğünü korumak için iki kademeli bir hata denetimi uygulanmıştır:

**1. Blok Bazlı Denetim (CRC16):**
Her 64 baytlık paket gönderilmeden önce basit bir XOR tabanlı `CRC16` hesaplaması yapılarak pakete eklenir. Alıcı bu paketi aldığında kendi CRC16 değerini hesaplar. Değerler uyuşmazsa veri yolda (havada) bozulmuş kabul edilir ve `NAK` yollanarak blok tekrar istenir.

**2. Tüm İmaj Doğrulaması (CRC32):**
Tüm 64 blok alıcıdaki sanal flash'a (diziye) eksiksiz yazıldıktan sonra, gönderici `COMPLETE` mesajı ile birlikte dosyanın orijinal **CRC32** (32-bit Cyclic Redundancy Check) değerini yollar. 
Alıcı cihaz, diske kaydettiği tüm veriyi baştan sona okuyarak kendi CRC32 değerini `compute_flash_crc32()` fonksiyonu ile hesaplar. İki değer uyuştuğunda "Aktarım Başarılı" kabul edilir ve firmware çalıştırılmaya (boot) hazır hale gelir.

```c
// udp-server.c: Aktarım bitince tüm sanal flash'ın CRC32 kontrolü
computed_crc32 = compute_flash_crc32(total_bytes_received);
if(computed_crc32 == received_crc32) {
    LOG_INFO("CRC32 dogrulama BASARILI!\n");
    LOG_INFO("Yuklenmeye hazir yeni firmware alimi tamamlandi.\n");
}
```
