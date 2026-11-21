/*
  ANENJI ANJ-4000W-24V-WIFI - monitor ESP32 + MAX485 (RS485)  [v3.2]
  - Konfiguracja z telefonu: ID, baud, rejestry, WiFi
  - Historia na LittleFS (wykres 24h z tooltipem, sumy dzienne, CSV)
  - OTA przez przeglądarkę + z GitHuba (version.txt + firmware.bin)
  - Piny: RO→GPIO16, DI→GPIO17, DE+RE→GPIO4
  Biblioteka: ModbusMaster
  Sieć awaryjna: ANENJI-SETUP / anenji1234 → http://192.168.4.1
  Partition Scheme: Default 4MB with spiffs (musi mieć OTA)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <HardwareSerial.h>
#include <ModbusMaster.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

const char* FW_VER = "2026.11.24.1";

// ====== Aktualizacja z GitHuba ======
// Domyślnie: repozytorium Dawid2711/anenji-monitor (można zmienić w Ustawieniach)
// W repo muszą być pliki: version.txt (jedna linia z wersją) i firmware.bin
String ghUser = "Dawid2711", ghRepo = "anenji-monitor";
bool  ghEnable = true;

String ghVerUrl() { return "https://raw.githubusercontent.com/" + ghUser + "/" + ghRepo + "/main/version.txt"; }
String ghBinUrl() { return "https://raw.githubusercontent.com/" + ghUser + "/" + ghRepo + "/main/firmware.bin"; }

bool ghNewVer = false;
String ghLatest = "";
uint32_t ghCheckAt = 0;
int ghCheckErr = 0;   // 0 = nie sprawdzano, >0 = błąd HTTP, -1 = brak odpowiedzi
String ghCheckMsg = "";
bool ghUpdating = false;
size_t ghBytes = 0;
bool ghCheckReq = false;
bool ghUpdateReq = false;

const char* AP_SSID = "ANENJI-SETUP";
const char* AP_PASS = "anenji1234";

#define RS485_RX  16
#define RS485_TX  17
#define MAX485_DE 4

HardwareSerial& invSerial = Serial2;
ModbusMaster node;
WebServer server(80);
Preferences prefs;

uint8_t  mbId     = 1;
uint32_t mbBaud   = 9600;
int8_t   batSign  = 1;
int16_t  addrOff  = 0;
float    bcOffset = 0.0f;

bool apActive = false;
uint32_t restartAt = 0;

float pv_power = 0, bat_soc = 0, load_power = 0, ac_voltage = 0, ac_freq = 0;
float out_voltage = 0, bat_voltage = 0, bat_current = 0, inv_temp = 0, op_mode = 0, pv1_voltage = 0;
float grid_raw = 0;
float bat_power = 0;
float grid_power = 0;
bool  modbusOk = false;

struct Sig {
  const char* key; const char* name;
  int defAddr; float defScale; bool defSigned;
  float* val;
  int addr; float scale; uint8_t type; bool sgn; uint8_t st;
};

enum { S_MODE, S_PV, S_LOAD, S_SOC, S_BV, S_BC, S_AC, S_FREQ, S_OUT, S_TEMP, S_PV1V, S_GRID, NUM_SIGS };

Sig sigs[NUM_SIGS] = {
  { "mode", "Tryb pracy",             201, 1.0,  false, &op_mode     },
  { "pv",   "Moc PV",                 223, 1.0,  false, &pv_power    },
  { "load", "Moc obciążenia",         213, 1.0,  false, &load_power  },
  { "soc",  "SOC baterii",            229, 1.0,  false, &bat_soc     },
  { "bv",   "Napięcie baterii",       215, 0.1,  false, &bat_voltage },
  { "bc",   "Prąd baterii (232)",     232, 0.1,  true,  &bat_current },
  { "ac",   "Napięcie sieci AC",      202, 0.1,  false, &ac_voltage  },
  { "freq", "Częstotliwość sieci",    203, 0.01, false, &ac_freq     },
  { "out",  "Napięcie wyjściowe",     210, 0.1,  false, &out_voltage },
  { "temp", "Temperatura falownika",  227, 1.0,  true,  &inv_temp    },
  { "pv1v", "Napięcie PV",            219, 0.1,  false, &pv1_voltage },
  { "grid", "Moc z sieci (204)",      204, 1.0,  true,  &grid_raw    },
};

void loadCfg() {
  mbId     = prefs.getUChar("id", 1);
  mbBaud   = prefs.getUInt("baud", 9600);
  batSign  = prefs.getChar("bs", 1);
  addrOff  = prefs.getShort("off", 0);
  bcOffset = prefs.getFloat("bco", 0.0f);
  ghUser   = prefs.getString("ghu", "Dawid2711");
  ghRepo   = prefs.getString("ghr", "anenji-monitor");
  ghEnable = prefs.getBool("ghe", true);
  for (int i = 0; i < NUM_SIGS; i++) {
    char k[8];
    snprintf(k, sizeof(k), "a%d", i); sigs[i].addr  = prefs.getInt(k, sigs[i].defAddr);
    snprintf(k, sizeof(k), "s%d", i); sigs[i].scale = prefs.getFloat(k, sigs[i].defScale);
    snprintf(k, sizeof(k), "t%d", i); sigs[i].type  = prefs.getUChar(k, 0);
    snprintf(k, sizeof(k), "g%d", i); sigs[i].sgn   = prefs.getBool(k, sigs[i].defSigned);
    sigs[i].st = 2;
  }
}

void saveCfg() {
  prefs.putUChar("id", mbId);
  prefs.putUInt("baud", mbBaud);
  prefs.putChar("bs", batSign);
  prefs.putShort("off", addrOff);
  prefs.putFloat("bco", bcOffset);
  prefs.putString("ghu", ghUser);
  prefs.putString("ghr", ghRepo);
  prefs.putBool("ghe", ghEnable);
  for (int i = 0; i < NUM_SIGS; i++) {
    char k[8];
    snprintf(k, sizeof(k), "a%d", i); prefs.putInt(k, sigs[i].addr);
    snprintf(k, sizeof(k), "s%d", i); prefs.putFloat(k, sigs[i].scale);
    snprintf(k, sizeof(k), "t%d", i); prefs.putUChar(k, sigs[i].type);
    snprintf(k, sizeof(k), "g%d", i); prefs.putBool(k, sigs[i].sgn);
  }
}

void preTransmission()  { digitalWrite(MAX485_DE, HIGH); }
void postTransmission() { digitalWrite(MAX485_DE, LOW); }

void nodeInit(uint8_t id) {
  node.begin(id, invSerial);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
}

void applyModbus() {
  invSerial.end();
  invSerial.begin(mbBaud, SERIAL_8N1, RS485_RX, RS485_TX);
  nodeInit(mbId);
}

// ====== Aktualizacja z GitHuba ======
static void ghDone(bool ok, const String& msg) {
  ghUpdating = false;
  ghCheckErr = ok ? 0 : -1;
  ghCheckMsg = msg;
}

static bool ghNameOk(const String& s) {
  if (s.length() < 1 || s.length() > 64) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
  }
  return true;
}

static void ghCheckStart() {
  Serial.printf("[GH] check: en=%d user='%s' repo='%s' wifi=%d heap=%u\n",
                ghEnable, ghUser.c_str(), ghRepo.c_str(), WiFi.status() == WL_CONNECTED, ESP.getFreeHeap());
  if (!ghEnable || ghUser.length() == 0 || ghRepo.length() == 0 || !ghNameOk(ghUser) || !ghNameOk(ghRepo)) {
    ghCheckErr = -1;
    ghCheckMsg = "Wpisz uzytkownika i repo oraz zaznacz 'wlacz'";
    return;
  }
  if (ghUpdating) return;
  if (WiFi.status() != WL_CONNECTED) { ghCheckErr = -1; ghCheckMsg = "Brak polaczenia WiFi"; return; }
  ghCheckErr = 0; ghCheckMsg = "Sprawdzam wersje...";
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);
  HTTPClient http;
  http.begin(client, ghVerUrl());
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String v = http.getString();
    v.trim();
    v.replace("\r", ""); v.replace("\n", "");
    if (v.length() == 0) { ghDone(false, "Plik version.txt pusty"); http.end(); return; }
    ghLatest = v;
    ghNewVer = (v != String(FW_VER));
    ghDone(true, ghNewVer ? "Dostepna: " + v : "Jestes na najnowszej: " + v);
  } else {
    ghDone(false, "HTTP " + String(code));
  }
  http.end();
}

static void ghUpdateStart() {
  if (!ghEnable || ghUser.length() == 0 || ghRepo.length() == 0 || !ghNameOk(ghUser) || !ghNameOk(ghRepo)) {
    ghCheckErr = -1;
    ghCheckMsg = "Wpisz uzytkownika i repo oraz zaznacz 'wlacz'";
    return;
  }
  if (ghUpdating) return;
  if (WiFi.status() != WL_CONNECTED) { ghCheckErr = -1; ghCheckMsg = "Brak polaczenia WiFi"; return; }
  if (ghLatest.length() == 0) { ghCheckErr = -1; ghCheckMsg = "Najpierw sprawdz wersje"; return; }

  ghUpdating = true; ghBytes = 0; ghCheckErr = 0;
  ghCheckMsg = "Pobieranie firmware.bin...";
  saveState(); saveHist();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(60000);
  HTTPClient http;
  http.setTimeout(60000);
  if (!http.begin(client, ghBinUrl())) { ghDone(false, "Blad polaczenia"); return; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); ghDone(false, "HTTP " + String(code)); return; }

  int len = http.getSize();   // -1 jesli serwer nie podal rozmiaru
  if (len > 0 && len < 100000) {
    http.end();
    ghDone(false, "Plik za maly (" + String(len) + " B) - to nie jest firmware");
    return;
  }
  if (!Update.begin(len > 0 ? (size_t)len : (size_t)UPDATE_SIZE_UNKNOWN)) {
    http.end();
    ghDone(false, String("Update.begin: ") + Update.errorString());
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  static uint8_t buf[2048];
  size_t total = 0;
  uint32_t lastData = millis();
  while (http.connected() && (len < 0 || (int)total < len)) {
    size_t avail = stream->available();
    if (!avail) {
      if (millis() - lastData > 20000) break;   // 20 s bez danych
      delay(10);
      continue;
    }
    int n = stream->readBytes(buf, min(avail, sizeof(buf)));
    if (n <= 0) continue;
    lastData = millis();
    if (total == 0 && buf[0] != 0xE9) {
      Update.abort(); http.end();
      ghDone(false, "To nie jest plik firmware (.ino.bin)");
      return;
    }
    if (Update.write(buf, n) != (size_t)n) {
      Update.abort(); http.end();
      ghDone(false, "Blad zapisu");
      return;
    }
    total += n; ghBytes = total;
    ghCheckMsg = "Pobieranie... " + String(total / 1024) + " KB";
    yield();
  }
  http.end();

  if (total == 0) { Update.abort(); ghDone(false, "Nie pobrano zadnych danych"); return; }
  if (len > 0 && (int)total != len) {
    Update.abort();
    ghDone(false, "Pobrano " + String(total) + " z " + String(len) + " B");
    return;
  }
  if (Update.end(true)) {
    ghDone(true, "Wgrano z GitHuba. Restart...");
    restartAt = millis() + 1500;
  } else {
    ghDone(false, String("Blad: ") + Update.errorString());
  }
}

// ====== Statystyki i historia ======
double pv_wh = 0, load_wh = 0, grid_wh = 0, pv_bat_wh = 0;
float daysKwh[7] = {0};
int curDay = 0;
uint32_t curDate = 0;

#define HIST_N 288
uint16_t hist_pv[HIST_N], hist_ld[HIST_N], hist_gr[HIST_N];
int histIdx = 0, histCount = 0;
double accPv = 0, accLd = 0, accGr = 0; int accN = 0;
bool histPending = false;
uint32_t histSavedEpoch = 0;

// 1 godzina x 720 = 30 dni
#define HIST_H_N 720
uint16_t hist_h_pv[HIST_H_N], hist_h_ld[HIST_H_N], hist_h_gr[HIST_H_N];
int hist_h_idx = 0, hist_h_cnt = 0;
double acc_h_pv = 0, acc_h_ld = 0, acc_h_gr = 0; int acc_h_n = 0;
uint32_t hist_h_saved = 0;

// 30 sekund x 120 = 1 godzina (tylko RAM)
#define HIST_S_N 120
uint16_t hist_s_pv[HIST_S_N], hist_s_ld[HIST_S_N], hist_s_gr[HIST_S_N];
int hist_s_idx = 0, hist_s_cnt = 0;
double accS_pv = 0, accS_ld = 0, accS_gr = 0; int accS_n = 0;

unsigned long lastRead = 0, lastHist = 0, lastShort = 0, lastSave = 0, lastHistSave = 0;

const char* DAYS_FILE = "/days.bin";
const int   DAYS_MAX  = 1000;
struct DayRec { uint32_t date; float pv, load, grid; };

struct Saved { uint32_t magic; int32_t day; uint32_t date; float pv, load, grid, pv_bat; float days[7]; };

struct HistFile {
  uint32_t magic; uint32_t epoch;
  uint16_t idx; uint16_t count;
  uint16_t pv[HIST_N]; uint16_t ld[HIST_N]; uint16_t gr[HIST_N];
};

void saveState() {
  File f = LittleFS.open("/state.bin", "w");
  if (!f) return;
  Saved s; s.magic = 0xA11E1106; s.day = curDay; s.date = curDate;
  s.pv = pv_wh; s.load = load_wh; s.grid = grid_wh; s.pv_bat = pv_bat_wh;
  for (int i = 0; i < 7; i++) s.days[i] = daysKwh[i];
  f.write((uint8_t*)&s, sizeof(s));
  f.close();
}

void loadState() {
  File f = LittleFS.open("/state.bin", "r");
  if (!f) return;
  Saved s;
  if (f.read((uint8_t*)&s, sizeof(s)) == sizeof(s) && s.magic == 0xA11E1106) {
    curDay = s.day; curDate = s.date; pv_wh = s.pv; load_wh = s.load; grid_wh = s.grid; pv_bat_wh = s.pv_bat;
    for (int i = 0; i < 7; i++) daysKwh[i] = s.days[i];
  }
  f.close();
}

void pushHist(uint16_t pv, uint16_t ld, uint16_t gr) {
  hist_pv[histIdx] = pv;
  hist_ld[histIdx] = ld;
  hist_gr[histIdx] = gr;
  histIdx = (histIdx + 1) % HIST_N;
  if (histCount < HIST_N) histCount++;
}

void pushHistHourly(uint16_t pv, uint16_t ld, uint16_t gr) {
  hist_h_pv[hist_h_idx] = pv;
  hist_h_ld[hist_h_idx] = ld;
  hist_h_gr[hist_h_idx] = gr;
  hist_h_idx = (hist_h_idx + 1) % HIST_H_N;
  if (hist_h_cnt < HIST_H_N) hist_h_cnt++;
}

void pushHistShort(uint16_t pv, uint16_t ld, uint16_t gr) {
  hist_s_pv[hist_s_idx] = pv;
  hist_s_ld[hist_s_idx] = ld;
  hist_s_gr[hist_s_idx] = gr;
  hist_s_idx = (hist_s_idx + 1) % HIST_S_N;
  if (hist_s_cnt < HIST_S_N) hist_s_cnt++;
}

void saveHist() {
  static HistFile h;
  time_t n = time(nullptr);
  h.magic = 0x48495354;
  h.epoch = (n > 1700000000) ? (uint32_t)n : 0;
  h.idx = histIdx; h.count = histCount;
  memcpy(h.pv, hist_pv, sizeof(hist_pv));
  memcpy(h.ld, hist_ld, sizeof(hist_ld));
  memcpy(h.gr, hist_gr, sizeof(hist_gr));
  File f = LittleFS.open("/hist.bin", "w");
  if (f) { f.write((uint8_t*)&h, sizeof(h)); f.close(); }

  static struct HistFileH {
    uint32_t magic; uint32_t epoch;
    uint16_t idx; uint16_t count;
    uint16_t pv[HIST_H_N]; uint16_t ld[HIST_H_N]; uint16_t gr[HIST_H_N];
  } hh;
  hh.magic = 0x48495349;
  hh.epoch = h.epoch;
  hh.idx = hist_h_idx; hh.count = hist_h_cnt;
  memcpy(hh.pv, hist_h_pv, sizeof(hh.pv));
  memcpy(hh.ld, hist_h_ld, sizeof(hh.ld));
  memcpy(hh.gr, hist_h_gr, sizeof(hh.gr));
  f = LittleFS.open("/histh2.bin", "w");
  if (f) { f.write((uint8_t*)&hh, sizeof(hh)); f.close(); }
}

void loadHist() {
  LittleFS.remove("/histh.bin");   // stary, uszkodzony format
  static HistFile h;
  File f = LittleFS.open("/hist.bin", "r");
  if (f) {
    if (f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && h.magic == 0x48495354) {
      memcpy(hist_pv, h.pv, sizeof(hist_pv));
      memcpy(hist_ld, h.ld, sizeof(hist_ld));
      memcpy(hist_gr, h.gr, sizeof(hist_gr));
      histIdx = h.idx % HIST_N;
      histCount = min((int)h.count, HIST_N);
      histSavedEpoch = h.epoch;
      histPending = (h.epoch > 1700000000);
    }
    f.close();
  }

  static struct HistFileH {
    uint32_t magic; uint32_t epoch;
    uint16_t idx; uint16_t count;
    uint16_t pv[HIST_H_N]; uint16_t ld[HIST_H_N]; uint16_t gr[HIST_H_N];
  } hh;
  f = LittleFS.open("/histh2.bin", "r");
  if (f) {
    if (f.read((uint8_t*)&hh, sizeof(hh)) == sizeof(hh) && hh.magic == 0x48495349) {
      memcpy(hist_h_pv, hh.pv, sizeof(hist_h_pv));
      memcpy(hist_h_ld, hh.ld, sizeof(hist_h_ld));
      memcpy(hist_h_gr, hh.gr, sizeof(hist_h_gr));
      hist_h_idx = hh.idx % HIST_H_N;
      hist_h_cnt = min((int)hh.count, HIST_H_N);
      hist_h_saved = hh.epoch;
    }
    f.close();
  }
}

void fixHistGap(time_t now) {
  histPending = false;
  if (histSavedEpoch < 1700000000 || now < (time_t)histSavedEpoch) return;
  uint32_t gap = (now - histSavedEpoch) / 300;
  if (gap >= HIST_N) { histCount = 0; return; }
  for (uint32_t i = 0; i < gap; i++) pushHist(0, 0, 0);

  if (hist_h_saved < 1700000000 || now < (time_t)hist_h_saved) return;
  uint32_t hgap = (now - hist_h_saved) / 3600;
  if (hgap >= HIST_H_N) { hist_h_cnt = 0; return; }
  for (uint32_t i = 0; i < hgap; i++) pushHistHourly(0, 0, 0);
}

void trimDays() {
  File f = LittleFS.open(DAYS_FILE, "r");
  if (!f) return;
  size_t n = f.size() / sizeof(DayRec);
  size_t keep = DAYS_MAX - 100;
  if (n <= keep) { f.close(); return; }
  DayRec* buf = (DayRec*)malloc(keep * sizeof(DayRec));
  if (!buf) { f.close(); return; }
  f.seek((n - keep) * sizeof(DayRec));
  size_t got = f.read((uint8_t*)buf, keep * sizeof(DayRec)) / sizeof(DayRec);
  f.close();
  File w = LittleFS.open(DAYS_FILE, "w");
  if (w) { w.write((uint8_t*)buf, got * sizeof(DayRec)); w.close(); }
  free(buf);
}

void appendDay(uint32_t date, float pv, float ld, float gd) {
  if (date == 0) return;
  DayRec r = { date, pv, ld, gd };
  File f = LittleFS.open(DAYS_FILE, "a");
  if (!f) return;
  f.write((uint8_t*)&r, sizeof(r));
  size_t sz = f.size();
  f.close();
  if (sz / sizeof(DayRec) > (size_t)DAYS_MAX) trimDays();
}

void rollDay() {
  appendDay(curDate, pv_wh / 1000.0, load_wh / 1000.0, grid_wh / 1000.0);
  for (int i = 6; i > 0; i--) daysKwh[i] = daysKwh[i - 1];
  daysKwh[0] = pv_wh / 1000.0;
  pv_wh = load_wh = grid_wh = 0;
}

void checkDay() {
  time_t n = time(nullptr);
  if (n < 1700000000) return;
  struct tm t; localtime_r(&n, &t);
  int key = t.tm_year * 1000 + t.tm_yday;
  uint32_t date = (uint32_t)(t.tm_year + 1900) * 10000UL + (uint32_t)(t.tm_mon + 1) * 100UL + t.tm_mday;
  if (curDay == 0) { curDay = key; curDate = date; saveState(); }
  else if (key != curDay) { rollDay(); curDay = key; curDate = date; saveState(); }
  if (histPending) fixHistGap(n);
}

// ====== Strona WWW ======
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="pl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Anenji Monitor</title>
<style>
:root{
  --card:#14263a;--mut:#8aa0b8;
  --bg:#0b1220;--fg:#e8eef6;--border:#2b4a6b;--input:#0b1220;--header:#10213a;
}
[data-theme="light"]{
  --card:#ffffff;--mut:#475569;
  --bg:#f1f5f9;--fg:#0f172a;--border:#cbd5e1;--input:#ffffff;--header:#e2e8f0;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font-family:system-ui,sans-serif;padding:14px;transition:background .3s,color .3s}
.wrap{max-width:560px;margin:0 auto}
.wrap{max-width:560px;margin:0 auto}
header{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;gap:8px}
.hr{display:flex;align-items:center;gap:6px}
h1{font-size:14px;margin:0;letter-spacing:.5px}
.badge{font-size:10px;padding:3px 9px;border-radius:99px;background:#14532d;color:#4ade80;white-space:nowrap}
.badge.off{background:#7f1d1d;color:#fca5a5}
.badge.warn{background:#78350f;color:#fcd34d}
.mbtn{font-size:10px;font-weight:700;padding:3px 10px;border-radius:99px;border:1px solid #2b4a6b;background:#10213a;color:#93c5fd;cursor:pointer}
.mbtn.demo{background:#78350f;border-color:#b45309;color:#fcd34d}
nav{display:flex;gap:6px;margin-bottom:14px}
.tab{flex:1;padding:8px;border-radius:10px;border:1px solid #223449;background:#10213a;color:#8aa0b8;font-size:13px;font-weight:600;cursor:pointer}
.tab.on{background:#1d4ed8;border-color:#2563eb;color:#fff}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.grid3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}
.card{background:var(--card);border-radius:12px;padding:12px}
.card small{color:var(--mut);font-size:11px;display:block;margin-bottom:6px}
.card b{font-size:26px}.card span{font-size:13px;color:var(--mut);margin-left:3px}
.grid3 .card b{font-size:18px}
h3{font-size:13px;margin:20px 0 8px;color:var(--mut);font-weight:600}
.box{background:var(--card);border-radius:12px;padding:8px;position:relative}
svg{width:100%;height:auto;display:block}
.flow{display:block;border-radius:14px}
.ln{stroke-width:3;stroke-dasharray:5 7;stroke-linecap:round;opacity:.2;fill:none}
.ln.f{opacity:1;animation:f .8s linear infinite}
.ln.r{opacity:1;animation:r .8s linear infinite}
@keyframes f{to{stroke-dashoffset:-12}}
@keyframes r{to{stroke-dashoffset:12}}
.t{font-family:system-ui,sans-serif;text-anchor:middle}
.foot{text-align:center;color:var(--mut);font-size:11px;margin:16px 0 4px}
.sg{background:var(--card);border-radius:12px;padding:10px 12px;margin-bottom:8px}
.sh{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}
.sh b{font-size:13px}.sval{font-size:13px;color:#fbbf24;font-weight:700}
.sr{display:flex;flex-wrap:wrap;gap:8px;align-items:flex-end}
.sr label{font-size:10px;color:var(--mut);display:flex;flex-direction:column;gap:3px}
.sr label.cb{flex-direction:row;align-items:center;gap:4px;padding-bottom:7px}
input[type=number],input[type=text],input[type=password],select{background:#0b1220;border:1px solid #2b4a6b;color:#e8eef6;border-radius:8px;padding:6px 8px;font-size:13px;width:90px}
select{width:100px}
.g input,.g select{width:100%}
.mini{background:#10213a;border:1px solid #2b4a6b;color:#93c5fd;border-radius:8px;padding:6px 12px;font-size:12px;cursor:pointer}
.tst{font-size:11px;color:var(--mut);padding-bottom:7px}
.btns{display:flex;gap:8px;margin:14px 0 6px}
.btn{flex:1;padding:11px;border-radius:10px;border:0;font-size:14px;font-weight:700;cursor:pointer}
.btn.sv{background:#16a34a;color:#fff}.btn.df{background:#374151;color:#e5e7eb}.btn.wr{background:#b45309;color:#fff}
.note{font-size:11px;color:var(--mut);line-height:1.5;margin:8px 2px}
#msg{text-align:center;font-size:12px;min-height:16px}
table.ht{width:100%;border-collapse:collapse;font-size:12px}
table.ht td,table.ht th{padding:5px 4px;text-align:right;border-top:1px solid #223449}
table.ht th{color:#8aa0b8;font-weight:600;border-top:0}
table.ht td:first-child,table.ht th:first-child{text-align:left}
#tooltip{position:absolute;background:#1e293b;border:1px solid #475569;border-radius:8px;padding:8px 12px;font-size:12px;pointer-events:none;opacity:0;transition:opacity .15s;z-index:10;white-space:nowrap;box-shadow:0 4px 12px rgba(0,0,0,.4)}
#tooltip.show{opacity:1}
</style></head><body><div class="wrap">
<header><h1>ANENJI SOLAR MONITOR</h1>
 <div class="hr"><button class="mbtn" id="mbtn" onclick="toggleMode()">LIVE</button><div class="badge" id="badge">ŁĄCZENIE…</div><button class="mbtn" id="tbtn" onclick="toggleTheme()" title="Motyw">🌙</button></div></header>
<nav><button class="tab on" id="tb1" onclick="showTab(1)">Pulpit</button><button class="tab" id="tb3" onclick="showTab(3)">Historia</button><button class="tab" id="tb2" onclick="showTab(2)">Ustawienia</button></nav>

<div id="tabDash">
<div class="grid">
 <div class="card"><small>☀ Produkcja PV</small><b id="cpv">0.00</b><span>kW</span></div>
 <div class="card"><small>🔋 Akumulator</small><b id="csoc">--</b><span>%</span></div>
 <div class="card"><small>🏠 Obciążenie</small><b id="cld">0</b><span>W</span></div>
 <div class="card"><small>⚡ Sieć</small><b id="cgr">0</b><span>W</span></div>
</div>

<h3>Przepływ energii</h3>
<svg class="flow" viewBox="0 0 360 290" style="background:#000">
 <defs><filter id="gl" filterUnits="userSpaceOnUse" x="0" y="0" width="360" height="290">
  <feGaussianBlur stdDeviation="1.6" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter></defs>
 <g stroke="#fde047" stroke-width="1.6" stroke-linecap="round" filter="url(#gl)">
  <circle cx="112" cy="22" r="6" fill="#fde047"/>
  <path d="M112 10v3M112 31v3M100 22h3M121 22h3M103.5 13.5l2 2M118.5 28.5l2 2M103.5 30.5l2-2M118.5 15.5l2-2"/>
 </g>
 <text id="nsol" x="128" y="27" fill="#fde047" font-size="16" font-weight="700" font-family="system-ui">0.0 kWh</text>
 <text x="128" y="37" fill="#9ca3af" font-size="6.5" font-family="system-ui">DZIŚ Z PV</text>
 <line id="lpv"  class="ln" x1="180" y1="90"  x2="180" y2="140" stroke="#fde047"/>
 <line id="lgr"  class="ln" x1="130" y1="170" x2="155" y2="170" stroke="#ec4899"/>
 <line id="lld"  class="ln" x1="205" y1="170" x2="230" y2="170" stroke="#60a5fa"/>
 <line id="lbat" class="ln" x1="180" y1="200" x2="180" y2="232" stroke="#4ade80"/>
 <rect x="130" y="46" width="100" height="44" rx="9" fill="#000" stroke="#fde047" stroke-width="1.5" filter="url(#gl)"/>
 <text class="t" x="180" y="58" fill="#9ca3af" font-size="7.5">PV</text>
 <text class="t" id="npv" x="180" y="74" fill="#fff" font-size="15" font-weight="700">0 W</text>
 <text class="t" id="npv1" x="180" y="85" fill="#fde047" font-size="7.5">-- V</text>
 <rect x="155" y="140" width="50" height="60" rx="8" fill="#f59e0b" filter="url(#gl)"/>
 <rect x="162" y="147" width="36" height="46" rx="5" fill="none" stroke="#000" stroke-width="2"/>
 <path d="M172 165a10 10 0 0 1 16-3M188 175a10 10 0 0 1-16 3" stroke="#000" fill="none" stroke-width="2.5" stroke-linecap="round"/>
 <path d="M190 157l-1 7-7-2z M170 183l1-7 7 2z" fill="#000"/>
 <g stroke="#ec4899" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" filter="url(#gl)">
  <path d="M42 140L32 192M42 140L52 192M34 176h16M37 160h10M26 152h32M31 166h22M32 192h-6M52 192h6"/>
 </g>
 <text class="t" id="ngd" x="42" y="122" fill="#ec4899" font-size="15" font-weight="700">0.0</text>
 <text class="t" x="42" y="131" fill="#9ca3af" font-size="6">DZIŚ Z SIECI</text>
 <rect x="68" y="152" width="62" height="36" rx="8" fill="#000" stroke="#ec4899" stroke-width="1.5" filter="url(#gl)"/>
 <text class="t" id="ngr" x="99" y="175" fill="#fff" font-size="13" font-weight="700">0 W</text>
 <rect x="230" y="152" width="62" height="36" rx="8" fill="#000" stroke="#60a5fa" stroke-width="1.5" filter="url(#gl)"/>
 <text class="t" id="nld" x="261" y="175" fill="#fff" font-size="13" font-weight="700">0 W</text>
 <g stroke="#4ade80" stroke-width="2.5" fill="none" stroke-linecap="round" stroke-linejoin="round" filter="url(#gl)">
  <path d="M298 170h8M306 170l-4-4M306 170l-4 4"/>
  <path d="M312 172l18-17 18 17M317 168v20h26v-20"/>
 </g>
 <text class="t" id="nhome" x="330" y="130" fill="#60a5fa" font-size="14" font-weight="700">0.0 kWh</text>
 <text class="t" x="330" y="139" fill="#9ca3af" font-size="6">DOM DZIŚ</text>
 <rect x="125" y="232" width="110" height="50" rx="9" fill="#000" stroke="#4ade80" stroke-width="1.5" filter="url(#gl)"/>
 <rect x="135" y="244" width="14" height="26" rx="2" fill="none" stroke="#4ade80" stroke-width="1.6"/>
 <rect x="139" y="241" width="6" height="3" fill="#4ade80"/>
 <rect id="bfill" x="137.5" y="268" width="9" height="0" fill="#4ade80"/>
 <text class="t" id="nbat" x="195" y="250" fill="#fff" font-size="14" font-weight="700">--%</text>
 <text class="t" id="nbw"  x="195" y="263" fill="#4ade80" font-size="9" font-weight="600">0 W</text>
 <text class="t" id="nbv"  x="195" y="274" fill="#9ca3af" font-size="7.5">-- V · -- A</text>
 <text class="t" id="nau" x="35" y="236" fill="#f59e0b" font-size="14" font-weight="700">0%</text>
 <text class="t" x="35" y="245" fill="#9ca3af" font-size="6.5">Autarkia</text>
 <text class="t" id="nra" x="85" y="236" fill="#f59e0b" font-size="14" font-weight="700">0%</text>
 <text class="t" x="85" y="245" fill="#9ca3af" font-size="6.5">PV/obciąż.</text>
 <text id="nac"   x="244" y="236" fill="#fb923c" font-size="8">-- V · -- Hz</text>
 <text id="ntemp" x="244" y="248" fill="#fb923c" font-size="8">Falownik -- °C</text>
 <text id="nmode" x="244" y="260" fill="#fff" font-size="8" font-weight="700">--</text>
</svg>

<h3>Statystyki dzienne</h3>
<div class="grid">
 <div class="card"><small>Produkcja dziś</small><b id="spv">0.0</b><span>kWh</span></div>
 <div class="card"><small>Zużycie dziś</small><b id="sld">0.0</b><span>kWh</span></div>
 <div class="card"><small>Pobór z sieci</small><b id="sgr">0.0</b><span>kWh</span></div>
 <div class="card"><small>PV → Bateria</small><b id="spvbat">0.0</b><span>kWh</span></div>
 <div class="card"><small>Autarkia</small><b id="sau">0</b><span>%</span></div>
</div>

<h3>Szczegóły falownika</h3>
<div class="grid3">
 <div class="card"><small>Bateria</small><b id="dbv">--</b><span>V</span></div>
 <div class="card"><small>Prąd bat.</small><b id="dbc">--</b><span>A</span></div>
 <div class="card"><small>Temp.</small><b id="dt">--</b><span>°C</span></div>
 <div class="card"><small>Sieć AC</small><b id="dac">--</b><span>V</span></div>
 <div class="card"><small>Częst.</small><b id="dfr">--</b><span>Hz</span></div>
 <div class="card"><small>Wyjście</small><b id="dout">--</b><span>V</span></div>
</div>

<h3>Historia produkcji – 24 h</h3>
<div class="box" id="histboxD">
 <svg id="histD" viewBox="0 0 560 190"></svg>
 <div id="tooltipD"></div>
</div>
<div style="font-size:11px;color:var(--mut);margin:6px 4px">
 <span style="color:#fbbf24">■</span> PV &nbsp;
 <span style="color:#38bdf8">■</span> Obciążenie &nbsp;
 <span style="color:#ec4899">■</span> Pobór z sieci &nbsp; (oś: kW)
</div>

<h3>Produkcja – ostatnie 7 dni</h3>
<div class="box"><svg id="days" viewBox="0 0 560 160"></svg></div>
<div class="foot" id="foot"></div>
</div>

<div id="tabHis" style="display:none">
 <h3 style="margin-top:4px">Historia produkcji PV</h3>
 <div class="sg" style="margin-bottom:10px">
  <div class="sr g" style="display:grid;grid-template-columns:1fr 1fr auto;gap:8px;align-items:center">
   <label>Zakres<select id="hrange" onchange="histLoad()"><option value="15m">15 minut</option><option value="1h">1 godzina</option><option value="6h">6 godzin</option><option value="24h" selected>24 godziny</option><option value="7d">7 dni</option><option value="30d">30 dni</option><option value="90d">90 dni</option><option value="1y">1 rok</option></select></label>
   <label>Widok<select id="hmode" onchange="histLoad()"><option value="all">Wszystkie (nałożone)</option><option value="pv">Tylko PV</option><option value="load">Tylko Obciążenie</option><option value="grid">Tylko Sieć</option></select></label>
   <span class="tst" id="hrng"></span>
  </div>
 </div>
 <div class="box" id="histboxHis">
  <svg id="histHis" viewBox="0 0 560 190"></svg>
  <div id="tooltipHis"></div>
 </div>
 <div style="font-size:11px;color:var(--mut);margin:6px 4px">
  <span style="color:#fbbf24">■</span> PV &nbsp;
  <span style="color:#38bdf8">■</span> Obciążenie &nbsp;
  <span style="color:#ec4899">■</span> Pobór z sieci &nbsp; (oś: kW)
 </div>

 <h3>Produkcja – ostatnie 7 dni</h3>
 <div class="box"><svg id="days" viewBox="0 0 560 160"></svg></div>

 <h3>Ostatnie 30 dni – produkcja PV [kWh]</h3>
 <div class="box"><svg id="h30" viewBox="0 0 560 170"></svg></div>
 <h3>Miesiące</h3>
 <div class="box" id="hmon" style="padding:10px"></div>
 <h3>Dni (najnowsze na górze)</h3>
 <div class="box" id="hday" style="padding:10px;max-height:340px;overflow:auto"></div>
 <div class="btns"><button class="btn df" onclick="location.href='/days.csv'">Pobierz CSV</button><button class="btn wr" onclick="hisClear()">Wyczyść historię</button></div>
 <div id="hmsg" class="tst"></div>
 <div class="note">Sumy dzienne zapisują się o północy (do ok. 1000 dni). Potrzebny jest czas z NTP (WiFi z internetem).</div>
</div>

<div id="tabSet" style="display:none">
 <h3 style="margin-top:4px">WiFi</h3>
 <div class="sg">
  <div class="tst" id="wst">--</div>
  <div class="sr g" style="display:grid;grid-template-columns:1fr;gap:8px">
   <label>Wykryte sieci<select id="wsel" onchange="wpick()"><option value="">-- naciśnij Skanuj --</option></select></label>
   <label>Nazwa sieci (SSID)<input type="text" id="wssid" autocapitalize="none" autocomplete="off"></label>
   <label>Hasło<input type="password" id="wpass" autocomplete="off"></label>
  </div>
  <div class="btns"><button class="btn df" onclick="wscan()">Skanuj</button><button class="btn wr" onclick="wsave()">Zapisz WiFi i restart</button></div>
  <div class="tst" id="wmsg"></div>
 </div>

 <h3>Modbus / RS485</h3>
 <div class="sg"><div class="sr g" style="display:grid;grid-template-columns:1fr 1fr;gap:8px">
  <label>ID urządzenia<input type="number" id="gid" min="1" max="247"></label>
  <label>Prędkość (baud)<select id="gbaud"><option>1200</option><option>2400</option><option>4800</option><option>9600</option><option>19200</option><option>38400</option><option>57600</option><option>115200</option></select></label>
  <label>Przesunięcie adresów<select id="goff"><option value="-1">-1</option><option value="0">0 (bez)</option><option value="1">+1</option></select></label>
  <label>Kierunek prądu baterii<select id="gbs"><option value="1">normalny</option><option value="-1">odwrócony</option></select></label>
  <label>Korekta prądu baterii [A]<input type="number" step="0.1" id="gbco"></label>
 </div>
 <div class="btns"><button class="btn df" onclick="scanId()">Szukaj ID falownika</button></div>
 <div class="tst" id="idmsg"></div>
 </div>

 <h3>Skaner rejestrów</h3>
 <div class="sg"><div class="sr">
  <label>Od adresu<input type="number" id="rf" value="200"></label>
  <label>Ile (max 60)<input type="number" id="rn" value="40"></label>
  <button class="mini" onclick="rscan()">Skanuj</button>
 </div><div id="rout" style="margin-top:8px"></div></div>

 <h3>Rejestry sygnałów</h3>
 <div id="sigs"></div>
 <div class="btns"><button class="btn sv" onclick="saveCfg()">Zapisz</button><button class="btn df" onclick="resetCfg()">Przywróć domyślne</button></div>
 <div id="msg"></div>
 <div class="btns"><button class="btn df" onclick="doRestart()">Restart ESP32</button></div>

 <h3>Aktualizacja z GitHuba</h3>
 <div class="sg">
  <div class="tst" id="ghst">--</div>
  <div class="sr g" style="display:grid;grid-template-columns:1fr 1fr;gap:8px">
   <label>Użytkownik GitHub<input type="text" id="ghu" autocapitalize="none" autocomplete="off" placeholder="Dawid2711"></label>
   <label>Repozytorium<input type="text" id="ghr" autocapitalize="none" autocomplete="off" placeholder="anenji-monitor"></label>
  </div>
  <div class="sr g" style="display:grid;grid-template-columns:1fr;gap:8px;margin-top:8px">
   <label class="cb"><input type="checkbox" id="ghe" checked> sprawdzaj codziennie o 12:00</label>
  </div>
  <div class="tst" id="ghmsg"></div>
  <div class="btns"><button class="btn df" onclick="ghCheck()">Sprawdź aktualizację</button><button class="btn wr" onclick="ghUpdate()">Wgraj z GitHuba</button></div>
  <div class="note">W repo <b>main</b> muszą być pliki: <code>version.txt</code> (jedna linia z wersją, np. 2026.11.24) i <code>firmware.bin</code> (Szkic → Eksportuj skompilowany plik binarny, plik <b>.ino.bin</b>). Najpierw wrzuć <code>firmware.bin</code>, potem <code>version.txt</code>. „Wgraj z GitHuba” wymaga hasła OTA (pole poniżej, domyślnie anenji).</div>
 </div>

 <h3>Aktualizacja firmware (OTA)</h3>
 <div class="sg">
  <div class="tst" id="otav">--</div>
  <div class="sr g" style="display:grid;grid-template-columns:1fr;gap:8px">
   <label>Plik firmware (.ino.bin)<input type="file" id="otaf" accept=".bin"></label>
   <label>Hasło OTA<input type="password" id="otapw" autocomplete="off" placeholder="domyślnie: anenji"></label>
  </div>
  <progress id="otap" max="100" value="0" style="width:100%;margin-top:10px"></progress>
  <div class="btns"><button class="btn wr" onclick="otaGo()">Wgraj firmware</button></div>
  <div class="tst" id="otam"></div>
  <details style="margin-top:6px"><summary class="tst" style="cursor:pointer">Zmiana hasła OTA</summary>
   <div class="sr g" style="display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:6px">
    <label>Stare hasło<input type="password" id="otaold" autocomplete="off"></label>
    <label>Nowe hasło (4–32 zn.)<input type="password" id="otanew" autocomplete="off"></label>
   </div>
   <div class="btns"><button class="btn df" onclick="otaPw()">Zmień hasło</button></div>
  </details>
 </div>

 <div class="note">
  • „Szukaj ID” sprawdza adresy 1, 38, 2…10 i zapisuje znaleziony.<br>
  • Zmiana ID/baud/rejestrów działa od razu po „Zapisz”.<br>
  • OTA: wgrywaj tylko plik <b>.ino.bin</b> (Szkic → Eksportuj skompilowany plik binarny).<br>
  • Nie odłączaj zasilania podczas wgrywania.
 </div>
</div>
</div>

<script>
const $=id=>document.getElementById(id);
const T=(id,t)=>{$(id).textContent=t;};
const DN=['Nd','Pn','Wt','Śr','Cz','Pt','So'];
const MODES={0:'START',1:'STANDBY',2:'SIEĆ',3:'OFF-GRID',4:'BYPASS',5:'ŁADOWANIE',6:'AWARIA'};

let TAB=1;
function showTab(n){
 TAB=n;
 $('tabDash').style.display=n==1?'':'none';
 $('tabSet').style.display=n==2?'':'none';
 $('tabHis').style.display=n==3?'':'none';
 $('tb1').className='tab'+(n==1?' on':'');
 $('tb2').className='tab'+(n==2?' on':'');
 $('tb3').className='tab'+(n==3?' on':'');
 if(n==2){loadCfg();wload();ghStatus();}
 if(n==3){daysLoad();histLoad();}
}

let DEMO=false;
try{DEMO=localStorage.getItem('demo')==='1';}catch(e){}
if(location.search.indexOf('demo')>=0)DEMO=true;
function paintBtn(){const b=$('mbtn');b.textContent=DEMO?'DEMO':'LIVE';b.className='mbtn'+(DEMO?' demo':'');}
function toggleMode(){
 DEMO=!DEMO;
 try{localStorage.setItem('demo',DEMO?'1':'0');}catch(e){}
 paintBtn();live();histLoad();
}
function toggleTheme(){
 const html=document.documentElement;
 const isLight=html.getAttribute('data-theme')==='light';
 html.setAttribute('data-theme',isLight?'dark':'light');
 try{localStorage.setItem('theme',isLight?'dark':'light');}catch(e){}
 const b=$('tbtn');b.textContent=isLight?'🌙':'☀️';
 // Update crosshair colors in real-time
 const crossColor=isLight?'#1e293b':'#fff';
 document.querySelectorAll('#crosshair, #fsCrosshair').forEach(el=>{
   if(el) el.setAttribute('stroke', crossColor);
 });
}
function applyTheme(){
 try{
  const t=localStorage.getItem('theme')||'dark';
  document.documentElement.setAttribute('data-theme',t);
  const b=$('tbtn');if(b)b.textContent=t==='light'?'☀️':'🌙';
 }catch(e){}
}
function openFullscreen(svgId){
  const src=$(svgId);if(!src)return;
  if(!histData.pv || !histData.pv.length) return;

  const overlay=document.createElement('div');
  overlay.style.cssText='position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.95);z-index:9999;display:flex;align-items:center;justify-content:center;flex-direction:column';
  overlay.onclick=e=>{if(e.target===overlay)closeFullscreen();};

  const container=document.createElement('div');
  container.style.cssText='width:95%;height:85%;max-width:1200px;max-height:800px;position:relative';
  container.id='fsChartBox';

  const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');
  svg.setAttribute('viewBox','0 0 560 190');
  svg.style.cssText='width:100%;height:100%;display:block';
  svg.id='fsHist';
  container.appendChild(svg);

  const tip=document.createElement('div');
  tip.id='fsTooltip';
  tip.style.cssText='position:absolute;background:#1e293b;border:1px solid #475569;border-radius:8px;padding:8px 12px;font-size:13px;pointer-events:none;opacity:0;transition:opacity .15s;z-index:10;white-space:nowrap;box-shadow:0 4px 12px rgba(0,0,0,.4)';
  container.appendChild(tip);

  const closeBtn=document.createElement('button');
  closeBtn.textContent='✕ Zamknij';
  closeBtn.style.cssText='margin-top:16px;padding:10px 20px;border-radius:8px;border:none;background:#16a34a;color:#fff;font-size:16px;cursor:pointer';
  closeBtn.onclick=closeFullscreen;

  overlay.appendChild(container);overlay.appendChild(closeBtn);
  document.body.appendChild(overlay);

  // Re-render chart with fullscreen dimensions
  renderFullscreenChart(svg, histData);

  function closeFullscreen(){overlay.remove();document.removeEventListener('keydown',esc);}
  function esc(e){if(e.key==='Escape')closeFullscreen();}
  document.addEventListener('keydown',esc);
}

function renderFullscreenChart(svg, d){
  const {pv,ld,gr,step,range,mode}=d;
  const showPv=!mode||mode==='all'||mode==='pv';
  const showLd=!mode||mode==='all'||mode==='load';
  const showGr=!mode||mode==='all'||mode==='grid';
  const n=pv.length;
  if(n<2){svg.innerHTML='<text x="20" y="50" fill="#8aa0b8" font-size="16">Brak danych</text>';return;}

  const W=560,H=190,L=36,R=8,Tp=8,B=20;
  const series=[]; if(showPv)series.push(pv); if(showLd)series.push(ld); if(showGr)series.push(gr);
  let mx=series.length?Math.max(100,...series.map(s=>Math.max(...s))):100;
  const u=mx>2000?1000:500;mx=Math.ceil(mx/u)*u;
  const X=i=>L+i*(W-L-R)/(n-1),Y=v=>Tp+(H-Tp-B)*(1-v/mx);

  let g='';
  for(let k=0;k<=4;k++){const v=mx*k/4,y=Y(v);
   g+=`<line x1="${L}" x2="${W-R}" y1="${y}" y2="${y}" stroke="#223449"/><text x="${L-4}" y="${y+3}" text-anchor="end" font-size="11" fill="#8aa0b8">${(v/1000).toFixed(1)}</text>`;}
  const pts=a=>a.map((v,i)=>X(i).toFixed(1)+','+Y(v).toFixed(1)).join(' ');
  const now=Date.now();
  for(let k=0;k<=4;k++){const i=Math.round(k*(n-1)/4),t=new Date(now-(n-1-i)*step*1000);
   let lab;
   if(range=='15m'||range=='1h'||range=='6h'||range=='24h')lab=`${String(t.getHours()).padStart(2,'0')}:${String(t.getMinutes()).padStart(2,'0')}`;
   else if(range=='7d'||range=='30d')lab=`${String(t.getDate()).padStart(2,'0')}.${String(t.getMonth()+1).padStart(2,'0')} ${String(t.getHours()).padStart(2,'0')}:00`;
   else if(range=='90d')lab=`${String(t.getDate()).padStart(2,'0')}.${String(t.getMonth()+1).padStart(2,'0')} ${String(t.getHours()).padStart(2,'0')}:00`;
   else lab=`${String(t.getDate()).padStart(2,'0')}.${String(t.getMonth()+1).padStart(2,'0')}.${String(t.getFullYear()).slice(-2)}`;
   g+=`<text x="${X(i)}" y="${H-5}" text-anchor="middle" font-size="11" fill="#8aa0b8">${lab}</text>`;
  }
  if(showPv)g+=`<polygon points="${X(0)},${Y(0)} ${pts(pv)} ${X(n-1)},${Y(0)}" fill="#fbbf2430"/>`;
  if(showPv)g+=`<polyline points="${pts(pv)}" fill="none" stroke="#fbbf24" stroke-width="2.5"/>`;
  if(showLd)g+=`<polyline points="${pts(ld)}" fill="none" stroke="#38bdf8" stroke-width="2"/>`;
  if(showGr)g+=`<polyline points="${pts(gr)}" fill="none" stroke="#ec4899" stroke-width="2"/>`;

  // crosshair - adaptive color
  const isLight=document.documentElement.getAttribute('data-theme')==='light';
  const crossColor=isLight?'#1e293b':'#fff';
  g+=`<line id="fsCrosshair" x1="${L}" y1="${Tp}" x2="${L}" y2="${H-B}" stroke="${crossColor}" stroke-width="1.5" stroke-dasharray="5 5" opacity="0.8" style="pointer-events:none"/>`;
  g+=`<rect id="fsHover" x="${L}" y="${Tp}" width="${W-L-R}" height="${H-Tp-B}" fill="transparent"/>`;
  svg.innerHTML=g;

  const area=document.getElementById('fsHover');
  const tip=document.getElementById('fsTooltip');
  const cross=document.getElementById('fsCrosshair');
  const box=$('fsChartBox');

  area.onmousemove=function(e){
    const rect=svg.getBoundingClientRect();
    const scaleX=560/rect.width;
    const mx=(e.clientX-rect.left)*scaleX;
    let idx=Math.round((mx-L)/((W-L-R)/(n-1)));
    idx=Math.max(0,Math.min(n-1,idx));
    const cx=X(idx);
    cross.setAttribute('x1',cx);cross.setAttribute('x2',cx);
    cross.style.opacity='0.8';
    const t=new Date(Date.now()-(n-1-idx)*step*1000);
    let timeStr;
    if(range=='15m'||range=='1h'||range=='6h'||range=='24h')timeStr=`${String(t.getHours()).padStart(2,'0')}:${String(t.getMinutes()).padStart(2,'0')}`;
    else if(range=='7d'||range=='30d')timeStr=`${String(t.getDate()).padStart(2,'0')}.${String(t.getMonth()+1).padStart(2,'0')} ${String(t.getHours()).padStart(2,'0')}:00`;
    else if(range=='90d')timeStr=`${String(t.getDate()).padStart(2,'0')}.${String(t.getMonth()+1).padStart(2,'0')} ${String(t.getHours()).padStart(2,'0')}:00`;
    else timeStr=`${String(t.getDate()).padStart(2,'0')}.${String(t.getMonth()+1).padStart(2,'0')}.${String(t.getFullYear()).slice(-2)}`;
    let html=`<b>${timeStr}</b><br>`;
    if(showPv)html+=`<span style="color:#fbbf24">PV: ${pv[idx]} W</span><br>`;
    if(showLd)html+=`<span style="color:#38bdf8">Obciążenie: ${ld[idx]} W</span><br>`;
    if(showGr)html+=`<span style="color:#ec4899">Sieć: ${gr[idx]} W</span><br>`;
    tip.innerHTML=html;tip.className='show';
    const brect=box.getBoundingClientRect();
    let left=e.clientX-brect.left+12,top=e.clientY-brect.top-10;
    if(left+200>brect.width)left=e.clientX-brect.left-210;
    tip.style.left=left+'px';tip.style.top=top+'px';
  };
  area.onmouseleave=()=>{tip.className='';cross.style.opacity='0';};
  area.ontouchstart=area.onmousemove;
  area.ontouchmove=area.onmousemove;
  area.ontouchend=()=>{tip.className='';cross.style.opacity='0';};
}

let tt=0,dSoc=55,dPvd=3.2,dLd=4.1,dGd=0.6,dPvBat=0.0;
function fake(){
 tt+=0.35;
 const pv=Math.max(0,2900*Math.sin(tt/4));
 const load=750+450*Math.sin(tt*0.9)+150*Math.sin(tt*2.3);
 let bp=(pv-load)*0.92;
 if(dSoc<=10&&bp<0)bp=0;
 if(dSoc>=100&&bp>0)bp=0;
 dSoc=Math.max(8,Math.min(100,dSoc+bp/2500));
 const grid=Math.max(0,load+bp-pv);
 dPvd+=pv*0.00056/1000*6; dLd+=load*0.00056/1000*6; dGd+=grid*0.00056/1000*6;
 if(bp>0) dPvBat+=bp*0.00056/1000*6;
 return {ok:true,mode:2,pv:pv,pv1v:pv>5?330+20*Math.sin(tt):0,soc:dSoc,load:load,grid:grid,bp:bp,
  bv:25+dSoc*0.04,bc:bp/(25+dSoc*0.04),ac:231+Math.sin(tt),freq:50,out:230,temp:34+4*Math.sin(tt/3),
  pvd:dPvd,ld:dLd,gd:dGd,pv_bat:dPvBat,rssi:-55,time:'demo',ip:'demo',ap:false,ver:'demo'};
}
function fakeHist(){
 const range = $('hrange') ? $('hrange').value : '24h';
 let pv=[],ld=[],gr=[],step=300,n=288;
 if(range=='15m'){n=30;step=300;}
 else if(range=='1h'){n=12;step=300;}
 else if(range=='6h'){n=72;step=300;}
 else if(range=='24h'){n=288;step=300;}
 else if(range=='7d'){n=168;step=3600;}
 else if(range=='30d'){n=720;step=3600;}
 else if(range=='90d'){n=720;step=10800;}
 else if(range=='1y'){n=365;step=86400;}
 for(let i=0;i<n;i++){
  const p=Math.round(Math.max(0,2700*Math.sin(Math.PI*i/n*0.9)+120*Math.sin(i*0.9)));
  const l=Math.round(550+350*Math.sin(i/22)+120*Math.sin(i*1.3));
  pv.push(p); ld.push(l); gr.push(Math.max(0,l-p));
 }
 return {step:step,range:range,pv:pv,ld:ld,gr:gr,days:[6.2,8.4,3.1,9.0,7.5,5.8,8.8]};
}

function flow(id,w){
 const e=$(id),a=Math.abs(w);
 e.setAttribute('class','ln '+(w>5?'f':w<-5?'r':''));
 e.style.animationDuration=Math.max(0.25,1.6-a/2500*1.35)+'s';
}

async function live(){
 try{
  const d=DEMO?fake():await(await fetch('/data')).json();
  const b=$('badge'),mode=(MODES[d.mode]!==undefined)?MODES[d.mode]:('TRYB '+d.mode);
  if(DEMO){b.textContent='DEMO';b.className='badge warn';}
  else if(!d.ok){b.textContent='FALOWNIK OFFLINE';b.className='badge off';}
  else{b.textContent=mode;b.className='badge'+(d.mode==6?' off':d.mode==4?' warn':'');}
  const aut=d.ld>0.05?Math.max(0,Math.min(100,100*(1-d.gd/d.ld))):0;
  const ratio=d.load>10?Math.min(100,100*d.pv/d.load):(d.pv>0?100:0);
  T('cpv',(d.pv/1000).toFixed(3));
  T('csoc',d.ok?d.soc.toFixed(1):'--');
  T('cld',d.load.toFixed(1));
  T('cgr',d.grid.toFixed(1));
  T('nsol',d.pvd.toFixed(2)+' kWh');
  T('npv',d.pv.toFixed(1)+' W');
  T('npv1',d.pv1v.toFixed(1)+' V');
  T('ngr',d.grid.toFixed(1)+' W');
  T('ngd',d.gd.toFixed(2));
  T('nld',d.load.toFixed(1)+' W');
  T('nhome',d.ld.toFixed(2)+' kWh');
  T('nbat',d.soc.toFixed(1)+'%');
  T('nbw',(d.bp>=0?'+':'')+d.bp.toFixed(1)+' W');
  T('nbv',d.bv.toFixed(2)+' V · '+d.bc.toFixed(2)+' A');
  T('nau',aut.toFixed(0)+'%');
  T('nra',ratio.toFixed(0)+'%');
  T('nac',d.ac.toFixed(0)+' V · '+d.freq.toFixed(1)+' Hz');
  T('ntemp','Falownik '+d.temp.toFixed(0)+' °C');
  T('nmode','● '+mode);
  const h=Math.max(0,Math.min(100,d.soc))*0.22;
  const f=$('bfill');f.setAttribute('height',h);f.setAttribute('y',268-h);
  f.setAttribute('fill',d.soc<20?'#f87171':d.soc<50?'#fbbf24':'#4ade80');
  flow('lpv',d.pv); flow('lgr',d.grid); flow('lld',d.load); flow('lbat',d.bp);
  T('spv',d.pvd.toFixed(1)); T('sld',d.ld.toFixed(1)); T('sgr',d.gd.toFixed(1)); T('spvbat',d.pv_bat.toFixed(1)); T('sau',aut.toFixed(0));
  T('dbv',d.bv.toFixed(1)); T('dbc',d.bc.toFixed(1)); T('dt',d.temp.toFixed(0));
  T('dac',d.ac.toFixed(0)); T('dfr',d.freq.toFixed(1)); T('dout',d.out.toFixed(0));
  T('foot',DEMO?'Tryb DEMO – dane symulowane':('IP '+d.ip+(d.ap?' · tryb AP':'')+' · '+d.time+' · WiFi '+d.rssi+' dBm · v'+d.ver));
  T('otav','Wersja firmware: '+d.ver);
  window._today=d.pvd;
  // Wykres 24h na pulpicie
  if(DEMO){
   drawDashboardHist(fakeHist().pv, fakeHist().ld, fakeHist().gr||[]);
  }else{
   fetch('/history?range=24h').then(r=>r.json()).then(h=>drawDashboardHist(h.pv,h.ld,h.gr||[])).catch(()=>{});
  }
 }catch(e){const b=$('badge');b.textContent='BRAK POŁĄCZENIA';b.className='badge off';}
}

/* ===== WYKRES 24h na Pulpicie (statyczny, wszystkie 3 serie) ===== */
function drawDashboardHist(pv,ld,gr){
 const n=pv.length, s=$('histD');
 if(!s || n<2) return;
 const W=560,H=190,L=36,R=8,Tp=8,B=20;
 let mx=100;
 for(const a of [pv,ld,gr]) for(let i=0;i<a.length;i++) if(a[i]>mx) mx=a[i];
 const u=mx>2000?1000:500; mx=Math.ceil(mx/u)*u;
 const X=i=>L+i*(W-L-R)/(n-1), Y=v=>Tp+(H-Tp-B)*(1-v/mx);
 let g='';
 for(let k=0;k<=4;k++){const v=mx*k/4,y=Y(v);
  g+=`<line x1="${L}" x2="${W-R}" y1="${y}" y2="${y}" stroke="#223449"/><text x="${L-4}" y="${y+3}" text-anchor="end" font-size="9" fill="#8aa0b8">${(v/1000).toFixed(1)}</text>`;}
 const pts=a=>a.map((v,i)=>X(i).toFixed(1)+','+Y(v).toFixed(1)).join(' ');
 const now=Date.now();
 for(let k=0;k<=4;k++){const i=Math.round(k*(n-1)/4),t=new Date(now-(n-1-i)*300000);
  g+=`<text x="${X(i)}" y="${H-5}" text-anchor="middle" font-size="9" fill="#8aa0b8">${String(t.getHours()).padStart(2,'0')}:${String(t.getMinutes()).padStart(2,'0')}</text>`;}
 g+=`<polygon points="${X(0)},${Y(0)} ${pts(pv)} ${X(n-1)},${Y(0)}" fill="#fbbf2430"/>`;
 g+=`<polyline points="${pts(pv)}" fill="none" stroke="#fbbf24" stroke-width="2"/>`;
 g+=`<polyline points="${pts(ld)}" fill="none" stroke="#38bdf8" stroke-width="1.5"/>`;
 g+=`<polyline points="${pts(gr)}" fill="none" stroke="#ec4899" stroke-width="1.5"/>`;
 s.innerHTML=g;
}

