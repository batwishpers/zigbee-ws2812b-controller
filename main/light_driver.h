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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* light intensity level */
#define LIGHT_DEFAULT_ON  1
#define LIGHT_DEFAULT_OFF 0

/* LED strip configuration */
#define CONFIG_EXAMPLE_STRIP_LED_GPIO   10
#define CONFIG_EXAMPLE_STRIP_LED_NUMBER 36

/**
* @brief Set light power (on/off).
*
* @param  power  The light power to be set
*/
void light_driver_set_power(bool power);

/**
* @brief Set light color (RGB).
*
* @param  red    Red component (0-255)
* @param  green  Green component (0-255)
* @param  blue   Blue component (0-255)
*/
void light_driver_set_color(uint8_t red, uint8_t green, uint8_t blue);

/**
* @brief Push a full frame of per-pixel colors to the strip.
*
* Each channel is power/brightness-scaled exactly like the static color path,
* and access is serialized with the same mutex. Unlike light_driver_set_color(),
* this does NOT mutate the stored static color, so an effect can render freely
* and the last user color is restored when the effect ends.
*
* @param rgb   Pointer to count*3 bytes, laid out R,G,B per pixel.
* @param count Number of pixels (clamped to CONFIG_EXAMPLE_STRIP_LED_NUMBER).
*/
void light_driver_set_pixels(const uint8_t *rgb, uint16_t count);

/**
* @brief Set light color and power.
*
* @param  power  The light power to be set
* @param  red    Red component (0-255)
* @param  green  Green component (0-255)
* @param  blue   Blue component (0-255)
*/
void light_driver_set_color_power(bool power, uint8_t red, uint8_t green, uint8_t blue);

/**
* @brief Get current light color.
*
* @param  red    Pointer to store red component
* @param  green  Pointer to store green component
* @param  blue   Pointer to store blue component
*/
void light_driver_get_color(uint8_t *red, uint8_t *green, uint8_t *blue);

/**
* @brief Convert CIE X/Y coordinates to RGB values
*
* @param x CIE X coordinate (0-65535)
* @param y CIE Y coordinate (0-65535)
* @param r Pointer to store red component (0-255)
* @param g Pointer to store green component (0-255)
* @param b Pointer to store blue component (0-255)
*/
void cie_xy_to_rgb(uint16_t x, uint16_t y, uint8_t *r, uint8_t *g, uint8_t *b);

/**
* @brief Set brightness level (0-255)
*
* @param brightness Brightness level (0-255, where 0 is off and 255 is full brightness)
*/
void light_driver_set_brightness(uint8_t brightness);

/**
* @brief Get current brightness level
*
* @return Current brightness level (0-255)
*/
uint8_t light_driver_get_brightness(void);

/**
* @brief Get current power state
*
* @return Current power state (true = on, false = off)
*/
bool light_driver_get_power(void);

/**
* @brief color light driver init, be invoked where you want to use color light
*
* @param power power on/off
*/
void light_driver_init(bool power);

#ifdef __cplusplus
} // extern "C"
#endif
