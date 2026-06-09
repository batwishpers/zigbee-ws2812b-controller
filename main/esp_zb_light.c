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
#include "string.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zb_light.h"
#include "light_state_nvs.h"
#include "button_driver.h"
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
#define COLOR_LOOP_RENDER_INTERVAL_MS  50

/* Manufacturer-specific cluster + attribute that select an animated light effect.
 * ZHA drives this via a custom quirk (see zha_quirk/zigbee_ws2812b_controller.py). The
 * firmware runs the chosen effect inside the existing render timer. */
#define LIGHT_EFFECT_CLUSTER_ID  0xFC00
#define LIGHT_EFFECT_ATTR_ID     0x0000
enum {
    LIGHT_EFFECT_NONE    = 0,
    LIGHT_EFFECT_RAINBOW = 1,
    LIGHT_EFFECT_FIRE    = 2,
    LIGHT_EFFECT_CANDLE  = 3,
    LIGHT_EFFECT_PLASMA  = 4,
};
#define LIGHT_EFFECT_COUNT 5  /* number of selectable modes incl. None */
static uint8_t s_effect_mode = LIGHT_EFFECT_NONE;

/* Manufacturer-specific cluster on each button endpoint that selects an
 * on-device action to run when that button is pressed — independent of any HA
 * automation. Exposed as a ZHA select via the quirk (see zigbee_ws2812b_controller.py).
 * Buttons still report their press to HA regardless of this setting. */
#define BUTTON_ACTION_CLUSTER_ID 0xFC01
#define BUTTON_ACTION_ATTR_ID    0x0000
enum {
    BTN_ACTION_NONE           = 0,  /* report press to HA only */
    BTN_ACTION_TOGGLE_LIGHT   = 1,  /* toggle LED-strip power */
    BTN_ACTION_NEXT_EFFECT    = 2,  /* cycle to the next effect */
    BTN_ACTION_EFFECT_RAINBOW = 3,
    BTN_ACTION_EFFECT_FIRE    = 4,
    BTN_ACTION_EFFECT_CANDLE  = 5,
    BTN_ACTION_EFFECT_PLASMA  = 6,
};
/* Per-button selected action, indexed by button number; loaded from NVS. */
static uint8_t s_button_action[BUTTON_MAX];

/* Number of LEDs on the strip. Effects derive all spatial maths from this so
 * they work for any build-time strip length (no hardcoded count). */
#define LED_COUNT CONFIG_EXAMPLE_STRIP_LED_NUMBER

/* Scratch frame filled by the spatial effect renderers, then pushed in one go
 * via light_driver_set_pixels(). R,G,B per pixel. Touched only by the render
 * timer task, so it needs no locking of its own. */
static uint8_t s_frame[LED_COUNT * 3];

/* Forward declarations for helpers defined later in this file */
static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b);
void cie_xy_to_rgb(uint16_t x, uint16_t y, uint8_t *r, uint8_t *g, uint8_t *b);
static void on_button_pressed(uint8_t index);

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

/* ---- Small math helpers shared by the spatial effect renderers ---- */

/* Saturating 8-bit subtract / add (clamp at 0 / 255). */
static inline uint8_t qsub8(uint8_t a, uint8_t b) { return (a > b) ? (uint8_t)(a - b) : 0; }
static inline uint8_t qadd8(uint8_t a, uint8_t b) { unsigned s = (unsigned)a + b; return (s > 255) ? 255 : (uint8_t)s; }

/* 0..255 random byte from the hardware RNG. */
static inline uint8_t rnd8(void) { return (uint8_t)(esp_random() & 0xFF); }

/* Map a heat value (0 = cold .. 255 = hottest) onto a fire ramp:
 * black -> red -> orange/yellow -> white. Same idea as FastLED's HeatColor(). */
