#include "gfx.h"
#include <algorithm>
#include <math.h>
#include "board_config.h"

// Software renderer for the 1024x600 RGB565 frame buffer: anti-aliased shapes,
// 4-bit alpha text and icons.

namespace { uint16_t* target = nullptr; }

void gfxSetTarget(uint16_t* frame) { target = frame; }

// ---------- pixels ----------
uint16_t blend(uint16_t bg, uint16_t fg, unsigned a) {
  // a: 0..256. Channels blended separately (a packed red/blue multiply overflows into red).
  const unsigned ia = 256-a;
  const unsigned r = ((fg>>11)*a + (bg>>11)*ia) >> 8;
  const unsigned g = (((fg>>5)&63)*a + ((bg>>5)&63)*ia) >> 8;
  const unsigned b = ((fg&31)*a + (bg&31)*ia) >> 8;
  return uint16_t((r<<11) | (g<<5) | b);
}
void plot(int x,int y,uint16_t c,unsigned a) {
  if(unsigned(x)>=unsigned(SCREEN_W) || unsigned(y)>=unsigned(SCREEN_H) || !a) return;
  uint16_t& d = target[y*SCREEN_W+x];
  d = a>=256 ? c : blend(d, c, a);
}
void span(int x,int y,int w,uint16_t c,unsigned a) {
  if(unsigned(y)>=unsigned(SCREEN_H)) return;
  const int x0 = std::max(0,x), x1 = std::min(SCREEN_W, x+w);
  uint16_t* p = target + y*SCREEN_W;
  if(a>=256) std::fill(p+x0, p+x1, c);
  else for(int i=x0; i<x1; ++i) p[i] = blend(p[i], c, a);
}

// Anti-aliased rounded rectangle (signed distance). ring>0 draws an outline of that width.
void shape(int x,int y,int w,int h,int r,uint16_t c,unsigned alpha,float ring) {
  r = std::min(r, std::min(w,h)/2);
  const float hw = w*0.5f, hh = h*0.5f, cx = x+hw, cy = y+hh;
  for(int py=y; py<y+h; ++py) {
    if(unsigned(py)>=unsigned(SCREEN_H)) continue;
    const float dy = fabsf(py+0.5f-cy) - (hh-r);
    const bool cornerRow = dy > 0;
    if(!cornerRow && ring<=0) {  // straight band: only the left/right r pixels are rounded
      span(x, py, w, c, alpha);
      continue;
    }
    for(int px=x; px<x+w; ++px) {
      const float dx = fabsf(px+0.5f-cx) - (hw-r);
      const float ox = std::max(dx,0.f), oy = std::max(dy,0.f);
      const float d = sqrtf(ox*ox+oy*oy) + std::min(std::max(dx,dy),0.f) - r;
      float cov = std::min(1.f, std::max(0.f, 0.5f-d));
      if(ring>0) cov -= std::min(1.f, std::max(0.f, 0.5f-(d+ring)));
      if(cov>0) plot(px, py, c, unsigned(alpha*cov));
    }
  }
}
void circle(int cx,int cy,int r,uint16_t c,unsigned alpha) { shape(cx-r, cy-r, 2*r, 2*r, r, c, alpha); }
void glass(int x,int y,int w,int h,int r,unsigned alpha) {
  shape(x, y, w, h, r, GLASS, alpha);
  shape(x, y, w, h, r, WHITE, 36, 1.2f);  // hairline edge
}
void divider(int x,int y,int w) { span(x, y, w, WHITE, 28); }

// ---------- text ----------
uint32_t nextChar(const char*& s) {
  uint8_t b=uint8_t(*s++);
  if(b<128) return b;
  int n=(b&0xE0)==0xC0?1:((b&0xF0)==0xE0?2:3);
  uint32_t c=b&((1<<(6-n))-1);
  while(n-- && *s) c=(c<<6)|(uint8_t(*s++)&63);
  return c;
}
const Glyph* findGlyph(const Font& f,uint32_t c) {
  int lo=0, hi=f.count-1;
  while(lo<=hi) { int m=(lo+hi)/2; if(f.glyphs[m].code==c) return &f.glyphs[m]; if(f.glyphs[m].code<c) lo=m+1; else hi=m-1; }
  return nullptr;
}
int textWidth(const char* s,const Font& f) {
  int w=0;
  while(*s) { const Glyph* g=findGlyph(f,nextChar(s)); w += g ? g->advance : f.height/2; }
  return w;
}
void text(int x,int y,const char* s,uint16_t color,const Font& f) {
  while(*s) {
    const Glyph* g=findGlyph(f,nextChar(s));
    if(!g) { x+=f.height/2; continue; }
    const uint8_t* p=f.bitmap+g->offset;
    const int stride=(g->advance+1)/2;
    for(int yy=0; yy<f.height; ++yy, p+=stride)
      for(int xx=0; xx<g->advance; ++xx) {
        const unsigned a = (xx&1) ? (p[xx>>1]&15) : (p[xx>>1]>>4);
        if(a) plot(x+xx, y+yy, color, a==15 ? 256 : a*17);
      }
    x+=g->advance;
  }
}
void centerText(int x,int y,int w,const char* s,uint16_t c,const Font& f) { text(x+(w-textWidth(s,f))/2, y, s, c, f); }
void rightText(int right,int y,const char* s,uint16_t c,const Font& f) { text(right-textWidth(s,f), y, s, c, f); }

