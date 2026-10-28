/*
  CYD (ESP32-2432S028R) - MQTT dashboard   [PlatformIO]
  ----------------------------------------------------------------
  Shows up to 6 components on one screen, laid out in an adaptive grid.

  Component types:
    - gauge   : coloured ring gauge for a numeric MQTT topic (title, topic, min, max, unit)
    - counter : large number from a numeric MQTT topic (title, topic, unit)
    - text    : static text, no text, or dynamic text from an MQTT topic (title, text/topic)

  Component layout is configured at BUILD time in dashboard.json (embedded as
  DASHBOARD_JSON by tools/gen_dashboard.py).

  WiFi and the MQTT broker are configured at RUNTIME, with no hardcoded details:
    - WiFi  : via Improv Wi-Fi (Serial) from the flasher page in the browser
    - broker: via the device's own config page (http://<device-ip>/)
  These are stored in NVS (Preferences) and survive a restart.

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
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>  // Unicode fonts (accented chars, degree sign)
#include <ImprovWiFiLibrary.h>
#include "ota_certs.h"             // root CAs for real HTTPS validation
#include "dashboard_generated.h"   // DASHBOARD_JSON (generated from dashboard.json)

// The semantic version is injected by GitHub Actions from the VERSION file.
#if __has_include("build_version.h")
  #include "build_version.h"
#endif
#ifndef FW_VERSION_SEM
  #define FW_VERSION_SEM "0.0.0-dev"
#endif

#define FW_NAME        "CYD Dashboard"
#define MQTT_CLIENTID  "cyd-dashboard"

// GitHub Pages - source of OTA updates
#define PAGES_BASE     "https://martindamp.github.io/cyd-temp-gauge/"
#define OTA_BIN_URL    PAGES_BASE "firmware/firmware.bin"
#define OTA_VER_URL    PAGES_BASE "version.txt"

#define DEF_MQTT_PORT  1883
#define STALE_MS       60000UL
#define MAX_COMPONENTS 6
#define GRID_H         304         // screen height used by the grid; bottom bar is the rest

// U8g2 fonts (Latin-1: accents + degree sign)
#define FONT_TITLE  u8g2_font_helvB10_tf
#define FONT_SMALL  u8g2_font_helvB08_tf
#define FONT_TEXT   u8g2_font_helvR12_tf

TFT_eSPI     tft = TFT_eSPI();
U8g2_for_TFT_eSPI u8f;
WiFiClient   net;
PubSubClient mqtt(net);
WebServer    server(80);
Preferences  prefs;
ImprovWiFi   improvSerial(&Serial);

// ---- runtime configuration (broker + WiFi) ----
struct Config {
  String   ssid, pass;
  String   mqttHost, mqttUser, mqttPass;
  uint16_t mqttPort;
} cfg;

// ---- dashboard components (build-time layout) ----
enum CompType { GAUGE, COUNTER, TEXT };
struct Component {
  CompType type;
  String   title;
  String   topic;       // "" = no topic
  String   staticText;  // for text components
  String   unit;
  float    vmin, vmax;  // for gauge
  // runtime state:
  bool     haveData;
  String   value;       // latest payload (trimmed)
  float    num;         // parsed numeric value
  uint32_t lastMs;
  bool     dirty;       // needs redraw (new data)
  bool     wasStale;    // stale state last drawn
};
Component comps[MAX_COMPONENTS];
int       nComps = 0;

// ---- UI state ----
bool     uiDash     = false;   // true when the dashboard is on screen
bool     serverStarted = false;
bool     mqttUp     = false;
uint32_t lastReconnect = 0;
int      cx, cy;               // screen centre (used by full-screen messages)

// ============================ config ============================
void loadConfig() {
  prefs.begin("cfg", true);
  cfg.ssid     = prefs.getString("ssid", "");
  cfg.pass     = prefs.getString("pass", "");
  cfg.mqttHost = prefs.getString("mhost", "");
  cfg.mqttPort = prefs.getUShort("mport", DEF_MQTT_PORT);
  cfg.mqttUser = prefs.getString("muser", "");
  cfg.mqttPass = prefs.getString("mpass", "");
  prefs.end();
}

void saveWiFi(const char* ssid, const char* pass) {
  prefs.begin("cfg", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

void loadDashboard() {
  nComps = 0;
  JsonDocument doc;
  if (deserializeJson(doc, DASHBOARD_JSON)) return;   // leave empty on parse error
  for (JsonVariant v : doc["components"].as<JsonArray>()) {
    if (nComps >= MAX_COMPONENTS) break;
    JsonObject c = v.as<JsonObject>();
    Component& k = comps[nComps];
    String t = (const char*)(c["type"] | "text");
    k.type       = (t == "gauge") ? GAUGE : (t == "counter") ? COUNTER : TEXT;
    k.title      = (const char*)(c["title"] | "");
    k.topic      = (const char*)(c["topic"] | "");
    k.staticText = (const char*)(c["text"]  | "");
    k.unit       = (const char*)(c["unit"]  | "");
    k.vmin       = c["min"] | 0.0f;
    k.vmax       = c["max"] | 100.0f;
    k.haveData   = false;
    k.value      = "";
    k.num        = 0;
    k.lastMs     = 0;
    k.dirty      = false;
    k.wasStale   = false;
    nComps++;
  }
}

// ============================ colours ============================
static inline uint16_t trackColor() { return tft.color565(28, 32, 44); }
static inline uint16_t staleColor() { return tft.color565(130, 132, 148); }
static inline uint16_t bandBlue()   { return tft.color565(0, 120, 255); }
static inline uint16_t bandGreen()  { return tft.color565(30, 200, 40); }
static inline uint16_t bandRed()    { return tft.color565(255, 45, 30); }

// ============================ layout ============================
// Cell rectangle for component i of n, within a 240 x GRID_H area.
void cellRect(int i, int n, int& x, int& y, int& w, int& h) {
  int cols, rows;
  if      (n <= 1) { cols = 1; rows = 1; }
  else if (n == 2) { cols = 1; rows = 2; }
  else if (n == 3) { cols = 1; rows = 3; }
  else if (n == 4) { cols = 2; rows = 2; }
  else             { cols = 2; rows = 3; }   // 5 or 6
  int cw = 240 / cols, ch = GRID_H / rows;
  int col = i % cols, row = i / cols;
  x = col * cw; y = row * ch; w = cw; h = ch;
}

// ============================ drawing helpers ============================
// Draw a numeric string centred, picking the largest TFT font that fits.
void drawBigNumber(int ncx, int ncy, const String& s, int maxW, int maxH, uint16_t color) {
  const uint8_t fonts[] = {7, 4, 2};   // 7-seg (48px), medium (26px), small (16px)
  uint8_t chosen = 2;
  for (uint8_t f : fonts) {
    if (tft.textWidth(s, f) <= maxW && tft.fontHeight(f) <= maxH) { chosen = f; break; }
  }
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(s, ncx, ncy, chosen);
}

// Draw a ring gauge with three colour bands by value thirds.
void drawRing(int gcx, int gcy, int rO, int rI, float vmin, float vmax, float value, bool have) {
  const int A0 = 30, A1 = 330;
  tft.drawSmoothArc(gcx, gcy, rO, rI, A0, A1, trackColor(), TFT_BLACK, true);
  if (!have) return;
  float span = vmax - vmin;
  float frac = (span != 0) ? constrain((value - vmin) / span, 0.0f, 1.0f) : 0.0f;
  float fill = A0 + frac * (A1 - A0);
  float b1 = A0 + (A1 - A0) / 3.0f;
  float b2 = A0 + 2.0f * (A1 - A0) / 3.0f;
  auto band = [&](float a, float b, uint16_t c) {
    if (b > a + 0.5f) tft.drawSmoothArc(gcx, gcy, rO, rI, (int)lroundf(a), (int)lroundf(b), c, TFT_BLACK, true);
  };
  if (fill > A0) band(A0, fminf(fill, b1), bandBlue());
  if (fill > b1) band(b1, fminf(fill, b2), bandGreen());
  if (fill > b2) band(b2, fill,            bandRed());
}

// Word-wrap text into a box, centred, using U8g2 (accents OK).
void drawWrappedText(int x, int y, int w, int h, const String& text, uint16_t color) {
  if (!text.length()) return;
  u8f.setFont(FONT_TEXT);
  u8f.setFontMode(1);
  u8f.setForegroundColor(color);
  const int lineH = 15;
  int maxLines = h / lineH; if (maxLines < 1) maxLines = 1;
  if (maxLines > 8) maxLines = 8;
  String lines[8]; int nl = 0;
  String cur = "", rem = text;
  while (rem.length() && nl < maxLines) {
    int sp = rem.indexOf(' ');
    String word = (sp < 0) ? rem : rem.substring(0, sp);
    String test = cur.length() ? cur + " " + word : word;
    if (cur.length() == 0 || u8f.getUTF8Width(test.c_str()) <= w) {
      cur = test;
    } else {
      lines[nl++] = cur; cur = word;
    }
    rem = (sp < 0) ? "" : rem.substring(sp + 1);
  }
  if (cur.length() && nl < maxLines) lines[nl++] = cur;
  int totalH = nl * lineH;
  int sy = y + (h - totalH) / 2 + 11;
  for (int k = 0; k < nl; k++) {
    int lw = u8f.getUTF8Width(lines[k].c_str());
    u8f.drawUTF8(x + (w - lw) / 2, sy + k * lineH, lines[k].c_str());
  }
}

// ============================ component rendering ============================
void drawComponent(int idx, bool clearCell) {
  Component& c = comps[idx];
  int x, y, w, h; cellRect(idx, nComps, x, y, w, h);
  if (clearCell) tft.fillRect(x, y, w, h, TFT_BLACK);
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 6, trackColor());

  const int pad = 6;
  int ix = x + pad, iy = y + pad, iw = w - 2 * pad, ih = h - 2 * pad;

  // Title
  int titleH = 0;
  if (c.title.length()) {
    u8f.setFont(FONT_TITLE);
    u8f.setFontMode(1);
    u8f.setForegroundColor(TFT_LIGHTGREY);
    int tw = u8f.getUTF8Width(c.title.c_str());
    u8f.drawUTF8(ix + (iw - tw) / 2, iy + 11, c.title.c_str());
    titleH = 16;
  }
  int by = iy + titleH, bh = ih - titleH;
  if (bh < 10) return;

  bool stale = c.haveData && (millis() - c.lastMs > STALE_MS);
  uint16_t numCol = !c.haveData ? TFT_DARKGREY : (stale ? staleColor() : TFT_WHITE);

  if (c.type == GAUGE) {
    int rO = min(iw, bh) / 2 - 2; if (rO < 14) rO = 14;
    int rI = rO - max(5, rO * 28 / 100);
    int gcx = ix + iw / 2, gcy = by + bh / 2;
    drawRing(gcx, gcy, rO, rI, c.vmin, c.vmax, c.num, c.haveData && !stale);
    String s = c.haveData ? c.value : "--";
    bool showUnit = c.unit.length() && rI > 22;
    int numMaxH = showUnit ? (int)(rI * 1.1f) : (int)(2 * rI * 0.85f);
    drawBigNumber(gcx, gcy - (showUnit ? 6 : 0), s, (int)(2 * rI * 0.92f), numMaxH, numCol);
    if (showUnit) {
      u8f.setFont(FONT_SMALL);
      u8f.setForegroundColor(TFT_LIGHTGREY);
      int uw = u8f.getUTF8Width(c.unit.c_str());
      u8f.drawUTF8(gcx - uw / 2, gcy + rI - 3, c.unit.c_str());
    }
  } else if (c.type == COUNTER) {
    String s = c.haveData ? c.value : "--";
    int uH = c.unit.length() ? 14 : 0;
    drawBigNumber(ix + iw / 2, by + (bh - uH) / 2, s, iw - 4, bh - uH - 2, numCol);
    if (c.unit.length()) {
      u8f.setFont(FONT_SMALL);
      u8f.setForegroundColor(TFT_LIGHTGREY);
      int uw = u8f.getUTF8Width(c.unit.c_str());
      u8f.drawUTF8(ix + (iw - uw) / 2, by + bh - 2, c.unit.c_str());
    }
  } else { // TEXT
    String body = c.topic.length() ? (c.haveData ? c.value : "") : c.staticText;
    drawWrappedText(ix, by, iw, bh, body, stale ? staleColor() : TFT_WHITE);
  }
}

// ============================ status bar + full dashboard ============================
void drawStatusBar() {
  int y = GRID_H;
  tft.fillRect(0, y, 240, 320 - y, TFT_BLACK);
  uint16_t c = (WiFi.status() == WL_CONNECTED && mqttUp) ? TFT_GREEN : TFT_RED;
  tft.fillCircle(232, y + 8, 4, c);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(WiFi.localIP().toString(), 6, y + 8, 2);
  }
}

void drawDashboard() {
  tft.fillScreen(TFT_BLACK);
  for (int i = 0; i < nComps; i++) { comps[i].dirty = false; drawComponent(i, false); }
  drawStatusBar();
  uiDash = true;
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

// ============================ MQTT ============================
void onMqtt(char* topic, byte* payload, unsigned int len) {
  char buf[64];
  if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
  memcpy(buf, payload, len);
  buf[len] = '\0';
  String t = topic;
  String val = buf; val.trim();
  for (int i = 0; i < nComps; i++) {
    if (comps[i].topic.length() && comps[i].topic == t) {
      comps[i].value    = val;
      comps[i].num      = atof(val.c_str());
      comps[i].haveData = true;
      comps[i].lastMs   = millis();
      comps[i].dirty    = true;
    }
  }
}

void subscribeTopics() {
  for (int i = 0; i < nComps; i++) {
    if (comps[i].topic.length()) mqtt.subscribe(comps[i].topic.c_str());
  }
}

bool connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || cfg.mqttHost.isEmpty()) return false;
  bool ok = cfg.mqttUser.length()
            ? mqtt.connect(MQTT_CLIENTID, cfg.mqttUser.c_str(), cfg.mqttPass.c_str())
            : mqtt.connect(MQTT_CLIENTID);
  if (ok) subscribeTopics();
  mqttUp = ok;
  return ok;
}

// ============================ config web page ============================
static String esc(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (char ch : s) {
    if (ch == '&') o += "&amp;";
    else if (ch == '"') o += "&quot;";
    else if (ch == '<') o += "&lt;";
    else o += ch;
  }
  return o;
}

void handleRoot() {
  String h;
  h.reserve(3200);
  h += F("<!doctype html><html lang=en><head><meta charset=utf-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>CYD Dashboard</title><style>"
         "body{font-family:system-ui,sans-serif;background:#11131a;color:#e9ecf2;margin:0;padding:24px;display:flex;justify-content:center}"
         "main{width:100%;max-width:420px}form{width:100%}h1{font-size:1.3rem}h2{font-size:1.1rem}"
         "label{display:block;margin:14px 0 4px;font-size:.9rem;color:#9aa3b2}"
         "input{width:100%;padding:10px;border-radius:8px;border:1px solid #2a2e3a;background:#0d0f15;color:#e9ecf2;font-size:1rem}"
         ".row{display:flex;gap:10px}.row>div{flex:1}"
         "button{margin-top:20px;width:100%;padding:12px;border:0;border-radius:8px;background:#ffcb2d;color:#1a1500;font-weight:700;font-size:1rem;cursor:pointer}"
         ".sec{margin-top:28px;border-top:1px solid #2a2e3a;padding-top:18px}"
         "table{width:100%;border-collapse:collapse;font-size:.85rem}td{padding:4px 6px;border-bottom:1px solid #1c2029;color:#9aa3b2}"
         "code{color:#ffcb2d}"
         "</style></head><body><main><form method=POST action=/save>"
         "<h1>CYD Dashboard</h1>"
         "<h2>MQTT broker</h2>");
  h += "<label>Broker (host/IP)</label><input name=mhost value=\"" + esc(cfg.mqttHost) + "\" placeholder='192.168.1.10'>";
  h += "<div class=row><div><label>Port</label><input name=mport type=number value=\"" + String(cfg.mqttPort) + "\"></div></div>";
  h += "<label>User (optional)</label><input name=muser value=\"" + esc(cfg.mqttUser) + "\">";
  h += "<label>Password (optional)</label><input name=mpass type=password value=\"" + esc(cfg.mqttPass) + "\">";
  h += F("<button>Save and restart</button></form>");

  // Components (read-only; configured at build time in dashboard.json)
  h += F("<div class=sec><h2>Components</h2>"
         "<p style='color:#9aa3b2;font-size:.85rem'>Configured at build time in <code>dashboard.json</code>.</p>"
         "<table>");
  for (int i = 0; i < nComps; i++) {
    const char* tn = comps[i].type == GAUGE ? "gauge" : comps[i].type == COUNTER ? "counter" : "text";
    String ttl = comps[i].title.length() ? comps[i].title : String("(no title)");
    String tpc = comps[i].topic.length() ? comps[i].topic : (comps[i].type == TEXT ? String("static") : String("-"));
    h += "<tr><td>" + esc(ttl) + "</td><td><code>" + tn + "</code></td><td>" + esc(tpc) + "</td></tr>";
  }
  h += F("</table></div>");

  // OTA
  h += F("<div class=sec><h2>Firmware</h2>");
  h += "<p style='color:#9aa3b2;font-size:.9rem'>Current version: <code>";
  h += FW_VERSION_SEM;
  h += F("</code></p>"
         "<button type=button id=chk onclick=checkUpd() style='background:#2a2e3a;color:#e9ecf2'>Check for update</button>"
         "<div id=ures style='margin-top:12px;font-size:.92rem;color:#9aa3b2'></div>"
         "<button type=button id=upd onclick=doUpd() style='display:none;background:#35c46a;color:#05230f'>Update now</button>"
         "</div>"
         "<div class=sec><h2>Diagnostics</h2><div class=row>"
         "<div><button type=button onclick=\"fetch('/colortest',{method:'POST'})\" style='background:#2a2e3a;color:#e9ecf2'>Show colour test</button></div>"
         "<div><button type=button onclick=\"fetch('/dashboard',{method:'POST'})\" style='background:#2a2e3a;color:#e9ecf2'>Back to dashboard</button></div>"
         "</div></div>"
         "<script>"
         "async function checkUpd(){var b=document.getElementById('chk'),r=document.getElementById('ures'),u=document.getElementById('upd');"
         "b.disabled=true;b.textContent='Checking\xE2\x80\xA6';r.textContent='';u.style.display='none';"
         "try{var d=await (await fetch('/checkupdate')).json();"
         "if(!d.latest){r.textContent='Could not reach GitHub.';}"
         "else if(d.update){r.innerHTML='New version available: <code>'+d.latest+'</code>';u.style.display='block';}"
         "else{r.textContent='You already have the latest version.';}"
         "}catch(e){r.textContent='Error: '+e;}b.disabled=false;b.textContent='Check for update';}"
         "async function doUpd(){var u=document.getElementById('upd'),r=document.getElementById('ures');"
         "u.disabled=true;u.textContent='Starting\xE2\x80\xA6';r.innerHTML='Updating \xE2\x80\x94 watch the CYD screen. The device will restart itself.';"
         "try{await fetch('/doupdate',{method:'POST'});}catch(e){}}"
         "</script></main></body></html>");
  server.send(200, "text/html", h);
}

void handleSave() {
  prefs.begin("cfg", false);
  if (server.hasArg("mhost")) prefs.putString("mhost", server.arg("mhost"));
  if (server.hasArg("mport")) prefs.putUShort("mport", (uint16_t)server.arg("mport").toInt());
  if (server.hasArg("muser")) prefs.putString("muser", server.arg("muser"));
  if (server.hasArg("mpass")) prefs.putString("mpass", server.arg("mpass"));
  prefs.end();
  server.send(200, "text/html",
    F("<meta charset=utf-8><body style='font-family:system-ui,sans-serif;background:#11131a;color:#e9ecf2;padding:40px;text-align:center'>"
      "<h2>Saved \xE2\x9C\x93</h2><p>The device is restarting\xE2\x80\xA6</p></body>"));
  delay(500);
  ESP.restart();
}

// ============================ OTA ============================
bool ensureTime() {
  if (time(nullptr) > 1700000000) return true;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < 8000) delay(100);
  return time(nullptr) > 1700000000;
}

long semverNum(const String& v) {
  int a = 0, b = 0, c = 0;
  sscanf(v.c_str(), "%d.%d.%d", &a, &b, &c);
  return (long)a * 1000000L + (long)b * 1000L + c;
}

String fetchLatestVersion() {
  if (WiFi.status() != WL_CONNECTED) return "";
  ensureTime();
  WiFiClientSecure sc;
  sc.setCACert(OTA_ROOT_CAS);
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
  int w = 200, x = cx - w / 2, y = cy + 10, bh = 18;
  tft.drawRect(x, y, w, bh, TFT_LIGHTGREY);
  tft.fillRect(x + 2, y + 2, (w - 4) * pct / 100, bh - 4, TFT_GREEN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String(pct) + "%  ", cx, y - 18, 4);
}

void runOta() {
  uiDash = false;
  mqtt.disconnect();
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Updating firmware", cx, cy - 40, 4);

  Update.onProgress(drawOtaProgress);
  ensureTime();
  WiFiClientSecure sc;
  sc.setCACert(OTA_ROOT_CAS);
  sc.setTimeout(15000);
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = httpUpdate.update(sc, OTA_BIN_URL);

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
  drawDashboard();
}

void handleDoUpdate() {
  server.send(200, "text/html",
    F("<meta charset=utf-8><body style='font-family:system-ui,sans-serif;background:#11131a;color:#e9ecf2;padding:40px;text-align:center'>"
      "<h2>Updating\xE2\x80\xA6</h2><p>Watch the screen. The device will restart when done.</p></body>"));
  delay(200);
  runOta();
}

// ============================ colour test ============================
void drawColorTest() {
  uiDash = false;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("COLOUR TEST", 8, 4, 2);
  struct Row { const char* n; uint8_t r, g, b; };
  static const Row rows[7] = {
    {"RED", 255, 0, 0}, {"GREEN", 0, 255, 0}, {"BLUE", 0, 0, 255},
    {"YELLOW", 255, 255, 0}, {"CYAN", 0, 255, 255}, {"MAGENTA", 255, 0, 255},
    {"WHITE", 255, 255, 255},
  };
  const int y0 = 24, bh = 42;
  for (int i = 0; i < 7; i++) {
    int y = y0 + i * bh;
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(rows[i].n, 8, y + 4, 4);
    char rgb[16];
    snprintf(rgb, sizeof(rgb), "%u,%u,%u", rows[i].r, rows[i].g, rows[i].b);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(rgb, 8, y + 26, 2);
    tft.fillRect(128, y + 2, 108, bh - 6, tft.color565(rows[i].r, rows[i].g, rows[i].b));
  }
}

void handleColorTest() {
  server.send(200, "text/html",
    F("<meta charset=utf-8><body style='font-family:system-ui,sans-serif;background:#11131a;color:#e9ecf2;padding:40px;text-align:center'>"
      "<h2>Colour test shown</h2><p>See the CYD screen. Press \"Back to dashboard\" when done.</p></body>"));
  drawColorTest();
}

void handleDash() {
  server.send(200, "text/html",
    F("<meta charset=utf-8><body style='font-family:system-ui,sans-serif;background:#11131a;color:#e9ecf2;padding:40px;text-align:center'>"
      "<h2>Back to dashboard</h2></body>"));
  drawDashboard();
}

void startServer() {
  if (serverStarted) return;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/checkupdate", HTTP_GET, handleCheckUpdate);
  server.on("/doupdate", HTTP_POST, handleDoUpdate);
  server.on("/colortest", HTTP_POST, handleColorTest);
  server.on("/dashboard", HTTP_POST, handleDash);
  server.begin();
  serverStarted = true;
}

// ============================ Improv ============================
void onImprovConnected(const char* ssid, const char* password) {
  saveWiFi(ssid, password);
  startServer();
  if (!uiDash) drawDashboard();
}

// ============================ setup/loop ============================
void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(0);
  u8f.begin(tft);
  cx = tft.width() / 2;
  cy = tft.height() / 2;

  loadConfig();
  loadDashboard();

  improvSerial.setDeviceInfo(
    ImprovTypes::ChipFamily::CF_ESP32,
    FW_NAME, FW_VERSION_SEM, "CYD-Dashboard",
    "http://{LOCAL_IPV4}/");
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
    drawDashboard();
  }
}

void loop() {
  improvSerial.handleSerial();

  if (WiFi.status() == WL_CONNECTED && !serverStarted) startServer();
  if (serverStarted) server.handleClient();

  if (!uiDash) { delay(10); return; }

  static bool lastConn = false;
  if (WiFi.status() == WL_CONNECTED && cfg.mqttHost.length()) {
    if (!mqtt.connected()) {
      mqttUp = false;
      if (millis() - lastReconnect > 3000) {
        lastReconnect = millis();
        connectMqtt();
      }
    } else {
      mqtt.loop();
    }
  }
  bool conn = (WiFi.status() == WL_CONNECTED && mqttUp);
  if (conn != lastConn) { lastConn = conn; drawStatusBar(); }

  // Redraw a component when it gets new data or when its stale state changes.
  for (int i = 0; i < nComps; i++) {
    bool nowStale = comps[i].haveData && (millis() - comps[i].lastMs > STALE_MS);
    if (comps[i].dirty || nowStale != comps[i].wasStale) {
      comps[i].dirty = false;
      comps[i].wasStale = nowStale;
      drawComponent(i, true);
    }
  }

  delay(50);
}

/*
  ---- MQTT payloads ----
  gauge / counter : a plain number, e.g. "21.5"
  text (dynamic)  : any text, shown as received

  ---- If a value arrives as JSON ----
  Parse it in onMqtt() with ArduinoJson (already a dependency), e.g.:
    JsonDocument d;
    if (!deserializeJson(d, buf)) val = String((float)(d["temperature"] | 0.0f));
*/