static void heat_to_rgb(uint8_t heat, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t t192 = (uint8_t)(((unsigned)heat * 191) / 255); /* scale 0..255 -> 0..191 */
    uint8_t ramp = (uint8_t)((t192 & 0x3F) << 2);           /* position within band, 0..252 */
    if (t192 & 0x80) {        /* hottest third: red+green saturated, ramp blue in */
        *r = 255; *g = 255; *b = ramp;
    } else if (t192 & 0x40) { /* middle third: red saturated, ramp green in */
        *r = 255; *g = ramp; *b = 0;
    } else {                  /* coolest third: ramp red in from black */
        *r = ramp; *g = 0; *b = 0;
    }
}

/* Smooth 1D value noise in [0,1): hash integer lattice points to pseudo-random
 * values and cosine-interpolate between them. Cheap, dependency-free, organic. */
static float hash01(int32_t n)
{
    uint32_t x = (uint32_t)n * 1664525u + 1013904223u;
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15;
    return (x & 0xFFFFFF) / (float)0x1000000;
}
static float value_noise1d(float x)
{
    int32_t xi = (int32_t)floorf(x);
    float xf = x - (float)xi;
    float a = hash01(xi);
    float b = hash01(xi + 1);
    float t = (1.0f - cosf(xf * 3.14159265f)) * 0.5f; /* smooth blend */
    return a + (b - a) * t;
}

/* Rainbow sweep — a hue gradient that spans the strip and flows over time. The
 * stack sets ColorLoopActive on a ZCL ColorLoopSet but does NOT advance the hue
 * (it just re-writes the start hue, which stays 0 = red), so we step a local
 * accumulator and render it. `fresh` re-seeds from the configured start hue.
 * Like the other effects, it deliberately leaves s_active_color_mode untouched:
 * the hue is animated locally and never written back to EnhancedCurrentHue, so
 * keying the restore off it would always come back as red. */
static void render_rainbow(bool fresh)
{
    static uint16_t base = 0;

    uint8_t  dir   = color_attr_u8(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_DIRECTION_ID, 1);
    uint16_t ltime = color_attr_u16(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_TIME_ID, 0x0019);
    if (ltime == 0) {
        ltime = 0x0019;
    }

    /* A color loop is a vivid rainbow sweep, so render at full saturation. We do
     * NOT read CurrentSaturation here: it can legitimately be 0 (white), which
     * would wash the loop out entirely. */
    const float sat = 0xFE / 255.0f;

    if (fresh) {
        base = color_attr_u16(ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_START_ENHANCED_HUE_ID, 0);
        ESP_LOGI(TAG, "Rainbow start — dir=%u time=%us start_hue=%u", dir, ltime, base);
    }

    /* Advance one render tick of a full 0..65535 sweep performed over `ltime`
     * seconds. Direction 0x00 = decrement, 0x01 = increment (per ZCL spec). */
    uint32_t step = (uint32_t)((65536.0f * COLOR_LOOP_RENDER_INTERVAL_MS) / (ltime * 1000.0f));
    if (step == 0) {
        step = 1;
    }
    base = (dir == 0x00) ? (uint16_t)(base - step) : (uint16_t)(base + step);

    /* Spread ~1.5 full rainbows across the strip, independent of its length. */
    const float spread = (1.5f * 65536.0f) / LED_COUNT;
    for (uint16_t i = 0; i < LED_COUNT; i++) {
        uint16_t hue = (uint16_t)(base + (uint16_t)(i * spread));
        uint8_t r, g, b;
        hsv_to_rgb((hue / 65535.0f) * 360.0f, sat, 1.0f, &r, &g, &b);
        s_frame[i * 3 + 0] = r;
        s_frame[i * 3 + 1] = g;
        s_frame[i * 3 + 2] = b;
    }
    light_driver_set_pixels(s_frame, LED_COUNT);
}

/* Fire — a per-pixel "Fire2012" simulation. A heat array is cooled, diffused
 * upward and randomly sparked at the base (index 0), then mapped to fire colors.
 * All tuning scales with LED_COUNT so the flame looks right at any strip length.
 * `fresh` clears the heat so the flame restarts from cold. Deliberately does NOT
 * touch s_active_color_mode, so the saved static color survives. */
