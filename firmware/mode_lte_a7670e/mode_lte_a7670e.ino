/*
 * ============================================================
 *  MODE SELULER 4G  —  Monitoring Hidroponik ESP32 + SIMCom A7670E + MQTT
 * ============================================================
 * Pakai mode ini bila di lokasi kebun TIDAK ADA WiFi (cukup sinyal seluler).
 * (Bila ada WiFi, pakai firmware/mode_wifi.)
 * Telemetry membawa "net":"lte" sehingga dashboard menampilkan "Mode Seluler 4G".
 *
 * Koneksi SELULER (tanpa WiFi). ESP32 memberi perintah AT ke modem
 * A7670E lewat UART; modem yang membawa data ke internet memakai kartu SIM.
 * MQTT dijalankan oleh FITUR MQTT BAWAAN MODEM (perintah AT+CMQTT...),
 * sehingga tidak perlu library TinyGSM/PubSubClient.
 *
 * Sensor, logika kontrol pH, LCD, dan topik MQTT SAMA dengan versi WiFi
 * (firmware/mode_wifi), sehingga dashboard yang sama tetap dipakai:
 *   https://syar02.github.io/Hidroponik/#<DEVICE_CODE>
 *
 * Arsitektur program (dua inti ESP32):
 *   - Inti 1 (loop)   : baca sensor, kontrol relay pH, LCD  -> tidak pernah
 *                       tertahan oleh proses modem/jaringan.
 *   - Inti 0 (netTask): modem, jaringan LTE, MQTT, kirim data.
 *
 * Wiring modem (lihat docs/mode_lte_a7670e/Skema_Mode_LTE_A7670E.jpg):
 *   ESP32 GPIO17 (TX) -> RXD modem      ESP32 GPIO16 (RX) <- TXD modem
 *   ESP32 GPIO4       -> PWRKEY (opsional)
 *   GND bersama. Modem WAJIB catu daya sendiri (puncak arus ±2 A).
 *
 * Library (Arduino Library Manager): Adafruit SHT31, BH1750, LiquidCrystal I2C
 */

#include <Wire.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_SHT31.h>
#include <BH1750.h>

// ======================= KONFIGURASI =======================
// APN operator. Umumnya: Telkomsel & XL = "internet", Indosat = "indosatgprs",
// Tri = "3gprs". Cek ke operator bila koneksi data gagal.
const char* APN      = "internet";
const char* APN_USER = "";
const char* APN_PASS = "";

// Kode unik perangkat (sama dengan link dashboard .../#kode)
const char* DEVICE_CODE = "hd7p2kq9vm";

// Kode USSD cek sisa paket data (perintah MQTT "kuota"). Boleh berupa urutan
// menu dipisah koma: kode awal lalu nomor pilihan. Kosongkan ("") agar dipilih
// otomatis dari nama operator:
//   XL/Axis   : *808#,7,4,2,2  (MyInfo > Cek Kuota > lewati 2 layar promo;
//               teruji 10-11-2026, hasilnya daftar paket untuk dipilih)
//   Telkomsel : *888#   Smartfren : *995#   lainnya (Indosat/IM3/Tri) : *123#
// Menu USSD bisa diubah operator sewaktu-waktu; isi manual bila tidak sesuai.
const char* KUOTA_USSD = "";

const char* MQTT_HOST = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;

// Pin modem
#define MODEM_RX_PIN       16        // ESP32 menerima  <- TXD modem
#define MODEM_TX_PIN       17        // ESP32 mengirim  -> RXD modem
#define MODEM_PWRKEY_PIN   4         // -1 bila PWRKEY tidak disambung
#define PWRKEY_ACTIVE_HIGH false     // true bila PWRKEY lewat transistor NPN
#define MODEM_BAUD         115200

// Interval (dibuat lebih hemat kuota daripada versi WiFi)
const unsigned long SENSOR_INTERVAL  = 1000;    // baca sensor
const unsigned long PUBLISH_INTERVAL = 5000;    // kirim telemetry (WiFi: 2000)
const unsigned long HISTORY_INTERVAL = 60000;   // kirim riwayat per jam
const unsigned long LINK_CHECK       = 30000;   // cek sinyal & registrasi
const unsigned long LCD_INTERVAL     = 3000;
const unsigned long MANUAL_TIMEOUT   = 30UL * 60UL * 1000UL;
// ===========================================================

const int pinPo    = 32;
const int pinRelay = 25;
const float V_REF = 3.3;
const float ADC_RESOLUTION = 4095.0;
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_SHT31 sht31 = Adafruit_SHT31();
BH1750 lightMeter;
Preferences prefs;
HardwareSerial SerialAT(2);

String BASE, T_TELE, T_STATUS, T_CONFIG, T_HISTORY, T_SET, T_KUOTA;

// Konfigurasi (disimpan di flash)
volatile float phMin = 5.5, phMax = 8.5, phOffset = 0.0, phHyst = 0.10;

// Data sensor (ditulis inti 1, dibaca inti 0)
volatile float phValue = NAN, voltage = NAN, temp = NAN, hum = NAN, lux = NAN;

