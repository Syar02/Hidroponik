# Mode WiFi

Pakai mode ini bila di lokasi kebun **ada WiFi 2,4 GHz**. ESP32 tersambung ke router dengan WiFi bawaannya sendiri, jadi tidak perlu kartu SIM maupun modem.

Skema: [`docs/mode_wifi/Skema_Mode_WiFi.jpg`](../../docs/mode_wifi/Skema_Mode_WiFi.jpg)

## Isi folder

| File | Fungsi |
|---|---|
| `mode_wifi.ino` | Firmware ESP32 mode WiFi |
| `wifi_config.example.h` | Contoh pengaturan WiFi (aman di-upload ke GitHub) |
| `wifi_config.h` | Pengaturan WiFi Anda (berisi password, **tidak** ikut ter-upload ke GitHub) |

## Langkah

1. Salin `wifi_config.example.h` menjadi `wifi_config.h`, lalu isi:
   ```cpp
   const char* WIFI_SSID   = "NAMA_WIFI";
   const char* WIFI_PASS   = "PASSWORD_WIFI";
   ```
2. Di Arduino IDE, pasang library **PubSubClient**, **Adafruit SHT31 Library**, **BH1750**, dan **LiquidCrystal I2C**.
3. Buka `mode_wifi.ino`. Kalau perlu, ubah `DEVICE_CODE` (harus sama dengan kode di link dashboard).
4. Pilih board **ESP32 Dev Module**, lalu upload.
5. Buka Serial Monitor (115200). Baris pertama akan menampilkan `=== MODE KONEKSI: WiFi (...) ===`, disusul `[WiFi] terhubung` dan `[MQTT] menghubungkan... OK`.

## Ciri-ciri mode ini

- **Data dikirim tiap 2 detik**, tanpa biaya kuota seluler.
- **Penanda di dashboard dan perangkat:**
  - Dashboard menampilkan label **"Mode WiFi"** di Beranda dan "WiFi (router lokal)" di menu Pengaturan.
  - LCD menampilkan **"Mode: WiFi"** saat ESP32 menyala.
- **Kalau WiFi gagal tersambung**, Serial Monitor menampilkan daftar WiFi yang terlihat oleh ESP32 beserta alasan gagalnya.

## Syarat jaringan

- **WiFi harus 2,4 GHz.** ESP32 tidak bisa tersambung ke WiFi 5 GHz.
- **Tidak bisa** WiFi yang meminta login lewat halaman web (hotel/kafe) atau WiFi WPA2-Enterprise (username dan password).
- **Sinyal di lokasi ESP32 sebaiknya lebih kuat dari −75 dBm.** Nilainya terlihat di menu Pengaturan dashboard.

Kalau di lokasi tidak ada WiFi atau WiFi-nya tidak stabil, pindah ke [mode seluler 4G](../mode_lte_a7670e/README.md).
