/*
  CYD (ESP32-2432S028R) – Temperatur-gauge via MQTT   [PlatformIO]
  ----------------------------------------------------------------
  Viser temperaturen fra et MQTT-topic som en farvet ring-gauge.

  Konfiguration sker uden hardcodede oplysninger:
    - WiFi  : via Improv Wi-Fi (Serial) fra flasher-siden i browseren
    - MQTT + temp-skala : via enhedens egen konfig-side (http://<enhedens-ip>/)
  Alt gemmes i NVS (Preferences) og overlever genstart.

  TFT_eSPI-konfiguration ligger i platformio.ini (build_flags).
*/

#include <Arduino.h>
#include <time.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>  // Unicode-fonte (ÆØÅ, gradtegn)
#include <ImprovWiFiLibrary.h>
#include "ota_certs.h"          // root-CA'er til ægte HTTPS-validering

// Semantisk version indsprøjtes af GitHub Actions fra VERSION-filen
// (src/build_version.h). Ved lokal build uden filen bruges 0.0.0-dev.
#if __has_include("build_version.h")
  #include "build_version.h"
#endif
#ifndef FW_VERSION_SEM
  #define FW_VERSION_SEM "0.0.0-dev"
#endif

#define FW_NAME        "CYD Temp Gauge"
#define MQTT_CLIENTID  "cyd-temp-gauge"

// GitHub Pages — kilde til OTA-opdateringer
#define PAGES_BASE     "https://martindamp.github.io/cyd-temp-gauge/"
#define OTA_BIN_URL    PAGES_BASE "firmware/firmware.bin"
#define OTA_VER_URL    PAGES_BASE "version.txt"

// Defaults indtil enheden konfigureres
#define DEF_MQTT_PORT  1883
#define DEF_MQTT_TOPIC "home/stue/temperatur"
#define DEF_TEMP_MIN   0.0f
#define DEF_TEMP_MAX   40.0f
#define STALE_MS       60000UL

TFT_eSPI     tft = TFT_eSPI();
U8g2_for_TFT_eSPI u8f;          // Unicode-tekst (overskrift + enhed)
WiFiClient   net;
PubSubClient mqtt(net);
WebServer    server(80);
Preferences  prefs;
ImprovWiFi   improvSerial(&Serial);

// ---- runtime-konfiguration ----
struct Config {
  String   ssid, pass;
  String   mqttHost, mqttUser, mqttPass, mqttTopic;
  uint16_t mqttPort;
  float    tmin, tmax;
  String   title, unit;
} cfg;

// ---- gauge-geometri (portrait 240x320) ----
int cx, cy, rOuter, rInner;
const int START_ANGLE = 30;
const int END_ANGLE   = 330;

volatile float lastTemp = NAN;
float    shownTemp  = -1000;
bool     haveData   = false;
uint32_t lastMsgMs  = 0, lastReconnect = 0;
bool     mqttUp     = false;
bool     serverStarted = false;
bool     uiGauge    = false;   // true når gauge-skærmen er aktiv
bool     staleShown = false;   // om "gammel data"-markøren står på skærmen nu

// ============================ config ============================
void loadConfig() {
  prefs.begin("cfg", true);
  cfg.ssid      = prefs.getString("ssid", "");
  cfg.pass      = prefs.getString("pass", "");
  cfg.mqttHost  = prefs.getString("mhost", "");
  cfg.mqttPort  = prefs.getUShort("mport", DEF_MQTT_PORT);
  cfg.mqttUser  = prefs.getString("muser", "");
  cfg.mqttPass  = prefs.getString("mpass", "");
  cfg.mqttTopic = prefs.getString("mtopic", DEF_MQTT_TOPIC);
  cfg.tmin      = prefs.getFloat("tmin", DEF_TEMP_MIN);
  cfg.tmax      = prefs.getFloat("tmax", DEF_TEMP_MAX);
  cfg.title     = prefs.getString("title", "TEMPERATUR");
  cfg.unit      = prefs.getString("unit", "\xC2\xB0""C");   // UTF-8 "°C"
  prefs.end();
}