// Status kontrol
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool relayOn = false;
volatile bool autoMode = true;
volatile unsigned long manualSince = 0;
volatile unsigned long relayOnMsToday = 0;
unsigned long relayLastTick = 0;
int lastDay = -1;
volatile bool forcePublish = false;

// Status jaringan (untuk LCD & telemetry)
volatile bool netUp = false, mqttUp = false, timeSynced = false;
volatile int csq = 99;

unsigned long tSensor = 0, tLcd = 0;
int displayPage = 0;

// ---------------- Riwayat 24 jam ----------------
struct Bucket { long hour; float sum[4]; uint16_t n[4]; };
Bucket hist[24];

void histAdd(int k, float v) {
  if (isnan(v)) return;
  time_t now = time(nullptr);
  if (now < 1700000000) return;
  long h = now / 3600;
  portENTER_CRITICAL(&stateMux);
  Bucket& b = hist[h % 24];
  if (b.hour != h) { b.hour = h; for (int i = 0; i < 4; i++) { b.sum[i] = 0; b.n[i] = 0; } }
  b.sum[k] += v; b.n[k]++;
  portEXIT_CRITICAL(&stateMux);
}

// ---------------- Util ----------------
void fmtNum(char* out, size_t len, float v, int dec) {
  if (isnan(v) || isinf(v)) snprintf(out, len, "null");
  else snprintf(out, len, "%.*f", dec, v);
}

void setRelay(bool on) {
  bool changed = false;
  portENTER_CRITICAL(&stateMux);
  if (on != relayOn) { relayOn = on; changed = true; }
  portEXIT_CRITICAL(&stateMux);
  if (changed) { digitalWrite(pinRelay, on ? RELAY_ON : RELAY_OFF); forcePublish = true; }
}

void saveConfig() {
  prefs.putFloat("ph_min", phMin);
  prefs.putFloat("ph_max", phMax);
  prefs.putFloat("ph_off", phOffset);
  prefs.putFloat("hyst", phHyst);
}

long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

// ---------------- Sensor (inti 1) ----------------
void readSensors() {
  long acc = 0;
  for (int i = 0; i < 20; i++) { acc += analogRead(pinPo); delayMicroseconds(500); }
  float adcValue = acc / 20.0;
  voltage = (adcValue / ADC_RESOLUTION) * V_REF;
  phValue = 7.0 + ((2.5 - voltage) / 0.18) + phOffset;

  temp = sht31.readTemperature();
  hum  = sht31.readHumidity();
  float l = lightMeter.readLightLevel();
  lux = (l < 0) ? NAN : l;

  histAdd(0, phValue); histAdd(1, temp); histAdd(2, hum); histAdd(3, lux);
}

// ---------------- Kontrol relay (inti 1) ----------------
// Logika SAMA dengan versi WiFi (lihat catatan di README tentang
// penyesuaian yang disarankan untuk pompa pH Down satu arah).
void controlRelay() {
  unsigned long now = millis();
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
  float ph = phValue;
  if (isnan(ph)) return;
  if (ph < phMin || ph > phMax) setRelay(true);
  else if (ph >= phMin + phHyst && ph <= phMax - phHyst) setRelay(false);
}

// =====================================================================
//  Lapisan perintah AT (hanya dipakai di inti 0)
// =====================================================================
String lineBuf, atResp, rxTopic, rxPayload;
String inbox[4];
int inboxN = 0;
bool mqttLost = false;

void readExact(String& out, int len, uint32_t timeout = 3000) {
  out = ""; unsigned long t0 = millis();
  while ((int)out.length() < len && millis() - t0 < timeout) {
    if (SerialAT.available()) out += (char)SerialAT.read(); else delay(1);
  }
}

// Pesan spontan dari modem (URC): pesan MQTT masuk & koneksi putus
bool handleUrc(const String& l) {
  if (l.startsWith("+CMQTTRXSTART:"))   { rxTopic = ""; rxPayload = ""; return true; }
  if (l.startsWith("+CMQTTRXTOPIC:"))   { readExact(rxTopic, l.substring(l.lastIndexOf(',') + 1).toInt()); return true; }
  if (l.startsWith("+CMQTTRXPAYLOAD:")) { String p; readExact(p, l.substring(l.lastIndexOf(',') + 1).toInt()); rxPayload += p; return true; }
  if (l.startsWith("+CMQTTRXEND:"))     { if (inboxN < 4) inbox[inboxN++] = rxPayload; return true; }
  if (l.startsWith("+CMQTTCONNLOST:") || l.startsWith("+CMQTTNONET")) {
    Serial.printf("[MQTT] koneksi putus: %s\n", l.c_str());
    mqttLost = true; return true;
  }
  return false;
}

bool readLine(String& out, uint32_t timeout) {
  unsigned long t0 = millis();
  do {
    while (SerialAT.available()) {
      char c = SerialAT.read();
      if (c == '\n') {
        out = lineBuf; lineBuf = ""; out.trim();
        if (out.length()) return true;
      } else if (c != '\r') {
        lineBuf += c;
        if (lineBuf.length() > 600) lineBuf = "";
      }
    }
    if (timeout) delay(1);
  } while (millis() - t0 < timeout);
  return false;
}

