/*
 * ============================================================
 *  MODE WiFi  —  Monitoring Hidroponik ESP32 + MQTT
 * ============================================================
 * Pakai mode ini bila di lokasi kebun ADA WiFi 2,4 GHz.
 * (Bila tidak ada WiFi, pakai firmware/mode_lte_a7670e.)
 *
 * Sensor : pH (analog GPIO32), SHT31 (suhu & kelembapan), BH1750 (lux)
 * Aktuator: Relay pompa koreksi pH (GPIO25, aktif LOW)
 * Tampilan: LCD I2C 16x2 (3 halaman bergantian)
 *
 * Data dikirim ke broker MQTT publik (broker.emqx.io) dan dibaca oleh
 * dashboard web:  https://syar02.github.io/Hidroponik/#<DEVICE_CODE>
 * Telemetry membawa "net":"wifi" sehingga dashboard menampilkan "Mode WiFi".
 *
 * Topik (BASE = "aranus/<DEVICE_CODE>/hidro"):
 *   BASE/telemetry  -> JSON data sensor terbaru (retained, tiap 2 detik)
 *   BASE/status     -> "online" / "offline" (retained, LWT)
 *   BASE/config     -> JSON ambang pH (retained)
 *   BASE/history    -> JSON rata-rata per jam 24 jam terakhir (retained, tiap 1 menit)
 *   BASE/set        <- perintah dari dashboard, format "kunci=nilai":
 *                        relay=on | relay=off | relay=toggle   (otomatis pindah ke mode manual)
 *                        mode=auto | mode=manual
 *                        ph_min=5.5  ph_max=8.5  ph_off=0.00  hyst=0.10
 *                        ping
 *
 * Library (Arduino Library Manager):
 *   - PubSubClient (Nick O'Leary)
 *   - Adafruit SHT31 Library
 *   - BH1750 (Christopher Laws)
 *   - LiquidCrystal I2C (Frank de Brabander / Marco Schwartz)
 */

#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_SHT31.h>
#include <BH1750.h>

// ======================= KONFIGURASI =======================
// Nama & password WiFi ada di wifi_config.h (salin dari wifi_config.example.h).
// File itu diabaikan git agar password tidak ikut ter-upload ke GitHub.
#include "wifi_config.h"

#define MODE_NAME "WiFi"

// Kode unik perangkat. Dipakai di link dashboard: .../#hd7p2kq9vm
// Ganti dengan kode acak milik Anda (huruf kecil + angka, 6-32 karakter).
const char* DEVICE_CODE = "hd7p2kq9vm";

const char* MQTT_HOST   = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;

// Zona waktu WIB (UTC+7) untuk reset durasi relay harian
const long GMT_OFFSET_SEC = 7 * 3600;

// Interval
const unsigned long SENSOR_INTERVAL   = 1000;   // baca sensor
const unsigned long PUBLISH_INTERVAL  = 2000;   // kirim telemetry
const unsigned long HISTORY_INTERVAL  = 60000;  // kirim riwayat per jam
const unsigned long LCD_INTERVAL      = 3000;   // ganti halaman LCD
// Mode manual otomatis kembali ke auto setelah 30 menit (pengaman pompa)
const unsigned long MANUAL_TIMEOUT    = 30UL * 60UL * 1000UL;
// ===========================================================

// Pin & ADC
const int pinPo    = 32;   // ADC1, aman dipakai bersama WiFi
const int pinRelay = 25;
const float V_REF = 3.3;
const float ADC_RESOLUTION = 4095.0;

// Relay aktif LOW (sesuai kode lama: LOW = NYALA)
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_SHT31 sht31 = Adafruit_SHT31();
BH1750 lightMeter;
Preferences prefs;
WiFiClient net;
PubSubClient mqtt(net);

String BASE, T_TELE, T_STATUS, T_CONFIG, T_HISTORY, T_SET;

// Konfigurasi (disimpan di flash)
float phMin = 5.5, phMax = 8.5, phOffset = 0.0, phHyst = 0.10;

// Data sensor
float phValue = NAN, voltage = NAN, temp = NAN, hum = NAN, lux = NAN;

// Kontrol relay
bool relayOn = false;
bool autoMode = true;
unsigned long manualSince = 0;
unsigned long relayOnMsToday = 0;
unsigned long relayLastTick = 0;
int lastDay = -1;

// Timer
unsigned long tSensor = 0, tPublish = 0, tHistory = 0, tLcd = 0, tWifi = 0, tMqtt = 0;
int displayPage = 0;
bool forcePublish = false;

// ---------------- Riwayat 24 jam (rata-rata per jam) ----------------
struct Bucket { long hour; float sum[4]; uint16_t n[4]; };
Bucket hist[24];

