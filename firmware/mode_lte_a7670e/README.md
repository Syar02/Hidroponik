# Mode Seluler 4G (SIMCom A7670E)

Pakai mode ini bila di lokasi kebun **tidak ada WiFi** tetapi ada sinyal seluler. ESP32 memberi perintah ke modem A7670E lewat kabel UART, lalu modem mengirim data ke internet memakai kartu SIM. Router WiFi tidak diperlukan.

Skema: [`docs/mode_lte_a7670e/Skema_Mode_LTE_A7670E.jpg`](../../docs/mode_lte_a7670e/Skema_Mode_LTE_A7670E.jpg)

## Wiring tambahan (selain sensor)

| ESP32 | A7670E | Keterangan |
|---|---|---|
| VIN 5V | VIN 5V | Dari adaptor 5V minimal 3A, **bukan** dari pin 5V/3V3 ESP32 |
| GND | GND | Wajib disatukan |
| GPIO17 (TX2) | RXD | Data ESP32 → modem |
| GPIO16 (RX2) | TXD | Data modem → ESP32 |
| GPIO4 | PWRKEY | Opsional, agar ESP32 bisa menyalakan modem |

- **Kapasitor:** pasang 1000 µF di antara VIN dan GND, dekat modem.
- **Antena LTE:** pasang sebelum modem dinyalakan.
- **Jenis board:** skema ini untuk board breakout A7670E (input 5V, UART 3,3V). Modul inti tanpa board butuh VBAT 3,4–4,2 V dan level shifter UART 1,8 V.

## Langkah

1. Siapkan kartu SIM:
   - Paket data harus aktif.
   - PIN SIM harus dimatikan (pasang dulu di HP untuk mematikannya).
2. Di Arduino IDE, pasang library **Adafruit SHT31 Library**, **BH1750**, dan **LiquidCrystal I2C**. Library MQTT tidak diperlukan, karena MQTT dijalankan oleh fitur bawaan modem.
3. Buka `mode_lte_a7670e.ino`, lalu sesuaikan `APN`:
   - Telkomsel/XL: `internet`
   - Indosat: `indosatgprs`
   - Tri: `3gprs`

   Sesuaikan juga `DEVICE_CODE` kalau perlu.
4. Pilih board **ESP32 Dev Module**, lalu upload.
5. Buka Serial Monitor (115200). Urutan pesan yang normal:
   ```
   === MODE KONEKSI: Seluler 4G (SIMCom A7670E, APN internet) ===
   [LTE] modem : A7670E-...
   [LTE] IMEI  : ...
   [LTE] data aktif, IP 10.x.x.x, sinyal CSQ 25
   [LTE] jam tersinkron: ...
   [MQTT] menghubungkan... OK
   ```

## Ciri-ciri mode ini

- **Data dikirim tiap 5 detik** untuk hemat kuota. Perkiraan pemakaian ±250 MB per bulan.
- **Penanda di dashboard dan perangkat:**
  - Dashboard menampilkan label **"Mode Seluler 4G"** di Beranda, serta "Seluler 4G · SIMCom A7670E" dan nilai CSQ di menu Pengaturan.
  - LCD menampilkan **"Mode: 4G LTE"** saat ESP32 menyala.
- **Pembagian kerja di ESP32:** urusan modem dan internet berjalan di inti 0, sedangkan sensor dan pompa di inti 1. Jadi saat modem mencari sinyal, kontrol pompa tidak ikut tertahan.
- **Pemulihan otomatis:**
  - Jaringan hilang ±2 menit: modem di-restart.
  - MQTT gagal terus: koneksi data diaktifkan ulang.

## Catatan

- **IMEI modem harus terdaftar di Indonesia.** Cek di imei.kemenperin.go.id. Kalau tidak terdaftar, operator memblokir koneksi data walaupun sinyal ada.
- **Mode respons angka.** Sebagian modul A7670E dikirim dalam mode respons angka (menjawab `0` alih-alih `OK`). Firmware mengubahnya otomatis ke mode teks dan menyimpannya permanen di modem (`AT&W`).
- **Library TinyGSM tidak dipakai.** Versi 0.12.0 (driver A7672X) gagal membuka koneksi TCP biasa ke modem ini, jadi firmware memakai perintah `AT+CMQTT...` bawaan modem.
- **Teruji di perangkat:** A7670E-LASA firmware V1.11.2 dengan kartu XL. Data masuk ke broker setiap 5 detik, dan perintah dari dashboard sampai ke ESP32.

Kalau di lokasi ada WiFi yang stabil, [mode WiFi](../mode_wifi/README.md) lebih sederhana dan tanpa biaya kuota.
