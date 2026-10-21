# CYD Temperatur-gauge (MQTT)

Et PlatformIO-projekt til **Cheap Yellow Display** (ESP32-2432S028R, 2,8" ILI9341),
der abonnerer på et MQTT-topic og viser temperaturen som en farvet ring-gauge
(blå → grøn → rød) med tallet i midten.

![board](https://img.shields.io/badge/board-ESP32--2432S028R-yellow)
![framework](https://img.shields.io/badge/framework-Arduino%20%2F%20PlatformIO-blue)

## Funktioner

- Ring-gauge med farve der følger temperaturen hen over en konfigurerbar skala
- Stort tal i 7-segment-font i midten
- Status-prik: grøn = WiFi + MQTT ok, rød = ingen forbindelse
- Data vises gråt ("forældet") hvis der ikke er kommet en ny besked i 60 sek.
- Automatisk MQTT-reconnect
- Hele TFT_eSPI-konfigurationen ligger i `platformio.ini` — biblioteket røres ikke

## Hardware

| | |
|---|---|
| Board | ESP32-2432S028R ("Cheap Yellow Display") |
| MCU | ESP32-WROOM-32 |
| Display | 2,8" 240×320 ILI9341 over SPI |
| Baggrundslys | GPIO 21 |

Ingen ekstra ledninger — CYD'en er et alt-i-et-board.

## Kom i gang

1. Åbn mappen i VS Code med **PlatformIO**-udvidelsen.
2. Ret toppen af `src/main.cpp`:
   - `WIFI_SSID` / `WIFI_PASS`
   - `MQTT_HOST`, evt. `MQTT_USER` / `MQTT_PASS`
   - `MQTT_TOPIC` så det matcher det, der publiceres
   - evt. `TEMP_MIN` / `TEMP_MAX` (skalaen, default 0–40 °C)
3. **Build** → **Upload** med CYD'en i USB. Seriel-monitor på 115200 baud.

`lib_deps` henter TFT_eSPI og PubSubClient automatisk ved første build.

## MQTT-payload

Koden forventer et rent tal, fx `21.5`.
Kommer temperaturen som JSON (fx `{"temperature": 21.5}`), er der en færdig
ArduinoJson-variant i kommentaren nederst i `src/main.cpp`.

## Fejlfinding

Står udkommenteret nederst i `platformio.ini`:

- Omvendte farver → `-D TFT_INVERSION_OFF=1`
- Rød/blå byttet om → `-D TFT_RGB_ORDER=TFT_BGR`
- Helt sort skærm → skift `ILI9341_2_DRIVER` ud med `ILI9341_DRIVER`

(CYD-kloner varierer en smule i display-controller.)

## Web-flasher (GitHub Pages)

Ved hvert push til `main` bygger GitHub Actions firmwaren, samler et merged
`.bin` og deployer en flasher-side til GitHub Pages. Derfra kan CYD'en flashes
direkte fra browseren via **ESP Web Tools** (Web Serial) — ingen Arduino IDE
eller PlatformIO nødvendig.

- Virker i **Chrome / Edge på desktop** (Web Serial findes ikke i Firefox/Safari/mobil)
- Aktivér Pages én gang: **Settings → Pages → Source: "GitHub Actions"**
- Side: `https://martindamp.github.io/cyd-temp-gauge/`

> ⚠️ **Credentials:** GitHub Pages-siden (og dermed det downloadbare `.bin`) er
> offentlig, også fra et privat repo. Hardcode derfor ikke rigtige WiFi-/MQTT-
> oplysninger i `src/main.cpp` hvis firmwaren bygges og udstilles her. Vil du
> kunne sætte WiFi fra browseren efter flash i stedet, bruges Improv Wi-Fi.

## OTA-opdatering (trådløst)

Enheden kan opdatere sig selv over WiFi — intet kabel nødvendigt efter første flash:

1. Åbn enhedens konfig-side (`http://<enhedens-ip>/` — IP'en står nederst på skærmen)
2. Under **Firmware** → klik **Søg efter opdatering**
3. Er der en nyere build på GitHub Pages, vises **Opdater nu** — klik den
4. CYD'en henter app-firmwaren fra Pages, viser en progress-bar og genstarter selv i den nye version

Sådan hænger det sammen: GitHub Actions bygger app-only `firmware/firmware.bin` og
publicerer en `version.txt` med den **semantiske version** til Pages. Enheden har sin
egen version bygget ind og opdaterer **kun hvis Pages-versionen er nyere** — aldrig
downgrade. `min_spiffs.csv` giver to app-partitioner, så den nye firmware skrives til
den inaktive og bootes over.

**Udgiv en ny version:** hæv tallet i `VERSION`-filen (fx `1.2.0` → `1.2.1`) og push.
CI bygger, og enheder i felten tilbyder opdateringen næste gang du trykker *Søg*.
Pusher du uden at hæve `VERSION`, redeployes firmwaren, men enhederne ser den som
samme version og tilbyder ingen opdatering.

**HTTPS:** OTA validerer serverens certifikat mod kendte root-CA'er (ISRG Root X1,
DigiCert Global Root G2, GlobalSign R3, USERTrust RSA — dækker `github.io`/Fastly),
med NTP-tidssynkronisering. Rotation af GitHubs leaf/mellem-certifikat påvirker ikke
OTA, så længe kæden går til en af disse langlivede roots. Rødderne ligger i
`src/ota_certs.h` (genereret fra Mozillas CA-bundle).

## Licens

MIT
