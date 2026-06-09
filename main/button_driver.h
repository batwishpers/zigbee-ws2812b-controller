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

/* Dedicated factory-reset button — fixed logic, NOT a Zigbee endpoint and NOT
 * part of s_button_gpios[]. Like every Zigbee device, holding it long enough
 * resets the device without needing `idf.py erase-flash`. Active-low, like the
 * momentary buttons. Held continuously for BUTTON_RESET_HOLD_MS fires the reset
 * callback once (network + all stored light settings). */
#define BUTTON_RESET_GPIO     GPIO_NUM_0
#define BUTTON_RESET_HOLD_MS  5000

/**
 * @brief Callback invoked once per confirmed (debounced) button press.
 *        Runs in the button polling task — not the Zigbee task.
 *
 * @param index  Button position in s_button_gpios[] (0-based).
 */
typedef void (*button_press_cb_t)(uint8_t index);

/**
 * @brief Callback invoked once when the dedicated reset button (BUTTON_RESET_GPIO)
 *        has been held for BUTTON_RESET_HOLD_MS. Runs in the button polling task.
 */
typedef void (*button_reset_cb_t)(void);

/**
 * @brief Configure the button GPIOs (input, internal pull-up) and start the
 *        polling task. Call once after the Zigbee stack is up.
 *
 * @param press_cb  Invoked on each momentary-button press (may be NULL).
 * @param reset_cb  Invoked once when the reset button is held long enough
 *                  (may be NULL to disable the reset button).
 */
void button_driver_init(button_press_cb_t press_cb, button_reset_cb_t reset_cb);

/**
 * @brief Number of configured buttons (length of s_button_gpios[]).
 *        Safe to call before button_driver_init().
 */
uint8_t button_driver_count(void);

#ifdef __cplusplus
}
#endif
