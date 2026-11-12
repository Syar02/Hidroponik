# Hidroponik MQTT: Monitoring ESP32

Dashboard web untuk memantau kebun hidroponik secara real-time dan mengendalikan pompa koreksi pH.
Dashboard ini statis (cukup dihosting di GitHub Pages), lalu terhubung langsung ke broker MQTT publik lewat WebSocket.

```
 ESP32 (pH, SHT31, BH1750, relay)            Browser / HP
        │  MQTT tcp :1883                         │  MQTT wss :8084
        └──────────►  broker.emqx.io  ◄───────────┘
                         topik: aranus/<KODE>/hidro/...
```

Akses dashboard: `https://syar02.github.io/Hidroponik/#<KODE>`

## Pilih mode koneksi

**Disarankan: [Mode Otomatis](firmware/mode_otomatis/README.md)** (`firmware/mode_otomatis/`). Satu firmware memakai WiFi dan modem 4G sekaligus: bila jalur utama putus, ESP32 otomatis pindah ke jalur cadangan lalu kembali lagi. Modem boleh dicabut tanpa mengganggu sensor dan pompa. Dashboard menampilkan `Otomatis · WiFi` / `Otomatis · 4G`, serta status kedua jalur di menu Pengaturan.

Firmware satu-jalur di bawah ini tetap tersedia bila hanya memakai salah satu jalur. Sensor, kontrol pompa, LCD, topik MQTT, kode perangkat, dan link dashboard **sama** di semua mode.

| | **Mode WiFi** | **Mode Seluler 4G** |
|---|---|---|
| Dipakai bila | Di lokasi ada WiFi 2,4 GHz | Di lokasi tidak ada WiFi, tapi ada sinyal seluler |
| Perangkat tambahan | — | Modem SIMCom A7670E + antena + kartu SIM |
| Biaya rutin | — | Paket data (±250 MB/bulan) |
| Kirim data | Tiap 2 detik | Tiap 5 detik |
| Firmware | [`firmware/mode_wifi/`](firmware/mode_wifi/README.md) | [`firmware/mode_lte_a7670e/`](firmware/mode_lte_a7670e/README.md) |
| Skema | `docs/mode_wifi/Skema_Mode_WiFi.jpg` | `docs/mode_lte_a7670e/Skema_Mode_LTE_A7670E.jpg` |
| Tanda di dashboard | Label **"Mode WiFi"** | Label **"Mode Seluler 4G"** |
| Tanda di LCD saat menyala | `Mode: WiFi` | `Mode: 4G LTE` |

**Cara pindah mode:** sambung atau lepas modem sesuai skema, lalu upload firmware mode yang dituju. Link dashboard tidak berubah. Dashboard otomatis menampilkan mode yang sedang aktif karena setiap firmware mengirim penanda `"net":"wifi"` atau `"net":"lte"`.

## Isi folder

| File | Fungsi |
|---|---|
| `index.html` | Dashboard (Beranda, Statistik, Kontrol Pompa, Pengaturan). Satu file, tanpa build. |
| `firmware/mode_otomatis/` | Firmware **mode otomatis** (WiFi + 4G, pindah jalur otomatis) dan panduannya. Daftar WiFi disimpan di `wifi_config.h`, yang tidak ikut ter-upload ke GitHub. |
| `firmware/mode_wifi/` | Firmware **mode WiFi** dan panduannya. Password WiFi disimpan di `wifi_config.h`, yang tidak ikut ter-upload ke GitHub. |
| `firmware/mode_lte_a7670e/` | Firmware **mode seluler 4G** (ESP32 + modem SIMCom A7670E) dan panduannya. |
| `docs/mode_otomatis/`, `docs/mode_wifi/`, `docs/mode_lte_a7670e/` | Skema wiring masing-masing mode. |
| `docs/Dokumen_Teknis_Hidroponik_IoT_v2.*` | Dokumen teknis versi 2.0 (Mode Otomatis WiFi + 4G) dalam .pdf, .docx, .doc. Versi 1.0 tetap disimpan. |
| `docs/` | Dokumen teknis dan ilustrasi prototipe. |
| `simulator/sim_esp32.py` | Simulator ESP32 untuk mencoba dashboard tanpa hardware (bisa meniru kedua mode). |

