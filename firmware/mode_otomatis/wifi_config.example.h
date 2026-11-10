// Daftar WiFi yang dikenal ESP32.
// Salin file ini menjadi wifi_config.h lalu isi nama & password WiFi Anda.
// ESP32 memindai WiFi sekitar lalu menyambung ke WiFi terdaftar yang sinyalnya
// paling kuat. Tambahkan baris baru untuk WiFi lain (hotspot HP, router kedua).
// Hanya WiFi 2,4 GHz. Bila hanya ingin memakai 4G, isi satu baris {"", ""}.
const WifiCred WIFI_LIST[] = {
  {"NAMA_WIFI", "PASSWORD_WIFI"},
  // {"HOTSPOT_HP", "password_hotspot"},
};