/* ===== WYKRES 24h z tooltipem (zakładka Historia) ===== */
let histData = {pv:[],ld:[],gr:[],step:300,range:'24h'};
let crosshairLine = null;

function lineChart(pv,ld,gr,step,range,mode){
 histData = {pv,ld,gr,step,range,mode};
 if(!mode) mode = 'all';
 const W=560,H=190,L=36,R=8,Tp=8,B=20,n=pv.length,s=$('histHis');
 if(!s) return;
 if(n<2){s.innerHTML='<text x="20" y="50" fill="#8aa0b8" font-size="12">Zbieranie danych…</text>';return;}

 // Determine which series to show based on mode
 let showPv = mode === 'all' || mode === 'pv';
 let showLd = mode === 'all' || mode === 'load';
 let showGr = mode === 'all' || mode === 'grid';
 let series = [];
 if(showPv) series.push(pv);
 if(showLd) series.push(ld);
 if(showGr) series.push(gr);
 // Determine max Y value safely (avoid spread overflow with large arrays)
  let mx = 100;
  for (const s of series) {
    for (let i = 0; i < s.length; i++) {
      if (s[i] > mx) mx = s[i];
    }
  }
 const u = mx > 2000 ? 1000 : 500;
 mx = Math.ceil(mx/u)*u;

 const X = i => L + i * (W-L-R) / (n-1);
 const Y = v => Tp + (H-Tp-B) * (1 - v/mx);
 let g = '';
 for(let k=0;k<=4;k++){
   const v = mx*k/4, y = Y(v);
   g += `<line x1="${L}" x2="${W-R}" y1="${y}" y2="${y}" stroke="#223449"/><text x="${L-4}" y="${y+3}" text-anchor="end" font-size="9" fill="#8aa0b8">${(v/1000).toFixed(1)}</text>`;
 }
 const pts = a => a.map((v,i) => X(i).toFixed(1) + ',' + Y(v).toFixed(1)).join(' ');
 const now = Date.now();
 for(let k=0;k<=4;k++){
   const i = Math.round(k*(n-1)/4), t = new Date(now - (n-1-i)*step*1000);
   g += `<text x="${X(i)}" y="${H-5}" text-anchor="middle" font-size="9" fill="#8aa0b8">${String(t.getHours()).padStart(2,'0')}:${String(t.getMinutes()).padStart(2,'0')}</text>`;
 }

 // Filled area only for PV in 'all' or 'pv' mode
 if(showPv) g += `<polygon points="${X(0)},${Y(0)} ${pts(pv)} ${X(n-1)},${Y(0)}" fill="#fbbf2430"/>`;
 if(showPv) g += `<polyline points="${pts(pv)}" fill="none" stroke="#fbbf24" stroke-width="2"/>`;
 if(showLd) g += `<polyline points="${pts(ld)}" fill="none" stroke="#38bdf8" stroke-width="1.5"/>`;
 if(showGr) g += `<polyline points="${pts(gr)}" fill="none" stroke="#ec4899" stroke-width="1.5"/>`;

 // crosshair - adaptive color
 const isLight = document.documentElement.getAttribute('data-theme') === 'light';
 const crossColor = isLight ? '#1e293b' : '#fff';
 g += `<line id="crosshair" x1="${L}" y1="${Tp}" x2="${L}" y2="${H-B}" stroke="${crossColor}" stroke-width="1" stroke-dasharray="4 4" opacity="0" style="pointer-events:none"/>`;
 // overlay
 g += `<rect id="hoverarea" x="${L}" y="${Tp}" width="${W-L-R}" height="${H-Tp-B}" fill="transparent"/>`;
 s.innerHTML = g;

 const area = document.getElementById('hoverarea');
 const tip = $('tooltipHis');
 const cross = document.getElementById('crosshair');
 area.onmousemove = function(e){
  const rect = s.getBoundingClientRect();
  const mx = (e.clientX - rect.left) * (560 / rect.width);
  let idx = Math.round((mx - L) / ((W-L-R)/(n-1)));
  idx = Math.max(0, Math.min(n-1, idx));
  const cx = X(idx);
  cross.setAttribute('x1', cx); cross.setAttribute('x2', cx);
  cross.style.opacity = '0.7';
  const t = new Date(Date.now() - (n-1-idx)*step*1000);
  let timeStr;
  if(range=='15m'||range=='1h'||range=='6h'||range=='24h') timeStr=`${String(t.getHours()).padStart(2,'0')}:${String(t.getMinutes()).padStart(2,'0')}`;
  else timeStr=`${String(t.getDate()).padStart(2,'0')}.${String(t.getMonth()+1).padStart(2,'0')} ${String(t.getHours()).padStart(2,'0')}:00`;

  let tooltipHtml = `<b>${timeStr}</b><br>`;
  if(showPv) tooltipHtml += `<span style="color:#fbbf24">PV: ${pv[idx]} W</span><br>`;
  if(showLd) tooltipHtml += `<span style="color:#38bdf8">Obciążenie: ${ld[idx]} W</span><br>`;
  if(showGr) tooltipHtml += `<span style="color:#ec4899">Sieć: ${gr[idx]} W</span><br>`;
  tip.innerHTML = tooltipHtml;
  tip.className = 'show';
  const box = $('histboxHis').getBoundingClientRect();
  let left = e.clientX - box.left + 12;
  let top = e.clientY - box.top - 10;
  if(left + 140 > box.width) left = e.clientX - box.left - 150;
  tip.style.left = left + 'px'; tip.style.top = top + 'px';
 };
 area.onmouseleave = ()=>{ tip.className = ''; cross.style.opacity = '0'; };
 area.ontouchstart = area.onmousemove;
 area.ontouchmove = area.onmousemove;
 area.ontouchend = ()=>{ tip.className = ''; cross.style.opacity = '0'; };
}