void histAdd(int k, float v) {
  if (isnan(v)) return;
  time_t now = time(nullptr);
  if (now < 1700000000) return;           // waktu belum sinkron NTP
  long h = now / 3600;
  Bucket& b = hist[h % 24];
  if (b.hour != h) { b.hour = h; for (int i = 0; i < 4; i++) { b.sum[i] = 0; b.n[i] = 0; } }
  b.sum[k] += v; b.n[k]++;
}

// ---------------- Util ----------------
void fmtNum(char* out, size_t len, float v, int dec) {
  if (isnan(v) || isinf(v)) snprintf(out, len, "null");
  else snprintf(out, len, "%.*f", dec, v);
}

void setRelay(bool on) {
  if (on == relayOn) return;
  relayOn = on;
  digitalWrite(pinRelay, on ? RELAY_ON : RELAY_OFF);
  forcePublish = true;
}

void saveConfig() {
  prefs.putFloat("ph_min", phMin);
  prefs.putFloat("ph_max", phMax);
  prefs.putFloat("ph_off", phOffset);
  prefs.putFloat("hyst", phHyst);
}

// ---------------- Sensor ----------------
void readSensors() {
  // Rata-rata 20 sampel ADC agar pH lebih stabil
  long acc = 0;
  for (int i = 0; i < 20; i++) { acc += analogRead(pinPo); delayMicroseconds(500); }
  float adcValue = acc / 20.0;
  voltage = (adcValue / ADC_RESOLUTION) * V_REF;
  phValue = 7.0 + ((2.5 - voltage) / 0.18) + phOffset;

  temp = sht31.readTemperature();
  hum  = sht31.readHumidity();

  float l = lightMeter.readLightLevel();
  lux = (l < 0) ? NAN : l;                // BH1750 mengembalikan nilai negatif saat error

  histAdd(0, phValue); histAdd(1, temp); histAdd(2, hum); histAdd(3, lux);
}

// ---------------- Kontrol relay ----------------
void controlRelay() {
  unsigned long now = millis();

  // Akumulasi durasi relay nyala hari ini
  if (relayOn) relayOnMsToday += now - relayLastTick;
  relayLastTick = now;
  struct tm ti;
  if (getLocalTime(&ti, 0)) {
    if (lastDay != -1 && ti.tm_yday != lastDay) relayOnMsToday = 0;
    lastDay = ti.tm_yday;
  }

  if (!autoMode) {
    if (now - manualSince >= MANUAL_TIMEOUT) { autoMode = true; forcePublish = true; }
    else return;
  }

  if (isnan(phValue)) return;
  // Logika sama seperti kode lama + histeresis agar relay tidak "cetak-cetek"
  if (phValue < phMin || phValue > phMax) setRelay(true);
  else if (phValue >= phMin + phHyst && phValue <= phMax - phHyst) setRelay(false);
}

// ---------------- MQTT ----------------
void publishConfig() {
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"ph_min\":%.2f,\"ph_max\":%.2f,\"ph_off\":%.2f,\"hyst\":%.2f}",
           phMin, phMax, phOffset, phHyst);
  mqtt.publish(T_CONFIG.c_str(), buf, true);
}

void publishTelemetry() {
  char sPh[12], sV[12], sT[12], sH[12], sL[14];
  fmtNum(sPh, sizeof(sPh), phValue, 2);
  fmtNum(sV,  sizeof(sV),  voltage, 3);
  fmtNum(sT,  sizeof(sT),  temp, 1);
  fmtNum(sH,  sizeof(sH),  hum, 1);
  fmtNum(sL,  sizeof(sL),  lux, 1);
  time_t now = time(nullptr);
  char buf[320];
  snprintf(buf, sizeof(buf),
    "{\"ph\":%s,\"v\":%s,\"t\":%s,\"h\":%s,\"lux\":%s,\"relay\":%d,\"mode\":\"%s\","
    "\"on_s\":%lu,\"rssi\":%d,\"net\":\"wifi\",\"up\":%lu,\"ts\":%ld}",
    sPh, sV, sT, sH, sL, relayOn ? 1 : 0, autoMode ? "auto" : "manual",
    relayOnMsToday / 1000UL, WiFi.RSSI(), millis() / 1000UL, (long)(now > 1700000000 ? now : 0));
  mqtt.publish(T_TELE.c_str(), buf, true);
}

