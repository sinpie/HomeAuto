#pragma once
#include <stdint.h>

// 4-bit alpha bitmap fonts (Noto Sans KR, OFL-1.1), rows padded to whole bytes.
// The data lives in assets/assets.bin; assetsBegin() fills these structs.
// Layout must match tools/assets_pack.py (12-byte glyph records).
struct Glyph { uint32_t code; uint16_t advance; uint32_t offset; };
static_assert(sizeof(Glyph) == 12, "glyph record layout");
struct Font { const Glyph* glyphs; uint16_t count; uint8_t height; const uint8_t* bitmap; };

// Font ids in the blob: 0 caption 19px, 1 body 23px, 2 title 30px, 3 digits 60px.
extern Font FONT_CAPTION, FONT_BODY, FONT_TITLE, FONT_DISPLAY;