static void render_fire(bool fresh)
{
    static uint8_t heat[LED_COUNT];
    if (fresh) {
        memset(heat, 0, sizeof(heat));
    }

    /* 1. Cool every cell down a little (normalized to strip length). */
    uint8_t cooling = (uint8_t)((55 * 10) / LED_COUNT + 2);
    for (uint16_t i = 0; i < LED_COUNT; i++) {
        heat[i] = qsub8(heat[i], (uint8_t)(rnd8() % (cooling + 1)));
    }

    /* 2. Heat drifts up the strip and blurs into its lower neighbours. */
    for (int k = LED_COUNT - 1; k >= 2; k--) {
        heat[k] = (uint8_t)(((unsigned)heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3);
    }

    /* 3. Randomly ignite a fresh spark near the base. */
    if (rnd8() < 120) {
        uint16_t span = (LED_COUNT < 7) ? LED_COUNT : 7;
        uint8_t y = (uint8_t)(rnd8() % span);
        heat[y] = qadd8(heat[y], (uint8_t)(160 + rnd8() % 96));
    }

    /* 4. Map each cell's heat to a fire color. */
    for (uint16_t i = 0; i < LED_COUNT; i++) {
        uint8_t r, g, b;
        heat_to_rgb(heat[i], &r, &g, &b);
        s_frame[i * 3 + 0] = r;
        s_frame[i * 3 + 1] = g;
        s_frame[i * 3 + 2] = b;
    }
    light_driver_set_pixels(s_frame, LED_COUNT);
}

/* Candle — each LED keeps its own warm hue/brightness and eases toward fresh
 * random targets, so neighbouring pixels shimmer out of phase (organic flicker
 * rather than the whole strip pulsing in unison). `fresh` re-seeds the per-pixel
 * state. Like Fire, leaves s_active_color_mode untouched. */
static void render_candle(bool fresh)
{
    static float level[LED_COUNT];
    static float lhue[LED_COUNT];
    if (fresh) {
        for (uint16_t i = 0; i < LED_COUNT; i++) {
            level[i] = 0.8f;
            lhue[i]  = 35.0f;
        }
    }
    for (uint16_t i = 0; i < LED_COUNT; i++) {
        float val_target = 0.55f + 0.45f * (rnd8() / 255.0f);
        float hue_target = 28.0f + 14.0f * (rnd8() / 255.0f);
        level[i] += (val_target - level[i]) * 0.15f;
        lhue[i]  += (hue_target - lhue[i]) * 0.15f;
        uint8_t r, g, b;
        hsv_to_rgb(lhue[i], 1.0f, level[i], &r, &g, &b);
        s_frame[i * 3 + 0] = r;
        s_frame[i * 3 + 1] = g;
        s_frame[i * 3 + 2] = b;
    }
    light_driver_set_pixels(s_frame, LED_COUNT);
}

/* Plasma — a slowly drifting value-noise field mapped to a full hue sweep, so
 * colors flow and morph organically along the strip. The noise sampling rate is
 * normalized to LED_COUNT (~4 features across the strip) for any length.
 * `fresh` resets the time origin. Leaves s_active_color_mode untouched. */
static void render_plasma(bool fresh)
{
    static float t = 0.0f;
    if (fresh) {
        t = 0.0f;
    }
    t += 0.05f; /* drift speed */

    for (uint16_t i = 0; i < LED_COUNT; i++) {
        float n = value_noise1d(i * (4.0f / LED_COUNT) + t);
        uint8_t r, g, b;
        hsv_to_rgb(n * 360.0f, 1.0f, 1.0f, &r, &g, &b);
        s_frame[i * 3 + 0] = r;
        s_frame[i * 3 + 1] = g;
        s_frame[i * 3 + 2] = b;
    }
    light_driver_set_pixels(s_frame, LED_COUNT);
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

    /* Don't render (or burn cycles) while the light is off — the strip is dark
     * anyway, and we want a clean restart when it's switched back on. */
    if (!light_driver_get_power()) {
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
        render_fire(fresh);
        break;
    case LIGHT_EFFECT_CANDLE:
        render_candle(fresh);
        break;
    case LIGHT_EFFECT_PLASMA:
        render_plasma(fresh);
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

    // Configure the push-button GPIOs and start polling for presses
    button_driver_init(on_button_pressed);

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

/* Select an animated effect from firmware (e.g. a button action), mirroring it
 * into the 0xFC00 attribute so ZHA's "Effect mode" select tracks the change.
 * Setting LIGHT_EFFECT_NONE restores the last static color. */
static void apply_effect_mode(uint8_t mode)
{
    s_effect_mode = mode;
    esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT_ENDPOINT,
        LIGHT_EFFECT_CLUSTER_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        LIGHT_EFFECT_ATTR_ID, &mode, false);
    if (mode == LIGHT_EFFECT_NONE) {
        apply_static_color();
    }
    ESP_LOGI(TAG, "Effect mode set to %d (button action)", mode);
}

/* Does the actual press handling. Scheduled via esp_zb_scheduler_alarm() so it
 * runs in the Zigbee stack's own task context — NOT the button polling task.
 * This matters: the command-send path runs deep into the stack and overflows a
 * small app-task stack. Running here (a Zigbee callback context) is the
 * supported place for stack APIs, so no esp_zb_lock_acquire() is needed. */
static void button_action_handler(uint8_t index)
{
    uint8_t ep = BUTTON_EP_BASE + index;

    /* Send an On/Off Toggle command to the coordinator. ZHA turns the received
     * command into a zha_event (cluster 0x0006, command "toggle", endpoint =
     * this button's ep), which you trigger automations on. Stateless: nothing
     * is held, so the button never "stays on" in HA. */
    esp_zb_zcl_on_off_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = 0x0000,        /* coordinator */
            .dst_endpoint = HA_ESP_LIGHT_ENDPOINT,  /* coordinator endpoint 1 */
            .src_endpoint = ep,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .on_off_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID,
    };
    esp_zb_zcl_on_off_cmd_req(&cmd);
    ESP_LOGI(TAG, "Button %u press -> Toggle command sent (src ep %u)", index, ep);

    /* Run the configured on-device action. */
    uint8_t action = (index < BUTTON_MAX) ? s_button_action[index] : BTN_ACTION_NONE;
    switch (action) {
    case BTN_ACTION_TOGGLE_LIGHT: {
        bool pwr = !light_driver_get_power();
        light_driver_set_power(pwr);
        /* Keep the light endpoint's On/Off attribute in sync for HA. */
        esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT_ENDPOINT,
            ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &pwr, false);
        break;
    }
    case BTN_ACTION_NEXT_EFFECT:
        apply_effect_mode((uint8_t)((s_effect_mode + 1) % LIGHT_EFFECT_COUNT));
        break;
    case BTN_ACTION_EFFECT_RAINBOW:
        apply_effect_mode(LIGHT_EFFECT_RAINBOW);
        break;
    case BTN_ACTION_EFFECT_FIRE:
        apply_effect_mode(LIGHT_EFFECT_FIRE);
        break;
    case BTN_ACTION_EFFECT_CANDLE:
        apply_effect_mode(LIGHT_EFFECT_CANDLE);
        break;
    case BTN_ACTION_EFFECT_PLASMA:
        apply_effect_mode(LIGHT_EFFECT_PLASMA);
        break;
    case BTN_ACTION_NONE:
    default:
        break;
    }
}