void saveWiFi(const char* ssid, const char* pass) {
  prefs.begin("cfg", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

// ============================ tegning ============================
// Tom/slukket bane: meget mørk, så enhver tændt farve står skarpt frem.
static inline uint16_t trackColor() { return tft.color565(28, 32, 44); }
// Forældet data: tydelig dæmpet grå — adskilt fra både tom bane og live farve.
static inline uint16_t staleColor() { return tft.color565(130, 132, 148); }

// Tre faste farvebånd efter værdi-intervallet (nederste/midterste/øverste trediedel)
static inline uint16_t bandBlue()  { return tft.color565(0, 120, 255); }
static inline uint16_t bandGreen() { return tft.color565(30, 200, 40); }
static inline uint16_t bandRed()   { return tft.color565(255, 45, 30); }

// Enheden (fx "°C", "%", "hPa") centreret via U8g2 — håndterer ° og ÆØÅ.
void drawUnit(const String& u) {
  tft.fillRect(cx - 54, cy + 40, 108, 26, TFT_BLACK);   // ryd (i ringens hul)
  u8f.setFont(u8g2_font_helvB14_tf);
  u8f.setFontMode(1);
  u8f.setForegroundColor(TFT_LIGHTGREY);
  int w = u8f.getUTF8Width(u.c_str());
  u8f.drawUTF8(cx - w / 2, cy + 62, u.c_str());
}

void drawStatic() {
  tft.fillScreen(TFT_BLACK);
  // Overskrift via U8g2 (ÆØÅ virker)
  u8f.setFont(u8g2_font_helvB18_tf);
  u8f.setFontMode(1);
  u8f.setForegroundColor(TFT_WHITE);
  int tw = u8f.getUTF8Width(cfg.title.c_str());
  u8f.drawUTF8(cx - tw / 2, 30, cfg.title.c_str());
  tft.drawSmoothArc(cx, cy, rOuter, rInner, START_ANGLE, END_ANGLE, trackColor(), TFT_BLACK, true);
  int lr = rOuter + 14;
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawNumber((int)cfg.tmin, cx - lr * sin(radians(START_ANGLE)), cy + lr * cos(radians(START_ANGLE)), 2);
  tft.drawNumber((int)cfg.tmax, cx - lr * sin(radians(END_ANGLE)),   cy + lr * cos(radians(END_ANGLE)),   2);
}

void drawStatus() {
  uint16_t c = (WiFi.status() == WL_CONNECTED && mqttUp) ? TFT_GREEN : TFT_RED;
  tft.fillCircle(cx, cy + rOuter + 28, 5, c);
  // IP nederst, så konfig-siden kan nås
  tft.fillRect(cx - 110, 302, 220, 16, TFT_BLACK);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(WiFi.localIP().toString(), cx, 309, 2);
  }
}

void drawValue(float temp, bool stale) {
  float tC = constrain(temp, cfg.tmin, cfg.tmax);
  float span = cfg.tmax - cfg.tmin;
  float frac = (span != 0) ? (tC - cfg.tmin) / span : 0.0f;
  float sweep = END_ANGLE - START_ANGLE;
  float fillAngle = START_ANGLE + frac * sweep;
  float b1 = START_ANGLE + sweep / 3.0f;        // grænse blå|grøn
  float b2 = START_ANGLE + 2.0f * sweep / 3.0f; // grænse grøn|rød

  // Hele ringen som mørk bane (ét buestykke — ingen sømme)
  tft.drawSmoothArc(cx, cy, rOuter, rInner, START_ANGLE, END_ANGLE, trackColor(), TFT_BLACK, true);

  // Tændte bånd: 3 faste farver efter værdi-trediedele, hvert som ÉT buestykke.
  // roundEnds=true -> rundede, FARVEDE ender (ikke mørke track-ender i enderne).
  // Båndene tegnes i rækkefølge og mødes, så senere bånd dækker forrige kant.
  auto band = [&](float a0, float a1, uint16_t c) {
    if (a1 > a0 + 0.5f)
      tft.drawSmoothArc(cx, cy, rOuter, rInner, (int)lroundf(a0), (int)lroundf(a1), c, TFT_BLACK, true);
  };
  if (fillAngle > START_ANGLE) band(START_ANGLE, fminf(fillAngle, b1), bandBlue());
  if (fillAngle > b1)          band(b1,          fminf(fillAngle, b2), bandGreen());
  if (fillAngle > b2)          band(b2,          fillAngle,            bandRed());

  char buf[8];
  dtostrf(temp, 0, 1, buf);
  // Bredere rydning (i hullet), så cifre fra et større tal ikke hænger ved
  tft.fillRect(cx - 80, cy - 30, 160, 60, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(stale ? staleColor() : TFT_WHITE, TFT_BLACK);
  tft.drawString(buf, cx, cy, 7);

  drawUnit(cfg.unit);

  // Gammel-data-markør i ringens åbne midte (over tallet) — ingen bue her.
  // Tegnes KUN når teksten vises; ryddes én gang når data bliver frisk igen,
  // så der ikke står en konstant sort bjælke.
  if (stale) {
    tft.fillRect(cx - 54, cy - 60, 108, 18, TFT_BLACK);
    tft.setTextColor(staleColor(), TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("gammel data", cx, cy - 51, 2);
    staleShown = true;
  } else if (staleShown) {
    tft.fillRect(cx - 54, cy - 60, 108, 18, TFT_BLACK);
    staleShown = false;
  }
}

void showMessage(const char* l1, const char* l2 = nullptr) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(l1, cx, cy - 12, 4);
  if (l2) {
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(l2, cx, cy + 18, 2);
  }
}

void enterGaugeUI() {
  drawStatic();
  uiGauge = true;
  shownTemp = -1000;                 // tving fuld gentegning næste loop
  if (haveData) {
    bool stale = (millis() - lastMsgMs > STALE_MS);
    drawValue(lastTemp, stale);      // gentegn straks med den kendte værdi
    shownTemp = lastTemp;
  } else {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("--", cx, cy, 7);
  }
  drawStatus();
}

// ============================ MQTT ============================
void onMqtt(char* topic, byte* payload, unsigned int len) {
  char buf[32];
  if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
  memcpy(buf, payload, len);
  buf[len] = '\0';
  // Rent tal forventes, fx "21.5". JSON? -> se noten nederst.
  lastTemp  = atof(buf);
  haveData  = true;
  lastMsgMs = millis();
}

bool connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || cfg.mqttHost.isEmpty()) return false;
  bool ok = cfg.mqttUser.length()
            ? mqtt.connect(MQTT_CLIENTID, cfg.mqttUser.c_str(), cfg.mqttPass.c_str())
            : mqtt.connect(MQTT_CLIENTID);
  if (ok) mqtt.subscribe(cfg.mqttTopic.c_str());
  mqttUp = ok;
  return ok;
}

