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
| `update` | A touchable button: tap to check for a firmware update, tap again to install it | `title` (no topic needed) |
| `graph` | A rolling chart of one or more numeric topics over time (built on the device) | `title`, `topic`, `unit`, `min`, `max` (omit `max` to auto-scale), `interval` (seconds between samples, default 120), `series` (multi-line), `fill` |

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

Common placement keys for every type: `row` (0–2), `col` (0–1), `rowspan` (1–3),
`expand` (bool, fill the row). Omit `row`/`col` to auto-place. A `graph` with
`rowspan: 3` and `expand: true` fills a whole page.

The `graph` component keeps its own rolling history on the device — you only
need to publish the current value to its topic (e.g. an HA automation that
publishes the wattage every couple of minutes). It samples that value every
`interval` seconds; the history resets on reboot and refills over time.

**Multiple series:** instead of a single `topic`, give a `graph` a `series`
array (up to 4) to plot several lines on one chart with a shared Y scale and a
colour legend:

```json
{ "type": "graph", "title": "Solar strings", "unit": "W", "min": 0, "backfill": true,
  "series": [
    { "topic": "home/solar/pv1", "entity": "sensor.pv1_power", "label": "PV1", "color": "#36d399" },
    { "topic": "home/solar/pv2", "entity": "sensor.pv2_power", "label": "PV2", "color": "#f5b900" }
  ]
}
```

Each series takes its own `topic` (live value), optional `entity`/`history_topic`/
`request_topic` (backfill, as above), `label` (legend) and `color` (`#RRGGBB`;
a default palette is used otherwise). A multi-series graph is drawn as lines
with no fill; a single-series graph is a filled area by default. Set `"fill":
false` to force lines, or `"fill": true` on a one-series graph to keep the fill.
Across all graphs the device holds up to 6 series buffers in total.

**Aggregation (`agg`):** when values arrive faster than the sample `interval`,
each point is built from the values received during that interval. `agg` picks
how: `"avg"` (default — faithful to energy, smooths noise), `"max"` (catch
peaks/clipping), `"min"`, or `"last"` (the old behaviour: just the most recent
value at the sample moment). Set it per graph (applies to all its series) or
per series (overrides the graph default). If no value arrives during an
interval, the last known value is carried forward so the line stays continuous.

Optional: set `"backfill": true` on a graph to also request a historical series
on connect. Off by default. Give the graph an `"entity"` (its Home Assistant
entity id, e.g. `"sensor.power"`) and the request/reply topics are derived from a
shared scheme, so a single HA responder can serve every graph:

- request published to `home/history/get/sensor/power`
- series expected back on `home/history/sensor/power`

(the entity id with `.` → `/`). The payload of the request is
`{"points":120,"interval":<interval>}`; the reply is a JSON array or CSV, oldest
first. Without an `entity`, it falls back to the per-topic form
(`<topic>/history/get` and `<topic>/history`); you can also override either topic
explicitly with `"request_topic"` / `"history_topic"`.

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
