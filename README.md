# ESP32 BLE Web Ducky

A powerful, web-controlled Bluetooth Low Energy (BLE) HID injector and BadUSB-style tool running on the ESP32.

This project allows you to emulate a Bluetooth keyboard and execute **Duckyscript** payloads remotely via a WiFi web interface. It supports live typing, file management, BLE scanning, and full device configuration.

## ⚡ Features

* **BLE HID Emulation:** Acts as a standard Bluetooth Keyboard (Logitech Vendor ID).
* **Web-Based Control:** Host a responsive web server to trigger payloads from your phone or laptop.
* **Duckyscript Interpreter:** Native support for standard Ducky commands (`STRING`, `DELAY`, `GUI`, `ENTER`, etc.).
* **Live Injection:** Type text or send shortcut keys (Ctrl+C, Win+R, etc.) in real-time through the web UI.
* **Dual WiFi Mode:** Works in Access Point (AP) mode and Station (Client) mode simultaneously.
* **File System:** Store, edit, and run multiple script files using LittleFS.
* **BLE Scanner:** Scan for nearby BLE devices and view RSSI/MAC addresses.
* **Stealth Features:** Configurable MAC address, Device Name, and Vendor IDs.
* **Status LED:** RGB feedback for connection status, script execution, and errors.

## 🛠 Hardware Requirements

* **ESP32-S3** (Recommended) or Standard ESP32.
    * *Note:* The code is currently configured with `RGB_PIN 48` (Common on **ESP32-S3 Zero** or **SuperMini** boards). If using a standard ESP32, change this pin in the `globals` section of the code.
* USB Cable for power/programming.

## 📦 Dependencies

If using **PlatformIO** (recommended), add these to your `platformio.ini`:

```ini
lib_deps =
    bblanchon/ArduinoJson @ ^7.0.0
    esphome/ESPAsyncWebServer-esphome @ ^3.1.0
    esphome/AsyncTCP-esphome @ ^2.0.0
    adafruit/Adafruit NeoPixel @ ^1.12.0
    h2zero/NimBLE-Arduino @ ^1.4.1