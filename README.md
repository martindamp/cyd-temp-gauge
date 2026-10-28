# CYD Dashboard (MQTT)

A PlatformIO project for the **Cheap Yellow Display** (ESP32-2432S028R, 2.8" ILI9341)
that shows up to **6 components** on one screen, each driven by an MQTT topic.
The layout is configured at build time in `dashboard.json`; WiFi and the MQTT broker
are configured at runtime on the device's own web page.

![board](https://img.shields.io/badge/board-ESP32--2432S028R-yellow)
![framework](https://img.shields.io/badge/framework-Arduino%20%2F%20PlatformIO-blue)

## Component types

| Type | Shows | Config keys |
|------|-------|-------------|
| `gauge` | Coloured ring gauge (blue/green/red bands by value third) with the value in the centre | `title`, `topic`, `min`, `max`, `unit` |
| `counter` | A large number from a topic, with a title and optional unit | `title`, `topic`, `unit` |
| `text` | Static text, dynamic text from a topic, or nothing | `title`, and `text` (static) **or** `topic` (dynamic) |

Components are laid out on a fixed **3-row × 2-col** grid (rows 0–2, cols 0–1, so
up to 6 components). Each component may set `row` and `col`; omit them to
auto-place it in the next free cell. Set `expand: true` to make a component fill
the rest of its row when the other column is free (e.g. a single component in
row 2 with `expand` spans the full width). A thin status bar at the bottom shows
the device IP and a connection dot (green = WiFi + MQTT OK, red = not connected).

## Dashboard configuration (`dashboard.json`)

Edit `dashboard.json` and rebuild / push. A pre-build script
(`tools/gen_dashboard.py`) validates it and embeds it into the firmware, so the
layout is fixed in each build.

```json
{
  "components": [
    { "type": "gauge",   "row": 0, "col": 0, "title": "Living room", "topic": "home/living/temp",  "min": 0,   "max": 40, "unit": "°C" },
    { "type": "gauge",   "row": 0, "col": 1, "title": "Outside",     "topic": "home/outside/temp", "min": -20, "max": 40, "unit": "°C" },
    { "type": "counter", "row": 1, "col": 0, "title": "Humidity",    "topic": "home/living/hum",   "unit": "%" },
    { "type": "counter", "row": 1, "col": 1, "title": "Power",       "topic": "home/power",        "unit": "W" },
    { "type": "text",    "row": 2, "col": 0, "expand": true, "title": "Status", "topic": "home/status" }
  ]
}
```

Common placement keys for every type: `row` (0–2), `col` (0–1), `expand` (bool).
Omit `row`/`col` to auto-place.

- `"°C"` is `°C`. Accented characters and the degree sign render correctly.
- gauge / counter topics expect a plain number (e.g. `21.5`); text topics take any text.
- The config page lists the configured components (read-only) with their cell, so
  you can see the layout and which topics to publish.

## Features

- Up to 6 gauge / counter / text components on one screen
- No hardcoded credentials: WiFi via Improv Wi-Fi, MQTT broker via the web page;
  both stored in NVS and surviving a restart
- "stale" dimming if a component gets no new message for 60 s
- Automatic MQTT reconnect, multi-topic subscribe
- Over-the-air (OTA) updates from GitHub Pages over validated HTTPS
- All TFT_eSPI configuration lives in `platformio.ini` — the library is not touched

## Hardware

| | |
|---|---|
| Board | ESP32-2432S028R ("Cheap Yellow Display") |
| MCU | ESP32-WROOM-32 |
| Display | 2.8" 240×320 ILI9341 over SPI |
| Backlight | GPIO 21 |

No extra wiring — the CYD is an all-in-one board.

## Getting started

1. Open the folder in VS Code with the **PlatformIO** extension.
2. Edit `dashboard.json` for your components.
3. **Build** → **Upload** with the CYD on USB. Serial monitor at 115200 baud.
4. The first time, provide WiFi over Improv (from the web flasher below) or via
   stored credentials. Then open the device's config page (its IP is shown at the
   bottom of the screen) to set the MQTT broker.

`lib_deps` pulls in TFT_eSPI, PubSubClient, Improv Wi-Fi, U8g2 and ArduinoJson on
the first build.

## Troubleshooting

Commented out at the bottom of `platformio.ini`:

- All colours look negative/inverted → swap `TFT_INVERSION_ON` for `TFT_INVERSION_OFF`
- Red/blue swapped → remove the `TFT_RGB_ORDER=TFT_BGR` line
- Completely black screen → swap `ILI9341_2_DRIVER` for `ILI9341_DRIVER`

There is also a built-in **colour test** on the config page (Diagnostics →
Show colour test). CYD clones vary slightly in display controller and colour handling.

## Web flasher (GitHub Pages)

On every push to `main`, GitHub Actions builds the firmware, merges a factory
`.bin` and deploys a flasher page to GitHub Pages. From there the CYD can be
flashed straight from the browser via **ESP Web Tools** (Web Serial).

- Works in **Chrome / Edge on desktop** (Web Serial is not in Firefox/Safari/mobile)
- Enable Pages once: **Settings → Pages → Source: "GitHub Actions"**
- Page: `https://martindamp.github.io/cyd-temp-gauge/`

The firmware ships with no credentials, so the public `.bin` on Pages contains no
secrets.

## OTA update (wireless)

1. Open the device's config page (`http://<device-ip>/` — the IP is on the screen)
2. Under **Firmware** → **Check for update**
3. If a newer build is on Pages, **Update now** appears — click it
4. The device downloads the app firmware, shows a progress bar and restarts into it

GitHub Actions publishes an app-only `firmware/firmware.bin` and a `version.txt`
(semantic version) to Pages. The device updates **only if the Pages version is
newer** — never a downgrade. `min_spiffs.csv` provides two app partitions for OTA.

**Release a new version:** bump `VERSION` (e.g. `2.0.0` → `2.0.1`) and push.

**HTTPS:** OTA validates the server certificate against known root CAs (ISRG Root
X1, DigiCert Global Root G2, GlobalSign R3, USERTrust RSA — covering
`github.io`/Fastly), with NTP time sync. The roots live in `src/ota_certs.h`.

## License

MIT
