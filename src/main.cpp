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
#include <SPI.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>  // Unicode fonts (accented chars, degree sign)
#include <XPT2046_Touchscreen.h>   // CYD resistive touch (page navigation)
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
#define MAX_PAGES      6
#define MAX_PER_PAGE   6
#define MAX_COMPONENTS (MAX_PAGES * MAX_PER_PAGE)
#define GRID_H         286         // screen height used by the grid; the rest is the nav bar

// CYD resistive touch (XPT2046) on a separate SPI bus
#define XPT2046_IRQ   36
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CLK   25
#define XPT2046_CS    33
// Raw-to-pixel calibration (typical CYD values; tweak if taps feel off)
#define TOUCH_MINX    200
#define TOUCH_MAXX    3700
#define TOUCH_MINY    240
#define TOUCH_MAXY    3800

// U8g2 fonts (Latin-1: accents + degree sign)
#define FONT_TITLE  u8g2_font_helvB10_tf
#define FONT_SMALL  u8g2_font_helvB08_tf
#define FONT_TEXT   u8g2_font_helvR12_tf

TFT_eSPI     tft = TFT_eSPI();
U8g2_for_TFT_eSPI u8f;
SPIClass     touchSPI(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
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
enum CompType { GAUGE, COUNTER, TEXT, UPDATE, GRAPH };
enum UpdState { UPD_IDLE, UPD_CHECKING, UPD_UPTODATE, UPD_AVAILABLE };
struct Component {
  CompType type;
  String   title;
  String   topic;       // "" = no topic
  String   staticText;  // for text components
  String   unit;
  float    vmin, vmax;  // for gauge
  // update component state:
  int      updState;    // UpdState
  String   latestVer;
  // graph component:
  uint32_t interval;    // seconds between samples
  int      graphFirst;  // first series buffer slot, or -1
  int      graphN;      // number of series
  bool     graphFill;   // fill the area below the line (single-series look)
  uint32_t lastSampleMs;
  bool     backfill;    // request historical data on connect
  // placement (build-time):
  int      page;        // which page this component belongs to
  int      row, col;    // grid position; -1 = auto-place in the next free cell
  int      rowspan;     // how many rows tall (1-3)
  bool     expand;      // fill the rest of the row when the other column is free
  // computed layout:
  bool     placed;
  int      rx, ry, rw, rh;
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
int       nPages = 1;
int       currentPage = 0;

// Rolling sample buffers for graph series (ring buffers). A graph may have
// several series; each series owns one buffer slot. MAX_SERIES is the total
// number of series across all graphs; MAX_SERIES_PER_GRAPH caps one graph.
#define GRAPH_POINTS       120
#define MAX_SERIES         6
#define MAX_SERIES_PER_GRAPH 4
// Shared backfill topic prefix. A series with an "entity" requests history on
// <HISTORY_PREFIX>/get/<entity-path> and receives it on <HISTORY_PREFIX>/<entity-path>,
// where <entity-path> is the entity id with '.' replaced by '/'. One HA
// responder (e.g. a pyscript service) can then serve every graph/series.
#define HISTORY_PREFIX "home/history"
float    gBuf[MAX_SERIES][GRAPH_POINTS];
int      gCount[MAX_SERIES];
int      gHead[MAX_SERIES];
String   gTopic[MAX_SERIES];    // live value topic for this series
String   gHist[MAX_SERIES];     // backfill reply topic ("" = no backfill)
String   gReq[MAX_SERIES];      // backfill request topic
String   gLabel[MAX_SERIES];    // legend label
uint16_t gColor[MAX_SERIES];    // series colour (565)
float    gLast[MAX_SERIES];     // latest live value (shown in the legend)
bool     gHave[MAX_SERIES];     // a live value has arrived
int      gComp[MAX_SERIES];     // owning component index
// Aggregation of the values that arrive within one sample interval.
enum AggMode { AGG_AVG, AGG_MAX, AGG_MIN, AGG_LAST };
uint8_t  gAgg[MAX_SERIES];      // AggMode
float    gAcc[MAX_SERIES];      // running sum (avg) or extremum (max/min) this bucket
int      gAccN[MAX_SERIES];     // values accumulated this bucket
int      nSeries = 0;

static uint8_t parseAgg(const char* s) {
  String a = s ? s : "";
  if (a == "max") return AGG_MAX;
  if (a == "min") return AGG_MIN;
  if (a == "last") return AGG_LAST;
  return AGG_AVG;   // default
}
// Fold one incoming value into the current bucket for series si.
void aggAccumulate(int si, float v) {
  if (gAccN[si] == 0) { gAcc[si] = v; gAccN[si] = 1; return; }
  switch (gAgg[si]) {
    case AGG_MAX:  if (v > gAcc[si]) gAcc[si] = v; gAccN[si]++; break;
    case AGG_MIN:  if (v < gAcc[si]) gAcc[si] = v; gAccN[si]++; break;
    case AGG_LAST: gAcc[si] = v;                   gAccN[si]++; break;
    default:       gAcc[si] += v;                  gAccN[si]++; break;  // AVG
  }
}
// Resolve the bucket to a single value, then reset it.
float aggFlush(int si) {
  float out = (gAgg[si] == AGG_AVG && gAccN[si] > 0) ? gAcc[si] / gAccN[si] : gAcc[si];
  gAcc[si] = 0; gAccN[si] = 0;
  return out;
}

void graphPush(int gi, float v) {
  gBuf[gi][gHead[gi]] = v;
  gHead[gi] = (gHead[gi] + 1) % GRAPH_POINTS;
  if (gCount[gi] < GRAPH_POINTS) gCount[gi]++;
}
// idxFromOldest: 0 = oldest kept sample, n-1 = newest
float graphAt(int gi, int n, int idxFromOldest) {
  int start = (gHead[gi] - n + GRAPH_POINTS) % GRAPH_POINTS;
  return gBuf[gi][(start + idxFromOldest) % GRAPH_POINTS];
}

// ---- UI state ----
bool     uiDash     = false;   // true when the dashboard is on screen
bool     serverStarted = false;
bool     mqttUp     = false;
uint32_t lastReconnect = 0;
uint32_t lastTouchMs = 0;
int      cx, cy;               // screen centre (used by full-screen messages)

// forward declarations (OTA helpers are defined further down)
String fetchLatestVersion();
long   semverNum(const String& v);
void   runOta();
void   drawComponent(int idx, bool clearCell);
void   drawDashboard();

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

// "#RRGGBB" (or "RRGGBB") -> 565; returns false if not a valid hex colour.
static bool hexTo565(const String& s, uint16_t& out) {
  String h = s; h.trim();
  if (h.startsWith("#")) h = h.substring(1);
  if (h.length() != 6) return false;
  char* end = nullptr;
  long v = strtol(h.c_str(), &end, 16);
  if (end == h.c_str() || *end) return false;
  out = tft.color565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
  return true;
}

// Register one graph series into the next free buffer slot. Derives backfill
// topics with the same precedence used for single-topic graphs:
//   explicit history_topic/request_topic -> entity (shared scheme) -> topic/history.
// Returns the slot, or -1 if no slots remain.
static int addSeries(int compIdx, const String& topic, const String& entity,
                     const String& histOverride, const String& reqOverride,
                     const String& label, uint16_t color, bool backfill, uint8_t agg) {
  if (nSeries >= MAX_SERIES) return -1;
  int si = nSeries++;
  gCount[si] = 0; gHead[si] = 0; gHave[si] = false; gLast[si] = 0;
  gAgg[si]   = agg; gAcc[si] = 0; gAccN[si] = 0;
  gComp[si]  = compIdx;
  gTopic[si] = topic;
  gLabel[si] = label;
  gColor[si] = color;
  gHist[si]  = histOverride;
  gReq[si]   = reqOverride;
  if (backfill) {
    if (entity.length()) {
      String path = entity; path.replace('.', '/');
      if (!gHist[si].length()) gHist[si] = String(HISTORY_PREFIX) + "/" + path;
      if (!gReq[si].length())  gReq[si]  = String(HISTORY_PREFIX) + "/get/" + path;
    } else if (topic.length()) {
      if (!gHist[si].length()) gHist[si] = topic + "/history";
      if (!gReq[si].length())  gReq[si]  = topic + "/history/get";
    }
  }
  return si;
}

static void parseComponent(JsonObject c, int page) {
  Component& k = comps[nComps];
  String t = (const char*)(c["type"] | "text");
  k.type       = (t == "gauge") ? GAUGE : (t == "counter") ? COUNTER
               : (t == "update") ? UPDATE : (t == "graph") ? GRAPH : TEXT;
  k.updState   = UPD_IDLE;
  k.latestVer  = "";
  k.title      = (const char*)(c["title"] | "");
  k.topic      = (const char*)(c["topic"] | "");
  k.staticText = (const char*)(c["text"]  | "");
  k.unit       = (const char*)(c["unit"]  | "");
  k.vmin       = c["min"] | 0.0f;
  k.vmax       = c["max"] | ((k.type == GRAPH) ? NAN : 100.0f);  // NAN = auto-scale (graph)
  k.interval   = (uint32_t)(c["interval"] | 120);
  if (k.interval < 1) k.interval = 1;
  k.lastSampleMs = 0;
  k.graphFirst = -1;
  k.graphN     = 0;
  k.graphFill  = true;
  k.backfill   = c["backfill"] | false;   // opt-in; needs an HA responder
  if (k.type == GRAPH) {
    // Default colour palette, used in order for series without an explicit colour.
    const uint16_t palette[MAX_SERIES_PER_GRAPH] = {
      tft.color565(0, 150, 210),   // teal
      tft.color565(245, 185, 0),   // amber
      tft.color565(80, 205, 120),  // green
      tft.color565(235, 95, 180),  // magenta
    };
    uint8_t defAgg = parseAgg(c["agg"] | "avg");   // graph-level default aggregation
    JsonArray series = c["series"].as<JsonArray>();
    int added = 0;
    if (!series.isNull()) {
      // Multi-series graph: each entry has its own topic/entity/label/colour.
      for (JsonObject s : series) {
        if (added >= MAX_SERIES_PER_GRAPH) break;
        String stopic  = (const char*)(s["topic"]  | "");
        String sentity = (const char*)(s["entity"] | "");
        String shist   = (const char*)(s["history_topic"] | "");
        String sreq    = (const char*)(s["request_topic"] | "");
        String slabel  = (const char*)(s["label"] | "");
        if (!slabel.length()) slabel = (const char*)(s["title"] | "");
        if (!slabel.length() && stopic.length()) {           // default label: last topic segment
          int sl = stopic.lastIndexOf('/'); slabel = (sl >= 0) ? stopic.substring(sl + 1) : stopic;
        }
        uint16_t scol = palette[added];
        if (s["color"].is<const char*>()) hexTo565((const char*)s["color"], scol);
        uint8_t sagg = s["agg"].is<const char*>() ? parseAgg((const char*)s["agg"]) : defAgg;
        int si = addSeries(nComps, stopic, sentity, shist, sreq, slabel, scol, k.backfill, sagg);
        if (si < 0) break;
        if (k.graphFirst < 0) k.graphFirst = si;
        added++;
      }
      k.graphFill = c["fill"] | false;    // lines by default for multi-series
    } else {
      // Single-series graph from the component's own topic/entity.
      String label = (const char*)(c["title"] | "");
      int si = addSeries(nComps, (const char*)(c["topic"] | ""),
                         (const char*)(c["entity"] | ""),
                         (const char*)(c["history_topic"] | ""),
                         (const char*)(c["request_topic"] | ""),
                         label, palette[0], k.backfill, defAgg);
      if (si >= 0) { k.graphFirst = si; added = 1; }
      k.graphFill = c["fill"] | true;     // filled area by default for one series
    }
    k.graphN = added;
    k.topic  = "";   // graph data flows through its series, not the component topic
  }
  k.page       = page;
  k.row        = c["row"] | -1;
  k.col        = c["col"] | -1;
  k.rowspan    = constrain((int)(c["rowspan"] | 1), 1, 3);
  k.expand     = c["expand"] | false;
  k.placed     = false;
  k.rx = k.ry = k.rw = k.rh = 0;
  k.haveData   = false;
  k.value      = "";
  k.num        = 0;
  k.lastMs     = 0;
  k.dirty      = false;
  k.wasStale   = false;
  nComps++;
}

static void parsePage(JsonArray arr, int page) {
  if (arr.isNull()) return;
  int inPage = 0;
  for (JsonVariant v : arr) {
    if (nComps >= MAX_COMPONENTS || inPage >= MAX_PER_PAGE) break;
    parseComponent(v.as<JsonObject>(), page);
    inPage++;
  }
}

void loadDashboard() {
  nComps = 0; nPages = 1; currentPage = 0; nSeries = 0;
  JsonDocument doc;
  if (deserializeJson(doc, DASHBOARD_JSON)) return;   // leave empty on parse error

  JsonArray pages = doc["pages"].as<JsonArray>();
  if (!pages.isNull()) {
    int p = 0;
    for (JsonVariant pv : pages) {
      if (p >= MAX_PAGES) break;
      parsePage(pv["components"].as<JsonArray>(), p);
      p++;
    }
    nPages = (p > 0) ? p : 1;
  } else {
    // Backward-compatible single-page form: {"components":[...]}
    parsePage(doc["components"].as<JsonArray>(), 0);
    nPages = 1;
  }
}

// Fixed 3-row x 2-col grid per page. Components keep their assigned (row,col);
// those without a valid assignment are auto-placed into the next free cell.
// expand=true fills the row (both columns) when free; rowspan grows downward.
#define GRID_COLS 2
#define GRID_ROWS 3
void layoutDashboard() {
  int cw = 240 / GRID_COLS, ch = GRID_H / GRID_ROWS;
  for (int pg = 0; pg < nPages; pg++) {
    int grid[GRID_ROWS][GRID_COLS];
    for (int r = 0; r < GRID_ROWS; r++)
      for (int c = 0; c < GRID_COLS; c++) grid[r][c] = -1;

    auto freeBlock = [&](int r, int c, int rs, bool both) -> bool {
      if (r < 0 || c < 0 || c >= GRID_COLS || r + rs > GRID_ROWS) return false;
      for (int rr = r; rr < r + rs; rr++) {
        if (grid[rr][c] != -1) return false;
        if (both && grid[rr][1 - c] != -1) return false;
      }
      return true;
    };
    auto occupy = [&](int i, int r, int c, int rs, bool both) {
      for (int rr = r; rr < r + rs; rr++) { grid[rr][c] = i; if (both) grid[rr][1 - c] = i; }
      comps[i].row = r; comps[i].col = c;
      comps[i].rx = both ? 0 : c * cw;
      comps[i].rw = both ? GRID_COLS * cw : cw;
      comps[i].ry = r * ch;
      comps[i].rh = rs * ch;
      comps[i].placed = true;
    };

    // Pass 1: explicit placements (honouring rowspan + expand).
    for (int i = 0; i < nComps; i++) {
      if (comps[i].page != pg) continue;
      comps[i].placed = false;
      int r = comps[i].row, c = comps[i].col, rs = comps[i].rowspan;
      if (r < 0 || c < 0) continue;                       // auto-placed in pass 2
      if (comps[i].expand && freeBlock(r, c, rs, true))      occupy(i, r, c, rs, true);
      else if (freeBlock(r, c, rs, false))                   occupy(i, r, c, rs, false);
    }
    // Pass 2: auto-place the rest into the first free cell (single row).
    for (int i = 0; i < nComps; i++) {
      if (comps[i].page != pg || comps[i].placed) continue;
      for (int r = 0; r < GRID_ROWS && !comps[i].placed; r++)
        for (int c = 0; c < GRID_COLS && !comps[i].placed; c++)
          if (grid[r][c] == -1) occupy(i, r, c, 1, comps[i].expand && grid[r][1 - c] == -1);
    }
  }
}

// ============================ colours ============================
static inline uint16_t trackColor() { return tft.color565(28, 32, 44); }
static inline uint16_t staleColor() { return tft.color565(130, 132, 148); }
static inline uint16_t bandBlue()   { return tft.color565(0, 120, 255); }
static inline uint16_t bandGreen()  { return tft.color565(30, 200, 40); }
static inline uint16_t bandRed()    { return tft.color565(255, 45, 30); }

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
  u8f.setBackgroundColor(TFT_BLACK);
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
  if (!c.placed) return;
  int x = c.rx, y = c.ry, w = c.rw, h = c.rh;
  if (clearCell) tft.fillRect(x, y, w, h, TFT_BLACK);
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 6, trackColor());

  const int pad = 6;
  int ix = x + pad, iy = y + pad, iw = w - 2 * pad, ih = h - 2 * pad;

  // Title
  int titleH = 0;
  if (c.title.length()) {
    u8f.setFont(FONT_TITLE);
    u8f.setFontMode(1);
    u8f.setBackgroundColor(TFT_BLACK);
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
      u8f.setBackgroundColor(TFT_BLACK);
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
      u8f.setBackgroundColor(TFT_BLACK);
      int uw = u8f.getUTF8Width(c.unit.c_str());
      u8f.drawUTF8(ix + (iw - uw) / 2, by + bh - 2, c.unit.c_str());
    }
  } else if (c.type == UPDATE) {
    // A tappable button: check for an update, then install it. Strong colours.
    uint16_t bg, border, fg, fg2;
    String line1, line2;
    switch (c.updState) {
      case UPD_CHECKING:
        bg = tft.color565(52, 66, 110); border = tft.color565(120, 150, 230); fg = TFT_WHITE; fg2 = TFT_LIGHTGREY;
        line1 = "Checking..."; break;
      case UPD_UPTODATE:
        bg = tft.color565(16, 92, 46); border = tft.color565(60, 220, 110); fg = TFT_WHITE; fg2 = tft.color565(170, 255, 200);
        line1 = "Up to date"; line2 = "v" FW_VERSION_SEM; break;
      case UPD_AVAILABLE:
        bg = tft.color565(28, 150, 42); border = tft.color565(90, 255, 90); fg = TFT_WHITE; fg2 = tft.color565(210, 255, 210);
        line1 = "Update now"; line2 = "\xE2\x86\x92 v" + c.latestVer; break;
      default:  // UPD_IDLE
        bg = tft.color565(44, 86, 180); border = tft.color565(120, 170, 255); fg = TFT_WHITE; fg2 = tft.color565(200, 216, 245);
        line1 = "Check for update"; line2 = "v" FW_VERSION_SEM; break;
    }
    int bx = ix, bby = by + 2, bw = iw, bhh = bh - 4;
    tft.fillRoundRect(bx, bby, bw, bhh, 8, bg);
    tft.drawRoundRect(bx, bby, bw, bhh, 8, border);
    tft.drawRoundRect(bx + 1, bby + 1, bw - 2, bhh - 2, 7, border);   // 2px border = bolder
    u8f.setFontMode(1);
    u8f.setBackgroundColor(bg);          // glyph background = button fill (no black boxes)
    int cxb = bx + bw / 2, cyb = bby + bhh / 2;
    if (line2.length()) {
      u8f.setFont(u8g2_font_helvB12_tf);
      u8f.setForegroundColor(fg);
      int w1 = u8f.getUTF8Width(line1.c_str());
      u8f.drawUTF8(cxb - w1 / 2, cyb - 1, line1.c_str());
      u8f.setFont(FONT_SMALL);
      u8f.setForegroundColor(fg2);
      int w2 = u8f.getUTF8Width(line2.c_str());
      u8f.drawUTF8(cxb - w2 / 2, cyb + 15, line2.c_str());
    } else {
      u8f.setFont(u8g2_font_helvB12_tf);
      u8f.setForegroundColor(fg);
      int w1 = u8f.getUTF8Width(line1.c_str());
      u8f.drawUTF8(cxb - w1 / 2, cyb + 5, line1.c_str());
    }
  } else if (c.type == GRAPH) {
    int ns = c.graphN, first = c.graphFirst;
    int baseY = by + bh - 1;                 // bottom of the chart
    int headH = 11;                          // top strip reserved for labels
    int chartH = baseY - (by + headH);       // plot height below the header
    if (chartH < 4) chartH = bh - 2;         // tiny cell: use full height
    // Shared Y scale across all series: min from config, max from config or auto.
    float ymn = c.vmin;
    float ymx = c.vmax;
    if (isnan(ymx)) {
      float mx = ymn;
      for (int s = 0; s < ns; s++) {
        int si = first + s, n = gCount[si];
        for (int k = 0; k < n; k++) { float v = graphAt(si, n, k); if (v > mx) mx = v; }
      }
      ymx = (mx > ymn) ? mx * 1.1f : ymn + 1.0f;
    }
    float span = ymx - ymn; if (span <= 0) span = 1.0f;
    int anyN = 0;
    for (int s = 0; s < ns; s++) { int n = gCount[first + s]; if (n > anyN) anyN = n; }

    if (c.graphFill && ns == 1) {
      // Filled area (single-series look): one vertical line per pixel column.
      int si = first, n = gCount[si];
      uint16_t fill = gColor[si];
      uint16_t top  = tft.color565(90, 220, 255);
      for (int px = 0; px < iw && n > 0; px++) {
        int idx = (n <= 1) ? 0 : (int)((long)px * (n - 1) / (iw - 1));
        float v = graphAt(si, n, idx);
        float tt = constrain((v - ymn) / span, 0.0f, 1.0f);
        int hgt = (int)(tt * chartH);
        int xcol = ix + px;
        if (hgt > 0) tft.drawFastVLine(xcol, baseY - hgt, hgt, fill);
        tft.drawPixel(xcol, baseY - hgt, top);
      }
    } else {
      // One polyline per series, no fill.
      for (int s = 0; s < ns; s++) {
        int si = first + s, n = gCount[si];
        if (n <= 0) continue;
        uint16_t col = gColor[si];
        int px0 = 0, py0 = 0; bool have0 = false;
        for (int k = 0; k < n; k++) {
          float v = graphAt(si, n, k);
          float tt = constrain((v - ymn) / span, 0.0f, 1.0f);
          int x = ix + (n <= 1 ? 0 : (int)((long)k * (iw - 1) / (n - 1)));
          int y = baseY - (int)(tt * chartH);
          if (have0)        tft.drawLine(px0, py0, x, y, col);
          else if (n == 1)  tft.drawPixel(x, y, col);
          px0 = x; py0 = y; have0 = true;
        }
      }
    }
    tft.drawFastHLine(ix, baseY + 1, iw, trackColor());

    // Header strip: current value (single) or colour legend (multi), + max.
    u8f.setFont(FONT_SMALL);
    u8f.setBackgroundColor(TFT_BLACK);
    if (ns <= 1) {
      bool hv = (first >= 0) && gHave[first];
      u8f.setForegroundColor(hv ? TFT_WHITE : TFT_DARKGREY);
      String cur = (hv ? String(gLast[first], 1) : String("--")) + (c.unit.length() ? " " + c.unit : "");
      u8f.drawUTF8(ix, by + 9, cur.c_str());
    } else {
      int lx = ix;
      for (int s = 0; s < ns; s++) {
        int si = first + s;
        String item = gLabel[si];
        if (gHave[si]) item += " " + String(gLast[si], 0);
        tft.fillRect(lx, by + 3, 6, 6, gColor[si]);
        lx += 9;
        u8f.setForegroundColor(TFT_WHITE);
        u8f.drawUTF8(lx, by + 9, item.c_str());
        lx += u8f.getUTF8Width(item.c_str()) + 8;
        if (lx > ix + iw - 24) break;   // ran out of room
      }
    }
    if (anyN > 0) {
      char mxs[12]; dtostrf(ymx, 0, 0, mxs);
      u8f.setForegroundColor(TFT_DARKGREY);
      int mw = u8f.getUTF8Width(mxs);
      u8f.drawUTF8(ix + iw - mw, by + 9, mxs);
    }
  } else { // TEXT
    String body = c.topic.length() ? (c.haveData ? c.value : "") : c.staticText;
    drawWrappedText(ix, by, iw, bh, body, stale ? staleColor() : TFT_WHITE);
  }
}

