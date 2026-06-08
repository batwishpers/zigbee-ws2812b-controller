# CLAUDE.md

## Overview

Zigbee Home Automation RGB status box firmware for Espressif ESP32 (H2/C6) SoCs,
built on ESP-IDF and the ESP Zigbee SDK (ZBOSS). The device joins a Zigbee
network as an **End Device** and presents itself as a color dimmable light,
driving a WS2812/SK68XX LED strip. Controllable from any Zigbee coordinator
(Zigbee2MQTT, Home Assistant, etc.).

## Build & Flash (ESP-IDF)

ESP-IDF v6.0 lives at `~/.espressif/v6.0.1/esp-idf`. Source its export script to
get `idf.py` on PATH (no need to search for it):

```bash
bash -lc 'source ~/.espressif/tools/activate_idf_v6.0.1.sh && ~/.espressif/v6.0.1/esp-idf/tools/idf.py <whatever parameters>'
```

Always use a bash, does not works with zsh.

Then:

```bash
idf.py set-target esp32h2     # or esp32c6 — see dependencies.lock (currently esp32h2)
idf.py -p PORT erase-flash    # recommended before first flash (clears NVS)
idf.py -p PORT flash monitor  # build, flash, open serial monitor (Ctrl-] to exit)
```

ZB_ED_ROLE must be defined (set via menuconfig / sdkconfig) or compilation fails
by design.

## Layout

- `main/esp_zb_light.c` — entry point (`app_main`), Zigbee task, signal handler,
  ZCL attribute write handler, HSV→RGB and CIE X/Y→RGB color conversion, and
  state persistence wiring.
- `main/esp_zb_light.h` — Zigbee config macros (ED role, endpoint `1`,
  channel mask, manufacturer/model strings).
- `main/light_driver.{c,h}` — LED strip abstraction over `led_strip` (RMT).
  Holds module-static RGB/power/brightness and applies brightness scaling.
  GPIO and LED count are `CONFIG_EXAMPLE_STRIP_LED_GPIO` (8) and
  `CONFIG_EXAMPLE_STRIP_LED_NUMBER` (36) in `light_driver.h`.
- `main/light_state_nvs.{c,h}` — persists last known light state to NVS with a
  500 ms debounce timer; loads on boot (orange ~5% default on first boot).
- `main/common/zcl_utility/` — vendored ESP helper for adding Basic-cluster
  manufacturer info to an endpoint.
- `zha_quirk/zigbee_status_box.py` — Home Assistant ZHA custom quirk that exposes
  the animated-effect selector (see "Light effects" below) as a select entity.
- `managed_components/` — IDF Component Manager deps (esp-zboss-lib,
  esp-zigbee-lib, led_strip); versions pinned in `dependencies.lock`.
- `partitions.csv` — custom partition table (factory app + Zigbee fat storage).
- `sdkconfig.defaults` — project config (custom partitions, ZBOSS ED enabled,
  mbedTLS tweaks for Zigbee security).
- `CMakeLists.txt` / `main/CMakeLists.txt` — ESP-IDF build (MINIMAL_BUILD on).

## How it works

1. `app_main` configures the radio/host platform and spawns `esp_zb_task`.
2. `esp_zb_task` initializes the ZBOSS stack, creates a color dimmable light
   endpoint, registers `zb_action_handler`, and starts commissioning.
3. On first start / reboot, `deferred_driver_init` loads saved state from NVS,
   restores LED color/brightness/power, and mirrors values back into the ZCL
   attribute table so the coordinator sees correct state on reconnect.
4. Coordinator writes flow through `zb_action_handler` → `zb_attribute_handler`,
   which dispatches On/Off, Level Control, and Color Control (Hue/Sat, X/Y,
   Enhanced Hue) attributes to the LED driver, then debounce-saves to NVS.

### Color mode caveat

The ZCL `ColorMode` attribute is not updated by the stack on raw attribute
writes, so it cannot be trusted to pick the restore conversion. The firmware
tracks the actually-applied mode in `s_active_color_mode` (set on every hue/sat
or X/Y write) and persists *that* as `color_mode`. When editing color handling,
keep this in sync — see comments in `esp_zb_light.c:31`.

## Light effects (animated themes)

The device supports selectable animated effects (None / Rainbow / Fire / Candle)
on top of the standard static color control.

- **Transport:** a manufacturer-specific custom cluster `0xFC00` with a single
  `enum8` attribute `0x0000` (`0=None, 1=Rainbow, 2=Fire, 3=Candle`,
  `READ_WRITE | REPORTING`), created in `esp_zb_task` via
  `esp_zb_zcl_attr_list_create` + `esp_zb_custom_cluster_add_custom_attr` +
  `esp_zb_cluster_list_add_custom_cluster`. The attribute is **not**
  manufacturer-code-tagged on either side — writing it with a manufacturer code
  makes ZBOSS reject it as `UNSUPPORTED_ATTRIBUTE`.
- **Rendering:** a single 100 ms FreeRTOS auto-reload timer (`color_loop_render_cb`,
  started in `deferred_driver_init`) dispatches on `s_effect_mode`. `render_rainbow`
  firmware-drives an Enhanced-Hue sweep (the ZBOSS stack sets `ColorLoopActive` on a
  ZCL `ColorLoopSet` but does *not* advance the hue itself); `render_flicker` powers
  both Fire (hue ~5–35°) and Candle (hue ~28–42°) using `esp_random()`-eased
  brightness/hue. A ZCL-driven `ColorLoopActive` is treated as Rainbow for backward
  compat. Fire/Candle deliberately leave `s_active_color_mode` untouched so the saved
  static color survives.
- **Static color wins:** any manual color write (Hue/Sat, X/Y, Enhanced Hue) calls
  `deactivate_effect()`, which clears `s_effect_mode` and writes `None` back to the
  `0xFC00` attribute so ZHA's select returns to NoEffect (no blink/restart).
- **Restore:** when an effect ends, `apply_static_color()` re-applies the last
  user color from the live ZCL attributes.
- **Not persisted:** `s_effect_mode` resets to None on reboot (no NVS field).
- **ZHA setup:** install `zha_quirk/zigbee_status_box.py` into HA's custom-quirks
  path (header in the file documents the steps). The quirk targets endpoint
  `1` (= `HA_ESP_LIGHT_ENDPOINT`) and matches manufacturer `batwishpers` / model
  `ws2812b-ctrl-v1`. These must stay byte-identical to `ESP_MANUFACTURER_NAME` /
  `ESP_MODEL_IDENTIFIER` in `esp_zb_light.h` (each string there carries a
  length-prefix byte that must equal the character count).

## Conventions

- C, ESP-IDF style. Module-private state is `s_`-prefixed file statics.
- Errors use `ESP_ERROR_CHECK` / `ESP_RETURN_ON_FALSE` and `esp_err_t`.
- Logging via `ESP_LOGI/W` with per-module `TAG` (e.g. `ESP_ZB_COLOR_LIGHT`).
- `.clangd` strips `-f*`/`-m*` flags so clangd works with the GCC xtensa/riscv
  toolchain.

## Notes

- `build/`, `sdkconfig.old`, `.cache`, `.vscode`, `doc/`, `docs/` are gitignored.
- `docs/` contain official zigbee cluster definition
