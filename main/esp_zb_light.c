/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier:  LicenseRef-Included
 *
 * Zigbee HA_on_off_light Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "math.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zb_light.h"
#include "light_state_nvs.h"
#include "esp_zigbee_attribute.h"
#include "esp_random.h"

#if !defined ZB_ED_ROLE
#error Define ZB_ED_ROLE in idf.py menuconfig to compile light (End Device) source code.
#endif

static const char *TAG = "ESP_ZB_COLOR_LIGHT";

/* Tracks which colour-control conversion was last applied to the LEDs. The ZCL
 * ColorMode attribute defaults to 0x01 (X/Y) and the stack does not update it on
 * raw attribute writes, so it can't be trusted to pick the restore conversion.
 * Updated on every hue/sat or X/Y write and persisted as the saved color_mode. */
static uint8_t s_active_color_mode = LIGHT_NVS_DEFAULT_COLOR_MODE;

/* s_active_color_mode value for Enhanced Hue (and active color loop). The ZCL
 * spec doesn't define this for the legacy ColorMode attribute, so it's a
 * firmware-internal marker used to pick the restore conversion. */
#define LIGHT_COLOR_MODE_ENHANCED_HUE  0x02U

/* How often the color loop re-renders the LED from EnhancedCurrentHue. */
#define COLOR_LOOP_RENDER_INTERVAL_MS  100

/* Manufacturer-specific cluster + attribute that select an animated light effect.
 * ZHA drives this via a custom quirk (see zha_quirk/zigbee_status_box.py). The
 * firmware runs the chosen effect inside the existing render timer. */
#define LIGHT_EFFECT_CLUSTER_ID  0xFC00
#define LIGHT_EFFECT_ATTR_ID     0x0000
enum {
    LIGHT_EFFECT_NONE    = 0,
    LIGHT_EFFECT_RAINBOW = 1,
    LIGHT_EFFECT_FIRE    = 2,
    LIGHT_EFFECT_CANDLE  = 3,
};
static uint8_t s_effect_mode = LIGHT_EFFECT_NONE;

/* Forward declarations for helpers defined later in this file */
static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b);
void cie_xy_to_rgb(uint16_t x, uint16_t y, uint8_t *r, uint8_t *g, uint8_t *b);

/* Periodic timer that mirrors the stack-managed EnhancedCurrentHue attribute
 * onto the LED while a color loop is active. */
static TimerHandle_t s_color_loop_timer = NULL;

static uint8_t color_attr_u8(uint16_t attr_id, uint8_t fallback)
{
    esp_zb_zcl_attr_t *a = esp_zb_zcl_get_attribute(HA_ESP_LIGHT_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id);
    return (a && a->data_p) ? *(uint8_t *)a->data_p : fallback;
}

static uint16_t color_attr_u16(uint16_t attr_id, uint16_t fallback)
{
    esp_zb_zcl_attr_t *a = esp_zb_zcl_get_attribute(HA_ESP_LIGHT_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id);
    return (a && a->data_p) ? *(uint16_t *)a->data_p : fallback;
}

/* Restore the LED to the last user-selected static color. Mirrors the restore
 * logic in deferred_driver_init, but reads live ZCL attribute values so it can be
 * called whenever an animated effect ends. Does not touch s_active_color_mode. */
static void apply_static_color(void)
{
    uint8_t r, g, b;
    if (s_active_color_mode == 0x00) {
        float h = (color_attr_u8(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID, 0) / 255.0f) * 360.0f;
        float s = color_attr_u8(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID, 255) / 255.0f;
        hsv_to_rgb(h, s, 1.0f, &r, &g, &b);
    } else if (s_active_color_mode == LIGHT_COLOR_MODE_ENHANCED_HUE) {
        float h = (color_attr_u16(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID, 0) / 65535.0f) * 360.0f;
        float s = color_attr_u8(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID, 255) / 255.0f;
        hsv_to_rgb(h, s, 1.0f, &r, &g, &b);
    } else {
        cie_xy_to_rgb(color_attr_u16(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID, 0),
                      color_attr_u16(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID, 0), &r, &g, &b);
    }
    light_driver_set_color(r, g, b);
}

