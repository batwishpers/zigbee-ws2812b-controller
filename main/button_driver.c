/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 */

#include "button_driver.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BUTTON";

/* ------------------------------------------------------------------------- *
 *  Add a button here: append its GPIO number to the array.
 *  Wiring is active-low: GPIO -> push button -> GND (internal pull-up keeps the
 *  line high when released). Avoid GPIO 8 (LED strip) and the boot-strapping
 *  pin (GPIO 9 on most ESP32-H2 devkits). Keep the count <= BUTTON_MAX.
 *  These defaults are a safe guess — verify they are free on your board.
 * ------------------------------------------------------------------------- */
static const gpio_num_t s_button_gpios[] = {
    GPIO_NUM_1,
    GPIO_NUM_2,
    GPIO_NUM_3,
};
#define BUTTON_COUNT (sizeof(s_button_gpios) / sizeof(s_button_gpios[0]))

/* Poll cadence and debounce: a level must hold for BUTTON_DEBOUNCE_SAMPLES
 * consecutive samples (~30 ms) before it is accepted as the new stable state. */
#define BUTTON_POLL_INTERVAL_MS 10
#define BUTTON_DEBOUNCE_SAMPLES 3

static button_press_cb_t s_press_cb = NULL;

uint8_t button_driver_count(void)
{
    return (uint8_t)BUTTON_COUNT;
}

/* Polls every button GPIO, debounces per-button, and fires s_press_cb on each
 * falling edge (released -> pressed). Idle level is 1 (pull-up); pressed is 0. */
static void button_poll_task(void *arg)
{
    uint8_t stable[BUTTON_COUNT];     /* last accepted, debounced level */
    uint8_t candidate[BUTTON_COUNT];  /* level currently being debounced */
    uint8_t count[BUTTON_COUNT];      /* consecutive samples seen at candidate */

    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        stable[i] = 1;
        candidate[i] = 1;
        count[i] = 0;
    }

    for (;;) {
        for (size_t i = 0; i < BUTTON_COUNT; i++) {
            uint8_t level = (uint8_t)gpio_get_level(s_button_gpios[i]);

            if (level == candidate[i]) {
                if (count[i] < BUTTON_DEBOUNCE_SAMPLES) {
                    count[i]++;
                }
            } else {
                candidate[i] = level;
                count[i] = 1;
            }

            if (count[i] >= BUTTON_DEBOUNCE_SAMPLES && candidate[i] != stable[i]) {
                stable[i] = candidate[i];
                /* Falling edge = a press. */
                if (stable[i] == 0 && s_press_cb) {
                    ESP_LOGI(TAG, "Button %u pressed (GPIO %d)", (unsigned)i, s_button_gpios[i]);
                    s_press_cb((uint8_t)i);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS));
    }
}

void button_driver_init(button_press_cb_t cb)
{
    s_press_cb = cb;

    uint64_t pin_mask = 0;
    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        pin_mask |= (1ULL << s_button_gpios[i]);
    }

    gpio_config_t io = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* 4096 words to match Zigbee_main: the press callback acquires the Zigbee
     * lock and calls esp_zb_zcl_set_attribute_val, which recurses into the
     * reporting machinery — a smaller stack overflows and resets the chip. */
    xTaskCreate(button_poll_task, "btn_poll", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Button driver started (%u buttons, %d ms poll)",
             (unsigned)BUTTON_COUNT, BUTTON_POLL_INTERVAL_MS);
}
