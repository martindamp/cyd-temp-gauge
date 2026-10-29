# CYD Dashboard (MQTT)

A PlatformIO firmware for the **Cheap Yellow Display** (ESP32‑2432S028R, 2.8"
ILI9341) that turns the board into a small configurable MQTT dashboard: gauges,
counters, text, charts and buttons laid out on one or more pages, in portrait or
landscape, with switchable colour themes — all driven by MQTT and updated
over‑the‑air.

![board](https://img.shields.io/badge/board-ESP32--2432S028R-yellow)
![framework](https://img.shields.io/badge/framework-Arduino%20%2F%20PlatformIO-blue)

## Contents

- [What it can do](#what-it-can-do)
- [Hardware](#hardware)
- [Quick start](#quick-start)
- [The device web page](#the-device-web-page)
- [Configuring the dashboard](#configuring-the-dashboard)
  - [Where the layout comes from](#where-the-layout-comes-from)
  - [JSON structure](#json-structure)
  - [Layout & placement](#layout--placement)
  - [Component types](#component-types)
  - [Graphs in depth](#graphs-in-depth)
  - [Colour themes](#colour-themes)
- [What to publish on MQTT](#what-to-publish-on-mqtt)
- [Home Assistant graph backfill](#home-assistant-graph-backfill)
- [OTA updates & releasing](#ota-updates--releasing)
- [Web flasher](#web-flasher)
- [Security model](#security-model)
- [Troubleshooting](#troubleshooting)
- [Project layout](#project-layout)
- [License](#license)

## What it can do

- **Up to 6 components per page**, on **up to 6 pages**, with on‑screen
  next/previous arrows to switch pages.
- **Component types:** `gauge`, `counter`, `text`, `graph` (single or
  multi‑series), `update` (firmware button) and `theme` (theme switcher).
- **Portrait or landscape** orientation, with a grid that adapts to each.
- **8 colour themes**, switchable on the device and persisted.
- **No hardcoded secrets:** Wi‑Fi via Improv (from the flasher) and the MQTT
  broker via the device web page; both stored in NVS and kept across reboots.
- **Runtime layout:** the whole dashboard can be loaded from a URL and reloaded
  live, so layout changes need no rebuild — only firmware changes do.
- **Graphs** keep a rolling history on the device, with optional per‑interval
  aggregation (avg/max/min/last) and optional history backfill over MQTT so a
  chart survives a reboot.
- **Stale indication:** a component dims if it gets no new message for 60 s.
- **OTA updates** from GitHub Pages over certificate‑validated HTTPS, with
  semantic‑version checks (never downgrades).
- **Web flasher** (ESP Web Tools) built and published by GitHub Actions.
- All TFT_eSPI configuration lives in `platformio.ini` — the library is untouched.

## Hardware

| | |
|---|---|
| Board | ESP32‑2432S028R ("Cheap Yellow Display") |
| MCU | ESP32‑WROOM‑32 |
| Display | 2.8" 240×320 ILI9341 over SPI |
| Touch | XPT2046 resistive (separate SPI bus) |
| Backlight | GPIO 21 |

No extra wiring — the CYD is an all‑in‑one board.

## Quick start

You can flash either from the browser (easiest) or from PlatformIO.

**A. Browser (web flasher)**

1. Open the flasher page: `https://martindamp.github.io/cyd-temp-gauge/`
   (Chrome or Edge on desktop — Web Serial isn't available on Firefox/Safari/mobile).
2. Connect the CYD over USB and click **Install**. If the flasher offers it, set
   your Wi‑Fi during install (via Improv); otherwise you can set it afterwards on
   the device's **Wi‑Fi** card.
3. When the dashboard appears, note the device IP shown in the bottom status bar
   and open `http://<device-ip>/` to set Wi‑Fi (if not already) and the MQTT
   broker (see [the web page](#the-device-web-page)).

**B. PlatformIO**

1. Open the folder in VS Code with the **PlatformIO** extension.
2. Edit [`dashboard.json`](dashboard.json) for your components (optional — there's
   a working default).
3. **Build → Upload** with the CYD on USB. Serial monitor at 115200 baud.
4. Provide Wi‑Fi over Improv (from the flasher page) or flash once via the web
   flasher to provision Wi‑Fi, then set the MQTT broker on the device page.

`lib_deps` pulls in TFT_eSPI, PubSubClient, Improv Wi‑Fi, U8g2 and ArduinoJson on
the first build.

## The device web page

Open `http://<device-ip>/` (the IP is in the bottom status bar). The page has:

| Card | What it does |
|------|--------------|
| **Wi‑Fi** | Change the network/password. Leave the password blank to keep the current one. A wrong network takes the device offline — reconnect over USB (flasher) to fix it. |
| **MQTT broker** | Host/IP, port (default `1883`), and optional user/password. |
| **Dashboard config** | Optional **Config URL** to load the layout from at runtime (see below). |
| **Components** | Read‑only list of the active layout (position, type, title, topic). **Reload from URL** re‑fetches and applies the layout live; **Use built‑in** drops the cached copy and returns to the compiled‑in layout. |
| **Firmware** | **Check for update** queries GitHub Pages; **Update now** appears if a newer build exists. |
| **Diagnostics** | **Colour test** (full‑screen colour check for panel‑variant debugging) and **Dashboard** (force a redraw). |

**Save and restart** stores the Wi‑Fi / MQTT / Config‑URL fields and reboots.

The bottom status bar on the display shows the device IP, a connection dot
(green = Wi‑Fi + MQTT OK, red = not connected) and, when there's more than one
page, the current page number and the prev/next arrows.

## Configuring the dashboard

### Where the layout comes from

The layout can come from two places; the first that yields a valid layout wins:

1. **Runtime Config URL** (if set on the web page) — fetched on boot and cached
   in flash, so it survives a reboot even if the URL is temporarily unreachable.
2. **Built‑in** `dashboard.json` — embedded into the firmware at build time.

So you only rebuild/flash when the **firmware** changes; layout tweaks are a file
edit on your server/repo plus a **Reload from URL**. If a fetched/cached layout
is missing or invalid, the device falls back to the built‑in one (a typo in your
JSON never blanks the screen).

The built‑in layout is validated and embedded at build time by a pre‑build
script, [`tools/gen_dashboard.py`](tools/gen_dashboard.py). The JSON format is
identical whether it's built‑in or fetched.

### JSON structure

```json
{
  "orientation": "portrait",
  "theme": "ocean",
  "pages": [
    { "components": [ /* up to 6 components */ ] },
    { "components": [ /* next page */ ] }
  ]
}
```

Top‑level keys:

| Key | Values | Default |
|-----|--------|---------|
| `orientation` | `"portrait"` or `"landscape"` | `portrait` |
| `theme` | one of the [theme names](#colour-themes) | `Midnight` |
| `pages` | array of pages, each `{ "components": [...] }`, up to 6 | — |

A single‑page shorthand is also accepted: `{ "components": [ ... ] }` with no
`pages`.

### Layout & placement

Components sit on a fixed grid that depends on orientation:

- **portrait** — 240×320, a **3‑row × 2‑col** grid (rows 0–2, cols 0–1)
- **landscape** — 320×240, a **2‑row × 3‑col** grid (rows 0–1, cols 0–2)

Either way, up to 6 components fit per page. Placement keys, valid on every type:

| Key | Meaning |
|-----|---------|
| `row`, `col` | Grid position. Omit **both** to auto‑place in the next free cell (scanned left‑to‑right, top‑to‑bottom, in JSON order). |
| `rowspan` | How many rows tall (1–3). |
| `colspan` | How many columns wide (1–3). |

A component spanning the whole page sets both spans, e.g. a `graph` with
`"rowspan": 3, "colspan": 2` (portrait) fills the page. If an explicit placement
or span doesn't fit, the component falls back to a single free cell; if the page
is full, extra components are simply not drawn (they never overlap).

> `expand: true` is still accepted as a deprecated alias for "full width"
> (`colspan` = all columns). Prefer `colspan`.

### Component types

Every component takes a `title` (shown as a heading) and the placement keys above.

#### `gauge`
A coloured ring gauge with the value in the centre. The ring is split into three
bands (low / mid / high) by value thirds, themed per colour scheme.

| Key | Meaning |
|-----|---------|
| `topic` | MQTT topic carrying a number |
| `min`, `max` | gauge range (default 0–100) |
| `unit` | shown after the value, e.g. `"°C"`, `"%"` |

```json
{ "type": "gauge", "row": 0, "col": 0, "title": "Living room",
  "topic": "home/living/temp", "min": 0, "max": 40, "unit": "°C" }
```

#### `counter`
A large number from a topic, with a title and optional unit.

| Key | Meaning |
|-----|---------|
| `topic` | MQTT topic carrying a number |
| `unit` | optional, shown after the number |

```json
{ "type": "counter", "title": "Power", "topic": "home/power", "unit": "W" }
```

#### `text`
Static text, dynamic text from a topic, or nothing.

| Key | Meaning |
|-----|---------|
| `text` | static text to show, **or** |
| `topic` | a topic whose payload (any string) is shown |

```json
{ "type": "text", "title": "Status", "topic": "home/status" }
```

#### `graph`
A rolling chart of one or more numeric topics over time, built on the device.
See [Graphs in depth](#graphs-in-depth) for `series`, `agg` and `backfill`.

| Key | Meaning |
|-----|---------|
| `topic` | single‑series: the topic to plot |
| `unit`, `min` | units / Y‑axis minimum |
| `max` | Y‑axis maximum — omit to auto‑scale |
| `interval` | seconds between samples (default 120) |
| `fill` | fill the area under the line (default: on for single series, off for multi) |
| `series` | array for a multi‑series chart |
| `agg` | per‑interval aggregation: `avg` (default) / `max` / `min` / `last` |
| `backfill`, `entity` | request history on connect (see below) |

```json
{ "type": "graph", "title": "Power today", "topic": "home/power",
  "entity": "sensor.power", "unit": "W", "min": 0, "interval": 120,
  "rowspan": 3, "colspan": 2, "backfill": true }
```

#### `update`
A touchable button. Tap to check GitHub Pages for a newer firmware; tap again to
install it (the device shows a progress bar and reboots). Needs only a `title`.

```json
{ "type": "update", "title": "Firmware" }
```

#### `theme`
A touchable button showing the current colour theme with colour swatches. Tap to
cycle through the [themes](#colour-themes); the choice is saved on the device and
persists across reboots. Needs only a `title`.

```json
{ "type": "theme", "title": "Theme" }
```

### Graphs in depth

**History model.** A graph keeps its own rolling ring buffer (120 points per
series) on the device. You just publish the current value to the topic; the
device samples it every `interval` seconds. The history resets on reboot and
refills over time (unless you enable backfill, below).

**Multiple series.** Replace `topic` with a `series` array (up to 4) to plot
several lines on one chart with a shared Y scale and a colour legend:

```json
{ "type": "graph", "title": "Solar strings", "unit": "W", "min": 0, "backfill": true,
  "series": [
    { "topic": "home/solar/pv1", "entity": "sensor.pv1_power", "label": "PV1", "color": "#36d399" },
    { "topic": "home/solar/pv2", "entity": "sensor.pv2_power", "label": "PV2", "color": "#f5b900" }
  ]
}
```

Each series takes its own `topic`, optional `entity`/`history_topic`/`request_topic`
(backfill), `label` (legend) and `color` (`#RRGGBB`; a default palette is used
otherwise). Multi‑series charts are drawn as lines with no fill; a single‑series
chart is a filled area by default. `fill` overrides this either way. The device
holds **up to 6 series buffers in total** across all graphs.

**Aggregation (`agg`).** When values arrive faster than `interval`, each plotted
point is built from the values received during that interval:

- `avg` (default) — faithful to energy, smooths noise (good for power).
- `max` — catch peaks / inverter clipping.
- `min` — catch dips.
- `last` — just the most recent value at the sample moment.

Set it per graph (applies to all series) or per series (overrides the graph
default). If no value arrives in an interval, the last known value is carried
forward so the line stays continuous.

**Backfill (survives reboot).** Set `"backfill": true` and give the graph (or
series) an `"entity"` (its Home Assistant entity id). On connect the device
requests a historical series and loads it before sampling live. Topics are
derived from a shared scheme so one responder can serve every graph:

- request published to `home/history/get/sensor/power`
- series expected back on `home/history/sensor/power`

(the entity id with `.` → `/`). The request payload is
`{"points":120,"interval":<interval>}`; the reply is a JSON array (oldest first),
e.g. `[412,430,…]`, or the same as CSV. Without an `entity` it falls back to
`<topic>/history/get` and `<topic>/history`; you can also override either topic
with `request_topic` / `history_topic`. See
[Home Assistant graph backfill](#home-assistant-graph-backfill) for a ready‑made
responder. Backfill is **off** by default.

### Colour themes

Eight themes are built in:

`Midnight` (default) · `Ocean` · `Autumn` · `Forest` · `Sunset` · `Mono` ·
`Nord` · `Grape`

Each defines the background, text, dim text, panel borders, an accent colour and
the three gauge bands. Set the default with the top‑level `"theme"` key (name is
case‑insensitive). Add a [`theme` component](#theme) for an on‑screen switcher;
the chosen theme is saved in NVS and overrides the config default until you pick
another.

## What to publish on MQTT

- `gauge`, `counter` and `graph` topics expect a **plain number** payload, e.g.
  `21.5`.
- `text` topics take **any string**.
- Accented characters and the degree sign render correctly (`"°C"` → °C).
- The **Components** card on the device page lists every active component with
  its cell and topic, so you can see exactly what to publish.

Example (mosquitto):

```bash
mosquitto_pub -h <broker> -t home/living/temp -m 21.5
mosquitto_pub -h <broker> -t home/status     -m "All systems OK"
```

A small randomised test publisher for all the demo topics is handy while
building a layout (publish to `home/living/temp`, `home/power`, `home/status`, …).

## Home Assistant graph backfill

To make graphs survive a reboot, Home Assistant answers the device's backfill
request with recent history. A ready‑made, generic
[pyscript](homeassistant/cyd_backfill.py) responder is included — one handler
serves every graph, keyed by the entity in the request topic.

1. Install **pyscript** (via HACS) and enable it in `configuration.yaml`:

   ```yaml
   pyscript:
     allow_all_imports: true
     hass_is_global: true
   ```

2. Copy [`homeassistant/cyd_backfill.py`](homeassistant/cyd_backfill.py) to
   `config/pyscript/` and reload pyscript (Developer Tools → Actions →
   `pyscript.reload`).
3. On the device side, set `"backfill": true` and an `"entity"` on the graph (or
   series).

Test it without the device: Developer Tools → Actions → `mqtt.publish`, topic
`home/history/get/sensor/power`, payload `{"points":120,"interval":120}`, and
watch `home/history/sensor/power`. Note that history only goes back as far as
your recorder's retention, so a 24 h window (`interval: 720`) needs a day of
recorded data.

## OTA updates & releasing

**On the device:** web page → **Firmware** → **Check for update** →
**Update now** (or tap an `update` component). The device downloads the app
image, shows a progress bar and reboots into it. It updates **only if the Pages
version is newer** — never a downgrade. `min_spiffs.csv` provides two app
partitions for OTA.

**To release a new firmware:** bump [`VERSION`](VERSION) (e.g. `2.0.0` → `2.0.1`)
and push to `main`. GitHub Actions builds, publishes an app image
(`firmware/firmware.bin`) and a `version.txt` (semantic version) to Pages, and
redeploys the flasher.

## Web flasher

On every push to `main`, GitHub Actions builds the firmware, merges a factory
`.bin` and deploys a flasher page to GitHub Pages, from which the CYD can be
flashed straight from the browser via **ESP Web Tools** (Web Serial).

- Works in **Chrome / Edge on desktop** (not Firefox/Safari/mobile).
- Enable Pages once: **Settings → Pages → Source: "GitHub Actions"**.
- Page: `https://martindamp.github.io/cyd-temp-gauge/`.

The firmware ships with no credentials, so the public `.bin` contains no secrets.

## Security model

- **OTA firmware** is fetched over HTTPS with the server certificate validated
  against known root CAs (ISRG Root X1, DigiCert Global Root G2, GlobalSign R3,
  USERTrust RSA — covering `github.io`/Fastly), with NTP time sync. The roots
  live in [`src/ota_certs.h`](src/ota_certs.h).
- **The runtime Config URL** is fetched over HTTP, or HTTPS **without**
  certificate validation (the layout is non‑critical and may live on any host),
  so point it at a source you trust — your own LAN server or a GitHub raw URL.
- Credentials (Wi‑Fi, MQTT) live only in the device's NVS, never in the repo or
  the published binaries.

## Troubleshooting

**Display colours.** CYD clones vary in panel/controller. If needed, adjust the
flags at the bottom of `platformio.ini`:

- All colours look negative/inverted → swap `TFT_INVERSION_ON` for `TFT_INVERSION_OFF`
- Red/blue swapped → remove the `TFT_RGB_ORDER=TFT_BGR` line
- Completely black screen → swap `ILI9341_2_DRIVER` for `ILI9341_DRIVER`

Use **Diagnostics → Colour test** on the web page to check a panel.

**Touch in landscape.** Touch mapping differs between portrait and landscape; the
calibration constants (`TOUCH_MINX/MAXX/MINY/MAXY` near the top of
`src/main.cpp`) are typical CYD values. Page navigation only needs the bottom
strip and left/right thirds, so it's tolerant, but if a button tap lands off in
landscape, tweak those constants.

**Can't reach the device page.** The IP is shown in the bottom status bar. If
Wi‑Fi was set wrong, reconnect over USB (flasher page) and re‑provision.

## Project layout

| Path | What |
|------|------|
| `src/main.cpp` | The entire firmware |
| `dashboard.json` | Built‑in dashboard layout (validated + embedded at build time) |
| `tools/gen_dashboard.py` | Pre‑build validator/generator for the layout |
| `src/ota_certs.h` | Root CAs for OTA HTTPS validation |
| `homeassistant/cyd_backfill.py` | Optional HA pyscript responder for graph backfill |
| `web/index.html` | Flasher page (served from GitHub Pages) |
| `.github/workflows/deploy.yml` | CI: build, merge factory bin, deploy flasher + OTA assets |
| `platformio.ini` | Board, build flags, partitions, libraries |
| `VERSION` | Semantic version used for OTA |

## License

MIT