// ============================ konfig-webside ============================
static String esc(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else if (c == '<') o += "&lt;";
    else o += c;
  }
  return o;
}

void handleRoot() {
  String h;
  h.reserve(2600);
  h += F("<!doctype html><html lang=da><head><meta charset=utf-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>CYD Temp Gauge</title><style>"
         "body{font-family:system-ui,sans-serif;background:#11131a;color:#e9ecf2;margin:0;padding:24px;display:flex;justify-content:center}"
         "form{width:100%;max-width:420px}h1{font-size:1.3rem}label{display:block;margin:14px 0 4px;font-size:.9rem;color:#9aa3b2}"
         "input{width:100%;padding:10px;border-radius:8px;border:1px solid #2a2e3a;background:#0d0f15;color:#e9ecf2;font-size:1rem}"
         ".row{display:flex;gap:10px}.row>div{flex:1}"
         "button{margin-top:20px;width:100%;padding:12px;border:0;border-radius:8px;background:#ffcb2d;color:#1a1500;font-weight:700;font-size:1rem;cursor:pointer}"
         "</style></head><body><form method=POST action=/save>"
         "<h1>CYD Temp Gauge</h1>");
  h += "<label>MQTT broker (host/IP)</label><input name=mhost value=\"" + esc(cfg.mqttHost) + "\" placeholder='192.168.1.10'>";
  h += "<div class=row><div><label>Port</label><input name=mport type=number value=\"" + String(cfg.mqttPort) + "\"></div>";
  h += "<div><label>Topic</label><input name=mtopic value=\"" + esc(cfg.mqttTopic) + "\"></div></div>";
  h += "<label>Bruger (valgfri)</label><input name=muser value=\"" + esc(cfg.mqttUser) + "\">";
  h += "<label>Password (valgfri)</label><input name=mpass type=password value=\"" + esc(cfg.mqttPass) + "\">";
  h += "<div class=row><div><label>Skala min</label><input name=tmin type=number step=any value=\"" + String(cfg.tmin, 0) + "\"></div>";
  h += "<div><label>Skala max</label><input name=tmax type=number step=any value=\"" + String(cfg.tmax, 0) + "\"></div></div>";
  h += "<div class=row><div><label>Overskrift</label><input name=title maxlength=16 value=\"" + esc(cfg.title) + "\"></div>";
  h += "<div><label>Enhed</label><input name=unit maxlength=6 value=\"" + esc(cfg.unit) + "\" placeholder='&deg;C'></div></div>";
  h += F("<button>Gem og genstart</button></form>");

  // --- OTA-sektion ---
  h += F("<div style='max-width:420px;margin-top:28px;border-top:1px solid #2a2e3a;padding-top:18px'>"
         "<h2 style='font-size:1.1rem'>Firmware</h2>");
  h += "<p style='color:#9aa3b2;font-size:.9rem'>Nuv\xC3\xA6rende version: <code style='color:#ffcb2d'>";
  h += FW_VERSION_SEM;
  h += F("</code></p>"
         "<button type=button id=chk onclick=checkUpd() style='width:100%;padding:12px;border:0;border-radius:8px;background:#2a2e3a;color:#e9ecf2;font-weight:700;font-size:1rem;cursor:pointer'>S\xC3\xB8g efter opdatering</button>"
         "<div id=ures style='margin-top:12px;font-size:.92rem;color:#9aa3b2'></div>"
         "<button type=button id=upd onclick=doUpd() style='display:none;width:100%;margin-top:12px;padding:12px;border:0;border-radius:8px;background:#35c46a;color:#05230f;font-weight:700;font-size:1rem;cursor:pointer'>Opdater nu</button>"
         "</div>"
         // --- Diagnostik ---
         "<div style='max-width:420px;margin-top:28px;border-top:1px solid #2a2e3a;padding-top:18px'>"
         "<h2 style='font-size:1.1rem'>Diagnostik</h2>"
         "<div class=row>"
         "<div><button type=button onclick=\"fetch('/colortest',{method:'POST'})\" style='width:100%;padding:12px;border:0;border-radius:8px;background:#2a2e3a;color:#e9ecf2;font-weight:700;font-size:1rem;cursor:pointer'>Vis farvetest</button></div>"
         "<div><button type=button onclick=\"fetch('/gauge',{method:'POST'})\" style='width:100%;padding:12px;border:0;border-radius:8px;background:#2a2e3a;color:#e9ecf2;font-weight:700;font-size:1rem;cursor:pointer'>Tilbage til gauge</button></div>"
         "</div></div>"
         "<script>"
         "async function checkUpd(){var b=document.getElementById('chk'),r=document.getElementById('ures'),u=document.getElementById('upd');"
         "b.disabled=true;b.textContent='S\xC3\xB8ger\xE2\x80\xA6';r.textContent='';u.style.display='none';"
         "try{var d=await (await fetch('/checkupdate')).json();"
         "if(!d.latest){r.textContent='Kunne ikke kontakte GitHub.';}"
         "else if(d.update){r.innerHTML='Ny version tilg\xC3\xA6ngelig: <code style=\"color:#ffcb2d\">'+d.latest+'</code>';u.style.display='block';}"
         "else{r.textContent='Du har allerede nyeste version.';}"
         "}catch(e){r.textContent='Fejl: '+e;}b.disabled=false;b.textContent='S\xC3\xB8g efter opdatering';}"
         "async function doUpd(){var u=document.getElementById('upd'),r=document.getElementById('ures');"
         "u.disabled=true;u.textContent='Starter\xE2\x80\xA6';r.innerHTML='Opdaterer \xE2\x80\x94 f\xC3\xB8lg med p\xC3\xA5 CYD-sk\xC3\xA6rmen. Enheden genstarter selv.';"
         "try{await fetch('/doupdate',{method:'POST'});}catch(e){}}"
         "</script></body></html>");
  server.send(200, "text/html", h);
}