function bars(days){
 const a=days.slice(0,6).reverse();a.push(window._today||0);
 const W=560,H=160,Tp=14,B=22,bw=44,gap=(W-7*bw)/8;
 const mx=Math.max(1,...a)*1.15,s=$('days');let g='';
 a.forEach((v,i)=>{
  const x=gap+i*(bw+gap),h=(H-Tp-B)*v/mx,y=H-B-h;
  const d=new Date(Date.now()-(6-i)*86400000);
  g+=`<rect x="${x}" y="${y}" width="${bw}" height="${h}" rx="5" fill="${i==6?'#fbbf24':'#b8860b'}"/>`;
  g+=`<text x="${x+bw/2}" y="${y-4}" text-anchor="middle" font-size="10" fill="#e8eef6">${v.toFixed(1)}</text>`;
  g+=`<text x="${x+bw/2}" y="${H-7}" text-anchor="middle" font-size="10" fill="#8aa0b8">${i==6?'Dziś':DN[d.getDay()]}</text>`;
 });
 s.innerHTML=g;
 s.onclick = () => openFullscreen('days');
}

async function histLoad(){
 const range = $('hrange').value;
 const mode = $('hmode').value;
 $('hrng').textContent = ' · ' + $('hrange').options[$('hrange').selectedIndex].text;
 try{
  const h=DEMO?fakeHist():await(await fetch('/history?range='+range)).json();
  lineChart(h.pv,h.ld,h.gr||[],h.step,h.range,mode);
  bars(h.days);
 }catch(e){}
}