// Tunggu OK/ERROR. 1 = OK, 0 = ERROR, -1 = timeout. Baris jawaban -> atResp
int waitOk(uint32_t timeout) {
  unsigned long t0 = millis(); String l;
  while (true) {
    unsigned long el = millis() - t0;
    if (el >= timeout || !readLine(l, timeout - el)) return -1;
    if (handleUrc(l)) continue;
    if (l == "OK") return 1;
    if (l == "ERROR" || l.startsWith("+CME ERROR") || l.startsWith("+CMS ERROR")) { atResp += l; return 0; }
    if (l.startsWith("AT")) continue;                      // echo
    atResp += l; atResp += '\n';
  }
}

int at(const String& cmd, uint32_t timeout = 3000) {
  atResp = "";
  SerialAT.print(cmd); SerialAT.print("\r\n");
  return waitOk(timeout);
}

// Tunggu baris berawalan prefix (cek dulu di atResp bila sudah datang lebih awal)
bool waitFor(const char* prefix, uint32_t timeout, String* outLine = nullptr) {
  int i = atResp.indexOf(prefix);
  if (i >= 0) {
    int e = atResp.indexOf('\n', i);
    if (outLine) *outLine = atResp.substring(i, e < 0 ? atResp.length() : e);
    return true;
  }
  unsigned long t0 = millis(); String l;
  while (true) {
    unsigned long el = millis() - t0;
    if (el >= timeout || !readLine(l, timeout - el)) return false;
    if (l.startsWith(prefix)) { if (outLine) *outLine = l; return true; }
    handleUrc(l);
  }
}

int lastInt(const String& l) { return l.substring(l.lastIndexOf(',') + 1).toInt(); }

// Perintah yang meminta data setelah prompt '>'
bool sendWithPrompt(const String& cmd, const String& data, uint32_t timeout = 3000) {
  atResp = "";
  SerialAT.print(cmd); SerialAT.print("\r\n");
  unsigned long t0 = millis(); bool prompt = false;
  while (!prompt && millis() - t0 < timeout) {
    while (SerialAT.available()) {
      char c = SerialAT.read();
      if (c == '>' && lineBuf.length() == 0) { prompt = true; break; }
      if (c == '\n') {
        String l = lineBuf; lineBuf = ""; l.trim();
        if (!l.length() || handleUrc(l)) continue;
        if (l == "ERROR" || l.startsWith("+CME ERROR")) return false;
      } else if (c != '\r') lineBuf += c;
    }
    if (!prompt) delay(1);
  }
  if (!prompt) return false;
  SerialAT.print(data);
  return waitOk(timeout) == 1;
}

void pollModem() {
  String l;
  while (SerialAT.available()) { if (readLine(l, 20)) handleUrc(l); }
}

// =====================================================================
//  Modem, jaringan, waktu (inti 0)
// =====================================================================
void pressPwrKey() {
  if (MODEM_PWRKEY_PIN < 0) return;
  // Pulsa ±0,6 dtk = "tekan tombol power" (menyalakan). Tekan > 2,5 dtk = mematikan.
  digitalWrite(MODEM_PWRKEY_PIN, PWRKEY_ACTIVE_HIGH ? HIGH : LOW);
  delay(600);
  digitalWrite(MODEM_PWRKEY_PIN, PWRKEY_ACTIVE_HIGH ? LOW : HIGH);
}

// Beberapa modul dikirim dalam mode respons angka (ATV0: "0" = OK) -> paksa teks
bool modemAlive() {
  for (int i = 0; i < 3; i++) {
    SerialAT.print("ATV1\r\n"); delay(150);
    SerialAT.print("ATE0\r\n"); delay(150);
    while (SerialAT.available()) SerialAT.read();
    lineBuf = "";
    if (at("AT", 1000) == 1) return true;
  }
  return false;
}

bool modemStart() {
  Serial.println("[LTE] memeriksa modem...");
  if (!modemAlive()) {
    Serial.println("[LTE] modem diam, menekan PWRKEY...");
    pressPwrKey();
    unsigned long t0 = millis(); bool ok = false;
    while (millis() - t0 < 30000) { delay(1000); if (modemAlive()) { ok = true; break; } }
    if (!ok) {
      Serial.println("[LTE] modem tidak merespons AT. Cek: TX/RX tertukar? catu daya modem? GND bersama? PWRKEY?");
      return false;
    }
  }
  at("AT+CMEE=2");
  at("AT&W");                 // simpan mode teks (ATV1) & echo off secara permanen di modem
  if (at("AT+CGMM", 2000) == 1) { String m = atResp; m.trim(); Serial.printf("[LTE] modem : %s\n", m.c_str()); }
  if (at("AT+CGSN", 2000) == 1) { String m = atResp; m.trim(); Serial.printf("[LTE] IMEI  : %s  (pastikan IMEI terdaftar di Indonesia)\n", m.c_str()); }
  if (at("AT+CPIN?", 5000) != 1 || atResp.indexOf("READY") < 0) {
    Serial.println("[LTE] SIM belum siap: cek kartu SIM terpasang benar / PIN SIM dimatikan");
    return false;
  }
  return true;
}