void publishHistory() {
  time_t now = time(nullptr);
  if (now < 1700000000) return;
  long cur = now / 3600;
  static char buf[1400];
  const int dec[4] = {2, 1, 0, 0};
  const char* keys[4] = {"ph", "t", "h", "lux"};
  int p = snprintf(buf, sizeof(buf), "{\"end\":%ld", cur * 3600L);
  for (int k = 0; k < 4; k++) {
    p += snprintf(buf + p, sizeof(buf) - p, ",\"%s\":[", keys[k]);
    for (int i = 23; i >= 0; i--) {                 // urut dari jam terlama -> terbaru
      long h = cur - i;
      Bucket& b = hist[h % 24];
      char s[14];
      if (b.hour == h && b.n[k] > 0) fmtNum(s, sizeof(s), b.sum[k] / b.n[k], dec[k]);
      else snprintf(s, sizeof(s), "null");
      p += snprintf(buf + p, sizeof(buf) - p, "%s%s", s, i ? "," : "");
    }
    p += snprintf(buf + p, sizeof(buf) - p, "]");
  }
  snprintf(buf + p, sizeof(buf) - p, "}");
  mqtt.publish(T_HISTORY.c_str(), buf, true);
}

void onMessage(char* topic, byte* payload, unsigned int len) {
  char msg[64];
  len = min(len, (unsigned int)sizeof(msg) - 1);
  memcpy(msg, payload, len); msg[len] = 0;
  for (char* c = msg; *c; c++) *c = tolower(*c);
  Serial.printf("[MQTT] perintah: %s\n", msg);

  char* eq = strchr(msg, '=');
  String key = eq ? String(msg).substring(0, eq - msg) : String(msg);
  String val = eq ? String(eq + 1) : String("");
  key.trim(); val.trim();

  if (key == "relay") {
    autoMode = false; manualSince = millis();
    if (val == "on") setRelay(true);
    else if (val == "off") setRelay(false);
    else if (val == "toggle") setRelay(!relayOn);
  } else if (key == "mode") {
    autoMode = (val != "manual");
    if (!autoMode) manualSince = millis();
  } else if (key == "ph_min" || key == "ph_max" || key == "ph_off" || key == "hyst") {
    float f = val.toFloat();
    if (key == "ph_min" && f >= 0 && f < phMax) phMin = f;
    if (key == "ph_max" && f <= 14 && f > phMin) phMax = f;
    if (key == "ph_off" && f >= -3 && f <= 3) phOffset = f;
    if (key == "hyst" && f >= 0 && f <= 1) phHyst = f;
    saveConfig();
    publishConfig();
  } else if (key == "ping") {
    publishConfig();
    publishHistory();
  }
  forcePublish = true;
}

const char* wifiStatusText(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL:  return "SSID tidak ditemukan (cek nama WiFi / harus 2.4 GHz)";
    case WL_CONNECT_FAILED: return "gagal (cek password)";
    case WL_CONNECTION_LOST:return "koneksi terputus";
    case WL_DISCONNECTED:   return "belum terhubung";
    case WL_IDLE_STATUS:    return "menunggu";
    default:                return "lainnya";
  }
}