/* ===== HISTORIA ===== */
function fmtD(d){const s=String(d);return s.slice(6,8)+'.'+s.slice(4,6)+'.'+s.slice(0,4);}
function autk(ld,gd){return ld>0.05?Math.max(0,Math.min(100,100*(1-gd/ld))):0;}
function bars30(a){
 const W=560,H=170,Tp=14,B=22,n=a.length,s=$('h30');
 if(!n){s.innerHTML='';return;}
 const gap=2,bw=Math.max(4,(W-10)/n-gap);
 const mx=Math.max(1,...a.map(x=>x.pv))*1.15;let g='';
 a.forEach((x,i)=>{
  const px=5+i*(bw+gap),h=(H-Tp-B)*x.pv/mx,y=H-B-h;
  g+=`<rect x="${px}" y="${y}" width="${bw}" height="${h}" rx="2" fill="${x.today?'#fbbf24':'#b8860b'}"/>`;
  if(n<=16||i%5==0||i==n-1)g+=`<text x="${px+bw/2}" y="${H-7}" text-anchor="middle" font-size="9" fill="#8aa0b8">${x.d%100}</text>`;
  if(n<=16)g+=`<text x="${px+bw/2}" y="${y-3}" text-anchor="middle" font-size="9" fill="#e8eef6">${x.pv.toFixed(1)}</text>`;
 });
 s.innerHTML=g;
 s.onclick = () => openFullscreen('h30');
}
async function daysLoad(){
 $('hmsg').textContent='Wczytuję…';
 try{
  const r=await(await fetch('/days')).json();
  const rec=(r.rec||[]).map(a=>({d:a[0],pv:a[1],ld:a[2],gd:a[3]}));
  if(r.today&&r.today.d)rec.push({d:r.today.d,pv:r.today.pv,ld:r.today.ld,gd:r.today.gd,today:true});
  if(!rec.length){
   $('hmsg').textContent='Brak historii. Potrzebny czas NTP.';
   $('hmon').textContent='—';$('hday').textContent='—';bars30([]);return;
  }
  bars30(rec.slice(-30));
  const m={};
  rec.forEach(x=>{const k=Math.floor(x.d/100);const o=m[k]||(m[k]={pv:0,ld:0,gd:0,n:0});o.pv+=x.pv;o.ld+=x.ld;o.gd+=x.gd;o.n++;});
  const keys=Object.keys(m).sort().reverse().slice(0,12);
  let h='<table class="ht"><tr><th>Miesiąc</th><th>PV</th><th>Zużycie</th><th>Sieć</th><th>Autark.</th></tr>';
  keys.forEach(k=>{const o=m[k],ks=String(k);
   h+=`<tr><td>${ks.slice(4,6)}.${ks.slice(0,4)}</td><td>${o.pv.toFixed(1)}</td><td>${o.ld.toFixed(1)}</td><td>${o.gd.toFixed(1)}</td><td>${autk(o.ld,o.gd).toFixed(0)}%</td></tr>`;});
  $('hmon').innerHTML=h+'</table>';
  let d='<table class="ht"><tr><th>Data</th><th>PV</th><th>Zużycie</th><th>Sieć</th><th>Autark.</th></tr>';
  rec.slice().reverse().slice(0,120).forEach(x=>{
   d+=`<tr><td>${fmtD(x.d)}${x.today?' (dziś)':''}</td><td>${x.pv.toFixed(2)}</td><td>${x.ld.toFixed(2)}</td><td>${x.gd.toFixed(2)}</td><td>${autk(x.ld,x.gd).toFixed(0)}%</td></tr>`;});
  $('hday').innerHTML=d+'</table>';
  $('hmsg').textContent='Zapisanych dni: '+(r.rec||[]).length;
 }catch(e){$('hmsg').textContent='Błąd połączenia';}
}
async function hisClear(){
 if(!confirm('Usunąć całą historię?'))return;
 try{await fetch('/dayclear',{method:'POST'});$('hmsg').textContent='Wyczyszczono';daysLoad();}
 catch(e){$('hmsg').textContent='Błąd';}
}