int readCsq() {
  if (at("AT+CSQ") == 1) { int i = atResp.indexOf("+CSQ:"); if (i >= 0) return atResp.substring(i + 5).toInt(); }
  return 99;
}

bool registered() {
  const char* cmds[2] = {"AT+CEREG?", "AT+CREG?"};
  for (const char* c : cmds) {
    if (at(c) == 1) {
      int i = atResp.indexOf(':');
      if (i < 0) continue;
      int st = lastInt(atResp.substring(i + 1, atResp.indexOf('\n', i)));
      if (st == 1 || st == 5) return true;        // 1 = jaringan asal, 5 = roaming
    }
  }
  return false;
}

String operatorName() {
  if (at("AT+COPS?", 5000) == 1) {
    int a = atResp.indexOf('"'), b = atResp.indexOf('"', a + 1);
    if (a >= 0 && b > a) return atResp.substring(a + 1, b);
  }
  return "?";
}

String dataIp() {
  if (at("AT+CGPADDR=1") == 1) {
    int i = atResp.indexOf(',');
    if (i > 0) { String ip = atResp.substring(i + 1); ip.trim(); ip.replace("\"", ""); if (ip.length() >= 7 && ip != "0.0.0.0") return ip; }
  }
  return "";
}

bool dataConnect() {
  at(String("AT+CGDCONT=1,\"IP\",\"") + APN + "\"");
  if (strlen(APN_USER))   // urutan parameter sesuai manual A76xx: <cid>,<tipe>,<password>,<user>
    at(String("AT+CGAUTH=1,1,\"") + APN_PASS + "\",\"" + APN_USER + "\"");
  at("AT+CGACT=1,1", 40000);
  return dataIp().length() > 0;
}

bool syncTime() {
  // Minta modem sinkron NTP (zona +7 jam = 28 x 15 menit), lalu baca jamnya
  at("AT+CNTP=\"pool.ntp.org\",28");
  if (at("AT+CNTP", 3000) == 1) waitFor("+CNTP:", 15000);
  if (at("AT+CCLK?") != 1) return false;
  int q = atResp.indexOf('"');
  if (q < 0) return false;
  String t = atResp.substring(q + 1);                      // yy/MM/dd,hh:mm:ss+zz
  int yy = t.substring(0, 2).toInt(), mo = t.substring(3, 5).toInt(), d = t.substring(6, 8).toInt();
  int h = t.substring(9, 11).toInt(), mi = t.substring(12, 14).toInt(), s = t.substring(15, 17).toInt();
  int tzq = t.substring(18).toInt(); if (t.charAt(17) == '-') tzq = -tzq;
  if (yy < 24) return false;
  time_t utc = (time_t)(daysFromCivil(2000 + yy, mo, d) * 86400L + h * 3600L + mi * 60L + s - tzq * 900L);
  timeval tv = { utc, 0 };
  settimeofday(&tv, nullptr);
  Serial.printf("[LTE] jam tersinkron: 20%02d-%02d-%02d %02d:%02d:%02d (UTC%+d)\n", yy, mo, d, h, mi, s, tzq / 4);
  return true;
}

// =====================================================================
//  MQTT bawaan modem (inti 0)
// =====================================================================
bool mqttPublish(const String& topic, const char* payload, bool retain) {
  if (!sendWithPrompt("AT+CMQTTTOPIC=0," + String(topic.length()), topic)) return false;
  if (!sendWithPrompt("AT+CMQTTPAYLOAD=0," + String(strlen(payload)), String(payload))) return false;
  if (at(String("AT+CMQTTPUB=0,0,60,") + (retain ? "1" : "0"), 5000) != 1) return false;
  String l;
  if (!waitFor("+CMQTTPUB: 0,", 15000, &l)) return false;
  return lastInt(l) == 0;
}

bool publishConfig() {
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"ph_min\":%.2f,\"ph_max\":%.2f,\"ph_off\":%.2f,\"hyst\":%.2f}",
           (float)phMin, (float)phMax, (float)phOffset, (float)phHyst);
  return mqttPublish(T_CONFIG, buf, true);
}

bool publishTelemetry() {
  char sPh[12], sV[12], sT[12], sH[12], sL[14], sR[8];
  fmtNum(sPh, sizeof(sPh), phValue, 2);
  fmtNum(sV,  sizeof(sV),  voltage, 3);
  fmtNum(sT,  sizeof(sT),  temp, 1);
  fmtNum(sH,  sizeof(sH),  hum, 1);
  fmtNum(sL,  sizeof(sL),  lux, 1);
  int q = csq;
  if (q >= 0 && q <= 31) snprintf(sR, sizeof(sR), "%d", -113 + 2 * q);   // CSQ -> dBm
  else snprintf(sR, sizeof(sR), "null");
  time_t now = time(nullptr);
  char buf[360];
  snprintf(buf, sizeof(buf),
    "{\"ph\":%s,\"v\":%s,\"t\":%s,\"h\":%s,\"lux\":%s,\"relay\":%d,\"mode\":\"%s\","
    "\"on_s\":%lu,\"rssi\":%s,\"csq\":%d,\"net\":\"lte\",\"up\":%lu,\"ts\":%ld}",
    sPh, sV, sT, sH, sL, relayOn ? 1 : 0, autoMode ? "auto" : "manual",
    relayOnMsToday / 1000UL, sR, q, millis() / 1000UL, (long)(now > 1700000000 ? now : 0));
  return mqttPublish(T_TELE, buf, true);
}

