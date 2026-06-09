/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Espressif Systems
 *    integrated circuit in a product or a software update for such product,
 *    must reproduce the above copyright notice, this list of conditions and
 *    the following disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * 4. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "esp_log.h"
#include "led_strip.h"
#include "light_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static led_strip_handle_t s_led_strip;
static uint8_t s_red = 255, s_green = 255, s_blue = 255;
static bool s_power = false;
static uint8_t s_ext_brightness = 255; // 0-255, where 255 is full brightness
static uint8_t s_brightness = 128; // 0-128, full led power is too much, limit to half power

/* Serializes RMT/led_strip access: the color-loop render timer (timer daemon
 * task) and the Zigbee task can both drive the strip concurrently. */
static SemaphoreHandle_t s_strip_mutex = NULL;

/* Push the current colour (brightness/power scaled) to the strip, guarded so
 * concurrent callers can't interleave set_pixel/refresh on the same channel. */
static void apply_to_strip(void)
{
    uint8_t scaled_red = (s_red * s_power * s_brightness) / 255;
    uint8_t scaled_green = (s_green * s_power * s_brightness) / 255;
    uint8_t scaled_blue = (s_blue * s_power * s_brightness) / 255;
    if (s_strip_mutex) {
        xSemaphoreTake(s_strip_mutex, portMAX_DELAY);
    }
    for (size_t i = 0; i < CONFIG_EXAMPLE_STRIP_LED_NUMBER; i++)
    {
        ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, i, scaled_red, scaled_green, scaled_blue));
    }
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    if (s_strip_mutex) {
        xSemaphoreGive(s_strip_mutex);
    }
}

void light_driver_set_pixels(const uint8_t *rgb, uint16_t count)
{
    if (rgb == NULL) {
        return;
    }
    if (count > CONFIG_EXAMPLE_STRIP_LED_NUMBER) {
        count = CONFIG_EXAMPLE_STRIP_LED_NUMBER;
    }
    if (s_strip_mutex) {
        xSemaphoreTake(s_strip_mutex, portMAX_DELAY);
    }
    for (uint16_t i = 0; i < count; i++) {
        uint8_t r = (rgb[i * 3 + 0] * s_power * s_brightness) / 255;
        uint8_t g = (rgb[i * 3 + 1] * s_power * s_brightness) / 255;
        uint8_t b = (rgb[i * 3 + 2] * s_power * s_brightness) / 255;
        ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, i, r, g, b));
    }
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    if (s_strip_mutex) {
        xSemaphoreGive(s_strip_mutex);
    }
}

void light_driver_set_power(bool power)
{
    s_power = power;
    apply_to_strip();
}

void light_driver_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    s_red = red;
    s_green = green;
    s_blue = blue;
    //ESP_LOGI("LIGHT_DRIVER", "Setting color: R=%d, G=%d, B=%d, Power=%d, Brightness=%d", red, green, blue, s_power, s_brightness);
    apply_to_strip();
}

void light_driver_set_color_power(bool power, uint8_t red, uint8_t green, uint8_t blue)
{
    s_power = power;
    s_red = red;
    s_green = green;
    s_blue = blue;
    apply_to_strip();
}

void light_driver_get_color(uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (red) *red = s_red;
    if (green) *green = s_green;
    if (blue) *blue = s_blue;
}

void light_driver_set_brightness(uint8_t brightness)
{
    s_ext_brightness = brightness;
    s_brightness = brightness / 2 ;
    ESP_LOGI("LIGHT_DRIVER", "Setting brightness: %d", brightness);
    apply_to_strip();
}

uint8_t light_driver_get_brightness(void)
{
    return s_ext_brightness;
}

bool light_driver_get_power(void)
{
    return s_power;
}

void light_driver_init(bool power)
{
    led_strip_config_t led_strip_conf = {
        .max_leds = CONFIG_EXAMPLE_STRIP_LED_NUMBER,
        .strip_gpio_num = CONFIG_EXAMPLE_STRIP_LED_GPIO,
    };
    led_strip_rmt_config_t rmt_conf = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&led_strip_conf, &rmt_conf, &s_led_strip));
    if (s_strip_mutex == NULL) {
        s_strip_mutex = xSemaphoreCreateMutex();
    }
    light_driver_set_power(power);
}
