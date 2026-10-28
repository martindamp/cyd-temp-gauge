/*
  CYD (ESP32-2432S028R) - MQTT temperature gauge   [PlatformIO]
  ----------------------------------------------------------------
  Shows the temperature from an MQTT topic as a coloured ring gauge.

  Configuration needs no hardcoded details:
    - WiFi : via Improv Wi-Fi (Serial) from the flasher page in the browser
    - MQTT + scale : via the device's own config page (http://<device-ip>/)
  Everything is stored in NVS (Preferences) and survives a restart.

  TFT_eSPI configuration lives in platformio.ini (build_flags).
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
#include <U8g2_for_TFT_eSPI.h>  // Unicode fonts (accented chars, degree sign)
#include <ImprovWiFiLibrary.h>
#include "ota_certs.h"          // root CAs for real HTTPS validation

// The semantic version is injected by GitHub Actions from the VERSION file
// (src/build_version.h). A local build without that file uses 0.0.0-dev.
#if __has_include("build_version.h")
  #include "build_version.h"
#endif
#ifndef FW_VERSION_SEM
  #define FW_VERSION_SEM "0.0.0-dev"
#endif

#define FW_NAME        "CYD Temp Gauge"
#define MQTT_CLIENTID  "cyd-temp-gauge"

// GitHub Pages - source of OTA updates
#define PAGES_BASE     "https://martindamp.github.io/cyd-temp-gauge/"
#define OTA_BIN_URL    PAGES_BASE "firmware/firmware.bin"
#define OTA_VER_URL    PAGES_BASE "version.txt"

// Defaults until the device is configured
#define DEF_MQTT_PORT  1883
#define DEF_MQTT_TOPIC "home/room/temperature"
#define DEF_TEMP_MIN   0.0f
#define DEF_TEMP_MAX   40.0f
#define STALE_MS       60000UL

TFT_eSPI     tft = TFT_eSPI();
U8g2_for_TFT_eSPI u8f;          // Unicode text (title + unit)
WiFiClient   net;
PubSubClient mqtt(net);
WebServer    server(80);
Preferences  prefs;
ImprovWiFi   improvSerial(&Serial);

// ---- runtime configuration ----
struct Config {
  String   ssid, pass;
  String   mqttHost, mqttUser, mqttPass, mqttTopic;
  uint16_t mqttPort;
  float    tmin, tmax;
  String   title, unit;
} cfg;

// ---- gauge geometry (portrait 240x320) ----
int cx, cy, rOuter, rInner;
const int START_ANGLE = 30;
const int END_ANGLE   = 330;

volatile float lastTemp = NAN;
float    shownTemp  = -1000;
bool     haveData   = false;
uint32_t lastMsgMs  = 0, lastReconnect = 0;
bool     mqttUp     = false;
bool     serverStarted = false;
bool     uiGauge    = false;   // true when the gauge screen is active
bool     staleShown = false;   // whether the "stale data" marker is on screen now

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
  cfg.title     = prefs.getString("title", "TEMPERATURE");
  cfg.unit      = prefs.getString("unit", "\xC2\xB0""C");   // UTF-8 "°C"
  prefs.end();
}

void saveWiFi(const char* ssid, const char* pass) {
  prefs.begin("cfg", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

// ============================ drawing ============================
// Empty/off track: very dark, so any lit colour stands out sharply.
static inline uint16_t trackColor() { return tft.color565(28, 32, 44); }
// Stale data: a clearly muted grey - distinct from both track and live colour.
static inline uint16_t staleColor() { return tft.color565(130, 132, 148); }

// Three fixed colour bands by value range (lower/middle/upper third)
static inline uint16_t bandBlue()  { return tft.color565(0, 120, 255); }
static inline uint16_t bandGreen() { return tft.color565(30, 200, 40); }
static inline uint16_t bandRed()   { return tft.color565(255, 45, 30); }

// Draw the unit (e.g. "°C", "%", "hPa") centred via U8g2 - handles ° and accents.
void drawUnit(const String& u) {
  tft.fillRect(cx - 54, cy + 40, 108, 26, TFT_BLACK);   // clear (inside the ring hole)
  u8f.setFont(u8g2_font_helvB14_tf);
  u8f.setFontMode(1);
  u8f.setForegroundColor(TFT_LIGHTGREY);
  int w = u8f.getUTF8Width(u.c_str());
  u8f.drawUTF8(cx - w / 2, cy + 62, u.c_str());
}

void drawStatic() {
  tft.fillScreen(TFT_BLACK);
  // Title via U8g2 (accented characters work)
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
  // IP at the bottom, so the config page can be reached
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
  float b1 = START_ANGLE + sweep / 3.0f;        // blue|green boundary
  float b2 = START_ANGLE + 2.0f * sweep / 3.0f; // green|red boundary

  // Whole ring as a dark track (one arc - no seams)
  tft.drawSmoothArc(cx, cy, rOuter, rInner, START_ANGLE, END_ANGLE, trackColor(), TFT_BLACK, true);

  // Lit bands: 3 fixed colours by value thirds, each as ONE arc.
  // roundEnds=true -> rounded, COLOURED ends (not dark track ends at the tips).
  // Bands are drawn in order and meet, so a later band covers the previous edge.
  auto band = [&](float a0, float a1, uint16_t c) {
    if (a1 > a0 + 0.5f)
      tft.drawSmoothArc(cx, cy, rOuter, rInner, (int)lroundf(a0), (int)lroundf(a1), c, TFT_BLACK, true);
  };
  if (fillAngle > START_ANGLE) band(START_ANGLE, fminf(fillAngle, b1), bandBlue());
  if (fillAngle > b1)          band(b1,          fminf(fillAngle, b2), bandGreen());
  if (fillAngle > b2)          band(b2,          fillAngle,            bandRed());

  char buf[8];
  dtostrf(temp, 0, 1, buf);
  // Wider clear (inside the hole), so digits from a larger number don't linger
  tft.fillRect(cx - 80, cy - 30, 160, 60, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(stale ? staleColor() : TFT_WHITE, TFT_BLACK);
  tft.drawString(buf, cx, cy, 7);

  drawUnit(cfg.unit);

  // Stale-data marker in the open centre of the ring (above the number) - no arc here.
  // Drawn ONLY when the text is shown; cleared once when data becomes fresh again,
  // so there is no permanent black bar.
  if (stale) {
    tft.fillRect(cx - 54, cy - 60, 108, 18, TFT_BLACK);
    tft.setTextColor(staleColor(), TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("stale data", cx, cy - 51, 2);
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
  shownTemp = -1000;                 // force a full redraw next loop
  if (haveData) {
    bool stale = (millis() - lastMsgMs > STALE_MS);
    drawValue(lastTemp, stale);      // redraw immediately with the known value
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
  // A plain number is expected, e.g. "21.5". JSON? -> see the note at the bottom.
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

// ============================ config web page ============================
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
  h += F("<!doctype html><html lang=en><head><meta charset=utf-8>"
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
  h += "<label>User (optional)</label><input name=muser value=\"" + esc(cfg.mqttUser) + "\">";
  h += "<label>Password (optional)</label><input name=mpass type=password value=\"" + esc(cfg.mqttPass) + "\">";
  h += "<div class=row><div><label>Scale min</label><input name=tmin type=number step=any value=\"" + String(cfg.tmin, 0) + "\"></div>";
  h += "<div><label>Scale max</label><input name=tmax type=number step=any value=\"" + String(cfg.tmax, 0) + "\"></div></div>";
  h += "<div class=row><div><label>Title</label><input name=title maxlength=16 value=\"" + esc(cfg.title) + "\"></div>";
  h += "<div><label>Unit</label><input name=unit maxlength=6 value=\"" + esc(cfg.unit) + "\" placeholder='&deg;C'></div></div>";
  h += F("<button>Save and restart</button></form>");

  // --- OTA section ---
  h += F("<div style='max-width:420px;margin-top:28px;border-top:1px solid #2a2e3a;padding-top:18px'>"
         "<h2 style='font-size:1.1rem'>Firmware</h2>");
  h += "<p style='color:#9aa3b2;font-size:.9rem'>Current version: <code style='color:#ffcb2d'>";
  h += FW_VERSION_SEM;
  h += F("</code></p>"
         "<button type=button id=chk onclick=checkUpd() style='width:100%;padding:12px;border:0;border-radius:8px;background:#2a2e3a;color:#e9ecf2;font-weight:700;font-size:1rem;cursor:pointer'>Check for update</button>"
         "<div id=ures style='margin-top:12px;font-size:.92rem;color:#9aa3b2'></div>"
         "<button type=button id=upd onclick=doUpd() style='display:none;width:100%;margin-top:12px;padding:12px;border:0;border-radius:8px;background:#35c46a;color:#05230f;font-weight:700;font-size:1rem;cursor:pointer'>Update now</button>"
         "</div>"
         // --- Diagnostics ---
         "<div style='max-width:420px;margin-top:28px;border-top:1px solid #2a2e3a;padding-top:18px'>"
         "<h2 style='font-size:1.1rem'>Diagnostics</h2>"
         "<div class=row>"
         "<div><button type=button onclick=\"fetch('/colortest',{method:'POST'})\" style='width:100%;padding:12px;border:0;border-radius:8px;background:#2a2e3a;color:#e9ecf2;font-weight:700;font-size:1rem;cursor:pointer'>Show colour test</button></div>"
         "<div><button type=button onclick=\"fetch('/gauge',{method:'POST'})\" style='width:100%;padding:12px;border:0;border-radius:8px;background:#2a2e3a;color:#e9ecf2;font-weight:700;font-size:1rem;cursor:pointer'>Back to gauge</button></div>"
         "</div></div>"
         "<script>"
         "async function checkUpd(){var b=document.getElementById('chk'),r=document.getElementById('ures'),u=document.getElementById('upd');"
         "b.disabled=true;b.textContent='Checking\xE2\x80\xA6';r.textContent='';u.style.display='none';"
         "try{var d=await (await fetch('/checkupdate')).json();"
         "if(!d.latest){r.textContent='Could not reach GitHub.';}"
         "else if(d.update){r.innerHTML='New version available: <code style=\"color:#ffcb2d\">'+d.latest+'</code>';u.style.display='block';}"
         "else{r.textContent='You already have the latest version.';}"
         "}catch(e){r.textContent='Error: '+e;}b.disabled=false;b.textContent='Check for update';}"
         "async function doUpd(){var u=document.getElementById('upd'),r=document.getElementById('ures');"
         "u.disabled=true;u.textContent='Starting\xE2\x80\xA6';r.innerHTML='Updating \xE2\x80\x94 watch the CYD screen. The device will restart itself.';"
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
      "<h2>Saved \xE2\x9C\x93</h2><p>The device is restarting\xE2\x80\xA6</p></body>"));
  delay(500);
  ESP.restart();
}

// ============================ OTA ============================
// Make sure the clock is correct (NTP) - TLS cert validation needs it
bool ensureTime() {
  if (time(nullptr) > 1700000000) return true;   // already set
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < 8000) delay(100);
  return time(nullptr) > 1700000000;
}

// Parse "MAJOR.MINOR.PATCH" -> a comparable number. Pre-release suffix is ignored.
long semverNum(const String& v) {
  int a = 0, b = 0, c = 0;
  sscanf(v.c_str(), "%d.%d.%d", &a, &b, &c);
  return (long)a * 1000000L + (long)b * 1000L + c;
}

String fetchLatestVersion() {
  if (WiFi.status() != WL_CONNECTED) return "";
  ensureTime();
  WiFiClientSecure sc;
  sc.setCACert(OTA_ROOT_CAS);       // real cert validation against known roots
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
  // Only a newer version is offered - never a downgrade
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
  tft.drawString("Updating firmware", cx, cy - 40, 4);

  Update.onProgress(drawOtaProgress);

  ensureTime();
  WiFiClientSecure sc;
  sc.setCACert(OTA_ROOT_CAS);       // real cert validation
  sc.setTimeout(15000);

  httpUpdate.rebootOnUpdate(true);  // restart automatically into the new firmware
  t_httpUpdate_return r = httpUpdate.update(sc, OTA_BIN_URL);

  // Only reached if it did NOT succeed (on success the device restarts itself)
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  if (r == HTTP_UPDATE_NO_UPDATES) {
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Already up to date", cx, cy, 4);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Update failed", cx, cy - 12, 4);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(httpUpdate.getLastErrorString(), cx, cy + 16, 2);
  }
  delay(4000);
  enterGaugeUI();
}

void handleDoUpdate() {
  server.send(200, "text/html",
    F("<meta charset=utf-8><body style='font-family:system-ui,sans-serif;background:#11131a;color:#e9ecf2;padding:40px;text-align:center'>"
      "<h2>Updating\xE2\x80\xA6</h2><p>Watch the screen. The device will restart when done.</p></body>"));
  delay(200);
  runOta();
}

// ============================ colour test ============================
// Shows 7 pure colours with name + RGB, so a colour-order/inversion fault
// can be read off directly: "the field labelled RED shows <colour>".
void drawColorTest() {
  uiGauge = false;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("COLOUR TEST", 8, 4, 2);

  struct Row { const char* n; uint8_t r, g, b; };
  static const Row rows[7] = {
    {"RED",     255,   0,   0},
    {"GREEN",     0, 255,   0},
    {"BLUE",      0,   0, 255},
    {"YELLOW",  255, 255,   0},
    {"CYAN",      0, 255, 255},
    {"MAGENTA", 255,   0, 255},
    {"WHITE",   255, 255, 255},
  };
  const int y0 = 24, bh = 42;
  for (int i = 0; i < 7; i++) {
    int y = y0 + i * bh;
    // Left: black field with name + RGB (always readable, white on black)
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(rows[i].n, 8, y + 4, 4);
    char rgb[16];
    snprintf(rgb, sizeof(rgb), "%u,%u,%u", rows[i].r, rows[i].g, rows[i].b);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(rgb, 8, y + 26, 2);
    // Right: colour swatch
    tft.fillRect(128, y + 2, 108, bh - 6, tft.color565(rows[i].r, rows[i].g, rows[i].b));
  }
}

void handleColorTest() {
  server.send(200, "text/html",
    F("<meta charset=utf-8><body style='font-family:system-ui,sans-serif;background:#11131a;color:#e9ecf2;padding:40px;text-align:center'>"
      "<h2>Colour test shown</h2><p>See the CYD screen. Press \"Back to gauge\" when done.</p></body>"));
  drawColorTest();
}

void handleGauge() {
  server.send(200, "text/html",
    F("<meta charset=utf-8><body style='font-family:system-ui,sans-serif;background:#11131a;color:#e9ecf2;padding:40px;text-align:center'>"
      "<h2>Back to gauge</h2></body>"));
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
  if (!uiGauge) enterGaugeUI();   // switch from the provisioning screen to the gauge
}

// ============================ setup/loop ============================
void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(0);
  u8f.begin(tft);                // Unicode fonts ready
  cx = tft.width() / 2;
  cy = 150;
  rOuter = 114;
  rInner = 86;    // wider colour band (28 px) for easier reading

  loadConfig();

  improvSerial.setDeviceInfo(
    ImprovTypes::ChipFamily::CF_ESP32,
    FW_NAME, FW_VERSION_SEM, "CYD-TempGauge",
    "http://{LOCAL_IPV4}/");          // the browser opens the config page after WiFi
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
    showMessage("Waiting for WiFi", "Set up via the flasher page");
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
  ---- If the temperature arrives as JSON ----
  Add to lib_deps in platformio.ini:  bblanchon/ArduinoJson @ ^7
  and replace the atof line in onMqtt() with:

    #include <ArduinoJson.h>
    JsonDocument doc;
    if (deserializeJson(doc, buf, len) == DeserializationError::Ok)
      lastTemp = doc["temperature"] | NAN;   // adjust the key name
*/