bool publishHistory() {
  time_t now = time(nullptr);
  if (now < 1700000000) return true;
  long cur = now / 3600;
  static Bucket snap[24];
  portENTER_CRITICAL(&stateMux);
  memcpy(snap, hist, sizeof(hist));
  portEXIT_CRITICAL(&stateMux);
  static char buf[1400];
  const int dec[4] = {2, 1, 0, 0};
  const char* keys[4] = {"ph", "t", "h", "lux"};
  int p = snprintf(buf, sizeof(buf), "{\"end\":%ld", cur * 3600L);
  for (int k = 0; k < 4; k++) {
    p += snprintf(buf + p, sizeof(buf) - p, ",\"%s\":[", keys[k]);
    for (int i = 23; i >= 0; i--) {
      long h = cur - i;
      Bucket& b = snap[h % 24];
      char s[14];
      if (b.hour == h && b.n[k] > 0) fmtNum(s, sizeof(s), b.sum[k] / b.n[k], dec[k]);
      else snprintf(s, sizeof(s), "null");
      p += snprintf(buf + p, sizeof(buf) - p, "%s%s", s, i ? "," : "");
    }
    p += snprintf(buf + p, sizeof(buf) - p, "]");
  }
  snprintf(buf + p, sizeof(buf) - p, "}");
  return mqttPublish(T_HISTORY, buf, true);
}

// =====================================================================
//  Cek sisa paket: USSD & SMS (inti 0). Hasil -> topik BASE/kuota (retained)
// =====================================================================
String jsonEsc(const String& s) {
  String o; o.reserve(s.length() + 16);
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}