/* ===== USTAWIENIA ===== */
let CFG=null;
const STT=['błąd','OK','nie czytano','wyłączony'];
const STC=['#f87171','#4ade80','#8aa0b8','#8aa0b8'];
function msg(t,c){const m=$('msg');m.textContent=t;m.style.color=c||'#8aa0b8';}

async function loadCfg(){
 try{CFG=await(await fetch('/cfg')).json();renderCfg();msg('');}
 catch(e){msg('Nie można pobrać ustawień','#f87171');}
}
function renderCfg(){
 $('gid').value=CFG.id;
 $('gbaud').value=CFG.baud;
 $('goff').value=CFG.off;
 $('gbs').value=CFG.bs;
 $('gbco').value=CFG.bco;
 $('ghu').value=CFG.ghu||'';
 $('ghr').value=CFG.ghr||'';
 $('ghe').checked=!!CFG.ghe;
 let h='';
 CFG.sigs.forEach((s,i)=>{
  h+=`<div class="sg"><div class="sh"><b>${s.n}</b><span class="sval" id="sv${i}">--</span></div>
  <div class="sr">
   <label class="cb"><input type="checkbox" id="en${i}" ${s.a>=0?'checked':''}> wł.</label>
   <label>Adres<input type="number" id="ad${i}" min="0" max="65535" value="${s.a>=0?s.a:(s.d>=0?s.d:0)}"></label>
   <label>Typ<select id="ty${i}"><option value="0">Holding</option><option value="1">Input</option></select></label>
   <label>Skala<input type="number" step="any" id="sc${i}" value="${s.s}"></label>
   <label class="cb"><input type="checkbox" id="sg${i}" ${s.g?'checked':''}> ze znakiem</label>
   <button class="mini" onclick="testReg(${i})">Test</button>
   <span class="tst" id="st${i}"></span>
  </div></div>`;
 });
 $('sigs').innerHTML=h;
 CFG.sigs.forEach((s,i)=>{$('ty'+i).value=s.t;});
 refreshVals(CFG);
}
function refreshVals(c){
 c.sigs.forEach((s,i)=>{
  const v=$('sv'+i);if(!v)return;
  v.textContent=(s.st==1)?String(+s.v):'--';
  const t=$('st'+i);
  if(t&&!t.dataset.test){t.textContent=STT[s.st];t.style.color=STC[s.st];}
 });
}
async function pollVals(){
 if(TAB!=2||!CFG)return;
 try{const c=await(await fetch('/cfg')).json();refreshVals(c);}catch(e){}
}
async function testReg(i){
 const el=$('st'+i);el.dataset.test='1';el.textContent='czytam…';el.style.color='#8aa0b8';
 try{
  const r=await(await fetch('/test?addr='+$('ad'+i).value+'&type='+$('ty'+i).value+'&off='+$('goff').value)).json();
  if(r.ok){
   const sv=r.raw>32767?r.raw-65536:r.raw;
   el.textContent='surowe: '+r.raw+(sv!==r.raw?' (ze znakiem '+sv+')':'');
   el.style.color='#4ade80';
  }else{el.textContent='brak odpowiedzi (kod '+r.err+')';el.style.color='#f87171';}
 }catch(e){el.textContent='błąd';el.style.color='#f87171';}
 setTimeout(()=>{delete el.dataset.test;},6000);
}
async function saveCfg(){
 const p=new URLSearchParams();
 p.set('id',$('gid').value);p.set('baud',$('gbaud').value);
 p.set('bs',$('gbs').value);p.set('off',$('goff').value);p.set('bco',$('gbco').value||'0');
 CFG.sigs.forEach((s,i)=>{
  p.set('a'+i,$('en'+i).checked?$('ad'+i).value:-1);
  p.set('s'+i,$('sc'+i).value);
  p.set('t'+i,$('ty'+i).value);
  p.set('g'+i,$('sg'+i).checked?1:0);
  p.set('ghu',$('ghu').value.trim());
  p.set('ghr',$('ghr').value.trim());
  p.set('ghe',$('ghe').checked?1:0);
 });
 msg('Zapisuję…');
 try{
  const r=await(await fetch('/save',{method:'POST',body:p})).json();
  if(r.ok){msg('Zapisano ✔','#4ade80');setTimeout(loadCfg,600);}
  else msg('Błąd zapisu','#f87171');
 }catch(e){msg('Błąd połączenia','#f87171');}
}
async function resetCfg(){
 if(!confirm('Przywrócić domyślne? (WiFi, OTA i historia zostają)'))return;
 try{await fetch('/reset',{method:'POST'});msg('Przywrócono ✔','#4ade80');loadCfg();}
 catch(e){msg('Błąd','#f87171');}
}
async function doRestart(){
 if(!confirm('Zrestartować ESP32?'))return;
 try{await fetch('/restart',{method:'POST'});}catch(e){}
 msg('Restart… odśwież za 10 s','#fbbf24');
}

