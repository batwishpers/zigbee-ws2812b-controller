/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Debounce window in milliseconds before a pending state is written to NVS.
 *        Every new state change resets this window, so only one NVS write occurs
 *        after a burst of attribute updates (e.g. a dimming sweep).
 */
#define LIGHT_STATE_NVS_DEBOUNCE_MS  500

/**
 * @brief Zigbee attribute values persisted across reboots.
 *        Fields match the ZCL attribute types exactly — no conversion on save or restore.
 *
 *   on_off           : On/Off cluster — ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID
 *   current_level    : Level Control  — ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID
 *   current_hue      : Color Control  — ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID
 *   current_saturation: Color Control — ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID
 *   current_x        : Color Control  — ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID
 *   current_y        : Color Control  — ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID
 *   color_mode       : Color Control  — ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID
 *                       0x00 = Hue/Saturation, 0x01 = XY
 */
typedef struct {
    bool     on_off;
    uint8_t  current_level;
    uint8_t  current_hue;
    uint8_t  current_saturation;
    uint16_t current_x;
    uint16_t current_y;
    uint8_t  color_mode;
} light_state_t;

/** Default state applied on first boot (no saved NVS data).
 *  Orange at ~5% brightness, Hue/Saturation mode. */
#define LIGHT_NVS_DEFAULT_ON_OFF        true
#define LIGHT_NVS_DEFAULT_LEVEL         13       /*!< ~5% of 255 */
#define LIGHT_NVS_DEFAULT_COLOR_MODE    0x00U    /*!< Hue/Saturation */
#define LIGHT_NVS_DEFAULT_HUE           18U      /*!< ~25° orange in 0-255 ZCL scale */
#define LIGHT_NVS_DEFAULT_SATURATION    255U
#define LIGHT_NVS_DEFAULT_X             0x616BU  /*!< ZCL D65 white — unused when mode=Hue/Sat */
#define LIGHT_NVS_DEFAULT_Y             0x607DU  /*!< ZCL D65 white — unused when mode=Hue/Sat */

/**
 * @brief Initialise the NVS persistence module.
 *        Must be called once before light_state_nvs_save_debounced().
 *        nvs_flash_init() must already have been called before this.
 *
 * @return ESP_OK on success, ESP_FAIL if the debounce timer could not be created.
 */
esp_err_t light_state_nvs_init(void);

/**
 * @brief Load the last persisted state from NVS.
 *        If no state is found (first boot) the struct is filled with the
 *        LIGHT_NVS_DEFAULT_* values and ESP_OK is still returned.
 *
 * @param[out] state  Pointer to the struct to populate.
 * @return ESP_OK on success or when defaults are used; error code otherwise.
 */
esp_err_t light_state_nvs_load(light_state_t *state);

/**
 * @brief Schedule a debounced NVS write.
 *        Copies *state internally and resets the LIGHT_STATE_NVS_DEBOUNCE_MS
 *        one-shot timer. The actual NVS write happens once the timer fires
 *        without being reset again.
 *        Safe to call from any task context.
 *
 * @param[in] state  Pointer to the state to persist.
 */
void light_state_nvs_save_debounced(const light_state_t *state);

#ifdef __cplusplus
}
#endif