void handleSave() {
  prefs.begin("cfg", false);
  if (server.hasArg("mhost"))  prefs.putString("mhost",  server.arg("mhost"));
  if (server.hasArg("mport"))  prefs.putUShort("mport",  (uint16_t)server.arg("mport").toInt());
  if (server.hasArg("muser"))  prefs.putString("muser",  server.arg("muser"));
  if (server.hasArg("mpass"))  prefs.putString("mpass",  server.arg("mpass"));
  if (server.hasArg("mtopic")) prefs.putString("mtopic", server.arg("mtopic"));
  if (server.hasArg("tmin"))   prefs.putFloat("tmin",    server.arg("tmin").toFloat());
  if (server.hasArg("tmax"))   prefs.putFloat("tmax",    server.arg("tmax").toFloat());
  if (server.hasArg("title"))  prefs.putString("title",  server.arg("title"));
  if (server.hasArg("unit"))   prefs.putString("unit",   server.arg("unit"));
  prefs.end();
  server.send(200, "text/html",
    F("<meta charset=utf-8><body style='font-family:system-ui,sans-serif;background:#11131a;color:#e9ecf2;padding:40px;text-align:center'>"
      "<h2>Gemt \xE2\x9C\x93</h2><p>Enheden genstarter\xE2\x80\xA6</p></body>"));
  delay(500);
  ESP.restart();
}