// Teks UCS2 dikirim modem sebagai heksa ("0053006900730061...") -> ubah ke UTF-8
String ucs2Decode(const String& h) {
  if (h.length() < 4 || h.length() % 4) return h;
  for (unsigned i = 0; i < h.length(); i++) if (!isxdigit(h[i])) return h;
  String o;
  for (unsigned i = 0; i + 3 < h.length(); i += 4) {
    uint16_t c = strtol(h.substring(i, i + 4).c_str(), nullptr, 16);
    if (c == 0x0D) continue;
    if (c < 0x80) o += (char)c;
    else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
    else { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
  }
  return o;
}

// Tebak UCS2 hanya bila semua heksa & diawali "00" (huruf Latin dalam UCS2)
String maybeUcs2(const String& s) {
  return (s.length() >= 8 && s.startsWith("00")) ? ucs2Decode(s) : s;
}

String kuotaCode() {
  if (strlen(KUOTA_USSD)) return KUOTA_USSD;
  String op = operatorName(); op.toUpperCase();
  if (op.indexOf("XL") >= 0 || op.indexOf("AXIS") >= 0 || op.startsWith("51011")) return "*808#,7,4,2,2";
  if (op.indexOf("TELKOMSEL") >= 0 || op.indexOf("TSEL") >= 0 || op.startsWith("51010")) return "*888#";
  if (op.indexOf("SMART") >= 0 || op.startsWith("51009") || op.startsWith("51028")) return "*995#";
  return "*123#";
}

// USSD membuat modem pindah sebentar ke 2G/3G sehingga MQTT bisa putus.
// Hasil yang gagal terkirim disimpan lalu dikirim ulang setelah MQTT tersambung.
String kuotaPending;

void publishKuota(const String& json) {
  if (mqttPublish(T_KUOTA, json.c_str(), true)) { kuotaPending = ""; return; }
  Serial.println("[KUOTA] hasil belum terkirim, dikirim ulang setelah MQTT tersambung");
  kuotaPending = json;
}

String nowField() {
  time_t now = time(nullptr);
  return String(",\"ts\":") + String((long)(now > 1700000000 ? now : 0)) + "}";
}

// Satu langkah USSD. Kembali: status (0 selesai, 1 menu, 2.. lihat stTxt) atau -1 bila gagal.
int ussdStep(const String& code, String& text, String& err) {
  String buf; text = ""; err = "";
  if (at("AT+CUSD=1,\"" + code + "\",15", 10000) != 1) {
    err = "Modem menolak perintah USSD"; String r = atResp; r.trim(); if (r.length()) err += " (" + r + ")";
    return -1;
  }
  int i = atResp.indexOf("+CUSD:");
  if (i >= 0) buf = atResp.substring(i);
  else if (!waitFor("+CUSD:", 30000, &buf)) { err = "Tidak ada balasan dari operator dalam 30 detik"; return -1; }
  // Teks balasan bisa terdiri dari beberapa baris: baca sampai tanda kutip penutup
  String l; unsigned long t0 = millis();
  while (buf.indexOf('"') >= 0 && buf.indexOf('"') == buf.lastIndexOf('"') && millis() - t0 < 5000)
    if (readLine(l, 1000)) { buf += '\n'; buf += l; }
  buf.trim();
  int st = buf.substring(6).toInt();                     // "+CUSD: <st>,"<teks>",<dcs>"
  int a = buf.indexOf('"'), b = buf.lastIndexOf('"');
  text = (a >= 0 && b > a) ? buf.substring(a + 1, b) : "";
  int dcs = (b > 0 && buf.indexOf(',', b) > 0) ? buf.substring(buf.indexOf(',', b) + 1).toInt() : 15;
  text = (dcs == 72 || (dcs & 0x0C) == 0x08) ? ucs2Decode(text) : maybeUcs2(text);
  text.trim();
  return st;
}

// Jalankan kode USSD, boleh berupa urutan menu dipisah koma: "*808#,7,1"
// (kode awal lalu nomor pilihan menu). Seluruh urutan dijalankan sekaligus di
// modem, jadi tidak terganggu MQTT yang putus sebentar selama USSD.
void runUssd(String seq) {
  seq.trim(); seq.replace(" ", "");
  bool ok = seq.length() > 0 && seq.length() <= 60 && seq[0] != ',' && seq[seq.length() - 1] != ',';
  for (unsigned i = 0; ok && i < seq.length(); i++)
    ok = isdigit(seq[i]) || seq[i] == '*' || seq[i] == '#' || seq[i] == ',';
  if (!ok) {
    publishKuota("{\"kind\":\"ussd\",\"q\":\"" + jsonEsc(seq) + "\",\"err\":\"Kode tidak valid (hanya angka, *, # dan koma)\"" + nowField());
    return;
  }
  Serial.printf("[KUOTA] USSD %s\n", seq.c_str());
  publishKuota("{\"kind\":\"ussd\",\"q\":\"" + seq + "\",\"st\":\"wait\"" + nowField());

  const char* stTxt[6] = {"selesai", "menu", "diakhiri jaringan", "dijawab perangkat lain", "tidak didukung", "waktu habis"};
  at("AT+CSCS=\"IRA\"");
  at("AT+CUSD=2", 3000);                                 // tutup sesi lama bila masih terbuka
  String text, err; int st = -1, from = 0;
  while (true) {
    int c = seq.indexOf(',', from);
    String step = seq.substring(from, c < 0 ? seq.length() : c);
    st = ussdStep(step, text, err);
    Serial.printf("[KUOTA] %s -> %s\n", step.c_str(), st < 0 ? err.c_str() : (st <= 5 ? stTxt[st] : "?"));
    if (c < 0 || st != 1) {
      if (c >= 0 && st >= 0) err = "Menu berhenti sebelum pilihan \"" + seq.substring(c + 1) + "\" (USSD " + stTxt[st <= 5 ? st : 5] + ")";
      break;
    }
    from = c + 1;
  }
  if (st == 1) at("AT+CUSD=2", 3000);                    // jangan biarkan menu menggantung
  if (text.length()) Serial.printf("[KUOTA] balasan:\n%s\n", text.c_str());
  String j = "{\"kind\":\"ussd\",\"q\":\"" + seq + "\",\"st\":" + String(st) + ",\"text\":\"" + jsonEsc(text) + "\"";
  if (!err.length() && !text.length() && st >= 2) err = String("USSD ") + stTxt[st <= 5 ? st : 5];
  if (err.length()) j += ",\"err\":\"" + jsonEsc(err) + "\"";
  publishKuota(j + nowField());
}

void cancelUssd() {
  at("AT+CUSD=2", 5000);
  publishKuota("{\"kind\":\"ussd\",\"q\":\"\",\"st\":2,\"text\":\"Sesi USSD dibatalkan\"" + nowField());
}

// Banyak operator mengirim info kuota lewat SMS -> ambil SMS terbaru di kartu SIM
void readLastSms() {
  Serial.println("[KUOTA] membaca SMS terbaru");
  at("AT+CMGF=1"); at("AT+CSCS=\"IRA\""); at("AT+CSDH=0");
  if (at("AT+CMGL=\"ALL\"", 15000) != 1) {
    publishKuota("{\"kind\":\"sms\",\"err\":\"Gagal membaca SMS dari kartu SIM\"" + nowField());
    return;
  }
  // Format: +CMGL: <idx>,"<stat>","<pengirim>",,"<waktu>"\n<isi>\n...
  int best = -1, bestPos = -1, p = 0;
  while ((p = atResp.indexOf("+CMGL:", p)) >= 0) {
    int idx = atResp.substring(p + 6).toInt();
    if (idx >= best) { best = idx; bestPos = p; }
    p += 6;
  }
  if (bestPos < 0) { publishKuota("{\"kind\":\"sms\",\"text\":\"Tidak ada SMS di kartu SIM\"" + nowField()); return; }
  int eol = atResp.indexOf('\n', bestPos);
  String head = atResp.substring(bestPos, eol < 0 ? atResp.length() : eol);
  int nx = atResp.indexOf("+CMGL:", eol);
  String body = eol < 0 ? "" : atResp.substring(eol + 1, nx < 0 ? atResp.length() : nx);
  body.trim();
  String from, when;                                        // ambil field berkutip ke-2 dan ke-3/4
  int q[10], nq = 0;
  for (unsigned i = 0; i < head.length() && nq < 10; i++) if (head[i] == '"') q[nq++] = i;
  if (nq >= 4) from = maybeUcs2(head.substring(q[2] + 1, q[3]));
  if (nq >= 6) when = head.substring(q[nq - 2] + 1, q[nq - 1]);
  String text = maybeUcs2(body);
  Serial.printf("[KUOTA] SMS dari %s (%s): %s\n", from.c_str(), when.c_str(), text.c_str());
  publishKuota("{\"kind\":\"sms\",\"from\":\"" + jsonEsc(from) + "\",\"date\":\"" + jsonEsc(when) +
               "\",\"text\":\"" + jsonEsc(text.substring(0, 700)) + "\"" + nowField());
}

void handleCommand(String msg) {
  msg.trim(); msg.toLowerCase();
  Serial.printf("[MQTT] perintah: %s\n", msg.c_str());
  int eq = msg.indexOf('=');
  String key = eq >= 0 ? msg.substring(0, eq) : msg;
  String val = eq >= 0 ? msg.substring(eq + 1) : "";
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
  } else if (key == "kuota") {               // cek sisa paket dengan kode bawaan operator
    runUssd(kuotaCode());
  } else if (key == "ussd") {                // kode USSD lain, boleh urutan menu: ussd=*808#,7
    if (val == "batal") cancelUssd(); else runUssd(val);
  } else if (key == "sms") {                 // baca SMS terbaru (info kuota sering dikirim via SMS)
    readLastSms();
  }
  forcePublish = true;
}