/* Rainbow sweep — drives EnhancedCurrentHue ourselves. The stack sets
 * ColorLoopActive on a ZCL ColorLoopSet but does NOT advance the hue (it just
 * re-writes the start hue, which stays 0 = red), so we step a local accumulator
 * and render it. `fresh` re-seeds from the configured start hue on (re)activation. */
static void render_rainbow(bool fresh)
{
    static uint16_t hue = 0;

    uint8_t  dir   = color_attr_u8(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_DIRECTION_ID, 1);
    uint16_t ltime = color_attr_u16(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_TIME_ID, 0x0019);
    if (ltime == 0) {
        ltime = 0x0019;
    }

    /* A color loop is a vivid rainbow sweep, so render at full saturation. We do
     * NOT read CurrentSaturation here: it can legitimately be 0 (white), which
     * would wash the loop out entirely. */
    const uint8_t sat = 0xFE;

    if (fresh) {
        hue = color_attr_u16(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_START_ENHANCED_HUE_ID, 0);
        ESP_LOGI(TAG, "Rainbow start — dir=%u time=%us start_hue=%u", dir, ltime, hue);
    }

    /* Advance one render tick of a full 0..65535 sweep performed over `ltime`
     * seconds. Direction 0x00 = decrement, 0x01 = increment (per ZCL spec). */
    uint32_t step = (uint32_t)((65536.0f * COLOR_LOOP_RENDER_INTERVAL_MS) / (ltime * 1000.0f));
    if (step == 0) {
        step = 1;
    }
    hue = (dir == 0x00) ? (uint16_t)(hue - step) : (uint16_t)(hue + step);

    uint8_t r, g, b;
    hsv_to_rgb((hue / 65535.0f) * 360.0f, sat / 255.0f, 1.0f, &r, &g, &b);
    light_driver_set_color(r, g, b);
    s_active_color_mode = LIGHT_COLOR_MODE_ENHANCED_HUE;
}

/* Organic warm-flicker render shared by fire and candle. Each tick eases the
 * brightness and hue toward fresh random targets so the result shimmers instead
 * of strobing. Deliberately does NOT touch s_active_color_mode, so the static
 * color saved for restore is preserved. */
static void render_flicker(float hue_lo, float hue_hi, float val_lo, float val_hi, float smoothing)
{
    static float level = 0.7f;
    static float hue   = 25.0f;

    float val_target = val_lo + (val_hi - val_lo) * (esp_random() / (float)UINT32_MAX);
    float hue_target = hue_lo + (hue_hi - hue_lo) * (esp_random() / (float)UINT32_MAX);

    level += (val_target - level) * smoothing;
    hue   += (hue_target - hue)   * smoothing;

    uint8_t r, g, b;
    hsv_to_rgb(hue, 1.0f, level, &r, &g, &b);
    light_driver_set_color(r, g, b);
}