// ============================ nav bar + full dashboard ============================
// Bottom bar: prev/next page arrows (when >1 page), page indicator, status dot, IP.
void drawNavBar() {
  int y = GRID_H, bh = 320 - GRID_H, my = y + bh / 2;
  tft.fillRect(0, y, 240, bh, TFT_BLACK);
  tft.drawFastHLine(0, y, 240, trackColor());

  if (nPages > 1) {
    tft.fillTriangle(16, my, 30, my - 9, 30, my + 9, TFT_LIGHTGREY);   // prev
    tft.fillTriangle(224, my, 210, my - 9, 210, my + 9, TFT_LIGHTGREY); // next
  }

  uint16_t dc = (WiFi.status() == WL_CONNECTED && mqttUp) ? TFT_GREEN : TFT_RED;
  if (nPages > 1) {
    tft.fillCircle(92, y + 11, 4, dc);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(String(currentPage + 1) + "/" + String(nPages), 104, y + 11, 2);
    if (WiFi.status() == WL_CONNECTED) {
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.drawString(WiFi.localIP().toString(), 120, y + 25, 1);
    }
  } else {
    tft.fillCircle(232, y + bh / 2, 4, dc);
    if (WiFi.status() == WL_CONNECTED) {
      tft.setTextDatum(ML_DATUM);
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.drawString(WiFi.localIP().toString(), 6, y + bh / 2, 2);
    }
  }
}

