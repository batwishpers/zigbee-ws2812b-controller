/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 */

#include "light_state_nvs.h"

#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "LIGHT_NVS";

/* NVS namespace and key names (keys must be ≤ 15 chars) */
#define NVS_NAMESPACE   "light_state"
#define NVS_KEY_PWR     "pwr"
#define NVS_KEY_LVL     "lvl"
#define NVS_KEY_HUE     "hue"
#define NVS_KEY_SAT     "sat"
#define NVS_KEY_CX      "cx"
#define NVS_KEY_CY      "cy"
#define NVS_KEY_CMODE   "cmode"

static TimerHandle_t   s_save_timer   = NULL;
static light_state_t   s_pending_state;

/* -------------------------------------------------------------------------- */
/*  Timer callback — runs in the FreeRTOS timer daemon task                   */
/* -------------------------------------------------------------------------- */

static void nvs_save_timer_cb(TimerHandle_t timer)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u8(handle,  NVS_KEY_PWR,   (uint8_t)s_pending_state.on_off);
    nvs_set_u8(handle,  NVS_KEY_LVL,   s_pending_state.current_level);
    nvs_set_u8(handle,  NVS_KEY_HUE,   s_pending_state.current_hue);
    nvs_set_u8(handle,  NVS_KEY_SAT,   s_pending_state.current_saturation);
    nvs_set_u16(handle, NVS_KEY_CX,    s_pending_state.current_x);
    nvs_set_u16(handle, NVS_KEY_CY,    s_pending_state.current_y);
    nvs_set_u8(handle,  NVS_KEY_CMODE, s_pending_state.color_mode);

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "State saved — pwr=%d lvl=%d hue=%d sat=%d x=%d y=%d mode=%d",
                 s_pending_state.on_off,
                 s_pending_state.current_level,
                 s_pending_state.current_hue,
                 s_pending_state.current_saturation,
                 s_pending_state.current_x,
                 s_pending_state.current_y,
                 s_pending_state.color_mode);
    } else {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    }
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t light_state_nvs_init(void)
{
    s_save_timer = xTimerCreate(
        "nvs_save",
        pdMS_TO_TICKS(LIGHT_STATE_NVS_DEBOUNCE_MS),
        pdFALSE,          /* one-shot */
        NULL,
        nvs_save_timer_cb);

    if (s_save_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create debounce timer");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "NVS save timer created (%d ms debounce)", LIGHT_STATE_NVS_DEBOUNCE_MS);
    return ESP_OK;
}

esp_err_t light_state_nvs_load(light_state_t *state)
{
    /* Always fill defaults first so partial NVS data is tolerated */
    state->on_off             = LIGHT_NVS_DEFAULT_ON_OFF;
    state->current_level      = LIGHT_NVS_DEFAULT_LEVEL;
    state->color_mode         = LIGHT_NVS_DEFAULT_COLOR_MODE;
    state->current_hue        = LIGHT_NVS_DEFAULT_HUE;
    state->current_saturation = LIGHT_NVS_DEFAULT_SATURATION;
    state->current_x          = LIGHT_NVS_DEFAULT_X;
    state->current_y          = LIGHT_NVS_DEFAULT_Y;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved state — using defaults (orange 5%%)");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open for read failed: %s — using defaults", esp_err_to_name(err));
        return err;
    }

    uint8_t  u8;
    uint16_t u16;

    if (nvs_get_u8(handle,  NVS_KEY_PWR,   &u8)  == ESP_OK) state->on_off             = (bool)u8;
    if (nvs_get_u8(handle,  NVS_KEY_LVL,   &u8)  == ESP_OK) state->current_level      = u8;
    if (nvs_get_u8(handle,  NVS_KEY_HUE,   &u8)  == ESP_OK) state->current_hue        = u8;
    if (nvs_get_u8(handle,  NVS_KEY_SAT,   &u8)  == ESP_OK) state->current_saturation = u8;
    if (nvs_get_u16(handle, NVS_KEY_CX,    &u16) == ESP_OK) state->current_x          = u16;
    if (nvs_get_u16(handle, NVS_KEY_CY,    &u16) == ESP_OK) state->current_y          = u16;
    if (nvs_get_u8(handle,  NVS_KEY_CMODE, &u8)  == ESP_OK) state->color_mode         = u8;

    nvs_close(handle);

    ESP_LOGI(TAG, "State loaded — pwr=%d lvl=%d hue=%d sat=%d x=%d y=%d mode=%d",
             state->on_off,
             state->current_level,
             state->current_hue,
             state->current_saturation,
             state->current_x,
             state->current_y,
             state->color_mode);

    return ESP_OK;
}

void light_state_nvs_save_debounced(const light_state_t *state)
{
    s_pending_state = *state;

    if (s_save_timer != NULL) {
        /* xTimerReset restarts the countdown; if already running it resets to 0 */
        xTimerReset(s_save_timer, 0);
    }
}