## 1. Upload firmware ESP32

Ikuti panduan mode yang dipilih:
- **Mode otomatis (disarankan):** [`firmware/mode_otomatis/README.md`](firmware/mode_otomatis/README.md)
- **Mode WiFi:** [`firmware/mode_wifi/README.md`](firmware/mode_wifi/README.md)
- **Mode seluler 4G:** [`firmware/mode_lte_a7670e/README.md`](firmware/mode_lte_a7670e/README.md)

Wiring sensor sama di kedua mode:
- pH ke GPIO32
- Relay ke GPIO25 (aktif LOW)
- I2C di SDA 21 / SCL 22: LCD di alamat 0x27, SHT31 di alamat 0x44

`DEVICE_CODE` berfungsi sebagai "kunci" dashboard. Pakai huruf kecil dan angka sebanyak 6–32 karakter, buat yang sulit ditebak, dan gunakan kode yang sama di kedua mode supaya link dashboard tidak berubah.

### Fitur yang sama di kedua mode
- **Histeresis pH (default 0,10).** Relay menyala saat pH < min atau > max, lalu baru mati setelah pH masuk ke rentang `min+hyst … max-hyst`.
- **Pembacaan ADC pH diambil rata-rata 20 sampel** supaya nilainya lebih stabil.
- **Ambang pH, offset kalibrasi, dan histeresis** bisa diubah dari dashboard, dan disimpan di flash ESP32.
- **Mode manual** dari dashboard otomatis kembali ke mode otomatis setelah 30 menit.
- **Kontrol relay tetap berjalan walaupun koneksi internet terputus.**
- **LCD halaman ke-3** menampilkan status pompa, mode, koneksi, dan MQTT.

## 2. Publikasikan dashboard ke GitHub Pages

```bash
cd hidroponik-mqtt
git init && git add .gitignore index.html README.md firmware simulator
git commit -m "Dashboard hidroponik MQTT"
git branch -M main
git remote add origin https://github.com/<username>/hidroponik.git
git push -u origin main
```
Setelah itu, di GitHub buka **Settings → Pages → Source: Deploy from a branch → main / (root)**.
Setelah 1–2 menit, buka `https://<username>.github.io/hidroponik/#<KODE>`.

Kode perangkat tersimpan di browser, jadi kunjungan berikutnya cukup lewat link tanpa `#`. Link lengkap bisa disalin dari menu **Pengaturan → Link dashboard**.

### Saat meng-update dashboard
GitHub Pages menyimpan cache halaman di browser selama ±10 menit. Agar pengguna langsung mendapat versi baru, setiap kali mengubah `index.html` **naikkan nilai** di baris ini:
```html
<meta name="app-version" content="2026-11-10.2">
```
Dashboard mengecek versi setiap kali dibuka atau tab-nya kembali aktif. Jika versi di server berbeda, dashboard memuat versi terbaru sendiri. Versi yang sedang dipakai tertulis di bagian bawah menu **Pengaturan**.

## 3. Coba tanpa hardware (simulator)

```bash
pip install paho-mqtt
python simulator/sim_esp32.py hdcobaan123          # meniru mode WiFi
python simulator/sim_esp32.py hdcobaan123 --lte    # meniru mode seluler 4G
```
Lalu buka `index.html` (bisa langsung dari file lokal) dengan `#hdcobaan123`. Simulator merespons perintah pompa, perubahan mode, dan perubahan ambang, persis seperti firmware.

## Topik MQTT

`BASE = aranus/<KODE>/hidro`