/* ===== OTA ===== */
function otaGo(){
 const f=$('otaf').files[0];
 if(!f){$('otam').textContent='Wybierz plik .bin';return;}
 if(!f.name.toLowerCase().endsWith('.bin')){$('otam').textContent='To musi być plik .bin';return;}
 if(!confirm('Wgrać '+f.name+' ('+Math.round(f.size/1024)+' KB)?'))return;
 const fd=new FormData();fd.append('firmware',f,f.name);
 const x=new XMLHttpRequest();
 x.open('POST','/ota?pw='+encodeURIComponent($('otapw').value||'anenji'));
 x.timeout=240000;
 x.upload.onprogress=e=>{if(e.lengthComputable){$('otap').value=Math.round(e.loaded*100/e.total);$('otam').textContent='Wgrywanie '+Math.round(e.loaded*100/e.total)+'%…';}};
 x.onload=()=>{
  let j={};try{j=JSON.parse(x.responseText);}catch(e){}
  if(x.status==200&&j.ok){$('otap').value=100;$('otam').innerHTML='<span style="color:#4ade80">Wgrano. Restart…</span>';otaWait();}
  else $('otam').innerHTML='<span style="color:#f87171">Błąd: '+(j.err||('HTTP '+x.status))+'</span>';
 };
 x.onerror=()=>{$('otam').innerHTML='<span style="color:#f87171">Połączenie przerwane.</span>';};
 x.ontimeout=()=>{$('otam').innerHTML='<span style="color:#f87171">Timeout.</span>';};
 $('otam').textContent='Start…';
 x.send(fd);
}
function otaWait(){
 let n=0;
 setTimeout(()=>{
  const iv=setInterval(async()=>{
   n++;
   try{
    const d=await(await fetch('/data',{cache:'no-store'})).json();
    clearInterval(iv);
    $('otam').innerHTML='<span style="color:#4ade80">Gotowe. Wersja '+d.ver+'.</span>';
    setTimeout(()=>location.reload(),1500);
   }catch(e){if(n>40){clearInterval(iv);$('otam').textContent='Brak odpowiedzi.';}}
  },1500);
 },4000);
}
async function otaPw(){
 const p=new URLSearchParams();p.set('old',$('otaold').value||'anenji');p.set('new',$('otanew').value);
 try{
  const r=await fetch('/otapw',{method:'POST',body:p});
  if(r.ok){$('otam').innerHTML='<span style="color:#4ade80">Hasło zmienione.</span>';$('otaold').value='';$('otanew').value='';}
  else $('otam').innerHTML='<span style="color:#f87171">Nie zmieniono.</span>';
 }catch(e){$('otam').textContent='Błąd';}
}

/* ===== GITHUB ===== */
function ghFields(){
  const p=new URLSearchParams();
  p.set('ghu',$('ghu').value.trim());
  p.set('ghr',$('ghr').value.trim());
  p.set('ghe',$('ghe').checked?1:0);
  return p;
}
async function ghStatus(){
 try{
  const r=await(await fetch('/ghstatus')).json();
  $('ghu').value=r.user||'';$('ghr').value=r.repo||'';$('ghe').checked=!!r.enable;
  let t;
  if(r.updating){t='<span style="color:#fbbf24">Wgrywanie… '+r.bytes/1024+' KB</span>';}
  else if(r.err===-1){t='<span style="color:#f87171">'+(r.msg||'Brak polaczenia')+'</span>';}
  else if(r.err>0){t='<span style="color:#f87171">Blad HTTP '+r.err+' · '+r.msg+'</span>';}
  else if(r.newver){t='<span style="color:#4ade80">Dostepna wersja '+r.latest+' (Twoja: '+r.ver+')</span>';}
  else {t='<span style="color:#8aa0b8">Najnowsza: '+r.latest+' · twoja '+r.ver+' · zgodne</span>';}
  $('ghst').innerHTML=t;
  $('ghmsg').textContent='';
 }catch(e){$('ghst').textContent='Brak odpowiedzi';}
}
async function ghCheck(){
 $('ghmsg').textContent='Sprawdzam…';
 try{
  const sv=await fetch('/ghsave',{method:'POST',body:ghFields()});
  if(!sv.ok){$('ghmsg').innerHTML='<span style="color:#f87171">Niepoprawna nazwa użytkownika lub repo (tylko litery, cyfry, - _ .).</span>';return;}
  await fetch('/ghcheck');
  for(let i=0;i<20;i++){
   await new Promise(r=>setTimeout(r,1000));
   const r=await(await fetch('/ghstatus')).json();
   if(r.msg!=='Sprawdzam wersje...'){await ghStatus();return;}
  }
  $('ghmsg').textContent='Brak odpowiedzi (timeout).';
 }catch(e){$('ghmsg').textContent='Błąd połączenia';}
}
async function ghUpdate(){
 if(!confirm('Wgrać najnowszą wersję z GitHuba? Urządzenie zrestartuje się.'))return;
 let v0='';
 try{v0=(await(await fetch('/data')).json()).ver;}catch(e){}
 const p=new URLSearchParams();p.set('pw',$('otapw').value||'anenji');
 $('ghmsg').textContent='Wgrywanie… to może potrwać 1–2 min, nie zamykaj strony.';
 try{
  const r=await fetch('/ghupdate',{method:'POST',body:p});
  if(r.status==403){$('ghmsg').innerHTML='<span style="color:#f87171">Złe hasło OTA (pole w sekcji poniżej).</span>';return;}
 }catch(e){}
 let n=0;
 const iv=setInterval(async()=>{
  n++;
  try{
   const d=await(await fetch('/data')).json();
   if(v0&&d.ver!==v0){
    clearInterval(iv);
    $('ghmsg').innerHTML='<span style="color:#4ade80">Gotowe. Wersja '+d.ver+'.</span>';
    setTimeout(()=>location.reload(),1500);
    return;
   }
   const s=await(await fetch('/ghstatus')).json();
   if(!s.updating&&s.err!==0){clearInterval(iv);$('ghmsg').innerHTML='<span style="color:#f87171">'+(s.msg||'Błąd')+'</span>';return;}
   if(s.updating)$('ghmsg').textContent='Pobrano '+(s.bytes/1024).toFixed(0)+' KB…';
  }catch(e){}
  if(n>70){clearInterval(iv);$('ghmsg').textContent='Brak odpowiedzi. Odśwież stronę i sprawdź wersję.';}
 },2000);
}

/* ===== SKANER ===== */
async function rscan(){
 $('rout').textContent='Czytam…';
 try{
  const r=await(await fetch('/scanregs?from='+($('rf').value||200)+'&n='+($('rn').value||40))).json();
  if(r.err){$('rout').textContent=r.err;return;}
  let h='<table style="width:100%;font-size:12px;border-collapse:collapse"><tr style="color:#8aa0b8"><td>Adres</td><td>Surowe</td><td>ze zn.</td><td>/10</td><td>/100</td></tr>';
  r.v.forEach((v,i)=>{
   if(v===null){h+=`<tr><td>${r.from+i}</td><td colspan="4" style="color:#6b7280">brak</td></tr>`;return;}
   const s=v>32767?v-65536:v;
   h+=`<tr style="border-top:1px solid #223449"><td>${r.from+i}</td><td>${v}</td><td>${s}</td><td>${(s/10).toFixed(1)}</td><td>${(s/100).toFixed(2)}</td></tr>`;
  });
  $('rout').innerHTML=h+'</table>';
 }catch(e){$('rout').textContent='Błąd';}
}

async function scanId(){
 $('idmsg').textContent='Szukam…';
 try{await fetch('/scanid?start=1');}catch(e){$('idmsg').textContent='Błąd';return;}
 const iv=setInterval(async()=>{
  try{
   const r=await(await fetch('/scanid')).json();
   if(r.running){$('idmsg').textContent='Sprawdzam ID '+r.cur+'…';return;}
   clearInterval(iv);
   if(r.found){$('idmsg').innerHTML='<span style="color:#4ade80">Znaleziono ID '+r.found+'</span>';loadCfg();}
   else $('idmsg').innerHTML='<span style="color:#f87171">Brak odpowiedzi. Sprawdź A/B i baud.</span>';
  }catch(e){}
 },1500);
}