// Pindai WiFi sekitar agar mudah mendiagnosa salah nama / 5 GHz
void scanWifi() {
  Serial.println("[WiFi] memindai jaringan...");
  int n = WiFi.scanNetworks();
  bool found = false;
  for (int i = 0; i < n; i++) {
    bool match = WiFi.SSID(i) == WIFI_SSID;
    found |= match;
    Serial.printf("  %s \"%s\"  %d dBm  ch%d\n", match ? ">>" : "  ",
                  WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
  }
  if (!found) Serial.printf("[WiFi] \"%s\" TIDAK terlihat. Pastikan hotspot/router aktif & 2.4 GHz.\n", WIFI_SSID);
  WiFi.scanDelete();
}

void ensureWifi() {
  static bool wasConnected = false;
  bool c = WiFi.status() == WL_CONNECTED;
  if (c != wasConnected) {
    wasConnected = c;
    if (c) Serial.printf("[WiFi] terhubung: %s (%d dBm)\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else   Serial.println("[WiFi] terputus");
  }
  if (c) return;
  if (millis() - tWifi < 20000) return;   // beri waktu proses koneksi selesai
  tWifi = millis();
  Serial.printf("[WiFi] status: %s -> mencoba lagi\n", wifiStatusText(WiFi.status()));
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void ensureMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (millis() - tMqtt < 5000) return;
  tMqtt = millis();
  String cid = String("esp32-") + DEVICE_CODE + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("[MQTT] menghubungkan... ");
  // LWT: broker akan mengirim "offline" jika ESP32 putus
  if (mqtt.connect(cid.c_str(), T_STATUS.c_str(), 1, true, "offline")) {
    Serial.println("OK");
    mqtt.publish(T_STATUS.c_str(), "online", true);
    mqtt.subscribe(T_SET.c_str());
    publishConfig();
    publishHistory();
    forcePublish = true;
  } else {
    Serial.printf("gagal rc=%d\n", mqtt.state());
  }
}

// ---------------- LCD ----------------
void lcdPrintNum(float v, int dec) { if (isnan(v)) lcd.print("--"); else lcd.print(v, dec); }

void updateLcd() {
  lcd.clear();
  if (displayPage == 0) {
    lcd.setCursor(0, 0); lcd.print("PH Value: "); lcdPrintNum(phValue, 2);
    lcd.setCursor(0, 1); lcd.print("Volt: "); lcdPrintNum(voltage, 2); lcd.print("V");
  } else if (displayPage == 1) {
    lcd.setCursor(0, 0); lcd.print("T:"); lcdPrintNum(temp, 1); lcd.print((char)223);
    lcd.print("C Hum:"); lcdPrintNum(hum, 0); lcd.print("%");
    lcd.setCursor(0, 1); lcd.print("Light: "); lcdPrintNum(lux, 1); lcd.print(" lux");
  } else {
    lcd.setCursor(0, 0); lcd.print("Pompa:"); lcd.print(relayOn ? "ON " : "OFF");
    lcd.print(autoMode ? " AUTO" : " MAN");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.status() == WL_CONNECTED ? "WiFi OK " : "WiFi -- ");
    lcd.print(mqtt.connected() ? "MQTT OK" : "MQTT --");
  }
}

// ---------------- Setup & Loop ----------------
void setup() {
  Serial.begin(115200);
  Wire.begin();  // SDA = GPIO21, SCL = GPIO22

  pinMode(pinRelay, OUTPUT);
  digitalWrite(pinRelay, RELAY_OFF);

  lcd.init();
  lcd.backlight();
  if (!sht31.begin(0x44)) Serial.println("Gagal mendeteksi sensor SHT31!");
  lightMeter.begin();
  analogSetAttenuation(ADC_11db);

  prefs.begin("hidro", false);
  phMin    = prefs.getFloat("ph_min", 5.5);
  phMax    = prefs.getFloat("ph_max", 8.5);
  phOffset = prefs.getFloat("ph_off", 0.0);
  phHyst   = prefs.getFloat("hyst", 0.10);

  for (int i = 0; i < 24; i++) hist[i].hour = -1;

  BASE      = String("aranus/") + DEVICE_CODE + "/hidro";
  T_TELE    = BASE + "/telemetry";
  T_STATUS  = BASE + "/status";
  T_CONFIG  = BASE + "/config";
  T_HISTORY = BASE + "/history";
  T_SET     = BASE + "/set";

  lcd.setCursor(0, 0); lcd.print("  Smart System  ");
  lcd.setCursor(0, 1); lcd.print("Mode: WiFi      ");
  Serial.println("\n=== MODE KONEKSI: WiFi (" + String(WIFI_SSID) + ") ===");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  Serial.printf("[WiFi] menghubungkan ke \"%s\"...\n", WIFI_SSID);
  // 2 percobaan @7 dtk: setelah reset, router sering menolak percobaan pertama
  for (int attempt = 0; attempt < 2 && WiFi.status() != WL_CONNECTED; attempt++) {
    if (attempt) { WiFi.disconnect(); delay(200); }
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 7000) delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WiFi] belum terhubung (%s), lanjut offline & coba lagi otomatis\n", wifiStatusText(WiFi.status()));
    scanWifi();                       // diagnosa: tampilkan WiFi yang terlihat
    tWifi = millis() - 15000;         // coba lagi ~5 detik lagi
  } else {
    tWifi = millis();
  }

  configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(1536);
  mqtt.setKeepAlive(30);

  lcd.clear();
  Serial.printf("Dashboard: https://syar02.github.io/Hidroponik/#%s\n", DEVICE_CODE);
  relayLastTick = millis();
}

void loop() {
  unsigned long now = millis();

  ensureWifi();
  ensureMqtt();
  mqtt.loop();

  if (now - tSensor >= SENSOR_INTERVAL) {
    tSensor = now;
    readSensors();
    controlRelay();   // kontrol tetap jalan walau WiFi/MQTT putus
    Serial.printf("pH: %.2f | Suhu: %.1f C | Hum: %.1f %% | Lux: %.1f | Relay: %s (%s)\n",
                  phValue, temp, hum, lux, relayOn ? "ON" : "OFF", autoMode ? "auto" : "manual");
  }

  if (mqtt.connected()) {
    if (forcePublish || now - tPublish >= PUBLISH_INTERVAL) {
      tPublish = now; forcePublish = false;
      publishTelemetry();
    }
    if (now - tHistory >= HISTORY_INTERVAL) {
      tHistory = now;
      publishHistory();
      // Tegaskan ulang status online (menimpa "offline" sisa sesi lama setelah restart)
      mqtt.publish(T_STATUS.c_str(), "online", true);
    }
  }

  if (now - tLcd >= LCD_INTERVAL) {
    tLcd = now;
    displayPage = (displayPage + 1) % 3;
    updateLcd();
  }
}
