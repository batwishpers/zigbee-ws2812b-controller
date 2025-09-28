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
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "math.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zb_light.h"

#if !defined ZB_ED_ROLE
#error Define ZB_ED_ROLE in idf.py menuconfig to compile light (End Device) source code.
#endif

static const char *TAG = "ESP_ZB_COLOR_LIGHT";
/********************* Define functions **************************/
static esp_err_t deferred_driver_init(void)
{
    light_driver_init(LIGHT_DEFAULT_OFF);
    
    // Test the LED strip with basic color test
    ESP_LOGI(TAG, "Testing LED strip with basic color test...");
    light_driver_set_power(true); // Turn on for test
    light_driver_test_color_order();
    light_driver_set_power(LIGHT_DEFAULT_OFF); // Turn off after test
    
    ESP_LOGI(TAG, "LED strip test complete");
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
                // Convert hue to RGB using current saturation
                float h = (current_hue / 255.0f) * 360.0f;
                float s = current_saturation / 255.0f;
                hsv_to_rgb(h, s, 1.0f, &red, &green, &blue);
                light_driver_set_color(red, green, blue);
            } else if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8) {
                current_saturation = message->attribute.data.value ? *(uint8_t *)message->attribute.data.value : 255;
                ESP_LOGI(TAG, "Current Saturation set to %d", current_saturation);
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
                // Get the stored X value and convert X/Y to RGB
                cie_xy_to_rgb(stored_x, current_y, &red, &green, &blue);
                ESP_LOGI(TAG, "CIE X/Y->RGB: X=%d, Y=%d -> R=%d, G=%d, B=%d", stored_x, current_y, red, green, blue);
                light_driver_set_color(red, green, blue);
            } else if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16) {
                uint16_t hue = message->attribute.data.value ? *(uint16_t *)message->attribute.data.value : 0;
                ESP_LOGI(TAG, "Enhanced Hue set to %d", hue);
                // Convert enhanced hue to RGB (0-65535 range)
                float h = (hue / 65535.0f) * 360.0f;
                float s = current_saturation / 255.0f;
                hsv_to_rgb(h, s, 1.0f, &red, &green, &blue);
                light_driver_set_color(red, green, blue);
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
        log_current_state();
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
    esp_zb_ep_list_t *esp_zb_color_light_ep = esp_zb_color_dimmable_light_ep_create(HA_ESP_LIGHT_ENDPOINT, &light_cfg);
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
