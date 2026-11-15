#pragma once
// GUITION JC1060P470C_I_W_Y: user's manufacturer ZIP, New/Old Panel examples.
#include "panel_variant.h"
#include "sdkconfig.h"
// Actual device identified by esptool: ESP32-P4 revision v3.2.
#if defined(CONFIG_ESP32P4_REV_MAX_FULL) && CONFIG_ESP32P4_REV_MAX_FULL < 300
#error "Select ESP32P4 Chip Variant: v3.00 or newer (ChipVariant=postv3)."
#endif
constexpr int SCREEN_W = 1024;
constexpr int SCREEN_H = 600;
constexpr int LCD_RESET_PIN = 5;
constexpr int LCD_BACKLIGHT_PIN = 23;
constexpr int TOUCH_SDA_PIN = 7;
constexpr int TOUCH_SCL_PIN = 8;
constexpr int TOUCH_RESET_PIN = -1;
constexpr int TOUCH_INT_PIN = -1;
// Read the GT911 configuration at startup; do not assume 800x480 or 1024x600.
constexpr bool TOUCH_SWAP_XY = false;
constexpr bool TOUCH_MIRROR_X = false;
constexpr bool TOUCH_MIRROR_Y = false;
// EW11 RS485-to-TCP server on the wallpad bus (read-only use).
// The address is home-specific: copy local_config.example.h to local_config.h
// (not committed to git) and set it there.
#if __has_include("local_config.h")
#include "local_config.h"
#endif
#ifndef WALLPAD_EW11_HOST
#define WALLPAD_EW11_HOST ""  // empty: wallpad link disabled until configured
#endif
#ifndef WALLPAD_EW11_PORT
#define WALLPAD_EW11_PORT 8899
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""      // empty: use the Ethernet port instead of WiFi
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
constexpr const char* WALLPAD_HOST = WALLPAD_EW11_HOST;
constexpr uint16_t WALLPAD_PORT = WALLPAD_EW11_PORT;
