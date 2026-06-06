/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 */

#pragma once

#include <stdint.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* First Zigbee endpoint used for buttons. Button i -> endpoint BUTTON_EP_BASE + i.
 * Endpoint 1 (HA_ESP_LIGHT_ENDPOINT) is the light. */
#define BUTTON_EP_BASE 2

/* Upper bound on the number of buttons. Used only to size the per-button action
 * table; the *actual* count comes from s_button_gpios[] in button_driver.c. */
#define BUTTON_MAX 8

/**
 * @brief Callback invoked once per confirmed (debounced) button press.
 *        Runs in the button polling task — not the Zigbee task.
 *
 * @param index  Button position in s_button_gpios[] (0-based).
 */
typedef void (*button_press_cb_t)(uint8_t index);

/**
 * @brief Configure the button GPIOs (input, internal pull-up) and start the
 *        polling task. Call once after the Zigbee stack is up.
 *
 * @param cb  Invoked on each press (may be NULL to disable callbacks).
 */
void button_driver_init(button_press_cb_t cb);

/**
 * @brief Number of configured buttons (length of s_button_gpios[]).
 *        Safe to call before button_driver_init().
 */
uint8_t button_driver_count(void);

#ifdef __cplusplus
}
#endif
