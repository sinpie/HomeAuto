#pragma once
#include <stdint.h>

// UI assets (background picture, icons, fonts) are not compiled into the firmware.
// They are one blob, assets/assets.bin (tools/assets_pack.py), written to the
// "ffat" data partition by tools/upload_p4.py. assetsBegin() memory-maps it.

bool assetsBegin();               // false: blob missing or invalid (see assetsError())
const char* assetsError();

extern const uint16_t* BG_IMAGE;  // RGB565 SCREEN_W x SCREEN_H, nullptr if missing
extern const uint8_t* ICON_BITMAP;   // 4-bit alpha icons, nullptr if missing
extern const uint32_t* ICON_OFFSETS; // [icon * ICON_SIZE_COUNT + size] -> offset in ICON_BITMAP
// Moon surface map (tools/make_moon.py), RGB565 size x size, largest level first.
struct MoonLevel { uint16_t size; const uint16_t* pixels; const int8_t* relief; };  // relief: x slope * 64
constexpr int MOON_MAX_LEVELS = 8;
extern MoonLevel MOON_MAP[MOON_MAX_LEVELS];
extern int MOON_LEVEL_COUNT;           // 0 if missing (the moon is then drawn flat)
