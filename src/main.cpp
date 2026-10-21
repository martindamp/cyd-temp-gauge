/*
  CYD (ESP32-2432S028R) – Temperatur-gauge via MQTT   [PlatformIO]
  ----------------------------------------------------------------
  Abonnerer på et MQTT-topic og viser temperaturen som en farvet
  ring-gauge (blå -> grøn -> rød) med tallet i midten.

  Al TFT_eSPI-konfiguration ligger i platformio.ini (build_flags).
*/

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <TFT_eSPI.h>

// ========================= KONFIGURATION =========================
const char*    WIFI_SSID     = "DIT_WIFI";
const char*    WIFI_PASS     = "DIT_PASSWORD";

const char*    MQTT_HOST     = "192.168.1.10";   // broker IP / hostname
const uint16_t MQTT_PORT     = 1883;
const char*    MQTT_USER     = "";               // tom = ingen auth
const char*    MQTT_PASS     = "";
const char*    MQTT_TOPIC    = "home/stue/temperatur";
const char*    MQTT_CLIENTID = "cyd-temp-gauge";

// Gauge-skala (°C)
const float    TEMP_MIN      = 0.0;
const float    TEMP_MAX      = 40.0;

// Tid uden ny besked før data vises som "forældet" (grå)
const uint32_t STALE_MS      = 60000;
// =================================================================

TFT_eSPI     tft = TFT_eSPI();
WiFiClient   net;
PubSubClient mqtt(net);

// Geometri (portrait 240x320)
int cx, cy, rOuter, rInner;
const int START_ANGLE = 30;    // 0° = kl. 6, voksende med uret
const int END_ANGLE   = 330;   // -> åbning i bunden, sweeper over toppen

volatile float lastTemp  = NAN;
float    shownTemp       = -1000;
bool     haveData        = false;
uint32_t lastMsgMs       = 0;
uint32_t lastReconnect   = 0;
bool     mqttUp          = false;

// Interpoleret farve: blå -> grøn -> rød hen over skalaen
uint16_t tempColor(float temp) {
  float t = constrain((temp - TEMP_MIN) / (TEMP_MAX - TEMP_MIN), 0.0f, 1.0f);
  uint8_t r, g, b;
  if (t < 0.5f) { float k = t / 0.5f;         r = 0;       g = 255 * k;       b = 255 * (1 - k); }
  else          { float k = (t - 0.5f) / 0.5f; r = 255 * k; g = 255 * (1 - k); b = 0; }
  return tft.color565(r, g, b);
}

void drawStatic() {
  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("TEMPERATUR", cx, 20, 4);

  // Baggrundsring
  tft.drawSmoothArc(cx, cy, rOuter, rInner, START_ANGLE, END_ANGLE,
                    TFT_DARKGREY, TFT_BLACK, true);

  // Min/max-labels ved ringens ender (x = cx - r*sin, y = cy + r*cos)
  int lr = rOuter + 14;
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawNumber((int)TEMP_MIN, cx - lr * sin(radians(START_ANGLE)),
                                cy + lr * cos(radians(START_ANGLE)), 2);
  tft.drawNumber((int)TEMP_MAX, cx - lr * sin(radians(END_ANGLE)),
                                cy + lr * cos(radians(END_ANGLE)), 2);
}

void drawStatus() {
  uint16_t c = (WiFi.status() == WL_CONNECTED && mqttUp) ? TFT_GREEN : TFT_RED;
  tft.fillCircle(cx, cy + rOuter + 34, 5, c);
}

void drawValue(float temp, bool stale) {
  float tC = constrain(temp, TEMP_MIN, TEMP_MAX);
  int angle = START_ANGLE + (int)round((tC - TEMP_MIN) / (TEMP_MAX - TEMP_MIN)
                                       * (END_ANGLE - START_ANGLE));
  uint16_t col = stale ? TFT_DARKGREY : tempColor(temp);

  // Fyldt del + resterende grå del -> ringen males helt om hver gang
  if (angle > START_ANGLE)
    tft.drawSmoothArc(cx, cy, rOuter, rInner, START_ANGLE, angle, col, TFT_BLACK, true);
  if (angle < END_ANGLE)
    tft.drawSmoothArc(cx, cy, rOuter, rInner, angle, END_ANGLE, TFT_DARKGREY, TFT_BLACK, true);

  // Tal i midten (7-segment-font)
  char buf[8];
  dtostrf(temp, 0, 1, buf);                         // fx "21.5"
  tft.fillRect(cx - 70, cy - 32, 140, 64, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(stale ? TFT_DARKGREY : TFT_WHITE, TFT_BLACK);
  tft.drawString(buf, cx, cy, 7);

  // Enhed  °C  (grader tegnes som lille ring)
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("C", cx + 12, cy + 54, 4);
  tft.drawCircle(cx - 8, cy + 48, 4, TFT_LIGHTGREY);
}

void onMqtt(char* topic, byte* payload, unsigned int len) {
  char buf[32];
  if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
  memcpy(buf, payload, len);
  buf[len] = '\0';

  // Forventer et rent tal, fx "21.5".
  // Er payload JSON (fx {"temperature":21.5})? -> se noten nederst.
  lastTemp  = atof(buf);
  haveData  = true;
  lastMsgMs = millis();
}

void connectWiFi() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("WiFi...", cx, cy, 4);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
}

bool connectMqtt() {
  if (WiFi.status() != WL_CONNECTED) return false;
  bool ok = (strlen(MQTT_USER) > 0)
            ? mqtt.connect(MQTT_CLIENTID, MQTT_USER, MQTT_PASS)
            : mqtt.connect(MQTT_CLIENTID);
  if (ok) mqtt.subscribe(MQTT_TOPIC);
  mqttUp = ok;
  return ok;
}

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(0);        // portrait 240x320  (brug 1 for landskab)
  cx = tft.width() / 2;      // 120
  cy = 150;
  rOuter = 112;
  rInner = 94;

  connectWiFi();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  connectMqtt();

  drawStatic();
  // Placeholder indtil første besked
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("--", cx, cy, 7);
  drawStatus();
}

void loop() {
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

  bool stale = haveData && (millis() - lastMsgMs > STALE_MS);

  // Tegn kun om når værdien (eller stale-tilstand) faktisk ændrer sig
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