static void color_loop_render_cb(TimerHandle_t timer)
{
    static uint8_t last_effect = LIGHT_EFFECT_NONE;

    /* A ZCL-driven color loop (ColorLoopActive) is treated as the rainbow effect. */
    uint8_t effect = s_effect_mode;
    if (effect == LIGHT_EFFECT_NONE &&
        color_attr_u8(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_ACTIVE_ID, 0) != 0) {
        effect = LIGHT_EFFECT_RAINBOW;
    }

    if (effect == LIGHT_EFFECT_NONE) {
        last_effect = LIGHT_EFFECT_NONE;
        return;
    }

    bool fresh = (effect != last_effect);
    last_effect = effect;

    switch (effect) {
    case LIGHT_EFFECT_RAINBOW:
        render_rainbow(fresh);
        break;
    case LIGHT_EFFECT_FIRE:
        render_flicker(5.0f, 35.0f, 0.40f, 1.0f, 0.30f);
        break;
    case LIGHT_EFFECT_CANDLE:
        render_flicker(28.0f, 42.0f, 0.50f, 0.95f, 0.15f);
        break;
    default:
        break;
    }
}
/********************* Define functions **************************/
static esp_err_t deferred_driver_init(void)
{
    // Initialise the debounce timer used for NVS saves
    if (light_state_nvs_init() != ESP_OK) {
        ESP_LOGW(TAG, "NVS save timer init failed — state will not persist across reboots");
    }

    // Load persisted Zigbee attribute values (orange defaults on first boot)
    light_state_t state;
    light_state_nvs_load(&state);
    s_active_color_mode = state.color_mode;

    // Initialise the LED strip hardware (strip starts dark)
    light_driver_init(LIGHT_DEFAULT_OFF);

    // Restore colour — dispatch to existing converters based on ZCL colour_mode
    uint8_t r, g, b;
    if (state.color_mode == 0x00) {
        // Hue / Saturation mode
        float h = (state.current_hue / 255.0f) * 360.0f;
        float s = state.current_saturation / 255.0f;
        hsv_to_rgb(h, s, 1.0f, &r, &g, &b);
    } else if (state.color_mode == LIGHT_COLOR_MODE_ENHANCED_HUE) {
        // Enhanced Hue mode (last applied via enhanced hue or color loop)
        float h = (state.enhanced_hue / 65535.0f) * 360.0f;
        float s = state.current_saturation / 255.0f;
        hsv_to_rgb(h, s, 1.0f, &r, &g, &b);
    } else {
        // XY mode (and any other mode — fall back to XY)
        cie_xy_to_rgb(state.current_x, state.current_y, &r, &g, &b);
    }
    light_driver_set_color(r, g, b);
    light_driver_set_brightness(state.current_level);
    light_driver_set_power(state.on_off);

    ESP_LOGI(TAG, "Driver restored — pwr=%s lvl=%d rgb=(%d,%d,%d) mode=%d",
             state.on_off ? "On" : "Off", state.current_level, r, g, b, state.color_mode);

    // Mirror the restored values back into the Zigbee cluster attributes so the
    // coordinator sees the correct state immediately after reconnect (no conversion —
    // we write the raw Zigbee values we loaded from NVS straight back).
    bool     pwr  = state.on_off;
    uint8_t  lvl  = state.current_level;
    uint8_t  hue  = state.current_hue;
    uint8_t  sat  = state.current_saturation;
    uint16_t cx   = state.current_x;
    uint16_t cy   = state.current_y;
    uint8_t  mode = state.color_mode;
    uint16_t ehue = state.enhanced_hue;

    esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &pwr, false);

    esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID, &lvl, false);

    esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID, &hue, false);

    esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID, &sat, false);

    esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID, &cx, false);

    esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID, &cy, false);

    esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID, &mode, false);

    esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID, &ehue, false);

    // Start the color-loop render timer (auto-reload). It only drives the LED
    // while the ColorLoopActive attribute is set, so it's harmless when idle.
    s_color_loop_timer = xTimerCreate("color_loop", pdMS_TO_TICKS(COLOR_LOOP_RENDER_INTERVAL_MS),
                                      pdTRUE, NULL, color_loop_render_cb);
    if (s_color_loop_timer == NULL || xTimerStart(s_color_loop_timer, 0) != pdPASS) {
        ESP_LOGE(TAG, "Color loop render timer failed to start");
    } else {
        ESP_LOGI(TAG, "Color loop render timer started (%d ms)", COLOR_LOOP_RENDER_INTERVAL_MS);
    }

    return ESP_OK;
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , TAG, "Failed to start Zigbee commissioning");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p       = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Deferred driver initialization %s", deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted");
            }
        } else {
            /* commissioning failed */
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(err_status));
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d, Short Address: 0x%04hx)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
        } else {
            ESP_LOGI(TAG, "Network steering was not successful (status: %s)", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

// Helper function to convert HSV to RGB
static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b) {
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    
    float r_f, g_f, b_f;
    
    if (h < 60) {
        r_f = c; g_f = x; b_f = 0;
    } else if (h < 120) {
        r_f = x; g_f = c; b_f = 0;
    } else if (h < 180) {
        r_f = 0; g_f = c; b_f = x;
    } else if (h < 240) {
        r_f = 0; g_f = x; b_f = c;
    } else if (h < 300) {
        r_f = x; g_f = 0; b_f = c;
    } else {
        r_f = c; g_f = 0; b_f = x;
    }
    
    *r = (uint8_t)((r_f + m) * 255);
    *g = (uint8_t)((g_f + m) * 255);
    *b = (uint8_t)((b_f + m) * 255);
}

// Helper function to convert CIE X/Y to RGB using proper color space conversion
void cie_xy_to_rgb(uint16_t x, uint16_t y, uint8_t *r, uint8_t *g, uint8_t *b) {
    // Convert from 0-65535 range to 0-1 range
    float fx = (float)x / 65535.0f;
    float fy = (float)y / 65535.0f;
    
    // Ensure valid CIE coordinates
    if (fx < 0.0f) fx = 0.0f;
    if (fx > 1.0f) fx = 1.0f;
    if (fy < 0.0f) fy = 0.0f;
    if (fy > 1.0f) fy = 1.0f;
    
    // Calculate z (luminance)
    float fz = 1.0f - fx - fy;
    if (fz < 0.0f) fz = 0.0f;
    
    // Convert CIE XYZ to RGB using sRGB color space
    // This is a simplified conversion - for production use, you'd want a more accurate matrix
    float rf = 3.2406f * fx - 1.5372f * fy - 0.4986f * fz;
    float gf = -0.9689f * fx + 1.8758f * fy + 0.0415f * fz;
    float bf = 0.0557f * fx - 0.2040f * fy + 1.0570f * fz;
    
    // Apply gamma correction (simplified)
    if (rf > 0.0031308f) {
        rf = 1.055f * powf(rf, 1.0f/2.4f) - 0.055f;
    } else {
        rf = 12.92f * rf;
    }
    
    if (gf > 0.0031308f) {
        gf = 1.055f * powf(gf, 1.0f/2.4f) - 0.055f;
    } else {
        gf = 12.92f * gf;
    }
    
    if (bf > 0.0031308f) {
        bf = 1.055f * powf(bf, 1.0f/2.4f) - 0.055f;
    } else {
        bf = 12.92f * bf;
    }
    
    // Clamp to 0-1 range and convert to 0-255
    if (rf < 0.0f) rf = 0.0f;
    if (rf > 1.0f) rf = 1.0f;
    if (gf < 0.0f) gf = 0.0f;
    if (gf > 1.0f) gf = 1.0f;
    if (bf < 0.0f) bf = 0.0f;
    if (bf > 1.0f) bf = 1.0f;
    
    *r = (uint8_t)(rf * 255);
    *g = (uint8_t)(gf * 255);
    *b = (uint8_t)(bf * 255);
}

/* Turn off any running animated effect and reflect that back to the coordinator
 * so ZHA's "Effect mode" select returns to NoEffect. Called when a manual color
 * is applied — the chosen color should win over an effect. */
static void deactivate_effect(void)
{
    if (s_effect_mode == LIGHT_EFFECT_NONE) {
        return;
    }
    s_effect_mode = LIGHT_EFFECT_NONE;
    uint8_t none = LIGHT_EFFECT_NONE;
    esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT_ENDPOINT,
        LIGHT_EFFECT_CLUSTER_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        LIGHT_EFFECT_ATTR_ID, &none, false);
}

