# CLAUDE.md — cyd-temp-gauge

Firmware for the ESP32 "Cheap Yellow Display" (ESP32-2432S028R, 2.8" ILI9341):
a configurable MQTT dashboard (gauges, counters, text, graphs, buttons) on one
or more pages, portrait/landscape, with colour themes. PlatformIO + Arduino.

## Commands
- Build: `pio run -e cyd` (the only env). A pre-build script
  `tools/gen_dashboard.py` validates `dashboard.json` and generates
  `src/dashboard_generated.h`.
- Release firmware (for OTA): bump `VERSION` (semver) and push to `main`. CI
  (`.github/workflows/deploy.yml`) builds, merges a factory bin, and deploys the
  flasher + OTA assets (`firmware.bin`, `version.txt`) to GitHub Pages.
- Watch CI: `gh run watch <id> --exit-status` (fetching full logs is blocked in
  the cloud sandbox; rely on job conclusions and local reasoning).

## Architecture
- Everything is in `src/main.cpp` (single file).
- Layout comes from `dashboard.json` → embedded at build time, OR fetched at
  runtime from a "Config URL" (cached in LittleFS, falls back to the built-in).
  Same JSON format either way; parsed in `parseDashboardJson()`.
- Runtime state (Wi-Fi, MQTT broker, chosen theme) lives in NVS (`Preferences`),
  not in the repo. No secrets are committed or in the published binary.
- Component types: gauge, counter, text, graph, update, theme, wifi, mqtt.
  Grid is 3x2 (portrait) / 2x3 (landscape); placement via row/col/rowspan/colspan.
- Overlays (page menu, theme picker, wifi scan, on-screen keyboard, mqtt form)
  are full-screen and suppress redraws while open.

## Conventions / gotchas
- Bump `VERSION` for any firmware change meant to reach devices over OTA.
- Keep ALL TFT_eSPI config in `platformio.ini` build flags — never edit the lib.
  This panel needs `TFT_INVERSION_ON` + `TFT_RGB_ORDER=TFT_BGR`.
- `src/dashboard_generated.h` and `src/build_version.h` are generated and
  .gitignored — never commit them.
- The cloud sandbox often can't download the ESP32 toolchain (proxy), so a local
  `pio run` may fail at platform install — rely on CI to compile.
- C++ ordering: functions are called before they're defined in several places;
  add a forward declaration near the top block when introducing one, or the
  build fails (this has bitten us).
- Touch: calibration constants `TOUCH_MINX/MAXX/MINY/MAXY` near the top of
  main.cpp; landscape touch mapping is best-effort.
- Graphs: up to 6 series buffers total (`MAX_SERIES`), 120 points each. Optional
  MQTT backfill uses `home/history/get/<entity>` ↔ `home/history/<entity>`; a
  ready HA pyscript responder is in `homeassistant/cyd_backfill.py`.

## Don't
- Don't put secrets in `dashboard.json` or the repo.
- Don't commit generated headers.
- Don't widen the config fetch to validate certs (it's intentionally insecure);
  OTA firmware fetch stays strictly cert-validated.
