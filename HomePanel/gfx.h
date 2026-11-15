#pragma once
#include <stdint.h>
#include "font_data.h"
#include "assets_data.h"

// Drawing primitives for the panel UI (see gfx.cpp). All coordinates are pixels
// on the 1024x600 screen; alpha is 0..256 (256 = opaque).

constexpr uint16_t rgb(unsigned c) { return ((c>>8)&0xF800)|((c>>5)&0x07E0)|((c>>3)&0x001F); }
// Palette: dark glass over the photo background, one emerald accent.
constexpr uint16_t WHITE=rgb(0xFFFFFF), BLACK=rgb(0x000000), GLASS=rgb(0x0B1230);
constexpr uint16_t TEXT=rgb(0xF8FAFC), MUTED=rgb(0xB6C2D9), FAINT=rgb(0x8C99B5);
constexpr uint16_t ACCENT=rgb(0x34D399), ON_ACCENT=rgb(0x052E21), WARN=rgb(0xFBBF24), ON_WARN=rgb(0x3B2503);
constexpr uint16_t DANGER=rgb(0xF87171), HEAT=rgb(0xFB923C), ON_HEAT=rgb(0x3A1503);

enum IconSize { S24, S32, S56 };

void gfxSetTarget(uint16_t* frame);  // frame buffer the functions below draw into

uint16_t blend(uint16_t bg, uint16_t fg, unsigned a);
void plot(int x,int y,uint16_t c,unsigned a);
void span(int x,int y,int w,uint16_t c,unsigned a);
void shape(int x,int y,int w,int h,int r,uint16_t c,unsigned alpha=256,float ring=0);  // ring>0: outline
void circle(int cx,int cy,int r,uint16_t c,unsigned alpha=256);
void glass(int x,int y,int w,int h,int r,unsigned alpha=168);  // translucent card with hairline edge
void divider(int x,int y,int w);

uint32_t nextChar(const char*& s);  // UTF-8 decode
const Glyph* findGlyph(const Font& f,uint32_t c);
int textWidth(const char* s,const Font& f);
void text(int x,int y,const char* s,uint16_t color,const Font& f=FONT_BODY);
void centerText(int x,int y,int w,const char* s,uint16_t c,const Font& f=FONT_BODY);
void rightText(int right,int y,const char* s,uint16_t c,const Font& f=FONT_BODY);

void icon(IconId id,IconSize size,int x,int y,uint16_t color);
void iconCenter(IconId id,IconSize size,int cx,int cy,uint16_t color);
void iconChip(int x,int y,IconId id,uint16_t fill,unsigned alpha,uint16_t ink);  // 52x52 rounded tile
void button(int x,int y,int w,int h,const char* label,uint16_t fill,unsigned alpha,uint16_t ink,int iconId=-1);