void drawDashboard() {
  tft.fillScreen(TFT_BLACK);
  for (int i = 0; i < nComps; i++) {
    comps[i].dirty = false;
    if (comps[i].page == currentPage) drawComponent(i, false);
  }
  drawNavBar();
  uiDash = true;
}

// An update-button component was tapped: check, then (on a second tap) install.
void handleUpdateTap(int i) {
  Component& c = comps[i];
  if (c.updState == UPD_AVAILABLE) {   // install the pending update
    runOta();                          // reboots on success; redraws on failure
    return;
  }
  c.updState = UPD_CHECKING;
  drawComponent(i, true);
  String latest = fetchLatestVersion();
  if (!latest.length()) { c.updState = UPD_IDLE; drawComponent(i, true); return; }
  c.latestVer = latest;
  c.updState = (semverNum(latest) > semverNum(FW_VERSION_SEM)) ? UPD_AVAILABLE : UPD_UPTODATE;
  drawComponent(i, true);
}

// Poll the touchscreen; tap the nav bar arrows to change page, or tap an
// update-button component to check/install firmware.
void handleTouch() {
  if (!uiDash) return;
  if (!(touchscreen.tirqTouched() && touchscreen.touched())) return;
  uint32_t now = millis();
  if (now - lastTouchMs < 350) return;
  TS_Point p = touchscreen.getPoint();
  if (p.z < 200) return;                        // ignore very light/no touch
  int tx = constrain(map(p.x, TOUCH_MINX, TOUCH_MAXX, 0, 239), 0, 239);
  int ty = constrain(map(p.y, TOUCH_MINY, TOUCH_MAXY, 0, 319), 0, 319);

  if (ty >= GRID_H - 10) {                       // nav bar
    if (nPages <= 1) return;
    lastTouchMs = now;
    if (tx < 90)       { currentPage = (currentPage + nPages - 1) % nPages; drawDashboard(); }
    else if (tx > 150) { currentPage = (currentPage + 1) % nPages; drawDashboard(); }
    return;
  }
  // component tap on the current page
  for (int i = 0; i < nComps; i++) {
    if (comps[i].page != currentPage || !comps[i].placed) continue;
    if (comps[i].type == UPDATE &&
        tx >= comps[i].rx && tx < comps[i].rx + comps[i].rw &&
        ty >= comps[i].ry && ty < comps[i].ry + comps[i].rh) {
      lastTouchMs = now;
      handleUpdateTap(i);
      return;
    }
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

// ============================ MQTT ============================
// Fill a graph ring buffer from a backfill payload: a JSON array ([1,2,3])
// or a comma-separated list (1,2,3), oldest first. Keeps the last GRAPH_POINTS.
void loadGraphHistory(int gi, const byte* payload, unsigned int len) {
  gCount[gi] = 0; gHead[gi] = 0;
  // find first non-space to detect JSON vs CSV
  unsigned int s = 0;
  while (s < len && (payload[s] == ' ' || payload[s] == '\t' || payload[s] == '\n')) s++;
  if (s < len && payload[s] == '[') {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) return;
    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull()) return;
    int total = arr.size();
    int skip = (total > GRAPH_POINTS) ? total - GRAPH_POINTS : 0;
    int idx = 0;
    for (JsonVariant v : arr) { if (idx++ < skip) continue; graphPush(gi, v.as<float>()); }
  } else {
    String csv; csv.reserve(len + 1);
    for (unsigned int i = 0; i < len; i++) csv += (char)payload[i];
    // count, then skip leading values so only the last GRAPH_POINTS remain
    int total = 1; for (unsigned int i = 0; i < (unsigned)csv.length(); i++) if (csv[i] == ',') total++;
    int skip = (total > GRAPH_POINTS) ? total - GRAPH_POINTS : 0;
    int start = 0, n = 0;
    while (start < csv.length()) {
      int comma = csv.indexOf(',', start);
      String tok = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
      tok.trim();
      if (n++ >= skip && tok.length()) graphPush(gi, tok.toFloat());
      if (comma < 0) break;
      start = comma + 1;
    }
  }
}

