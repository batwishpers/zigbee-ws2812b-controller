| Supported Targets | ESP32-H2 | ESP32-C6 |
| ----------------- | -------- | -------- |

# Zigbee HA Status Box (RGB light + effects + push buttons)

Firmware for an ESP32-H2/C6 that joins a Zigbee network as an **End Device** and
presents a **color dimmable light** driving a WS2812/SK68XX LED strip, plus:

- selectable **animated effects** (Rainbow / Fire / Candle) on top of static color, and
- optional physical **momentary push buttons**, each exposed as its own Zigbee
  endpoint, that fire press events to the coordinator and can also run an
  on-device action (toggle the light, cycle effects, …) with no automation.

Built on ESP-IDF and the ESP Zigbee SDK (ZBOSS). It works with any Zigbee
coordinator for the light, and ships with a **Home Assistant ZHA quirk** that
exposes the effect selector, the button presses (as device triggers), and the
per-button action selectors.

> Manufacturer / model reported by the firmware: **`batwishpers` / `ws2812b-ctrl-v1`**
> (see `ESP_MANUFACTURER_NAME` / `ESP_MODEL_IDENTIFIER` in `main/esp_zb_light.h`).

## Features

- **RGB color control** — Hue/Saturation, Enhanced Hue, and CIE X/Y, converted to RGB on-device.
- **Brightness control** — 0–255 with smooth scaling.
- **Power on/off** with state restored from NVS on boot.
- **Animated effects** — None / Rainbow / Fire / Candle, selectable over Zigbee.
- **State persistence** — last color/brightness/power saved to NVS (debounced), restored on reboot.
- **Push buttons** — momentary switches reported to the coordinator + optional on-device actions, persisted to NVS.

## Zigbee endpoints & clusters

| Endpoint | Role | Clusters |
| --- | --- | --- |
| `1` | Color dimmable light | On/Off, Level Control, Color Control (Hue/Sat, Enhanced Hue, X/Y, Color Loop), Basic/Identify, **custom `0xFC00`** effect selector |
| `2 … 1+N` | Momentary button (one per button) | On/Off **client** (sends Toggle on press), On/Off Switch Configuration (`SwitchType = Momentary`), Identify, **custom `0xFC01`** action selector |

**Custom clusters** (manufacturer-specific, plain attributes — no manufacturer code):

- `0xFC00` attr `0x0000` (enum8) — effect: `0=None, 1=Rainbow, 2=Fire, 3=Candle`.
- `0xFC01` attr `0x0000` (enum8) — per-button on-device action:
  `0=None, 1=ToggleLight, 2=NextEffect, 3=Rainbow, 4=Fire, 5=Candle`.

## Hardware

- **ESP32-H2 dev board** (acts as Zigbee end device). ESP32-C6 also supported (see `dependencies.lock`).
- **WS2812 / SK68XX LED strip** on **GPIO 8**.
- **Push buttons** (optional), wired active-low: `GPIO → button → GND` (internal pull-up is enabled in firmware).
- **USB cable** for power/flashing, and any **Zigbee coordinator** (Home Assistant ZHA, Zigbee2MQTT, …).

### Connections

| ESP32 pin | Connects to | Code reference |
| --- | --- | --- |
| GPIO 8 | LED strip data in (DIN) | `CONFIG_EXAMPLE_STRIP_LED_GPIO` (`light_driver.h`) |
| 5V/3.3V | LED strip VCC | — |
| GND | LED strip GND **and** button commons | — |
| GPIO 0, 1, 4 | Button 1, 2, 3 (other side → GND) | `s_button_gpios[]` (`button_driver.c`) |

> The default button pins (`GPIO_NUM_0/1/4`) are a safe-default guess — confirm
> they are free on your board. **Avoid GPIO 8 (LED) and the boot-strap pin
> (GPIO 9 on most ESP32-H2 devkits).** A solid common ground between the buttons,
> the LED strip, and the ESP is important for clean WS2812 signaling.
> 5V LED strips may need a level shifter or separate supply for the data line.

**Adding more buttons:** append the GPIO to `s_button_gpios[]` in
`main/button_driver.c` (keep the count ≤ `BUTTON_MAX`), and bump `BUTTON_COUNT`
in `zha_quirk/zigbee_status_box.py` to match. Nothing else to change.

## Build & flash (ESP-IDF v6.0)

