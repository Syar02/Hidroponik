# Hidroponik MQTT: Monitoring ESP32

Dashboard web untuk memantau kebun hidroponik secara real-time dan mengendalikan pompa koreksi pH.
Dashboard ini statis (cukup dihosting di GitHub Pages), lalu terhubung langsung ke broker MQTT publik lewat WebSocket.

```
 ESP32 (pH, SHT31, BH1750, relay)            Browser / HP
        │  MQTT tcp :1883                         │  MQTT wss :8084
        └──────────►  broker.emqx.io  ◄───────────┘
                         topik: aranus/<KODE>/hidro/...
```

Akses dashboard: `https://<username>.github.io/<nama-repo>/#<KODE>`
Contoh: `https://rafly-88.github.io/hidroponik/#hd7p2kq9vm`

## Isi folder

| File | Fungsi |
|---|---|
| `index.html` | Dashboard (Beranda, Statistik, Kontrol Pompa, Pengaturan). Satu file, tanpa build. |
| `firmware/hidroponik_esp32/hidroponik_esp32.ino` | Kode ESP32: kode lama ditambah WiFi, MQTT, riwayat 24 jam, dan kontrol dari web. |
| `simulator/sim_esp32.py` | Simulator ESP32 untuk mencoba dashboard tanpa hardware. |

## 1. Upload firmware ESP32

1. Di Arduino IDE, pasang library berikut lewat Library Manager:
   - **PubSubClient** (Nick O'Leary)
   - **Adafruit SHT31 Library**
   - **BH1750** (Christopher Laws)
   - **LiquidCrystal I2C**
2. Buka `hidroponik_esp32.ino`, lalu ubah bagian **KONFIGURASI**:
   ```cpp
   const char* WIFI_SSID   = "NAMA_WIFI";
   const char* WIFI_PASS   = "PASSWORD_WIFI";
   const char* DEVICE_CODE = "hd7p2kq9vm";   // ganti dengan kode acak milik Anda
   ```
   `DEVICE_CODE` berfungsi sebagai "kunci" dashboard. Pakai huruf kecil dan angka sebanyak 6–32 karakter, dan buat yang sulit ditebak.
3. Pilih board **ESP32 Dev Module**, lalu upload. Serial Monitor (115200) akan menampilkan pembacaan sensor dan status koneksi.

Wiring sama dengan sebelumnya: pH ke GPIO32, relay ke GPIO25 (aktif LOW), I2C di SDA 21 / SCL 22, LCD di alamat 0x27, SHT31 di alamat 0x44.

### Perubahan dibanding kode lama
- **Histeresis pH (default 0,10).** Relay menyala saat pH < min atau > max, lalu baru mati setelah pH masuk ke rentang `min+hyst … max-hyst`. Dengan begitu relay tidak bolak-balik nyala-mati di sekitar batas.
- **Pembacaan ADC pH diambil rata-rata 20 sampel** supaya nilainya lebih stabil.
- **Ambang pH, offset kalibrasi, dan histeresis** bisa diubah dari dashboard, dan disimpan di flash ESP32.
- **Mode manual** dari dashboard otomatis kembali ke mode otomatis setelah 30 menit. Ini pengaman agar pompa tidak tertinggal menyala.
- **Kontrol relay tetap berjalan walaupun WiFi atau MQTT terputus.**
- LCD sekarang punya halaman ke-3 yang menampilkan status pompa, mode, WiFi, dan MQTT.

## 2. Publikasikan dashboard ke GitHub Pages

```bash
cd hidroponik-mqtt
git init && git add index.html README.md firmware simulator
git commit -m "Dashboard hidroponik MQTT"
git branch -M main
git remote add origin https://github.com/<username>/hidroponik.git
git push -u origin main
```
Setelah itu, di GitHub buka **Settings → Pages → Source: Deploy from a branch → main / (root)**.
Setelah 1–2 menit, buka `https://<username>.github.io/hidroponik/#<KODE>`.

Kode perangkat tersimpan di browser, jadi kunjungan berikutnya cukup lewat link tanpa `#`. Link lengkap bisa disalin dari menu **Pengaturan → Link dashboard**.

## 3. Coba tanpa hardware (simulator)

```bash
pip install paho-mqtt
python simulator/sim_esp32.py hdcobaan123
```
Lalu buka `index.html` (bisa langsung dari file lokal) dengan `#hdcobaan123`. Simulator merespons perintah pompa, perubahan mode, dan perubahan ambang, persis seperti firmware.

## Topik MQTT

`BASE = aranus/<KODE>/hidro`

| Topik | Arah | Isi |
|---|---|---|
| `BASE/telemetry` | ESP32 → web (retained, tiap 2 dtk) | `{"ph":6.82,"v":2.29,"t":27.1,"h":70.2,"lux":1234.0,"relay":0,"mode":"auto","on_s":120,"rssi":-61,"up":3600,"ts":1762400000}` |
| `BASE/status` | ESP32 → web (retained, LWT) | `online` / `offline` |
| `BASE/config` | ESP32 → web (retained) | `{"ph_min":5.5,"ph_max":8.5,"ph_off":0.00,"hyst":0.10}` |
| `BASE/history` | ESP32 → web (retained, tiap 1 mnt) | `{"end":<epoch jam ini>,"ph":[24 nilai],"t":[...],"h":[...],"lux":[...]}`, berisi rata-rata per jam dari yang terlama sampai terbaru (`null` jika tidak ada data) |
| `BASE/set` | web → ESP32 | `relay=on` · `relay=off` · `relay=toggle` · `mode=auto` · `mode=manual` · `ph_min=5.5` · `ph_max=8.5` · `ph_off=0.1` · `hyst=0.1` · `ping` |

Untuk debugging, Anda bisa memantau semua topik dengan MQTTX atau `mosquitto_sub -h broker.emqx.io -t 'aranus/<KODE>/hidro/#' -v`.

## Catatan penting

- **Keamanan.** `broker.emqx.io` adalah broker publik tanpa password. Siapa pun yang mengetahui kode perangkat bisa melihat data **dan menyalakan pompa**. Kode acak yang panjang cukup untuk prototipe atau tugas. Untuk pemakaian sungguhan, gunakan broker pribadi dengan username/password dan TLS (misalnya HiveMQ Cloud atau EMQX Cloud versi gratis). Setelah itu, ubah `MQTT_HOST`/`MQTT_PORT` di firmware dan `BROKER_URL` di `index.html`.
- **Riwayat 24 jam disimpan di RAM ESP32**, jadi hilang setiap kali ESP32 restart. Riwayat juga baru terisi setelah jam tersinkron lewat NTP (butuh internet). Untuk riwayat jangka panjang (mingguan atau bulanan), perlu server penyimpan data, misalnya Node-RED + InfluxDB atau Google Sheets.
- Grafik **Live (30 mnt)** hanya mengumpulkan data selama halaman dashboard terbuka.
- Batas status suhu, kelembapan, dan cahaya di kartu dashboard bisa diubah di bagian `LIMITS` pada `index.html`.
- Kalibrasi pH: celupkan probe ke larutan buffer pH 7. Jika dashboard menunjukkan, misalnya, 7,30, isi **Kalibrasi offset** dengan `-0.30`.