void onMqtt(char* topic, byte* payload, unsigned int len) {
  String t = topic;
  // Graph backfill message? (match a series' history topic)
  for (int si = 0; si < nSeries; si++) {
    if (gHist[si].length() && gHist[si] == t) {
      loadGraphHistory(si, payload, len);
      int ci = gComp[si];
      if (ci >= 0 && comps[ci].page == currentPage) comps[ci].dirty = true;
      return;
    }
  }
  // Normal value
  char buf[64];
  if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
  memcpy(buf, payload, len);
  buf[len] = '\0';
  String val = buf; val.trim();
  // Non-graph components bound directly to a topic.
  for (int i = 0; i < nComps; i++) {
    if (comps[i].topic.length() && comps[i].topic == t) {
      comps[i].value    = val;
      comps[i].num      = atof(val.c_str());
      comps[i].haveData = true;
      comps[i].lastMs   = millis();
      comps[i].dirty    = true;
    }
  }
  // Graph series live values.
  for (int si = 0; si < nSeries; si++) {
    if (gTopic[si].length() && gTopic[si] == t) {
      gLast[si] = atof(val.c_str());
      gHave[si] = true;
      aggAccumulate(si, gLast[si]);
      int ci = gComp[si];
      if (ci >= 0) {
        comps[ci].haveData = true;
        comps[ci].lastMs   = millis();
        if (comps[ci].page == currentPage) comps[ci].dirty = true;
      }
    }
  }
}

