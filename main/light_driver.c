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

static led_strip_handle_t s_led_strip;
static uint8_t s_red = 255, s_green = 255, s_blue = 255;
static bool s_power = false;
static uint8_t s_brightness = 255; // 0-255, where 255 is full brightness

void light_driver_set_power(bool power)
{
    s_power = power;
    // LED strip uses RGB color order, so we need to send Red, Green, Blue
    // Apply brightness scaling: (color * power * brightness) / 255
    uint8_t scaled_red = (s_red * s_power * s_brightness) / 255;
    uint8_t scaled_green = (s_green * s_power * s_brightness) / 255;
    uint8_t scaled_blue = (s_blue * s_power * s_brightness) / 255;
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, scaled_red, scaled_green, scaled_blue));
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
}

void light_driver_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    s_red = red;
    s_green = green;
    s_blue = blue;
    ESP_LOGI("LIGHT_DRIVER", "Setting color: R=%d, G=%d, B=%d, Power=%d, Brightness=%d", red, green, blue, s_power, s_brightness);
    // LED strip uses RGB color order, so we need to send Red, Green, Blue
    // Apply brightness scaling: (color * power * brightness) / 255
    uint8_t scaled_red = (s_red * s_power * s_brightness) / 255;
    uint8_t scaled_green = (s_green * s_power * s_brightness) / 255;
    uint8_t scaled_blue = (s_blue * s_power * s_brightness) / 255;
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, scaled_red, scaled_green, scaled_blue));
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
}

void light_driver_set_color_power(bool power, uint8_t red, uint8_t green, uint8_t blue)
{
    s_power = power;
    s_red = red;
    s_green = green;
    s_blue = blue;
    // LED strip uses RGB color order, so we need to send Red, Green, Blue
    // Apply brightness scaling: (color * power * brightness) / 255
    uint8_t scaled_red = (s_red * s_power * s_brightness) / 255;
    uint8_t scaled_green = (s_green * s_power * s_brightness) / 255;
    uint8_t scaled_blue = (s_blue * s_power * s_brightness) / 255;
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, scaled_red, scaled_green, scaled_blue));
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
}

void light_driver_get_color(uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (red) *red = s_red;
    if (green) *green = s_green;
    if (blue) *blue = s_blue;
}

void light_driver_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
    ESP_LOGI("LIGHT_DRIVER", "Setting brightness: %d", brightness);
    // Update the LED with current color and new brightness
    uint8_t scaled_red = (s_red * s_power * s_brightness) / 255;
    uint8_t scaled_green = (s_green * s_power * s_brightness) / 255;
    uint8_t scaled_blue = (s_blue * s_power * s_brightness) / 255;
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, scaled_red, scaled_green, scaled_blue));
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
}

uint8_t light_driver_get_brightness(void)
{
    return s_brightness;
}

bool light_driver_get_power(void)
{
    return s_power;
}

void light_driver_set_test_color(uint8_t color_index)
{
    uint8_t red, green, blue;
    
    switch (color_index) {
        case 0: // Red
            red = 255; green = 0; blue = 0;
            break;
        case 1: // Green
            red = 0; green = 255; blue = 0;
            break;
        case 2: // Blue
            red = 0; green = 0; blue = 255;
            break;
        case 3: // White
            red = 255; green = 255; blue = 255;
            break;
        case 4: // Yellow
            red = 255; green = 255; blue = 0;
            break;
        case 5: // Cyan
            red = 0; green = 255; blue = 255;
            break;
        case 6: // Magenta
            red = 255; green = 0; blue = 255;
            break;
        default:
            red = 128; green = 128; blue = 128; // Gray
            break;
    }
    
    ESP_LOGI("LIGHT_DRIVER", "Setting test color %d: R=%d, G=%d, B=%d", color_index, red, green, blue);
    light_driver_set_color(red, green, blue);
}

void light_driver_test_color_order(void)
{
    ESP_LOGI("LIGHT_DRIVER", "Testing color order - should see: Red, Green, Blue, White");
    
    // Test pure colors to verify color order
    ESP_LOGI("LIGHT_DRIVER", "Testing RED (should be red)");
    light_driver_set_color(255, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    ESP_LOGI("LIGHT_DRIVER", "Testing GREEN (should be green)");
    light_driver_set_color(0, 255, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    ESP_LOGI("LIGHT_DRIVER", "Testing BLUE (should be blue)");
    light_driver_set_color(0, 0, 255);
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    ESP_LOGI("LIGHT_DRIVER", "Testing WHITE (should be white)");
    light_driver_set_color(255, 255, 255);
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    ESP_LOGI("LIGHT_DRIVER", "Color order test complete");
}

void light_driver_test_all_color_orders(void)
{
    ESP_LOGI("LIGHT_DRIVER", "Testing ALL possible color orders to find correct mapping");
    ESP_LOGI("LIGHT_DRIVER", "Watch the LED and note which test shows the correct colors");
    
    // Test 1: RGB order
    ESP_LOGI("LIGHT_DRIVER", "TEST 1: RGB order - Red should be red");
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, 255, 0, 0)); // R, G, B
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test 2: GRB order (current)
    ESP_LOGI("LIGHT_DRIVER", "TEST 2: GRB order - Red should be red");
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, 0, 255, 0)); // G, R, B
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test 3: BGR order
    ESP_LOGI("LIGHT_DRIVER", "TEST 3: BGR order - Red should be red");
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, 0, 0, 255)); // B, G, R
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test 4: BRG order
    ESP_LOGI("LIGHT_DRIVER", "TEST 4: BRG order - Red should be red");
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, 0, 255, 0)); // B, R, G
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test 5: RBG order
    ESP_LOGI("LIGHT_DRIVER", "TEST 5: RBG order - Red should be red");
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, 255, 0, 0)); // R, B, G
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test 6: GBR order
    ESP_LOGI("LIGHT_DRIVER", "TEST 6: GBR order - Red should be red");
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, 0, 0, 255)); // G, B, R
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI("LIGHT_DRIVER", "Color order test complete - which test showed RED correctly?");
}