ESP-IDF lives at `~/.espressif/v6.0.1/esp-idf`. Source its activation script
first (use **bash**, not zsh):

```bash
source ~/.espressif/tools/activate_idf_v6.0.1.sh
```

Then:

```bash
idf.py set-target esp32h2        # or esp32c6
idf.py -p PORT erase-flash       # recommended before first flash (clears NVS + Zigbee storage)
idf.py -p PORT flash monitor     # build, flash, open serial monitor (Ctrl-] to exit)
```

`ZB_ED_ROLE` must be defined (set in `sdkconfig` / via menuconfig) or compilation
fails by design.

### Erasing NVS

The light/effect state and per-button actions live in the `nvs` partition; the
Zigbee network join lives in `zb_storage`.

```bash
idf.py -p PORT erase-flash                                   # everything (also forces re-pair)
parttool.py -p PORT erase_partition --partition-name nvs     # only saved light/button state
```

(Close the serial monitor first — it holds the port.)

## Home Assistant (ZHA) setup

1. Copy `zha_quirk/zigbee_status_box.py` into your HA custom-quirks directory,
   e.g. `config/custom_zha_quirks/`.
2. Enable quirks in `configuration.yaml`:
   ```yaml
   zha:
     enable_quirks: true
     custom_quirks_path: /config/custom_zha_quirks/
   ```
3. Restart HA, then remove & re-add (or reconfigure) the device so ZHA reloads the quirk.

You then get:

- a light entity (color + brightness),
- an **"Effect mode"** select (None / Rainbow / Fire / Candle),
- a **"Button N action"** select per button, and
- a **"Button N pressed"** device automation trigger per button.

`BUTTON_EP_BASE` / `BUTTON_COUNT` in the quirk must match `button_driver.c`.

### Triggering automations on a button press

Either pick the device trigger **"Button N pressed"** in the automation UI, or
listen for the raw event:

```yaml
trigger:
  - platform: event
    event_type: zha_event
    event_data:
      endpoint_id: 2        # 2/3/4 = button 1/2/3
      cluster_id: 6
      command: toggle
```

To confirm presses reach HA, use **Developer Tools → Events → listen to
`zha_event`** and press a button (the serial monitor also logs
`Button N press -> Toggle command sent`).

## How it works (firmware)

1. `app_main` configures the radio/host platform and spawns `esp_zb_task`.
2. `esp_zb_task` initializes ZBOSS, builds the light endpoint (`1`) plus one
   On/Off-Switch endpoint per button, registers `zb_action_handler`, and commissions.
3. `deferred_driver_init` (on first start / reboot) loads saved state from NVS,
   restores the LED, mirrors values back into the ZCL attribute table, starts the
   100 ms effect render timer, and starts the button driver.
4. Coordinator writes flow through `zb_action_handler` → `zb_attribute_handler`
   (On/Off, Level, Color Control, `0xFC00` effect, `0xFC01` button action), then
   debounce-save to NVS.
5. A button press: `button_driver`'s 10 ms poll task debounces, then schedules
   `button_action_handler` onto the ZBOSS stack context (via
   `esp_zb_scheduler_alarm`, so deep stack calls don't overflow the poll task).
   The handler sends an On/Off `Toggle` to the coordinator and runs the button's
   configured on-device action.

See `CLAUDE.md` for deeper architecture notes (color-mode handling, effect
rendering, and the button threading model).

## Troubleshooting

**Device not seen / lost after a crash:** if the device reset, it may have left
the network — remove and re-add it in your coordinator. `erase-flash` forces a
clean re-pair.

**Button presses not visible in HA:** confirm the quirk is installed and the
device was re-added. Watch `zha_event` in Developer Tools while pressing. If
still nothing, the On/Off output cluster may need binding to the coordinator
(Settings → device → Clusters).

**LED strip: only the first LED lights / wrong colors:** this is a
wiring/signal-integrity issue, not firmware — check the data line, a solid common
ground, and 5V power. Color order is RGB.

**Build fails with a `ZB_ED_ROLE` error:** the End Device role isn't enabled in
`sdkconfig` — it's required by design.

## References

- [ESP Zigbee SDK Docs](https://docs.espressif.com/projects/esp-zigbee-sdk)
- [ESP Zigbee SDK Repo](https://github.com/espressif/esp-zigbee-sdk)
