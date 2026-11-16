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

enum IconSize { S24, S32, S56, S80 };

void gfxSetTarget(uint16_t* frame);  // frame buffer the functions below draw into

uint16_t blend(uint16_t bg, uint16_t fg, unsigned a);
void plot(int x,int y,uint16_t c,unsigned a);
void span(int x,int y,int w,uint16_t c,unsigned a);
void dimFrame();   // darken the whole frame (dialog backdrop), fast path
void shape(int x,int y,int w,int h,int r,uint16_t c,unsigned alpha=256,float ring=0);  // ring>0: outline
void circle(int cx,int cy,int r,uint16_t c,unsigned alpha=256);
void glass(int x,int y,int w,int h,int r,unsigned alpha=168);  // translucent card with hairline edge
// Glass cache: glassBegin() fills the frame with the background (or with the cached
// background + glass of `key`, then glass() draws nothing). glassEnd() returns false when
// the cached glass did not match this frame's cards: draw the frame again. key < 0: no cache.
// Constraint: a glass() card must not overlap anything drawn before it in the same frame
// (the cache draws all glass first). glassEnd() always resets, so a frame without
// glassBegin (the map picker) draws its glass normally.
void glassBegin(const uint16_t* background, int key);
bool glassEnd();
void divider(int x,int y,int w);

uint32_t nextChar(const char*& s);  // UTF-8 decode
const Glyph* findGlyph(const Font& f,uint32_t c);
int textWidth(const char* s,const Font& f);
void text(int x,int y,const char* s,uint16_t color,const Font& f=FONT_BODY,unsigned alpha=256);
void centerText(int x,int y,int w,const char* s,uint16_t c,const Font& f=FONT_BODY);
void rightText(int right,int y,const char* s,uint16_t c,const Font& f=FONT_BODY);

void icon(IconId id,IconSize size,int x,int y,uint16_t color,unsigned alpha=256);
void iconCenter(IconId id,IconSize size,int cx,int cy,uint16_t color);
void iconChip(int x,int y,IconId id,uint16_t fill,unsigned alpha,uint16_t ink);  // 52x52 rounded tile
void button(int x,int y,int w,int h,const char* label,uint16_t fill,unsigned alpha,uint16_t ink,int iconId=-1);

// Anti-aliased filled disc; pixels below clipY (screen y) are skipped (horizon).
void disc(float cx,float cy,float r,uint16_t c,unsigned alpha=256,float clipY=1e9f);
// Lunar eclipse: Earth's shadow centre and radii in moon radii (astro.h lunarShadow).
struct MoonShadow { float dx, dy, umbraR, penumbraR; };
// Solar eclipse: the moon's centre and radius in sun radii (astro.h solarCover).
struct SunCover { float dx, dy, moonR; };
// Moon at its real phase, drawn from the surface map in the assets (tools/make_moon.py:
// seas and craters at their real positions). age in days (0 new .. 14.77 full .. 29.53). The lit
// side is on the right while waxing, on the left while waning (northern hemisphere).
void moonDisc(float cx,float cy,float r,float age,uint16_t lit,uint16_t dark,unsigned darkAlpha,const MoonShadow* shadow=nullptr);
// Sun disc; with a cover, the part hidden by the moon is cut out.
void sunDisc(float cx,float cy,float r,uint16_t c,const SunCover* cover=nullptr);
