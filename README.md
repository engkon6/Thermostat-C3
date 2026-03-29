# Thermostat-C3 (Smart Thermostat for ESP32-C3)

A high-performance, feature-rich smart thermostat firmware designed for the **Wemos LOLIN C3 PICO** (ESP32-C3). It integrates a local OLED display, physical controls, MQTT synchronization, and a sleek web dashboard for remote management.

![Hardware Overview](https://img.shields.io/badge/Hardware-Wemos%20LOLIN%20C3%20PICO-blue)
![Platform](https://img.shields.io/badge/Platform-ESP32--C3-orange)
![Framework](https://img.shields.io/badge/Framework-Arduino-green)

---

## 🚀 Features

- **Dual-Control Management:** Adjust settings via physical buttons or the real-time **Web Dashboard**.
- **MQTT Synchronization:** Seamlessly integrates with Home Assistant, Tasmota, and other MQTT-based systems.
- **OLED Interface:** High-contrast SH1106 (128x64) display with flipping support and auto-dimming.
- **Battery Powered:** Integrated battery monitoring (requires jumper pad soldering on LOLIN C3 PICO).
- **Power Efficiency:** Includes **Deep Sleep** support to maximize battery life when idle.
- **Smart Security:** 
  - **Child Lock:** Dual-button hold to unlock physical setpoint controls.
  - **Auto-Lock:** Automatically re-locks controls after 10 seconds of inactivity.
- **WiFi Connectivity:** Hassle-free setup via **WiFiManager** (AP Mode portal).
- **Remote Updates:** Supports **ArduinoOTA** and **HTTP Update Server** for cable-free maintenance.

---

## 🔌 Hardware & Wiring

### Recommended Hardware
- **MCU:** [Wemos LOLIN C3 PICO](https://www.wemos.cc/en/latest/c3/c3_pico.html) (ESP32-C3)
- **Display:** SH1106 I2C OLED (128x64)
- **Controls:** 4x Tactile Buttons (Active LOW)
- **Power:** 3.7V LiPo Battery (JST-PH 2.0mm)

### Pinout Mapping
| Function | Pin (GPIO) | Description |
| :--- | :--- | :--- |
| **I2C SDA** | `GPIO 8` | Data line for SH1106 Display |
| **I2C SCL** | `GPIO 10`| Clock line for SH1106 Display |
| **Button 1**| `GPIO 2` | Setpoint Down (-) |
| **Button 2**| `GPIO 4` | Setpoint Up (+) |
| **Button 3**| `GPIO 5` | Display Flip / Screen Rotation |
| **Button 4**| `GPIO 6` | Display Toggle (On/Off) / Wake |
| **Battery** | `GPIO 3` | Analog Battery Monitoring (Jumper Required) |
| **RGB LED** | `GPIO 7` | Internal WS2812B Status LED |

---

## 🛠️ Software Setup

### Prerequisites
Ensure you have the following libraries installed in your Arduino environment:
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://arduinojson.org/)
- [PubSubClient](https://github.com/knolleary/pubsubclient)
- [ESP8266 and ESP32 OLED driver](https://github.com/ThingPulse/esp8266-oled-ssd1306) (for SH1106)

### Compilation Settings
- **Board:** `Lolin C3 PICO` (or `ESP32C3 Dev Module`)
- **USB CDC On Boot:** `Enabled` (for serial monitoring)
- **Flash Mode:** `DIO`
- **Partition Scheme:** `Default 4MB with SPIFFS`

### Command Line (arduino-cli)
```bash
arduino-cli compile --fqbn esp32:esp32:lolin_c3_pico ./Thermostat_29
arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:lolin_c3_pico ./Thermostat_29
```

---

## 📖 Usage

1. **Initial Boot:** On the first run, the device will create a WiFi AP named `Thermostat-XXXXXX`. Connect to it and navigate to `192.168.4.1` to provide your WiFi and MQTT credentials.
2. **Web Dashboard:** Access the UI via `http://<IP_ADDRESS>/` to monitor live temperature, setpoints, and battery status.
3. **Child Lock:** Press and hold **Button 1** and **Button 2** simultaneously for 2 seconds to unlock the temperature controls.
4. **Display Flip:** Long press **Button 3** to rotate the display orientation by 180 degrees.
5. **Power Save:** The display will dim after 30 seconds of inactivity. Press any button to wake.

---

## 📝 License
Distributed under the MIT License. See `LICENSE` for more information.