void subscribeTopics() {
  // Non-graph components.
  for (int i = 0; i < nComps; i++) {
    if (comps[i].topic.length()) mqtt.subscribe(comps[i].topic.c_str());
  }
  // Graph series: live topic, plus backfill history topic + request.
  for (int si = 0; si < nSeries; si++) {
    if (gTopic[si].length()) mqtt.subscribe(gTopic[si].c_str());
    if (gHist[si].length()) {
      mqtt.subscribe(gHist[si].c_str());
      uint32_t iv = (gComp[si] >= 0) ? comps[gComp[si]].interval : 120;
      String req = "{\"points\":" + String(GRAPH_POINTS) + ",\"interval\":" + String(iv) + "}";
      mqtt.publish(gReq[si].c_str(), req.c_str());
    }
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
  h.reserve(3600);
  h += F("<!doctype html><html lang=en><head><meta charset=utf-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>CYD Dashboard</title><style>"
         ":root{color-scheme:dark}"
         "*{box-sizing:border-box}"
         "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#0d0f15;color:#e9ecf2;margin:0;padding:24px 16px;display:flex;justify-content:center;line-height:1.5}"
         "main{width:100%;max-width:440px}"
         "h1{font-size:1.4rem;margin:0 0 4px}"
         ".sub{color:#8b94a7;font-size:.85rem;margin:0 0 18px}"
         ".card{background:#171a22;border:1px solid #242938;border-radius:14px;padding:18px;margin-bottom:16px}"
         ".card h2{font-size:1rem;margin:0 0 4px}"
         ".hint{margin:0 0 6px;color:#8b94a7;font-size:.82rem}"
         "label{display:block;margin:12px 0 4px;font-size:.82rem;color:#8b94a7}"
         "input{width:100%;padding:10px 12px;border-radius:9px;border:1px solid #2a3042;background:#0d0f15;color:#e9ecf2;font-size:1rem}"
         "input:focus{outline:none;border-color:#ffcb2d}"
         ".row{display:flex;gap:12px}.row>div{flex:1}"
         "button{width:100%;padding:12px;border:0;border-radius:9px;font-weight:700;font-size:.95rem;cursor:pointer}"
         ".primary{background:#ffcb2d;color:#1a1500;margin-top:16px}"
         ".sec{background:#242938;color:#e9ecf2}"
         ".ok{background:#35c46a;color:#05230f;margin-top:12px}"
         ".diag{display:flex;gap:12px}"
         "table{width:100%;border-collapse:collapse;font-size:.85rem;margin-top:6px}"
         "td{padding:6px 4px;border-bottom:1px solid #20242f;color:#b7bfce;vertical-align:top}"
         "td.m{color:#8b94a7}code{color:#ffcb2d;font-size:.9em}"
         ".res{margin-top:12px;font-size:.9rem;color:#8b94a7}"
         "</style></head><body><main>"
         "<h1>CYD Dashboard</h1><p class=sub>Device configuration</p>"
         "<form method=POST action=/save>"
         "<div class=card><h2>WiFi</h2>"
         "<p class=hint>Leave the password blank to keep the current one. If the new "
         "network is wrong the device goes offline \xE2\x80\x94 reconnect over USB "
         "(flasher page) to fix it.</p>");
  h += "<label>Network (SSID)</label><input name=ssid value=\"" + esc(cfg.ssid) + "\">";
  h += F("<label>Password</label><input name=wpass type=password placeholder='(unchanged)'></div>"
         "<div class=card><h2>MQTT broker</h2>");
  h += "<label>Broker (host/IP)</label><input name=mhost value=\"" + esc(cfg.mqttHost) + "\" placeholder='192.168.1.10'>";
  h += "<div class=row><div><label>Port</label><input name=mport type=number value=\"" + String(cfg.mqttPort) + "\"></div>";
  h += "<div><label>User (optional)</label><input name=muser value=\"" + esc(cfg.mqttUser) + "\"></div></div>";
  h += "<label>Password (optional)</label><input name=mpass type=password value=\"" + esc(cfg.mqttPass) + "\">";
  h += F("<button class=primary>Save and restart</button></div></form>");

  // Components (read-only; configured at build time in dashboard.json)
  h += F("<div class=card><h2>Components</h2>"
         "<p class=hint>Configured at build time in <code>dashboard.json</code>.</p>"
         "<table><tr><td class=m>Pos</td><td class=m>Type</td><td class=m>Title</td><td class=m>Topic</td></tr>");
  for (int i = 0; i < nComps; i++) {
    const char* tn = comps[i].type == GAUGE ? "gauge" : comps[i].type == COUNTER ? "counter"
                     : comps[i].type == UPDATE ? "update" : comps[i].type == GRAPH ? "graph" : "text";
    String ttl = comps[i].title.length() ? comps[i].title : String("(no title)");
    String tpc;
    if (comps[i].type == GRAPH) {
      for (int s = 0; s < comps[i].graphN; s++) { int si = comps[i].graphFirst + s;
        if (gTopic[si].length()) { if (tpc.length()) tpc += ", "; tpc += gTopic[si]; } }
      if (!tpc.length()) tpc = "-";
    } else {
      tpc = comps[i].topic.length() ? comps[i].topic : (comps[i].type == TEXT ? String("static") : String("-"));
    }
    String pos = comps[i].placed
                    ? "p" + String(comps[i].page + 1) + " r" + String(comps[i].row) + " c" + String(comps[i].col) +
                      (comps[i].rw > 120 ? " \xE2\x86\x94" : "")
                    : String("-");
    h += "<tr><td class=m>" + pos + "</td><td class=m><code>" + tn + "</code></td><td>" + esc(ttl) + "</td><td class=m>" + esc(tpc) + "</td></tr>";
  }
  h += F("</table></div>");

  // OTA
  h += F("<div class=card><h2>Firmware</h2>");
  h += "<p class=hint>Current version: <code>";
  h += FW_VERSION_SEM;
  h += F("</code></p>"
         "<button type=button class=sec id=chk onclick=checkUpd()>Check for update</button>"
         "<div class=res id=ures></div>"
         "<button type=button class=ok id=upd onclick=doUpd() style='display:none'>Update now</button>"
         "</div>"
         "<div class=card><h2>Diagnostics</h2><div class=diag>"
         "<button type=button class=sec onclick=\"fetch('/colortest',{method:'POST'})\">Colour test</button>"
         "<button type=button class=sec onclick=\"fetch('/dashboard',{method:'POST'})\">Dashboard</button>"
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
  if (server.hasArg("ssid")) prefs.putString("ssid", server.arg("ssid"));
  // Only overwrite the WiFi password when a new one was typed.
  if (server.hasArg("wpass") && server.arg("wpass").length()) prefs.putString("pass", server.arg("wpass"));
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

  // Touch on its own SPI bus (separate from the display)
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchSPI);
  touchscreen.setRotation(0);

  loadConfig();
  loadDashboard();
  layoutDashboard();

  improvSerial.setDeviceInfo(
    ImprovTypes::ChipFamily::CF_ESP32,
    FW_NAME, FW_VERSION_SEM, "CYD-Dashboard",
    "http://{LOCAL_IPV4}/");
  improvSerial.onImprovConnected(onImprovConnected);

  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setCallback(onMqtt);
  mqtt.setBufferSize(2048);          // room for graph backfill arrays

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

  handleTouch();

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
  if (conn != lastConn) { lastConn = conn; drawNavBar(); }

  // Sample graph components at their interval (on every page, so history keeps
  // building even while another page is shown).
  for (int i = 0; i < nComps; i++) {
    if (comps[i].type == GRAPH && comps[i].graphFirst >= 0 && comps[i].haveData) {
      if (comps[i].lastSampleMs == 0 ||
          millis() - comps[i].lastSampleMs >= comps[i].interval * 1000UL) {
        comps[i].lastSampleMs = millis();
        for (int s = 0; s < comps[i].graphN; s++) {
          int si = comps[i].graphFirst + s;
          if (!gHave[si]) continue;
          // Use the aggregated value for this bucket; if nothing arrived this
          // interval, carry the last known value forward so the line continues.
          float sample = (gAccN[si] > 0) ? aggFlush(si) : gLast[si];
          graphPush(si, sample);
        }
        if (comps[i].page == currentPage) comps[i].dirty = true;
      }
    }
  }

  // Redraw a component (on the current page) when it gets new data or its
  // stale state changes. Off-page components keep their state and are drawn
  // fresh when you navigate to them.
  for (int i = 0; i < nComps; i++) {
    if (comps[i].page != currentPage) continue;
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
