#pragma once
// Copy this file to local_config.h (ignored by git) and set your home's values.
#define WALLPAD_EW11_HOST "192.168.0.100"  // EW11 IP address on your LAN
#define WALLPAD_EW11_PORT 8899             // EW11 TCP server port

// WiFi (optional). Leave WIFI_SSID empty to use the Ethernet port instead.
// The board's WiFi runs on its ESP32-C6 coprocessor: 2.4 GHz networks only.
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