bool mqttStart() {
  // Bersihkan sesi lama (mis. ESP32 restart sementara modem tetap menyala)
  at("AT+CMQTTDISC=0,60", 3000); waitFor("+CMQTTDISC:", 3000);
  at("AT+CMQTTREL=0", 2000);
  if (at("AT+CMQTTSTOP", 3000) == 1) waitFor("+CMQTTSTOP:", 5000);

  String l;
  at("AT+CMQTTSTART", 3000);
  if (!waitFor("+CMQTTSTART:", 8000, &l) || (lastInt(l) != 0 && lastInt(l) != 23)) {
    Serial.printf("[MQTT] layanan MQTT modem gagal dimulai (%s)\n", l.c_str());
    return false;
  }
  String cid = String("esp32-") + DEVICE_CODE + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (at("AT+CMQTTACCQ=0,\"" + cid + "\",0") != 1) { Serial.println("[MQTT] ACCQ gagal"); return false; }
  at("AT+CMQTTCFG=\"checkUTF8\",0,0");
  // LWT: broker mengirim "offline" bila modem putus tanpa pamit
  sendWithPrompt("AT+CMQTTWILLTOPIC=0," + String(T_STATUS.length()), T_STATUS);
  sendWithPrompt("AT+CMQTTWILLMSG=0,7,1", "offline");

  Serial.print("[MQTT] menghubungkan... ");
  if (at(String("AT+CMQTTCONNECT=0,\"tcp://") + MQTT_HOST + ":" + MQTT_PORT + "\",60,1", 5000) != 1 ||
      !waitFor("+CMQTTCONNECT: 0,", 30000, &l) || lastInt(l) != 0) {
    Serial.printf("gagal (%s)\n", l.c_str());
    return false;
  }
  Serial.println("OK");
  mqttLost = false;
  mqttPublish(T_STATUS, "online", true);
  if (!sendWithPrompt("AT+CMQTTSUBTOPIC=0," + String(T_SET.length()) + ",1", T_SET) ||
      at("AT+CMQTTSUB=0", 3000) != 1 || !waitFor("+CMQTTSUB: 0,", 10000, &l) || lastInt(l) != 0) {
    Serial.println("[MQTT] subscribe perintah gagal");
    return false;
  }
  publishConfig();
  publishHistory();
  if (kuotaPending.length()) publishKuota(kuotaPending);
  forcePublish = true;
  return true;
}

void modemHardReset() {
  Serial.println("[LTE] restart modem (AT+CRESET)");
  at("AT+CRESET", 3000);
  netUp = false; mqttUp = false;
  vTaskDelay(pdMS_TO_TICKS(20000));
}