/* ===== WIFI ===== */
async function wload(){
 try{
  const w=await(await fetch('/wifi')).json();
  let t='Zapisana sieć: '+(w.ssid||'brak')+' · ';
  t+=w.sta?('połączono, IP '+w.ip):'brak połączenia';
  if(w.ap)t+=' · AP: ANENJI-SETUP';
  $('wst').textContent=t;
  if(!$('wssid').value)$('wssid').value=w.ssid||'';
 }catch(e){}
}
function wpick(){const v=$('wsel').value;if(v)$('wssid').value=v;}
async function wscan(){
 $('wmsg').textContent='Skanowanie…';
 try{await fetch('/wscan?start=1');}catch(e){$('wmsg').textContent='Błąd';return;}
 const t0=Date.now();
 const iv=setInterval(async()=>{
  try{
   const r=await(await fetch('/wscan')).json();
   if(r.scanning&&Date.now()-t0<25000)return;
   clearInterval(iv);
   const n=r.nets||[];
   $('wsel').innerHTML='<option value="">-- wybierz ('+n.length+') --</option>'+n.map(x=>'<option value="'+x.s.replace(/"/g,'&quot;')+'">'+x.s+' ('+x.r+' dBm)</option>').join('');
   $('wmsg').textContent=n.length?'Wybierz sieć 2,4 GHz':'Nie znaleziono sieci.';
  }catch(e){}
 },1200);
}
async function wsave(){
 const ssid=$('wssid').value.trim(),pass=$('wpass').value;
 if(!ssid){$('wmsg').textContent='Podaj SSID';return;}
 if(!confirm('Zapisać "'+ssid+'" i zrestartować?'))return;
 const p=new URLSearchParams();p.set('ssid',ssid);p.set('pass',pass);
 try{
  const r=await(await fetch('/wsave',{method:'POST',body:p})).json();
  if(r.ok)$('wmsg').innerHTML='<span style="color:#4ade80">Zapisano. Restart…</span>';
  else $('wmsg').textContent='Błąd';
 }catch(e){$('wmsg').textContent='Restart…';}
}

paintBtn();applyTheme();live();histLoad();
setInterval(live,2000);setInterval(histLoad,60000);setInterval(pollVals,2500);
</script></body></html>)rawliteral";

// ====== Pomocnicze ======
String jesc(const String& s) {
  String r; r.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { r += '\\'; r += c; }
    else if ((uint8_t)c >= 0x20) r += c;
  }
  return r;
}

