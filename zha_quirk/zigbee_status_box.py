"""ZHA custom quirk for the Zigbee HA status box.

Exposes the device's custom manufacturer-specific entities to Home Assistant:

* The animated "effect mode" attribute (custom cluster 0xFC00, attribute 0x0000)
  on endpoint 1 as a *select* entity.
* Each physical push button as its own endpoint (2, 3, ...) carrying:
    - an On/Off *client* (output) cluster: the button sends a Toggle command on
      every press, which HA surfaces as a `zha_event` and as a device automation
      trigger ("Button N pressed"). It is stateless — nothing stays "on".
    - an On/Off Switch Configuration cluster declaring SwitchType=Momentary
      (exposed as a "Switch type" config entity), and
    - a custom "button action" attribute (cluster 0xFC01, attribute 0x0000)
      exposed as a *select* so you can pick an on-device action that runs
      locally on the firmware (toggle the light, cycle/choose an effect) without
      any HA automation.

To trigger automations: use the device's automation triggers ("Button N
pressed"), or listen for the raw `zha_event` (cluster_id 6, command "toggle",
endpoint_id = 1 + button number).

Installation
------------
1. Copy this file into your Home Assistant config under a quirks directory, e.g.
       config/custom_zha_quirks/zigbee_status_box.py
2. In Settings -> Devices & Services -> ZHA -> Configure, set
   "Custom quirks path" to that directory (or add to configuration.yaml:
       zha:
         custom_quirks_path: /config/custom_zha_quirks/
   ).
3. Restart Home Assistant, then remove & re-add (or reconfigure) the device so
   ZHA reloads its quirk. The "Effect mode" select, one On/Off entity per button,
   and one "Button N action" select per button should appear.

The firmware reports manufacturer "batwishpers" and model "ws2812b-ctrl-v1".
These must stay byte-identical to ESP_MANUFACTURER_NAME / ESP_MODEL_IDENTIFIER
in main/esp_zb_light.h (sans the length-prefix byte).

BUTTON_COUNT must match the number of GPIOs in s_button_gpios[] in
main/button_driver.c, and BUTTON_EP_BASE must match BUTTON_EP_BASE there.
"""

from zigpy.quirks import CustomCluster
from zigpy.quirks.v2 import QuirkBuilder
import zigpy.types as t
from zigpy.zcl.foundation import BaseAttributeDefs, ZCLAttributeDef

MANUFACTURER = "batwishpers"
MODEL = "ws2812b-ctrl-v1"

EFFECT_CLUSTER_ID = 0xFC00
EFFECT_ATTR_ID = 0x0000

BUTTON_ACTION_CLUSTER_ID = 0xFC01
BUTTON_ACTION_ATTR_ID = 0x0000

ENDPOINT_ID = 1

# Keep in sync with main/button_driver.c (s_button_gpios[] length and
# BUTTON_EP_BASE).
BUTTON_EP_BASE = 2
BUTTON_COUNT = 3


class EffectMode(t.enum8):
    """Animated light effect selected on the device."""

    NoEffect = 0
    Rainbow = 1
    Fire = 2
    Candle = 3


class ButtonAction(t.enum8):
    """On-device action a button runs locally when pressed."""

    NoAction = 0
    ToggleLight = 1
    NextEffect = 2
    Rainbow = 3
    Fire = 4
    Candle = 5


class LightEffectCluster(CustomCluster):
    """Manufacturer-specific cluster carrying the effect selector."""

    cluster_id = EFFECT_CLUSTER_ID
    name = "Light Effect"
    ep_attribute = "light_effect"

    class AttributeDefs(BaseAttributeDefs):
        # NOT manufacturer-specific: the firmware registers a plain custom
        # attribute (no manufacturer code), so the write frame must not carry
        # one — otherwise ZBOSS rejects it with UNSUPPORTED_ATTRIBUTE.
        effect_mode = ZCLAttributeDef(
            id=EFFECT_ATTR_ID,
            type=EffectMode,
            access="rw",
            is_manufacturer_specific=False,
        )


class ButtonActionCluster(CustomCluster):
    """Manufacturer-specific cluster carrying a button's on-device action."""

    cluster_id = BUTTON_ACTION_CLUSTER_ID
    name = "Button Action"
    ep_attribute = "button_action"

    class AttributeDefs(BaseAttributeDefs):
        # Plain custom attribute (no manufacturer code) — see note above.
        button_action = ZCLAttributeDef(
            id=BUTTON_ACTION_ATTR_ID,
            type=ButtonAction,
            access="rw",
            is_manufacturer_specific=False,
        )


_builder = (
    QuirkBuilder(MANUFACTURER, MODEL)
    .replaces(LightEffectCluster, endpoint_id=ENDPOINT_ID)
    .enum(
        attribute_name=LightEffectCluster.AttributeDefs.effect_mode.name,
        enum_class=EffectMode,
        cluster_id=EFFECT_CLUSTER_ID,
        endpoint_id=ENDPOINT_ID,
        translation_key="effect_mode",
        fallback_name="Effect mode",
    )
)

# Per-button: replace the custom action cluster and expose it as a select. The
# On/Off output cluster on each button endpoint is surfaced by ZHA automatically
# and fires a zha_event on press.
_triggers = {}
for _i in range(BUTTON_COUNT):
    _ep = BUTTON_EP_BASE + _i
    _builder = _builder.replaces(
        ButtonActionCluster, endpoint_id=_ep
    ).enum(
        attribute_name=ButtonActionCluster.AttributeDefs.button_action.name,
        enum_class=ButtonAction,
        cluster_id=BUTTON_ACTION_CLUSTER_ID,
        endpoint_id=_ep,
        translation_key=f"button_{_i + 1}_action",
        fallback_name=f"Button {_i + 1} action",
    )
    # Named device automation trigger for each button's press (Toggle command on
    # the On/Off cluster, id 6). Key is (trigger_type, trigger_subtype).
    _triggers[("remote_button_short_press", f"button_{_i + 1}")] = {
        "endpoint_id": _ep,
        "cluster_id": 0x0006,
        "command": "toggle",
    }

_builder.device_automation_triggers(_triggers).add_to_registry()