// Helper function to log current state for debugging
static void log_current_state(void)
{
    uint8_t red, green, blue;
    light_driver_get_color(&red, &green, &blue);
    bool power = light_driver_get_power();
    uint8_t brightness = light_driver_get_brightness();
    
    ESP_LOGI(TAG, "Current State - Power: %s, Brightness: %d, RGB: (%d,%d,%d)", 
             power ? "On" : "Off", brightness, red, green, blue);
}




static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    esp_err_t ret = ESP_OK;
    bool light_state = 0;
    uint8_t red = 255, green = 255, blue = 255;
    static uint8_t current_hue = 0;
    static uint8_t current_saturation = 255;
    static uint16_t stored_x = 0;
    static uint8_t current_level = 255; // 0-255, where 255 is full brightness

    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG, "Received message: error status(%d)",
                        message->info.status);

    /* While a color loop runs, the stack spams EnhancedCurrentHue writes (it does
     * not advance the hue itself). Drop them silently — color_loop_render_cb owns
     * the LED and animates the hue locally; honoring these would fight it and flood the log. */
    if (message->info.dst_endpoint == HA_ESP_LIGHT_ENDPOINT &&
        message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
        message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID &&
        color_attr_u8(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_ACTIVE_ID, 0) != 0) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Received message: endpoint(%d), cluster(0x%x), attribute(0x%x), data size(%d)", message->info.dst_endpoint, message->info.cluster,
             message->attribute.id, message->attribute.data.size);
    if (message->attribute.data.value) {
        ESP_LOGI(TAG, "Attribute value: %d", *(uint16_t*)message->attribute.data.value);
    }
    if (message->info.dst_endpoint == HA_ESP_LIGHT_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
                light_state = message->attribute.data.value ? *(bool *)message->attribute.data.value : light_state;
                ESP_LOGI(TAG, "Light sets to %s", light_state ? "On" : "Off");
                light_driver_set_power(light_state);
            }
        } else if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8) {
                current_level = message->attribute.data.value ? *(uint8_t *)message->attribute.data.value : 255;
                ESP_LOGI(TAG, "Current Level set to %d", current_level);
                light_driver_set_brightness(current_level);
            }
        } else if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8) {
                current_hue = message->attribute.data.value ? *(uint8_t *)message->attribute.data.value : 0;
                ESP_LOGI(TAG, "Current Hue set to %d", current_hue);
                deactivate_effect();
                s_active_color_mode = 0x00;
                // Convert hue to RGB using current saturation
                float h = (current_hue / 255.0f) * 360.0f;
                float s = current_saturation / 255.0f;
                hsv_to_rgb(h, s, 1.0f, &red, &green, &blue);
                light_driver_set_color(red, green, blue);
            } else if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8) {
                current_saturation = message->attribute.data.value ? *(uint8_t *)message->attribute.data.value : 255;
                ESP_LOGI(TAG, "Current Saturation set to %d", current_saturation);
                deactivate_effect();
                s_active_color_mode = 0x00;
                // Convert hue to RGB using current hue
                float h = (current_hue / 255.0f) * 360.0f;
                float s = current_saturation / 255.0f;
                hsv_to_rgb(h, s, 1.0f, &red, &green, &blue);
                light_driver_set_color(red, green, blue);
            } else if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16) {
                uint16_t current_x = message->attribute.data.value ? *(uint16_t *)message->attribute.data.value : 0;
                ESP_LOGI(TAG, "Color X set to %d", current_x);
                // Store X for when Y is received (will be used in Y handler)
                stored_x = current_x;
            } else if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16) {
                uint16_t current_y = message->attribute.data.value ? *(uint16_t *)message->attribute.data.value : 0;
                ESP_LOGI(TAG, "Color Y set to %d", current_y);
                deactivate_effect();
                s_active_color_mode = 0x01;
                // Get the stored X value and convert X/Y to RGB
                cie_xy_to_rgb(stored_x, current_y, &red, &green, &blue);
                ESP_LOGI(TAG, "CIE X/Y->RGB: X=%d, Y=%d -> R=%d, G=%d, B=%d", stored_x, current_y, red, green, blue);
                light_driver_set_color(red, green, blue);
            } else if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16) {
                uint16_t hue = message->attribute.data.value ? *(uint16_t *)message->attribute.data.value : 0;
                ESP_LOGI(TAG, "Enhanced Hue set to %d", hue);
                deactivate_effect();
                s_active_color_mode = LIGHT_COLOR_MODE_ENHANCED_HUE;
                // Convert enhanced hue to RGB (0-65535 range)
                float h = (hue / 65535.0f) * 360.0f;
                float s = current_saturation / 255.0f;
                hsv_to_rgb(h, s, 1.0f, &red, &green, &blue);
                light_driver_set_color(red, green, blue);
            }
        } else if (message->info.cluster == LIGHT_EFFECT_CLUSTER_ID) {
            if (message->attribute.id == LIGHT_EFFECT_ATTR_ID && message->attribute.data.value) {
                s_effect_mode = *(uint8_t *)message->attribute.data.value;
                ESP_LOGI(TAG, "Effect mode set to %d", s_effect_mode);
                if (s_effect_mode == LIGHT_EFFECT_NONE) {
                    apply_static_color();
                }
            }
        }
    }
    return ret;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
        // Log current state after any attribute change
        //log_current_state();
        // Persist the updated Zigbee attribute values (debounced NVS write).
        // We read directly from the attribute table — the ZBOSS stack has already
        // updated the in-memory values before invoking this callback.
        {
            light_state_t state = {0};
            esp_zb_zcl_attr_t *attr;

            attr = esp_zb_zcl_get_attribute(HA_ESP_LIGHT_ENDPOINT,
                ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
            if (attr) state.on_off = *(bool *)attr->data_p;

            attr = esp_zb_zcl_get_attribute(HA_ESP_LIGHT_ENDPOINT,
                ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
            if (attr) state.current_level = *(uint8_t *)attr->data_p;

            attr = esp_zb_zcl_get_attribute(HA_ESP_LIGHT_ENDPOINT,
                ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID);
            if (attr) state.current_hue = *(uint8_t *)attr->data_p;

            attr = esp_zb_zcl_get_attribute(HA_ESP_LIGHT_ENDPOINT,
                ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID);
            if (attr) state.current_saturation = *(uint8_t *)attr->data_p;

            attr = esp_zb_zcl_get_attribute(HA_ESP_LIGHT_ENDPOINT,
                ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID);
            if (attr) state.current_x = *(uint16_t *)attr->data_p;

            attr = esp_zb_zcl_get_attribute(HA_ESP_LIGHT_ENDPOINT,
                ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID);
            if (attr) state.current_y = *(uint16_t *)attr->data_p;

            attr = esp_zb_zcl_get_attribute(HA_ESP_LIGHT_ENDPOINT,
                ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID);
            if (attr) state.enhanced_hue = *(uint16_t *)attr->data_p;

            // The ZCL ColorMode attribute is unreliable here (see s_active_color_mode),
            // so persist the mode that actually drove the LED colour.
            state.color_mode = s_active_color_mode;

            light_state_nvs_save_debounced(&state);
        }
        break;
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        break;
    }
    return ret;
}