// ---------- icons ----------
void icon(IconId id,IconSize size,int x,int y,uint16_t color,unsigned alpha) {
  if(!ICON_BITMAP) return;  // assets not loaded
  const int n = ICON_SIZES[size], stride = (n+1)/2;
  const uint8_t* p = ICON_BITMAP + ICON_OFFSETS[id*ICON_SIZE_COUNT + size];
  for(int yy=0; yy<n; ++yy, p+=stride)
    for(int xx=0; xx<n; ++xx) {
      const unsigned a = (xx&1) ? (p[xx>>1]&15) : (p[xx>>1]>>4);
      if(a) plot(x+xx, y+yy, color, (a==15 ? 256 : a*17) * alpha / 256);
    }
}
void iconCenter(IconId id,IconSize size,int cx,int cy,uint16_t color) {
  icon(id, size, cx-ICON_SIZES[size]/2, cy-ICON_SIZES[size]/2, color);
}
// Rounded icon tile used at the top-left of cards.
void iconChip(int x,int y,IconId id,uint16_t fill,unsigned alpha,uint16_t ink) {
  shape(x, y, 52, 52, 16, fill, alpha);
  iconCenter(id, S32, x+26, y+26, ink);
}
void button(int x,int y,int w,int h,const char* label,uint16_t fill,unsigned alpha,uint16_t ink,int iconId) {
  shape(x, y, w, h, std::min(20, h/2), fill, alpha);
  const int tw = textWidth(label, FONT_BODY) + (iconId>=0 ? 32 : 0);
  int tx = x + (w-tw)/2;
  if(iconId>=0) { iconCenter(IconId(iconId), S24, tx+12, y+h/2, ink); tx += 32; }
  text(tx, y+(h-FONT_BODY.height)/2, label, ink);
}

void disc(float cx,float cy,float r,uint16_t c,unsigned alpha,float clipY) {
  const int y0 = int(cy - r - 1), y1 = int(cy + r + 1), x0 = int(cx - r - 1), x1 = int(cx + r + 1);
  for(int py=y0; py<=y1; ++py) {
    if(py + 0.5f > clipY) break;
    for(int px=x0; px<=x1; ++px) {
      const float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
      const float cov = std::min(1.f, std::max(0.f, r + 0.5f - sqrtf(dx*dx + dy*dy)));
      if(cov > 0) plot(px, py, c, unsigned(alpha * cov));
    }
  }
}

void moonDisc(float cx,float cy,float r,float age,uint16_t lit,uint16_t dark,unsigned darkAlpha) {
  const float theta = 2 * float(M_PI) * age / 29.530588853f;
  const bool waxing = theta < float(M_PI);
  const float k = cosf(theta);
  const int y0 = int(cy - r - 1), y1 = int(cy + r + 1), x0 = int(cx - r - 1), x1 = int(cx + r + 1);
  for(int py=y0; py<=y1; ++py)
    for(int px=x0; px<=x1; ++px) {
      const float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
      const float cov = std::min(1.f, std::max(0.f, r + 0.5f - sqrtf(dx*dx + dy*dy)));
      if(cov <= 0) continue;
      // terminator: x = ry*cos(theta) (waxing, lit to the right) / mirrored while waning
      const float ry = sqrtf(std::max(0.f, r*r - dy*dy));
      const float edge = waxing ? (dx - ry * k) : (-ry * k - dx);
      const float litCov = std::min(1.f, std::max(0.f, edge + 0.5f));
      plot(px, py, dark, unsigned(darkAlpha * cov));
      if(litCov > 0) plot(px, py, lit, unsigned(256 * cov * litCov));
    }
}
