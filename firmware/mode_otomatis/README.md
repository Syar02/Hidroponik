# Mode Otomatis (WiFi + Seluler 4G)

Satu firmware untuk dua jalur internet. ESP32 memakai **jalur utama** selama tersedia. Kalau jalur utama putus, ESP32 otomatis pindah ke **jalur cadangan**, lalu kembali ke jalur utama setelah jalur itu stabil lagi.

| Kejadian | Yang dilakukan ESP32 |
|---|---|
| WiFi mati / di luar jangkauan / tanpa internet | Pindah ke 4G (±10–15 detik) |
| WiFi kembali dan stabil 30 detik | Kembali ke WiFi |
| Sinyal 4G hilang / kuota habis (4G sebagai utama) | Pindah ke WiFi terdaftar yang ada di sekitar |
| Modem dicabut atau mati | Tetap jalan lewat WiFi. Modem dicek ulang di latar belakang (15 dtk, 30 dtk, ... maks 5 menit) |
| Keduanya putus | Sensor, pompa, dan LCD tetap jalan. Data terkirim lagi begitu salah satu jalur kembali |

Sensor, kontrol pompa, dan LCD berjalan di inti 1 ESP32, terpisah dari urusan jaringan di inti 0. Jadi modem yang dicabut atau WiFi yang putus **tidak memperlambat pembacaan sensor maupun kontrol pompa**. Pada uji dengan modem dicabut, sensor tetap terbaca tepat setiap 1 detik.

## Isi folder

| File | Fungsi |
|---|---|
| `mode_otomatis.ino` | Firmware gabungan |
| `wifi_config.example.h` | Contoh daftar WiFi (aman di-upload ke GitHub) |
| `wifi_config.h` | Daftar WiFi Anda (berisi password, **tidak** ikut ter-upload ke GitHub) |

## Langkah

1. Salin `wifi_config.example.h` menjadi `wifi_config.h`, lalu isi WiFi yang boleh dipakai. Boleh lebih dari satu; ESP32 memilih WiFi terdaftar dengan sinyal paling kuat:
   ```cpp
   const WifiCred WIFI_LIST[] = {
     {"ARANUS TEAM", "password_router"},
     {"HOTSPOT_HP",  "password_hotspot"},
   };
   ```
2. Buka `mode_otomatis.ino` dan cek bagian KONFIGURASI:
   - `WIFI_UTAMA = true`: WiFi utama, 4G cadangan (hemat kuota, disarankan). Isi `false` bila 4G yang utama.
   - `APN` sesuai kartu SIM (Telkomsel/XL `internet`, Indosat `indosatgprs`, Tri `3gprs`).
   - `DEVICE_CODE` sama dengan kode di link dashboard.
3. Di Arduino IDE, pasang library **PubSubClient**, **Adafruit SHT31 Library**, **BH1750**, dan **LiquidCrystal I2C**.
4. Pilih board **ESP32 Dev Module**, lalu upload.

Wiring modem sama dengan mode 4G: [`docs/mode_lte_a7670e/Skema_Mode_LTE_A7670E.jpg`](../../docs/mode_lte_a7670e/Skema_Mode_LTE_A7670E.jpg). Modem boleh tidak dipasang; firmware tetap jalan dengan WiFi saja.

## Cara melihat jalur yang sedang dipakai

- **Dashboard › Beranda:** label `Otomatis · WiFi` atau `Otomatis · 4G`.
- **Dashboard › Pengaturan:** baris **WiFi** dan **Seluler 4G** menampilkan status masing-masing, misalnya `ARANUS TEAM · −56 dBm · aktif` dan `XL · CSQ 24 · siaga`, atau `Modem tidak terpasang / mati`.
- **LCD** bergantian menampilkan `Online via WiFi` / `Online via 4G`, dan satu halaman status:
  ```
  WiFi:OK -56dBm
  4G  :tanpa modem
  ```
- **Serial Monitor (115200):** setiap perpindahan tercatat, misalnya `[NET] pindah jalur: WiFi -> 4G`.

## Cara kerja singkat

- **Kedua jalur siaga bersamaan.** WiFi tetap tersambung ke router, dan modem tetap terdaftar di jaringan dengan data aktif. Hanya satu jalur yang tersambung ke MQTT, jadi perpindahan cepat.
- **Internet dicek, bukan hanya sinyal.** Jalur siaga diuji berkala dengan membuka koneksi singkat ke broker: WiFi tiap 1 menit, 4G tiap 10 menit (±4 MB/bulan). WiFi yang tersambung tapi tanpa internet, atau 4G dengan kuota habis, tidak akan dipilih.
- **Tidak bolak-balik.** Putus sesaat (< 8 detik) belum dianggap putus, dan jalur utama harus stabil 30 detik sebelum dipakai lagi.
- **Cek kuota saat WiFi aktif** berjalan lewat modem tanpa mengganggu kiriman data, karena MQTT tetap lewat WiFi.

## Topik tambahan

`aranus/<KODE>/hidro/net` (retained) berisi status kedua jalur:
```json
{"active":"wifi","prio":"wifi",
 "wifi":{"st":"ok","ssid":"ARANUS TEAM","rssi":-56,"known":1},
 "lte":{"st":"nomodem","op":""},"ts":1794300155}
```
Nilai `st` WiFi: `ok`, `noinet`, `cari`, `none` (tidak ada WiFi terdaftar di sekitar), `fail` (password salah?), `off`.
Nilai `st` 4G: `ok`, `noinet`, `cari` (mencari sinyal), `nodata`, `nosim`, `nomodem`, `cek`.

## Status pengujian (10-11-2026)

- **Teruji di perangkat:**
  - Boot dengan modem dicabut: WiFi aktif dalam ±16 detik, modem dilaporkan "tidak terpasang", sensor tetap 1 detik sekali.
  - WiFi diputus paksa: tersambung lagi dan online dalam ±5 detik.
  - Perintah dari dashboard (pompa, ambang pH) masuk lewat WiFi.
- **Belum teruji karena modem sedang dicabut:** perpindahan WiFi ↔ 4G. Bagian modem memakai kode yang sama dengan [mode 4G](../mode_lte_a7670e/README.md), yang sudah teruji. Uji perpindahan disarankan setelah modem dipasang: matikan router, lalu pastikan dashboard berganti ke `Otomatis · 4G`.