// ============================ OTA ============================
// Sørg for korrekt ur (NTP) — TLS-certvalidering kræver det
bool ensureTime() {
  if (time(nullptr) > 1700000000) return true;   // allerede sat
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < 8000) delay(100);
  return time(nullptr) > 1700000000;
}

// Parse "MAJOR.MINOR.PATCH" -> sammenligneligt tal. Pre-release-suffiks ignoreres.
long semverNum(const String& v) {
  int a = 0, b = 0, c = 0;
  sscanf(v.c_str(), "%d.%d.%d", &a, &b, &c);
  return (long)a * 1000000L + (long)b * 1000L + c;
}

String fetchLatestVersion() {
  if (WiFi.status() != WL_CONNECTED) return "";
  ensureTime();
  WiFiClientSecure sc;
  sc.setCACert(OTA_ROOT_CAS);       // ægte cert-validering mod kendte roots
  sc.setTimeout(12000);
  HTTPClient http;
  if (!http.begin(sc, OTA_VER_URL)) return "";
  int code = http.GET();
  String v;
  if (code == 200) { v = http.getString(); v.trim(); }
  http.end();
  return v;
}

void handleCheckUpdate() {
  String latest = fetchLatestVersion();
  String cur = FW_VERSION_SEM;
  // Kun nyere version tilbydes — aldrig downgrade
  bool avail = latest.length() && semverNum(latest) > semverNum(cur);
  String j = "{\"current\":\"" + cur + "\",\"latest\":\"" + latest +
             "\",\"update\":" + (avail ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

void drawOtaProgress(size_t done, size_t total) {
  static int last = -1;
  int pct = total ? (int)(done * 100 / total) : 0;
  if (pct == last) return;
  last = pct;
  int w = 200, x = cx - w / 2, y = cy + 10, h = 18;
  tft.drawRect(x, y, w, h, TFT_LIGHTGREY);
  tft.fillRect(x + 2, y + 2, (w - 4) * pct / 100, h - 4, TFT_GREEN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String(pct) + "%  ", cx, y - 18, 4);
}

void runOta() {
  uiGauge = false;
  mqtt.disconnect();
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Opdaterer firmware", cx, cy - 40, 4);

  Update.onProgress(drawOtaProgress);

  ensureTime();
  WiFiClientSecure sc;
  sc.setCACert(OTA_ROOT_CAS);       // ægte cert-validering
  sc.setTimeout(15000);

  httpUpdate.rebootOnUpdate(true);  // genstart automatisk i den nye firmware
  t_httpUpdate_return r = httpUpdate.update(sc, OTA_BIN_URL);

  // Nås kun hvis det IKKE lykkedes (ved succes genstarter enheden selv)
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  if (r == HTTP_UPDATE_NO_UPDATES) {
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Allerede nyeste", cx, cy, 4);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Opdatering fejlede", cx, cy - 12, 4);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(httpUpdate.getLastErrorString(), cx, cy + 16, 2);
  }
  delay(4000);
  enterGaugeUI();
}

void handleDoUpdate() {
  server.send(200, "text/html",
    F("<meta charset=utf-8><body style='font-family:system-ui,sans-serif;background:#11131a;color:#e9ecf2;padding:40px;text-align:center'>"
      "<h2>Opdaterer\xE2\x80\xA6</h2><p>F\xC3\xB8lg med p\xC3\xA5 sk\xC3\xA6rmen. Enheden genstarter n\xC3\xA5r den er f\xC3\xA6rdig.</p></body>"));
  delay(200);
  runOta();
}

// ============================ farvetest ============================
// Viser 7 rene farver med navn + RGB, så fejl i farverækkefølge/inversion
// kan aflæses direkte: "feltet mærket ROED viser <farve>".
void drawColorTest() {
  uiGauge = false;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("FARVETEST", 8, 4, 2);

  struct Row { const char* n; uint8_t r, g, b; };
  static const Row rows[7] = {
    {"ROED",    255,   0,   0},
    {"GROEN",     0, 255,   0},
    {"BLAA",      0,   0, 255},
    {"GUL",     255, 255,   0},
    {"CYAN",      0, 255, 255},
    {"MAGENTA", 255,   0, 255},
    {"HVID",    255, 255, 255},
  };
  const int y0 = 24, bh = 42;
  for (int i = 0; i < 7; i++) {
    int y = y0 + i * bh;
    // Venstre: sort felt med navn + RGB (altid læsbart, hvidt på sort)
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(rows[i].n, 8, y + 4, 4);
    char rgb[16];
    snprintf(rgb, sizeof(rgb), "%u,%u,%u", rows[i].r, rows[i].g, rows[i].b);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(rgb, 8, y + 26, 2);
    // Højre: farveprøve
    tft.fillRect(128, y + 2, 108, bh - 6, tft.color565(rows[i].r, rows[i].g, rows[i].b));
  }
}

void handleColorTest() {
  server.send(200, "text/html",
    F("<meta charset=utf-8><body style='font-family:system-ui,sans-serif;background:#11131a;color:#e9ecf2;padding:40px;text-align:center'>"
      "<h2>Farvetest vist</h2><p>Se CYD-sk\xC3\xA6rmen. Tryk \xE2\x80\x9DTilbage til gauge\xE2\x80\x9D n\xC3\xA5r du er f\xC3\xA6rdig.</p></body>"));
  drawColorTest();
}

void handleGauge() {
  server.send(200, "text/html",
    F("<meta charset=utf-8><body style='font-family:system-ui,sans-serif;background:#11131a;color:#e9ecf2;padding:40px;text-align:center'>"
      "<h2>Tilbage til gauge</h2></body>"));
  enterGaugeUI();
}

void startServer() {
  if (serverStarted) return;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/checkupdate", HTTP_GET, handleCheckUpdate);
  server.on("/doupdate", HTTP_POST, handleDoUpdate);
  server.on("/colortest", HTTP_POST, handleColorTest);
  server.on("/gauge", HTTP_POST, handleGauge);
  server.begin();
  serverStarted = true;
}

// ============================ Improv ============================
void onImprovConnected(const char* ssid, const char* password) {
  saveWiFi(ssid, password);
  startServer();
  if (!uiGauge) enterGaugeUI();   // skift fra provisionerings-skærm til gauge
}

// ============================ setup/loop ============================
void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(0);
  u8f.begin(tft);                // Unicode-fonte klar
  cx = tft.width() / 2;
  cy = 150;
  rOuter = 114;
  rInner = 86;    // bredere farvebånd (28 px) for bedre aflæsning

  loadConfig();

  improvSerial.setDeviceInfo(
    ImprovTypes::ChipFamily::CF_ESP32,
    FW_NAME, FW_VERSION_SEM, "CYD-TempGauge",
    "http://{LOCAL_IPV4}/");          // browseren åbner konfig-siden efter WiFi
  improvSerial.onImprovConnected(onImprovConnected);

  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setCallback(onMqtt);

  WiFi.mode(WIFI_STA);
  if (cfg.ssid.length()) {
    showMessage("WiFi...", cfg.ssid.c_str());
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      improvSerial.handleSerial();
      delay(50);
    }
  } else {
    showMessage("Afventer WiFi", "Saet op via flasher-siden");
  }

  if (WiFi.status() == WL_CONNECTED) {
    startServer();
    enterGaugeUI();
  }
}

