# CYD Temperature Gauge (MQTT)

A PlatformIO project for the **Cheap Yellow Display** (ESP32-2432S028R, 2.8" ILI9341)
that subscribes to an MQTT topic and shows the temperature as a coloured ring gauge
(blue → green → red bands) with the value in the centre.

![board](https://img.shields.io/badge/board-ESP32--2432S028R-yellow)
![framework](https://img.shields.io/badge/framework-Arduino%20%2F%20PlatformIO-blue)

## Features

- Ring gauge with three fixed colour bands across a configurable scale
- Large 7-segment value in the centre
- Status dot: green = WiFi + MQTT OK, red = no connection
- Shows a "stale data" marker if no new message has arrived for 60 s
- Automatic MQTT reconnect
- No hardcoded credentials: WiFi via Improv Wi-Fi, everything else via the
  device's own config page; all settings are stored in NVS and survive a restart
- Configurable title and unit (supports accented characters and the degree sign)
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
2. **Build** → **Upload** with the CYD on USB. Serial monitor at 115200 baud.
3. The first time, the device asks for WiFi over Improv (from the web flasher
   below) or connects with stored credentials. Then open the device's config
   page (its IP is shown at the bottom of the screen) to set the MQTT broker,
   topic, scale, title and unit.

`lib_deps` pulls in TFT_eSPI, PubSubClient, Improv Wi-Fi and U8g2 on the first build.

## MQTT payload

The code expects a plain number, e.g. `21.5`.
If the temperature arrives as JSON (e.g. `{"temperature": 21.5}`), there is a
ready-made ArduinoJson variant in the comment at the bottom of `src/main.cpp`.

## Troubleshooting

Commented out at the bottom of `platformio.ini`:

- All colours look negative/inverted → swap `TFT_INVERSION_ON` for `TFT_INVERSION_OFF`
- Red/blue swapped → remove the `TFT_RGB_ORDER=TFT_BGR` line
- Completely black screen → swap `ILI9341_2_DRIVER` for `ILI9341_DRIVER`

There is also a built-in **colour test** on the config page (Diagnostics →
Show colour test) that shows seven named colours for diagnosing such faults.

(CYD clones vary slightly in display controller and colour handling.)

## Web flasher (GitHub Pages)

On every push to `main`, GitHub Actions builds the firmware, merges a factory
`.bin` and deploys a flasher page to GitHub Pages. From there the CYD can be
flashed straight from the browser via **ESP Web Tools** (Web Serial) — no
Arduino IDE or PlatformIO needed.

- Works in **Chrome / Edge on desktop** (Web Serial is not in Firefox/Safari/mobile)
- Enable Pages once: **Settings → Pages → Source: "GitHub Actions"**
- Page: `https://martindamp.github.io/cyd-temp-gauge/`

The firmware ships with no credentials, so the public `.bin` on Pages contains
no secrets. WiFi is provisioned via Improv in the browser after flashing, and
MQTT via the device's own config page.

## OTA update (wireless)

The device can update itself over WiFi — no cable needed after the first flash:

1. Open the device's config page (`http://<device-ip>/` — the IP is shown at the
   bottom of the screen)
2. Under **Firmware** → click **Check for update**
3. If a newer build is on GitHub Pages, **Update now** appears — click it
4. The CYD downloads the app firmware from Pages, shows a progress bar and
   restarts itself into the new version

How it fits together: GitHub Actions builds an app-only `firmware/firmware.bin`
and publishes a `version.txt` with the **semantic version** to Pages. The device
has its own version compiled in and updates **only if the Pages version is newer**
— never a downgrade. `min_spiffs.csv` provides two app partitions, so the new
firmware is written to the inactive one and booted into.

**Release a new version:** bump the number in the `VERSION` file (e.g. `1.2.0` →
`1.2.1`) and push. CI builds, and devices in the field offer the update the next
time you press *Check*. Pushing without bumping `VERSION` redeploys the firmware,
but devices see it as the same version and offer no update.

**HTTPS:** OTA validates the server certificate against known root CAs (ISRG Root
X1, DigiCert Global Root G2, GlobalSign R3, USERTrust RSA — covering
`github.io`/Fastly), with NTP time sync. Rotation of GitHub's leaf/intermediate
certificate does not affect OTA as long as the chain goes to one of these
long-lived roots. The roots live in `src/ota_certs.h` (generated from Mozilla's
CA bundle).

## License

MIT
