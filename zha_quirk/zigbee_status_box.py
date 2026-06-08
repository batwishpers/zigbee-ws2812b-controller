"""ZHA custom quirk for the Zigbee HA status box.

Exposes the manufacturer-specific "effect mode" attribute (custom cluster
0xFC00, attribute 0x0000) as a Home Assistant *select* entity so you can pick
the animated light effect running on the firmware.

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
   ZHA reloads its quirk. A new "Effect mode" select entity should appear.

The firmware reports manufacturer "batwishpers" and model "ws2812b-ctrl-v1".
These must stay byte-identical to ESP_MANUFACTURER_NAME / ESP_MODEL_IDENTIFIER
in main/esp_zb_light.h (sans the length-prefix byte).
"""

from zigpy.quirks import CustomCluster
from zigpy.quirks.v2 import QuirkBuilder
import zigpy.types as t
from zigpy.zcl.foundation import BaseAttributeDefs, ZCLAttributeDef

MANUFACTURER = "batwishpers"
MODEL = "ws2812b-ctrl-v1"

EFFECT_CLUSTER_ID = 0xFC00
EFFECT_ATTR_ID = 0x0000

ENDPOINT_ID = 1


class EffectMode(t.enum8):
    """Animated light effect selected on the device."""

    NoEffect = 0
    Rainbow = 1
    Fire = 2
    Candle = 3


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


(
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
    .add_to_registry()
)