void light_driver_test_targeted_orders(void)
{
    ESP_LOGI("LIGHT_DRIVER", "Testing targeted color orders based on observations");
    ESP_LOGI("LIGHT_DRIVER", "Current: Red->Orange, Green->Yellow, Blue->DarkOrange");
    ESP_LOGI("LIGHT_DRIVER", "Testing likely candidates...");
    
    // Test 1: RBG order (Red, Blue, Green)
    ESP_LOGI("LIGHT_DRIVER", "TEST 1: RBG order - Red should be red");
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, 255, 0, 0)); // R, B, G
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test 2: GBR order (Green, Blue, Red)
    ESP_LOGI("LIGHT_DRIVER", "TEST 2: GBR order - Red should be red");
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, 0, 0, 255)); // G, B, R
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test 3: BRG order (Blue, Red, Green)
    ESP_LOGI("LIGHT_DRIVER", "TEST 3: BRG order - Red should be red");
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, 0, 255, 0)); // B, R, G
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test 4: RGB order (Red, Green, Blue) - standard
    ESP_LOGI("LIGHT_DRIVER", "TEST 4: RGB order - Red should be red");
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, 255, 0, 0)); // R, G, B
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI("LIGHT_DRIVER", "Targeted test complete - which test showed PURE RED?");
}

void light_driver_test_cie_xy_conversion(void)
{
    ESP_LOGI("LIGHT_DRIVER", "Testing CIE X/Y color space conversion");
    ESP_LOGI("LIGHT_DRIVER", "Testing known CIE coordinates for Red, Green, Blue");
    
    uint8_t r, g, b;
    
    // Test Red: CIE X=0.64, Y=0.33 (approximate red coordinates)
    ESP_LOGI("LIGHT_DRIVER", "Testing RED: CIE X=0.64, Y=0.33");
    uint16_t red_x = (uint16_t)(0.64f * 65535);
    uint16_t red_y = (uint16_t)(0.33f * 65535);
    cie_xy_to_rgb(red_x, red_y, &r, &g, &b);
    ESP_LOGI("LIGHT_DRIVER", "Red CIE->RGB: R=%d, G=%d, B=%d", r, g, b);
    light_driver_set_color(r, g, b);
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test Green: CIE X=0.30, Y=0.60 (approximate green coordinates)
    ESP_LOGI("LIGHT_DRIVER", "Testing GREEN: CIE X=0.30, Y=0.60");
    uint16_t green_x = (uint16_t)(0.30f * 65535);
    uint16_t green_y = (uint16_t)(0.60f * 65535);
    cie_xy_to_rgb(green_x, green_y, &r, &g, &b);
    ESP_LOGI("LIGHT_DRIVER", "Green CIE->RGB: R=%d, G=%d, B=%d", r, g, b);
    light_driver_set_color(r, g, b);
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test Blue: CIE X=0.15, Y=0.06 (approximate blue coordinates)
    ESP_LOGI("LIGHT_DRIVER", "Testing BLUE: CIE X=0.15, Y=0.06");
    uint16_t blue_x = (uint16_t)(0.15f * 65535);
    uint16_t blue_y = (uint16_t)(0.06f * 65535);
    cie_xy_to_rgb(blue_x, blue_y, &r, &g, &b);
    ESP_LOGI("LIGHT_DRIVER", "Blue CIE->RGB: R=%d, G=%d, B=%d", r, g, b);
    light_driver_set_color(r, g, b);
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test White: CIE X=0.33, Y=0.33 (D65 white point)
    ESP_LOGI("LIGHT_DRIVER", "Testing WHITE: CIE X=0.33, Y=0.33");
    uint16_t white_x = (uint16_t)(0.33f * 65535);
    uint16_t white_y = (uint16_t)(0.33f * 65535);
    cie_xy_to_rgb(white_x, white_y, &r, &g, &b);
    ESP_LOGI("LIGHT_DRIVER", "White CIE->RGB: R=%d, G=%d, B=%d", r, g, b);
    light_driver_set_color(r, g, b);
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI("LIGHT_DRIVER", "CIE X/Y conversion test complete");
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
    light_driver_set_power(power);
}
