| Supported Targets | ESP32-C5 | ESP32-h2 | ESP32-H2 |
| ----------------- | -------- | -------- | -------- |

# Zigbee HA Color Light with LED Strip

This project implements a full-featured Zigbee Home Automation color light using an ESP32-h2 and WS2812/SK68XX LED strip. It supports RGB color control, brightness adjustment, and provides comprehensive state reporting for integration with Zigbee2MQTT and other home automation systems.

## Features

- **RGB Color Control**: Full color spectrum support with accurate CIE X/Y color space conversion
- **Brightness Control**: 0-255 brightness levels with smooth scaling
- **Power Control**: On/off functionality
- **Zigbee HA Compliance**: Implements standard Zigbee Home Automation color light clusters
- **State Reporting**: Real-time state logging and attribute reporting
- **LED Strip Support**: Compatible with WS2812/SK68XX type LED strips
- **Zigbee2MQTT Compatible**: Works with popular home automation platforms

## Supported Clusters

- **On/Off Cluster**: Basic power control
- **Level Control Cluster**: Brightness control (0-255)
- **Color Control Cluster**: RGB color control with CIE X/Y coordinates
  - Color Mode (X/Y mode)
  - Current X/Y coordinates
  - Enhanced Current Hue
  - Current Saturation

The ESP Zigbee SDK provides more examples and tools for productization:
* [ESP Zigbee SDK Docs](https://docs.espressif.com/projects/esp-zigbee-sdk)
* [ESP Zigbee SDK Repo](https://github.com/espressif/esp-zigbee-sdk)

## Hardware Required

* **ESP32-h2 Development Board**: Acting as Zigbee end-device
* **WS2812/SK68XX LED Strip**: For RGB color output (connected to GPIO 8)
* **USB Cable**: For power supply and programming
* **Zigbee Coordinator**: Any Zigbee coordinator (Zigbee2MQTT, Home Assistant, etc.)

## Hardware Connections

| ESP32-h2 Pin | LED Strip Connection |
|--------------|---------------------|
| GPIO 8       | Data Input (DIN)    |
| 3.3V         | VCC (if 3.3V compatible) |
| GND          | GND                 |

**Note**: If using 5V LED strips, you may need a level shifter or power the LED strip separately.

## Configure the project

Before project configuration and build, make sure to set the correct chip target using:
```bash
idf.py set-target esp32h2
```

## Erase the NVRAM

Before flashing, it is recommended to erase NVRAM if you don't want to keep previous examples or other projects stored info:
```bash
idf.py -p PORT erase-flash
```

## Build and Flash

Build the project, flash it to the board, and start the monitor tool to view the serial output:
```bash
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type `Ctrl-]`.)

## Usage

### Zigbee2MQTT Integration

1. **Pair the device** with your Zigbee coordinator
2. **Configure in Z2M** - the device should appear as a color light
3. **Control via Z2M**:
   - Toggle power on/off
   - Adjust brightness (0-255)
   - Set RGB colors using CIE X/Y coordinates
   - Read current state and color values

### Color Control

The device supports multiple color control methods:
- **CIE X/Y Coordinates**: Standard color space for accurate color reproduction
- **RGB Values**: Direct RGB color specification
- **HSV Conversion**: Automatic conversion from hue/saturation values

### State Monitoring

The device provides comprehensive state logging:
- Current power state (on/off)
- Brightness level (0-255)
- RGB color values
- CIE X/Y coordinates
- Enhanced hue and saturation values

## Example Output

As you run the example, you will see the following log:

```
I (403) app_start: Starting scheduler on CPU0
I (408) main_task: Started on CPU0
I (408) main_task: Calling app_main()
I (428) phy: phy_version: 230,2, 9aae6ea, Jan 15 2024, 11:17:12
I (428) phy: libbtbb version: 944f18e, Jan 15 2024, 11:17:25
I (438) main_task: Returned from app_main()
I (548) ESP_ZB_COLOR_LIGHT: ZDO signal: ZDO Config Ready (0x17), status: ESP_FAIL
I (548) ESP_ZB_COLOR_LIGHT: Initialize Zigbee stack
W (548) rmt: channel resolution loss, real=10666666
I (558) gpio: GPIO[8]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 
I (548) ESP_ZB_COLOR_LIGHT: Deferred driver initialization successful
I (568) ESP_ZB_COLOR_LIGHT: Device started up in factory-reset mode
I (578) ESP_ZB_COLOR_LIGHT: Start network steering
I (3558) ESP_ZB_COLOR_LIGHT: Joined network successfully (Extended PAN ID: 74:4d:bd:ff:fe:63:f7:30, PAN ID: 0x13af, Channel:13, Short Address: 0x7c16)

// Color control example
I (10238) ESP_ZB_COLOR_LIGHT: Received message: endpoint(10), cluster(0x6), attribute(0x0), data size(1)
I (10238) ESP_ZB_COLOR_LIGHT: Light sets to On
I (10238) ESP_ZB_COLOR_LIGHT: Current State - Power: On, Brightness: 255, RGB: (255,0,0)

// Brightness control example
I (10798) ESP_ZB_COLOR_LIGHT: Received message: endpoint(10), cluster(0x8), attribute(0x0), data size(1)
I (10798) ESP_ZB_COLOR_LIGHT: Current Level set to 128
I (10798) ESP_ZB_COLOR_LIGHT: Current State - Power: On, Brightness: 128, RGB: (255,0,0)

// Color change example
I (11228) ESP_ZB_COLOR_LIGHT: Received message: endpoint(10), cluster(0x300), attribute(0x7), data size(2)
I (11228) ESP_ZB_COLOR_LIGHT: Color X set to 32768
I (11228) ESP_ZB_COLOR_LIGHT: Received message: endpoint(10), cluster(0x300), attribute(0x8), data size(2)
I (11228) ESP_ZB_COLOR_LIGHT: Color Y set to 32768
I (11228) ESP_ZB_COLOR_LIGHT: CIE X/Y->RGB: X=32768, Y=32768 -> R=0, G=255, B=0
I (11228) ESP_ZB_COLOR_LIGHT: Current State - Power: On, Brightness: 128, RGB: (0,255,0)
```

## Control Functions

The device can be controlled through any Zigbee coordinator:

- **Power Control**: Turn the light on/off
- **Brightness Control**: Adjust brightness from 0-255
- **Color Control**: Set RGB colors using CIE X/Y coordinates
- **State Reading**: Read current power, brightness, and color values

## Troubleshooting

### Common Issues

**Zigbee2MQTT Color Read Errors:**
- Ensure the device is properly paired and recognized as a color light
- Try re-pairing the device after flashing new firmware
- Check Z2M logs for detailed error messages

**LED Strip Not Working:**
- Verify GPIO 8 connection to LED strip data input
- Check power supply (3.3V or 5V depending on LED strip)
- Ensure LED strip is WS2812/SK68XX compatible

**Color Accuracy Issues:**
- The device uses CIE X/Y color space for accurate color reproduction
- RGB values are automatically converted to/from CIE coordinates
- Color order is RGB (Red, Green, Blue)

**Brightness Control:**
- Brightness range is 0-255 (0 = off, 255 = full brightness)
- Brightness scaling is applied to all RGB values
- Power off overrides brightness (LED will be off regardless of brightness setting)

### Debug Information

The device provides comprehensive logging:
- All Zigbee commands received
- Current state after each change
- Color conversion details
- Network status and pairing information

For any technical queries, please open an [issue](https://github.com/espressif/esp-idf/issues) on GitHub. We will get back to you soon.