void netTask(void*) {
  bool modemReady = false;
  int regFail = 0, dataFail = 0, mqttFail = 0, pubFail = 0;
  unsigned long tPub = 0, tHist = 0, tLink = 0;

  for (;;) {
    if (!modemReady) {
      netUp = false; mqttUp = false;
      modemReady = modemStart();
      if (!modemReady) { vTaskDelay(pdMS_TO_TICKS(10000)); continue; }
    }

    if (!mqttUp) {
      // Modem bisa saja baru restart (kembali ke mode angka) -> pastikan masih menjawab
      if (at("AT", 1000) != 1 && !modemAlive()) { modemReady = false; continue; }
      csq = readCsq();
      if (!registered()) {
        netUp = false;
        Serial.printf("[LTE] mencari jaringan... (CSQ %d)\n", (int)csq);
        if (++regFail >= 24) { modemHardReset(); modemReady = false; regFail = 0; }   // ±2 menit
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
      regFail = 0;
      String ip = dataIp();
      if (!ip.length()) {
        netUp = false;
        Serial.printf("[LTE] terdaftar di %s, mengaktifkan data (APN \"%s\")...\n", operatorName().c_str(), APN);
        if (!dataConnect()) {
          Serial.println("[LTE] data gagal aktif: cek APN, pulsa/kuota, dan IMEI");
          if (++dataFail >= 5) { modemHardReset(); modemReady = false; dataFail = 0; }
          vTaskDelay(pdMS_TO_TICKS(5000));
          continue;
        }
        ip = dataIp();
      }
      dataFail = 0;
      if (!netUp) Serial.printf("[LTE] data aktif, IP %s, sinyal CSQ %d\n", ip.c_str(), (int)csq);
      netUp = true;
      if (!timeSynced) timeSynced = syncTime();

      if (mqttStart()) { mqttUp = true; mqttFail = pubFail = 0; tLink = millis(); }
      else {
        if (++mqttFail >= 5) {                   // MQTT gagal terus -> aktifkan ulang data
          Serial.println("[LTE] aktifkan ulang koneksi data");
          at("AT+CGACT=0,1", 20000); mqttFail = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
      }
      continue;
    }

    // ---- Online ----
    pollModem();
    while (inboxN > 0) {
      String m = inbox[0];
      for (int i = 1; i < inboxN; i++) inbox[i - 1] = inbox[i];
      inboxN--;
      handleCommand(m);
    }
    if (mqttLost) { mqttUp = false; continue; }

    unsigned long now = millis();
    if (forcePublish || now - tPub >= PUBLISH_INTERVAL) {
      tPub = now; forcePublish = false;
      if (publishTelemetry()) pubFail = 0;
      else if (++pubFail >= 3) { Serial.println("[MQTT] kirim gagal 3x, sambung ulang"); mqttUp = false; continue; }
    }
    if (now - tHist >= HISTORY_INTERVAL) {
      tHist = now;
      publishHistory();
      mqttPublish(T_STATUS, "online", true);
    }
    if (now - tLink >= LINK_CHECK) {
      tLink = now;
      csq = readCsq();
      if (!registered()) { Serial.println("[LTE] jaringan hilang"); netUp = false; mqttUp = false; continue; }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ---------------- LCD (inti 1) ----------------
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
    lcd.print(netUp ? "LTE " : "LTE--");
    if (netUp) { lcd.print(csq); lcd.print(" "); }
    lcd.print(mqttUp ? "MQTT OK" : "MQTT --");
  }
}

// ---------------- Setup & Loop ----------------
void setup() {
  Serial.begin(115200);
  Wire.begin();

  pinMode(pinRelay, OUTPUT);
  digitalWrite(pinRelay, RELAY_OFF);
  if (MODEM_PWRKEY_PIN >= 0) {
    pinMode(MODEM_PWRKEY_PIN, OUTPUT);
    digitalWrite(MODEM_PWRKEY_PIN, PWRKEY_ACTIVE_HIGH ? LOW : HIGH);   // posisi lepas
  }

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
  T_KUOTA   = BASE + "/kuota";

  setenv("TZ", "WIB-7", 1);   // zona waktu lokal untuk reset durasi pompa harian
  tzset();

  lcd.setCursor(0, 0); lcd.print("  Smart System  ");
  lcd.setCursor(0, 1); lcd.print("Mode: 4G LTE    ");
  Serial.println("\n=== MODE KONEKSI: Seluler 4G (SIMCom A7670E, APN " + String(APN) + ") ===");

  SerialAT.setRxBufferSize(2048);
  SerialAT.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  // Modem & MQTT di inti 0, sensor & kontrol tetap di inti 1
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);

  delay(1500);
  lcd.clear();
  relayLastTick = millis();
  Serial.printf("Dashboard: https://syar02.github.io/Hidroponik/#%s\n", DEVICE_CODE);
}

void loop() {
  unsigned long now = millis();

  if (now - tSensor >= SENSOR_INTERVAL) {
    tSensor = now;
    readSensors();
    controlRelay();   // tetap jalan walau sinyal/MQTT putus
    Serial.printf("pH: %.2f | Suhu: %.1f C | Hum: %.1f %% | Lux: %.1f | Relay: %s (%s) | LTE:%s CSQ:%d MQTT:%s\n",
                  (float)phValue, (float)temp, (float)hum, (float)lux, relayOn ? "ON" : "OFF",
                  autoMode ? "auto" : "manual", netUp ? "ok" : "--", (int)csq, mqttUp ? "ok" : "--");
  }

  if (now - tLcd >= LCD_INTERVAL) {
    tLcd = now;
    displayPage = (displayPage + 1) % 3;
    updateLcd();
  }
  delay(5);
}