String currentIp() {
  return (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
}

String decPl(float v) { String s = String(v, 2); s.replace('.', ','); return s; }

// ====== HTTP handlers ======
void handleRoot() { server.send_P(200, "text/html; charset=utf-8", index_html); }

void handleData() {
  char tbuf[8] = "--:--";
  time_t n = time(nullptr);
  if (n > 1700000000) { struct tm t; localtime_r(&n, &t); strftime(tbuf, sizeof(tbuf), "%H:%M", &t); }
  String j; j.reserve(600);
  j  = "{\"pv\":"   + String(pv_power, 1);
  j += ",\"pv1v\":" + String(pv1_voltage, 1);
  j += ",\"soc\":"  + String(bat_soc, 1);
  j += ",\"load\":" + String(load_power, 1);
  j += ",\"grid\":" + String(grid_power, 1);
  j += ",\"bp\":"   + String(bat_power, 1);
  j += ",\"bv\":"   + String(bat_voltage, 1);
  j += ",\"bc\":"   + String(bat_current * batSign, 1);
  j += ",\"ac\":"   + String(ac_voltage, 1);
  j += ",\"freq\":" + String(ac_freq, 2);
  j += ",\"out\":"  + String(out_voltage, 1);
  j += ",\"temp\":" + String(inv_temp, 0);
  j += ",\"mode\":" + String((int)op_mode);
  j += ",\"pvd\":"  + String(pv_wh / 1000.0, 2);
  j += ",\"ld\":"   + String(load_wh / 1000.0, 2);
  j += ",\"gd\":"   + String(grid_wh / 1000.0, 2);
  j += ",\"pv_bat\":" + String(pv_bat_wh / 1000.0, 2);
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"ip\":\"" + currentIp() + "\"";
  j += String(",\"ap\":") + (apActive ? "true" : "false");
  j += ",\"ver\":\"" + String(FW_VER) + "\"";
  j += ",\"time\":\"" + String(tbuf) + "\"";
  j += String(",\"ok\":") + (modbusOk ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

void handleHistory() {
  String range = server.arg("range");
  uint32_t stepSec = 300;
  int maxPoints = HIST_N;
  bool useHourly = false, useShort = false;

  if (range == "15m") { stepSec = 30; maxPoints = 30; useShort = true; }
  else if (range == "1h") { stepSec = 30; maxPoints = 120; useShort = true; }
  else if (range == "6h") { stepSec = 300; maxPoints = 72; }
  else if (range == "24h") { stepSec = 300; maxPoints = 288; }
  else if (range == "7d") { stepSec = 3600; maxPoints = 168; useHourly = true; }
  else if (range == "30d") { stepSec = 3600; maxPoints = 720; useHourly = true; }
  else if (range == "90d") { stepSec = 3600 * 3; maxPoints = 720; useHourly = true; }
  else if (range == "1y") { stepSec = 3600 * 24; maxPoints = 365; useHourly = true; }
  else { range = "24h"; }

  uint16_t *pvArr = useShort ? hist_s_pv : (useHourly ? hist_h_pv : hist_pv);
  uint16_t *ldArr = useShort ? hist_s_ld : (useHourly ? hist_h_ld : hist_ld);
  uint16_t *grArr = useShort ? hist_s_gr : (useHourly ? hist_h_gr : hist_gr);
  int cnt = useShort ? hist_s_cnt : (useHourly ? hist_h_cnt : histCount);
  int idxBase = useShort ? hist_s_idx : (useHourly ? hist_h_idx : histIdx);
  int maxCnt = useShort ? HIST_S_N : (useHourly ? HIST_H_N : HIST_N);
  int n = min(cnt, maxPoints);

  String j; j.reserve(8000);
  j = "{\"range\":\"" + range + "\",\"step\":" + String(stepSec) + ",\"n\":" + String(n) + ",\"pv\":[";
  for (int i = 0; i < n; i++) {
    int idx = (idxBase - cnt + i + maxCnt) % maxCnt;
    if (i) j += ',';
    j += pvArr[idx];
  }
  j += "],\"ld\":[";
  for (int i = 0; i < n; i++) {
    int idx = (idxBase - cnt + i + maxCnt) % maxCnt;
    if (i) j += ',';
    j += ldArr[idx];
  }
  j += "],\"gr\":[";
  for (int i = 0; i < n; i++) {
    int idx = (idxBase - cnt + i + maxCnt) % maxCnt;
    if (i) j += ',';
    j += grArr[idx];
  }
  j += "],\"days\":[";
  for (int i = 0; i < 7; i++) { if (i) j += ','; j += String(daysKwh[i], 2); }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleDays() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"today\":{\"d\":" + String(curDate) + ",\"pv\":" + String(pv_wh / 1000.0, 2) +
                     ",\"ld\":" + String(load_wh / 1000.0, 2) + ",\"gd\":" + String(grid_wh / 1000.0, 2) + "},\"rec\":[");
  String chunk; chunk.reserve(640);
  File f = LittleFS.open(DAYS_FILE, "r");
  if (f) {
    DayRec r; bool first = true;
    while (f.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
      if (!first) chunk += ',';
      first = false;
      chunk += "[" + String(r.date) + "," + String(r.pv, 2) + "," + String(r.load, 2) + "," + String(r.grid, 2) + "]";
      if (chunk.length() > 500) { server.sendContent(chunk); chunk = ""; }
    }
    f.close();
  }
  server.sendContent(chunk + "]}");
  server.sendContent("");
}

void handleDaysCsv() {
  server.sendHeader("Content-Disposition", "attachment; filename=anenji-historia.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");
  server.sendContent("\xEF\xBB\xBF" "data;pv_kwh;zuzycie_kwh;siec_kwh\r\n");
  char b[110];
  File f = LittleFS.open(DAYS_FILE, "r");
  if (f) {
    DayRec r;
    while (f.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
      snprintf(b, sizeof(b), "%04u-%02u-%02u;%s;%s;%s\r\n",
               (unsigned)(r.date / 10000), (unsigned)((r.date / 100) % 100),
               (unsigned)(r.date % 100), decPl(r.pv).c_str(), decPl(r.load).c_str(), decPl(r.grid).c_str());
      server.sendContent(b);
    }
    f.close();
  }
  if (curDate) {
    snprintf(b, sizeof(b), "%04u-%02u-%02u;%s;%s;%s\r\n",
             (unsigned)(curDate / 10000), (unsigned)((curDate / 100) % 100),
             (unsigned)(curDate % 100), decPl(pv_wh / 1000.0).c_str(),
             decPl(load_wh / 1000.0).c_str(), decPl(grid_wh / 1000.0).c_str());
    server.sendContent(b);
  }
  server.sendContent("");
}

void handleDayClear() {
  LittleFS.remove(DAYS_FILE);
  for (int i = 0; i < 7; i++) daysKwh[i] = 0;
  saveState();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCfg() {
  String j; j.reserve(3200);
  j  = "{\"id\":" + String(mbId) + ",\"baud\":" + String(mbBaud);
  j += ",\"bs\":" + String((int)batSign) + ",\"off\":" + String(addrOff);
  j += ",\"bco\":" + String(bcOffset, 2);
  j += ",\"ghu\":\"" + jesc(ghUser) + "\",\"ghr\":\"" + jesc(ghRepo) + "\"";
  j += String(",\"ghe\":") + (ghEnable ? "true" : "false");
  j += ",\"sigs\":[";
  for (int i = 0; i < NUM_SIGS; i++) {
    Sig &s = sigs[i];
    if (i) j += ',';
    j += "{\"k\":\"" + String(s.key) + "\",\"n\":\"" + jesc(String(s.name)) + "\"";
    j += ",\"d\":" + String(s.defAddr) + ",\"a\":" + String(s.addr);
    j += ",\"s\":" + String(s.scale, 4) + ",\"t\":" + String(s.type);
    j += String(",\"g\":") + (s.sgn ? "1" : "0");
    j += ",\"v\":" + String(*s.val, 2) + ",\"st\":" + String(s.st) + "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleSave() {
  if (server.hasArg("id"))   mbId    = constrain(server.arg("id").toInt(), 1, 247);
  if (server.hasArg("baud")) mbBaud  = constrain(server.arg("baud").toInt(), 1200, 115200);
  if (server.hasArg("bs"))   batSign = server.arg("bs").toInt() < 0 ? -1 : 1;
  if (server.hasArg("off"))  addrOff = constrain(server.arg("off").toInt(), -5, 5);
  if (server.hasArg("bco"))  bcOffset = constrain(server.arg("bco").toFloat(), -20.0f, 20.0f);
  for (int i = 0; i < NUM_SIGS; i++) {
    String n = String(i);
    if (server.hasArg("a" + n)) sigs[i].addr = constrain(server.arg("a" + n).toInt(), -1, 65535);
    if (server.hasArg("s" + n)) {
      float sc = server.arg("s" + n).toFloat();
      sigs[i].scale = (sc == 0.0f) ? 1.0f : sc;
    }
    if (server.hasArg("t" + n)) sigs[i].type = server.arg("t" + n).toInt() ? 1 : 0;
    if (server.hasArg("g" + n)) sigs[i].sgn  = server.arg("g" + n).toInt() ? true : false;
    sigs[i].st = 2;
  }
  if (server.hasArg("ghu")) ghUser = server.arg("ghu");
  if (server.hasArg("ghr")) ghRepo = server.arg("ghr");
  if (server.hasArg("ghe")) ghEnable = server.arg("ghe").toInt() ? true : false;
  if (ghEnable && ghUser.length() && ghRepo.length()) { ghCheckAt = 1; ghCheckStart(); }
  saveCfg();
  applyModbus();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleReset() {
  String ws = prefs.getString("wssid", "");
  String wp = prefs.getString("wpass", "");
  String op = prefs.getString("otapw", "anenji");
  prefs.clear();
  prefs.putString("wssid", ws);
  prefs.putString("wpass", wp);
  prefs.putString("otapw", op);
  loadCfg();
  applyModbus();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleTest() {
  int addr = server.arg("addr").toInt() + server.arg("off").toInt();
  int type = server.arg("type").toInt();
  if (addr < 0) addr = 0;
  uint8_t r = type ? node.readInputRegisters((uint16_t)addr, 1)
                   : node.readHoldingRegisters((uint16_t)addr, 1);
  String j;
  if (r == node.ku8MBSuccess)
    j = "{\"ok\":true,\"raw\":" + String(node.getResponseBuffer(0)) + "}";
  else
    j = "{\"ok\":false,\"err\":" + String(r) + "}";
  server.send(200, "application/json", j);
}

void handleRestart() {
  server.send(200, "application/json", "{\"ok\":true}");
  restartAt = millis() + 800;
}

// ====== OTA ======
bool otaOk = false, otaAuth = false;
String otaErr;
size_t otaBytes = 0;

String otaPass() { return prefs.getString("otapw", "anenji"); }

void handleOtaUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaOk = false; otaErr = ""; otaBytes = 0;
    otaAuth = (server.arg("pw") == otaPass());
    if (!otaAuth) { otaErr = "Zle haslo OTA"; return; }
    saveState(); saveHist();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) otaErr = "Brak miejsca / zly schemat partycji";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!otaAuth || otaErr.length()) return;
    if (otaBytes == 0 && up.buf[0] != 0xE9) { otaErr = "To nie jest plik firmware (.ino.bin)"; Update.abort(); return; }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) { otaErr = "Blad zapisu"; Update.abort(); return; }
    otaBytes += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END) {
    if (!otaAuth || otaErr.length()) return;
    if (Update.end(true)) otaOk = true;
    else otaErr = String("Blad: ") + Update.errorString();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort(); otaErr = "Przerwano";
  }
}

void handleOtaDone() {
  if (otaOk) {
    server.send(200, "application/json", "{\"ok\":true}");
    restartAt = millis() + 1500;
  } else {
    String j = "{\"ok\":false,\"err\":\"" + jesc(otaErr.length() ? otaErr : String("Blad")) + "\"}";
    server.send(500, "application/json", j);
  }
}

void handleOtaPw() {
  if (server.arg("old") != otaPass()) { server.send(403, "application/json", "{\"ok\":false}"); return; }
  String n = server.arg("new");
  if (n.length() < 4 || n.length() > 32) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  prefs.putString("otapw", n);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ====== WiFi ======
void handleWifi() {
  String j = "{\"ssid\":\"" + jesc(prefs.getString("wssid", "")) + "\"";
  j += String(",\"sta\":") + (WiFi.status() == WL_CONNECTED ? "true" : "false");
  j += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += String(",\"ap\":") + (apActive ? "true" : "false");
  j += ",\"apip\":\"" + WiFi.softAPIP().toString() + "\"}";
  server.send(200, "application/json", j);
}

void handleWscan() {
  if (server.hasArg("start")) {
    int s = WiFi.scanComplete();
    if (s >= 0) WiFi.scanDelete();
    if (s != WIFI_SCAN_RUNNING) WiFi.scanNetworks(true, true);
    server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { server.send(200, "application/json", "{\"scanning\":true}"); return; }
  if (n < 0) { server.send(200, "application/json", "{\"scanning\":false,\"nets\":[]}"); return; }
  String j = "{\"scanning\":false,\"nets\":[";
  bool first = true;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool weaker = false;
    for (int k = 0; k < n; k++)
      if (k != i && WiFi.SSID(k) == s && WiFi.RSSI(k) > WiFi.RSSI(i)) { weaker = true; break; }
    if (weaker) continue;
    if (!first) j += ',';
    first = false;
    j += "{\"s\":\"" + jesc(s) + "\",\"r\":" + String(WiFi.RSSI(i)) + "}";
  }
  j += "]}";
  WiFi.scanDelete();
  server.send(200, "application/json", j);
}

void handleWsave() {
  String s = server.arg("ssid"), p = server.arg("pass");
  if (s.length() < 1 || s.length() > 32 || p.length() > 63) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  prefs.putString("wssid", s);
  prefs.putString("wpass", p);
  server.send(200, "application/json", "{\"ok\":true}");
  restartAt = millis() + 1500;
}

// ====== Szukanie ID ======
const uint8_t SCAN_LIST[] = {1, 38, 2, 3, 4, 5, 6, 7, 8, 9, 10};
bool scanRun = false;
uint8_t scanIdx = 0, scanFound = 0, scanCur = 0;

void handleScanId() {
  if (server.hasArg("start") && !scanRun) {
    scanRun = true; scanIdx = 0; scanFound = 0; scanCur = 0;
  }
  String j = String("{\"running\":") + (scanRun ? "true" : "false");
  j += ",\"cur\":" + String(scanCur) + ",\"found\":" + String(scanFound) + "}";
  server.send(200, "application/json", j);
}

void scanStep() {
  if (!scanRun) return;
  if (scanIdx >= sizeof(SCAN_LIST)) { scanRun = false; scanCur = 0; nodeInit(mbId); return; }
  uint8_t id = SCAN_LIST[scanIdx++];
  scanCur = id;
  nodeInit(id);
  uint8_t r = node.readHoldingRegisters(201, 1);
  if (r == node.ku8MBSuccess) {
    scanFound = id; mbId = id; saveCfg();
    scanRun = false; nodeInit(mbId);
  }
  server.handleClient();
}

// ====== Skaner rejestrów ======
void handleScanRegs() {
  if (scanRun) { server.send(409, "application/json", "{\"err\":\"trwa szukanie ID\"}"); return; }
  int from = constrain(server.arg("from").toInt(), 0, 65000);
  int n = constrain(server.arg("n").toInt(), 1, 60);
  String j = "{\"from\":" + String(from) + ",\"v\":[";
  int tmo = 0;
  for (int i = 0; i < n; ) {
    int chunk = min(10, n - i);
    int a = from + i + addrOff; if (a < 0) a = 0;
    bool done = false;
    if (tmo < 3) {
      uint8_t r = node.readHoldingRegisters((uint16_t)a, chunk);
      if (r == node.ku8MBSuccess) {
        for (int k = 0; k < chunk; k++) { if (i + k) j += ','; j += String(node.getResponseBuffer(k)); }
        tmo = 0; done = true;
      } else if (r == node.ku8MBResponseTimedOut) tmo++;
    }
    if (!done) {
      for (int k = 0; k < chunk; k++) {
        if (i + k) j += ',';
        if (tmo >= 3) { j += "null"; continue; }
        uint8_t r = node.readHoldingRegisters((uint16_t)(a + k), 1);
        if (r == node.ku8MBSuccess) { j += String(node.getResponseBuffer(0)); tmo = 0; }
        else { j += "null"; if (r == node.ku8MBResponseTimedOut) tmo++; }
      }
    }
    i += chunk;
    yield();
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// ====== Aktualizacja z GitHuba (endpointy) ======
void handleGhStatus() {
  String j; j.reserve(260);
  j  = "{\"enable\":" + String(ghEnable ? "true" : "false");
  j += ",\"user\":\"" + jesc(ghUser) + "\",\"repo\":\"" + jesc(ghRepo) + "\"";
  j += ",\"latest\":\"" + jesc(ghLatest) + "\"";
  j += ",\"newver\":" + String(ghNewVer ? "true" : "false");
  j += ",\"ver\":\"" + String(FW_VER) + "\"";
  j += ",\"err\":" + String(ghCheckErr);
  j += ",\"msg\":\"" + jesc(ghCheckMsg) + "\"";
  j += ",\"updating\":" + String(ghUpdating ? "true" : "false");
  j += ",\"bytes\":" + String(ghBytes);
  j += "}";
  server.send(200, "application/json", j);
}

void handleGhCheck() {
  ghCheckReq = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleGhSave() {
  String u = server.arg("ghu"), r = server.arg("ghr");
  u.trim(); r.trim();
  if (!ghNameOk(u) || !ghNameOk(r)) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  ghUser = u; ghRepo = r;
  ghEnable = server.arg("ghe").toInt() ? true : false;
  prefs.putString("ghu", ghUser);
  prefs.putString("ghr", ghRepo);
  prefs.putBool("ghe", ghEnable);
  ghLatest = ""; ghNewVer = false;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleGhUpdate() {
  if (server.arg("pw") != otaPass()) { server.send(403, "application/json", "{\"ok\":false}"); return; }
  ghUpdateReq = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

// ====== Modbus ======
bool readAll() {
  bool any = false;
  int fails = 0;
  bool bcRead = false;
  for (int i = 0; i < NUM_SIGS; i++) {
    Sig &s = sigs[i];
    if (s.addr < 0) { s.st = 3; continue; }
    int a = s.addr + addrOff;
    if (a < 0) a = 0;
    uint8_t r = s.type ? node.readInputRegisters((uint16_t)a, 1)
                       : node.readHoldingRegisters((uint16_t)a, 1);
    if (r == node.ku8MBSuccess) {
      uint16_t raw = node.getResponseBuffer(0);
      float v = s.sgn ? (float)(int16_t)raw : (float)raw;
      *s.val = v * s.scale;
      s.st = 1; any = true; fails = 0;
      if (i == S_BC) bcRead = true;
    } else {
      s.st = 0;
      if (++fails >= 3) {
        for (int j = i + 1; j < NUM_SIGS; j++) if (sigs[j].addr >= 0) sigs[j].st = 2;
        break;
      }
    }
    server.handleClient();
    yield();
  }
  if (any) {
    if (bcRead) bat_current += bcOffset;
    bat_power = bat_voltage * bat_current * batSign;
    if (sigs[S_GRID].addr >= 0 && sigs[S_GRID].st == 1) {
      grid_power = grid_raw > 0 ? grid_raw : 0;
    } else if ((int)op_mode == 3) {
      grid_power = 0;
    } else {
      float g = load_power + bat_power - pv_power;
      grid_power = g > 0 ? g : 0;
    }
  }
  return any;
}

// ====== WiFi start ======
void startAp() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  apActive = true;
  Serial.print("AP: "); Serial.print(AP_SSID); Serial.print("  IP: "); Serial.println(WiFi.softAPIP());
  String ws = prefs.getString("wssid", "");
  if (ws.length()) WiFi.begin(ws.c_str(), prefs.getString("wpass", "").c_str());
}

void wifiStart() {
  String ws = prefs.getString("wssid", "");
  String wp = prefs.getString("wpass", "");
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (ws.length()) {
    WiFi.begin(ws.c_str(), wp.c_str());
    Serial.print("Laczenie z WiFi: "); Serial.println(ws);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) { delay(300); Serial.print("."); }
    Serial.println();
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  } else {
    startAp();
  }
}

void wifiWatch() {
  static unsigned long discSince = 0;
  if (WiFi.status() == WL_CONNECTED) { discSince = 0; return; }
  if (apActive) return;
  if (!discSince) discSince = millis();
  if (millis() - discSince > 300000UL) { startAp(); }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nStart... wersja " + String(FW_VER));

  prefs.begin("anenji", false);
  loadCfg();

  if (LittleFS.begin(true)) { loadState(); loadHist(); } else Serial.println("LittleFS niedostepny");

  pinMode(MAX485_DE, OUTPUT);
  digitalWrite(MAX485_DE, LOW);
  applyModbus();

  wifiStart();
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
  if (MDNS.begin("anenji")) MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/history", HTTP_GET, handleHistory);
  server.on("/days", HTTP_GET, handleDays);
  server.on("/days.csv", HTTP_GET, handleDaysCsv);
  server.on("/dayclear", HTTP_POST, handleDayClear);
  server.on("/cfg", HTTP_GET, handleCfg);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/test", HTTP_GET, handleTest);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/ota", HTTP_POST, handleOtaDone, handleOtaUpload);
  server.on("/otapw", HTTP_POST, handleOtaPw);
  server.on("/wifi", HTTP_GET, handleWifi);
  server.on("/wscan", HTTP_GET, handleWscan);
  server.on("/wsave", HTTP_POST, handleWsave);
  server.on("/scanid", HTTP_GET, handleScanId);
  server.on("/scanregs", HTTP_GET, handleScanRegs);
  server.on("/ghstatus", HTTP_GET, handleGhStatus);
  server.on("/ghsave", HTTP_POST, handleGhSave);
  server.on("/ghcheck", HTTP_GET, handleGhCheck);
  server.on("/ghupdate", HTTP_POST, handleGhUpdate);
  server.begin();
  Serial.println("Serwer HTTP uruchomiony.");
}

void loop() {
  server.handleClient();
  checkDay();
  wifiWatch();

  if (restartAt && (long)(millis() - restartAt) >= 0) {
    saveState(); saveHist();
    delay(100);
    ESP.restart();
  }

  if (ghCheckReq) { ghCheckReq = false; ghCheckStart(); }
  if (ghUpdateReq) { ghUpdateReq = false; ghUpdateStart(); }

  // Sprawdzanie wersji na GitHubie: raz po starcie (~90 s) i codziennie od 12:00
  if (ghEnable && !ghUpdating && WiFi.status() == WL_CONNECTED) {
    static bool bootChecked = false;
    static int lastKey = -1;
    time_t tn = time(nullptr);
    if (tn > 1700000000) {
      struct tm tmx; localtime_r(&tn, &tmx);
      int key = tmx.tm_year * 400 + tmx.tm_yday;
      if (!bootChecked && millis() > 90000UL) {
        bootChecked = true;
        if (tmx.tm_hour >= 12) lastKey = key;
        ghCheckStart();
      } else if (tmx.tm_hour == 12 && key != lastKey) {
        lastKey = key;
        ghCheckStart();
      }
    }
  }

  if (scanRun) { scanStep(); return; }

  unsigned long now = millis();
  unsigned long interval = modbusOk ? 2000UL : 8000UL;

  if (now - lastRead >= interval) {
    unsigned long dt = now - lastRead;
    if (dt > 5000) dt = 5000;
    lastRead = now;
    modbusOk = readAll();
    if (modbusOk) {
      double h = dt / 3600000.0;
      pv_wh   += pv_power * h;
      load_wh += load_power * h;
      grid_wh += grid_power * h;
      if (bat_power > 0) pv_bat_wh += bat_power * h;
      accPv += pv_power; accLd += load_power; accGr += grid_power; accN++;
      accS_pv += pv_power; accS_ld += load_power; accS_gr += grid_power; accS_n++;
    }
    Serial.printf("Mode=%d PV=%.0fW SOC=%.0f%% Load=%.0fW Bat=%.1fV %.1fA Grid=%.0fW AC=%.1fV Modbus=%s\n",
                  (int)op_mode, pv_power, bat_soc, load_power, bat_voltage, bat_current,
                  grid_power, ac_voltage, modbusOk ? "OK" : "BRAK");
  }

  // próbka co 30 s (wykresy 15 min i 1 h)
  if (now - lastShort >= 30000UL) {
    lastShort = now;
    pushHistShort(accS_n ? (uint16_t)(accS_pv / accS_n) : 0,
                  accS_n ? (uint16_t)(accS_ld / accS_n) : 0,
                  accS_n ? (uint16_t)(accS_gr / accS_n) : 0);
    accS_pv = accS_ld = accS_gr = 0; accS_n = 0;
  }

  // próbka co 5 min (wykresy 6 h i 24 h)
  if (now - lastHist >= 300000UL) {
    lastHist = now;
    pushHist(accN ? (uint16_t)(accPv / accN) : (uint16_t)pv_power,
             accN ? (uint16_t)(accLd / accN) : (uint16_t)load_power,
             accN ? (uint16_t)(accGr / accN) : (uint16_t)grid_power);
    acc_h_pv += accPv; acc_h_ld += accLd; acc_h_gr += accGr; acc_h_n += accN;
    accPv = accLd = accGr = 0; accN = 0;
  }

  // próbka co godzinę (wykresy 7 i 30 dni)
  static uint32_t lastHour = 0;
  if (now - lastHour >= 3600000UL) {
    lastHour = now;
    if (acc_h_n > 0) {
      pushHistHourly((uint16_t)(acc_h_pv / acc_h_n),
                     (uint16_t)(acc_h_ld / acc_h_n),
                     (uint16_t)(acc_h_gr / acc_h_n));
    } else {
      pushHistHourly((uint16_t)pv_power, (uint16_t)load_power, (uint16_t)grid_power);
    }
    acc_h_pv = acc_h_ld = acc_h_gr = 0; acc_h_n = 0;
  }

  if (now - lastSave >= 600000UL) { lastSave = now; saveState(); }
  if (now - lastHistSave >= 1800000UL) { lastHistSave = now; saveHist(); }
}