| Topik | Arah | Isi |
|---|---|---|
| `BASE/telemetry` | ESP32 → web (retained, WiFi tiap 2 dtk, 4G tiap 5 dtk) | `{"ph":6.82,"v":2.29,"t":27.1,"h":70.2,"lux":1234.0,"relay":0,"mode":"auto","on_s":120,"rssi":-61,"net":"wifi","up":3600,"ts":1762400000}`. Mode 4G mengirim `"net":"lte"` dan tambahan `"csq"`. |
| `BASE/status` | ESP32 → web (retained, LWT) | `online` / `offline` |
| `BASE/config` | ESP32 → web (retained) | `{"ph_min":5.5,"ph_max":8.5,"ph_off":0.00,"hyst":0.10}` |
| `BASE/history` | ESP32 → web (retained, tiap 1 mnt) | `{"end":<epoch jam ini>,"ph":[24 nilai],"t":[...],"h":[...],"lux":[...]}`, berisi rata-rata per jam dari yang terlama sampai terbaru (`null` jika tidak ada data) |
| `BASE/set` | web → ESP32 | `relay=on` · `relay=off` · `relay=toggle` · `mode=auto` · `mode=manual` · `ph_min=5.5` · `ph_max=8.5` · `ph_off=0.1` · `hyst=0.1` · `ping` · mode 4G & otomatis: `kuota` · `ussd=*808#,7` · `sms` |
| `BASE/net` | ESP32 → web (retained, khusus mode otomatis) | Status kedua jalur: `{"active":"wifi","prio":"wifi","wifi":{"st":"ok","ssid":"...","rssi":-56},"lte":{"st":"nomodem"}}` |
| `BASE/kuota` | ESP32 → web (retained, mode 4G & otomatis) | Hasil cek sisa paket. USSD: `{"kind":"ussd","q":"*808#,7,4,2,2","st":1,"text":"..."}` (`st` 0/2 = selesai, 1 = menu, `"wait"` = sedang menunggu). SMS: `{"kind":"sms","from":"XLSMART","date":"...","text":"..."}`. Bila gagal ada field `err` |
| `BASE/kuota_log` | STM32 → web (retained, khusus firmware [hidroponik-stm32](../hidroponik-stm32/README.md#log-sisa-kuota)) | Log sisa kuota (maks. 12 catatan, terlama → terbaru): `{"auto_h":12,"next":<epoch>,"items":[{"ts":<epoch>,"kb":1039360,"exp":20261209}]}`. Tampil di **Pengaturan › Log sisa kuota**. Perintah: `kuota_auto=<jam>` (0 = mati), `kuota_log=hapus` |

Untuk debugging, Anda bisa memantau semua topik dengan MQTTX atau `mosquitto_sub -h broker.emqx.io -t 'aranus/<KODE>/hidro/#' -v`.

## Catatan penting

- **Keamanan.** `broker.emqx.io` adalah broker publik tanpa password. Siapa pun yang mengetahui kode perangkat bisa melihat data **dan menyalakan pompa**. Kode acak yang panjang cukup untuk prototipe atau tugas. Untuk pemakaian sungguhan, gunakan broker pribadi dengan username/password dan TLS (misalnya HiveMQ Cloud atau EMQX Cloud versi gratis). Setelah itu, ubah `MQTT_HOST`/`MQTT_PORT` di firmware dan `BROKER_URL` di `index.html`.
- **Riwayat 24 jam disimpan di RAM ESP32**, jadi hilang setiap kali ESP32 restart. Riwayat juga baru terisi setelah jam tersinkron lewat NTP (butuh internet). Untuk riwayat jangka panjang (mingguan atau bulanan), perlu server penyimpan data, misalnya Node-RED + InfluxDB atau Google Sheets.
- Grafik **Live (30 mnt)** hanya mengumpulkan data selama halaman dashboard terbuka.
- Batas status suhu, kelembapan, dan cahaya di kartu dashboard bisa diubah di bagian `LIMITS` pada `index.html`.
- Kalibrasi pH: celupkan probe ke larutan buffer pH 7. Jika dashboard menunjukkan, misalnya, 7,30, isi **Kalibrasi offset** dengan `-0.30`.