void loop() {
  improvSerial.handleSerial();

  if (WiFi.status() == WL_CONNECTED && !serverStarted) startServer();
  if (serverStarted) server.handleClient();

  if (!uiGauge) { delay(10); return; }

  if (WiFi.status() == WL_CONNECTED && cfg.mqttHost.length()) {
    if (!mqtt.connected()) {
      mqttUp = false;
      if (millis() - lastReconnect > 3000) {
        lastReconnect = millis();
        connectMqtt();
        drawStatus();
      }
    } else {
      mqtt.loop();
    }
  }

  bool stale = haveData && (millis() - lastMsgMs > STALE_MS);
  static bool shownStale = false;
  if (haveData && (fabs(lastTemp - shownTemp) >= 0.05f || stale != shownStale)) {
    shownTemp  = lastTemp;
    shownStale = stale;
    drawValue(shownTemp, stale);
    drawStatus();
  }

  delay(50);
}

/*
  ---- Hvis temperaturen kommer som JSON ----
  Tilføj til lib_deps i platformio.ini:  bblanchon/ArduinoJson @ ^7
  og erstat atof-linjen i onMqtt() med:

    #include <ArduinoJson.h>
    JsonDocument doc;
    if (deserializeJson(doc, buf, len) == DeserializationError::Ok)
      lastTemp = doc["temperature"] | NAN;   // tilpas nøglenavnet
*/