static void esp_zb_task(void *pvParameters)
{
    /* initialize Zigbee stack */
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_nwk_cfg);
    esp_zb_color_dimmable_light_cfg_t light_cfg = ESP_ZB_DEFAULT_COLOR_DIMMABLE_LIGHT_CONFIG();
    // Advertise Hue/Sat (0x01) | Enhanced Hue (0x02) | Color Loop (0x04) | X/Y (0x08).
    // Enhanced Hue and Color Loop bits are required for coordinators to expose the loop.
    light_cfg.color_cfg.color_capabilities = 0x000F;

    esp_zb_cluster_list_t *cluster_list = esp_zb_color_dimmable_light_clusters_create(&light_cfg);

    // The default color cluster omits the enhanced-hue and color-loop attributes;
    // add them so the coordinator can drive (and the stack can run) a color loop.
    esp_zb_attribute_list_t *color_cluster = esp_zb_cluster_list_get_cluster(
        cluster_list, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    uint8_t  cur_hue     = ESP_ZB_ZCL_COLOR_CONTROL_CURRENT_HUE_DEFAULT_VALUE;
    /* Spec default is 0x00 (fully desaturated = white). Seed at max so a color loop
     * shows saturated colors before the coordinator sets a saturation of its own. */
    uint8_t  cur_sat     = 0xFE;
    uint16_t remain_time = ESP_ZB_ZCL_COLOR_CONTROL_REMAINING_TIME_DEFAULT_VALUE;
    uint16_t enh_hue     = ESP_ZB_ZCL_COLOR_CONTROL_ENHANCED_CURRENT_HUE_DEFAULT_VALUE;
    uint8_t  loop_active = ESP_ZB_ZCL_COLOR_CONTROL_COLOR_LOOP_ACTIVE_DEFAULT_VALUE;
    uint8_t  loop_dir    = ESP_ZB_ZCL_COLOR_CONTROL_COLOR_LOOP_DIRECTION_DEFAULT_VALUE;
    uint16_t loop_time   = ESP_ZB_ZCL_COLOR_CONTROL_COLOR_LOOP_TIME_DEF_VALUE;
    uint16_t loop_start  = ESP_ZB_ZCL_COLOR_CONTROL_COLOR_LOOP_START_DEF_VALUE;
    uint16_t loop_stored = ESP_ZB_ZCL_COLOR_CONTROL_COLOR_LOOP_STORED_ENHANCED_HUE_DEFAULT_VALUE;

    /* The default config only creates the X/Y attribute set (plus EnhancedColorMode
     * and ColorCapabilities). We advertise color_capabilities=0x000F, so we must also
     * create the Hue/Sat set (CurrentHue/CurrentSaturation/RemainingTime) and the
     * enhanced-hue + color-loop attributes — otherwise the stack dereferences a NULL
     * descriptor and aborts when a coordinator drives those features (e.g. picking a
     * color while a loop runs triggers an enhanced-move-to-hue/sat transition).
     * The typed esp_zb_color_control_cluster_add_attr() wrapper rejects several of
     * these IDs, so use the generic helper, which creates any attribute ID
     * unconditionally, and check every return. */
    struct {
        uint16_t id;
        uint8_t  type;
        uint8_t  access;
        void    *value;
    } loop_attrs[] = {
        { ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID,                  ESP_ZB_ZCL_ATTR_TYPE_U8,  ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &cur_hue     },
        { ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID,           ESP_ZB_ZCL_ATTR_TYPE_U8,  ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &cur_sat     },
        { ESP_ZB_ZCL_ATTR_COLOR_CONTROL_REMAINING_TIME_ID,               ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,                                   &remain_time },
        { ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID,         ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &enh_hue     },
        { ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_ACTIVE_ID,            ESP_ZB_ZCL_ATTR_TYPE_U8,  ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,                                   &loop_active },
        { ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_DIRECTION_ID,         ESP_ZB_ZCL_ATTR_TYPE_U8,  ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,                                   &loop_dir    },
        { ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_TIME_ID,              ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,                                   &loop_time   },
        { ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_START_ENHANCED_HUE_ID,  ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,                                 &loop_start  },
        { ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_STORED_ENHANCED_HUE_ID, ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,                                 &loop_stored },
    };
    for (size_t i = 0; i < sizeof(loop_attrs) / sizeof(loop_attrs[0]); i++) {
        esp_err_t add_err = esp_zb_cluster_add_attr(
            color_cluster, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            loop_attrs[i].id, loop_attrs[i].type, loop_attrs[i].access, loop_attrs[i].value);
        if (add_err != ESP_OK) {
            ESP_LOGW(TAG, "add color attr 0x%04x failed: %s", loop_attrs[i].id, esp_err_to_name(add_err));
        }
    }

    /* Manufacturer-specific cluster exposing the animated-effect selector to ZHA. */
    uint8_t effect_default = LIGHT_EFFECT_NONE;
    esp_zb_attribute_list_t *effect_cluster = esp_zb_zcl_attr_list_create(LIGHT_EFFECT_CLUSTER_ID);
    esp_zb_custom_cluster_add_custom_attr(effect_cluster, LIGHT_EFFECT_ATTR_ID,
        ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &effect_default);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, effect_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_ep_list_t *esp_zb_color_light_ep = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_config = {
        .endpoint = HA_ESP_LIGHT_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_COLOR_DIMMABLE_LIGHT_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(esp_zb_color_light_ep, cluster_list, ep_config);
    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };

    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_color_light_ep, HA_ESP_LIGHT_ENDPOINT, &info);
    esp_zb_device_register(esp_zb_color_light_ep);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