/* Invoked by button_driver on each confirmed press, in the button polling task.
 * Defers the work to the Zigbee stack context (see button_action_handler). The
 * scheduling call is shallow, so it's safe under the lock on the small task. */
static void on_button_pressed(uint8_t index)
{
    if (esp_zb_lock_acquire(portMAX_DELAY)) {
        esp_zb_scheduler_alarm(button_action_handler, index, 0);
        esp_zb_lock_release();
    }
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
    } else if (message->info.dst_endpoint >= BUTTON_EP_BASE &&
               message->info.cluster == BUTTON_ACTION_CLUSTER_ID) {
        /* HA reconfigured a button's on-device action (custom 0xFC01 select). */
        if (message->attribute.id == BUTTON_ACTION_ATTR_ID && message->attribute.data.value) {
            uint8_t idx = message->info.dst_endpoint - BUTTON_EP_BASE;
            if (idx < BUTTON_MAX) {
                s_button_action[idx] = *(uint8_t *)message->attribute.data.value;
                ESP_LOGI(TAG, "Button %d action set to %d", idx, s_button_action[idx]);
                light_state_nvs_save_button_actions(s_button_action, button_driver_count());
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
        
        // Persist the updated Zigbee attribute values (debounced NVS write).
        // We read directly from the attribute table — the ZBOSS stack has already
        // updated the in-memory values before invoking this callback. Only the
        // light endpoint's state lives here; button-action writes persist
        // themselves (see zb_attribute_handler), so skip them.
        if (((esp_zb_zcl_set_attr_value_message_t *)message)->info.dst_endpoint == HA_ESP_LIGHT_ENDPOINT) {
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

    /* One endpoint per physical push button (endpoints BUTTON_EP_BASE..). Each
     * carries an On/Off server cluster (toggled + reported on press so HA can
     * trigger automations), an On/Off Switch Configuration cluster declaring a
     * momentary switch, an Identify cluster, and a custom 0xFC01 cluster that
     * selects the on-device action run on press. */
    for (uint8_t i = 0; i < BUTTON_MAX; i++) {
        s_button_action[i] = BTN_ACTION_NONE;
    }
    uint8_t btn_count = button_driver_count();
    light_state_nvs_load_button_actions(s_button_action, btn_count);

    for (uint8_t i = 0; i < btn_count; i++) {
        esp_zb_cluster_list_t *btn_clusters = esp_zb_zcl_cluster_list_create();

        /* On/Off as a CLIENT (output) cluster: the button *sends* commands rather
         * than holding state. ZHA then models it as a stateless switch/remote (no
         * controllable switch entity) and fires a zha_event on each press. */
        esp_zb_on_off_cluster_cfg_t on_off_cfg = { .on_off = false };
        esp_zb_cluster_list_add_on_off_cluster(btn_clusters,
            esp_zb_on_off_cluster_create(&on_off_cfg), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

        esp_zb_identify_cluster_cfg_t identify_cfg = { .identify_time = 0 };
        esp_zb_cluster_list_add_identify_cluster(btn_clusters,
            esp_zb_identify_cluster_create(&identify_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

        esp_zb_on_off_switch_cluster_cfg_t sw_cfg = {
            .switch_type   = ESP_ZB_ZCL_ON_OFF_SWITCH_CONFIGURATION_SWITCH_TYPE_MOMENTARY,
            .switch_action = ESP_ZB_ZCL_ON_OFF_SWITCH_CONFIGURATION_SWITCH_ACTIONS_TYPE1,
        };
        esp_zb_cluster_list_add_on_off_switch_config_cluster(btn_clusters,
            esp_zb_on_off_switch_config_cluster_create(&sw_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

        uint8_t action_default = s_button_action[i];
        esp_zb_attribute_list_t *action_cluster = esp_zb_zcl_attr_list_create(BUTTON_ACTION_CLUSTER_ID);
        esp_zb_custom_cluster_add_custom_attr(action_cluster, BUTTON_ACTION_ATTR_ID,
            ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
            &action_default);
        esp_zb_cluster_list_add_custom_cluster(btn_clusters, action_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

        esp_zb_endpoint_config_t btn_ep_config = {
            .endpoint = BUTTON_EP_BASE + i,
            .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .app_device_id = ESP_ZB_HA_ON_OFF_SWITCH_DEVICE_ID,
            .app_device_version = 0,
        };
        esp_zb_ep_list_add_ep(esp_zb_color_light_ep, btn_clusters, btn_ep_config);
        ESP_LOGI(TAG, "Button endpoint %d created (action default %d)",
                 BUTTON_EP_BASE + i, action_default);
    }

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
