#include "gfx.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include "board_config.h"
#include "esp_heap_caps.h"
#include "esp_async_memcpy.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Software renderer for the 1024x600 RGB565 frame buffer: anti-aliased shapes,
// 4-bit alpha text and icons.

namespace { uint16_t* target = nullptr; }

void gfxSetTarget(uint16_t* frame) { target = frame; }

// ---------- pixels ----------
// RGB565 spread over 32 bits (green in the high half) so one multiply blends all three
// channels without them overflowing into each other.
inline uint32_t spread(uint16_t c) { return (c | (uint32_t(c) << 16)) & 0x07E0F81Fu; }
inline uint16_t pack(uint32_t v) { v &= 0x07E0F81Fu; return uint16_t(v | (v >> 16)); }
uint16_t blend(uint16_t bg, uint16_t fg, unsigned a) {
  // a: 0..256 (5-bit precision is enough on RGB565)
  const uint32_t a5 = (a + 4) >> 3, b = spread(bg);
  return pack(b + (((spread(fg) - b) * a5) >> 5));
}
void plot(int x,int y,uint16_t c,unsigned a) {
  if(unsigned(x)>=unsigned(SCREEN_W) || unsigned(y)>=unsigned(SCREEN_H) || !a) return;
  uint16_t& d = target[y*SCREEN_W+x];
  d = a>=256 ? c : blend(d, c, a);
}
void span(int x,int y,int w,uint16_t c,unsigned a) {
  if(unsigned(y)>=unsigned(SCREEN_H)) return;
  const int x0 = std::max(0,x), x1 = std::min(SCREEN_W, x+w);
  if(x1 <= x0) return;   // fully off-screen (also keeps fill/loops in bounds)
  uint16_t* p = target + y*SCREEN_W;
  if(a>=256) { std::fill(p+x0, p+x1, c); return; }
  const uint32_t a5 = (a + 4) >> 3, f = spread(c);
  if(!a5) return;
  for(int i=x0; i<x1; ++i) { const uint32_t b = spread(p[i]); p[i] = pack(b + (((f - b) * a5) >> 5)); }
}

// Whole-frame darkening to ~38% for dialogs: two pixels per 32-bit word, shifts only.
void dimFrame() {
  uint32_t* p = reinterpret_cast<uint32_t*>(target);
  for(size_t i = 0; i < size_t(SCREEN_W) * SCREEN_H / 2; ++i) {
    const uint32_t c = p[i];
    p[i] = ((c >> 2) & 0x39E739E7u) + ((c >> 3) & 0x18E318E3u);   // 1/4 + 1/8 of each channel
  }
}

// Anti-aliased rounded rectangle (signed distance). ring>0 draws an outline of that width.
void shape(int x,int y,int w,int h,int r,uint16_t c,unsigned alpha,float ring) {
  r = std::min(r, std::min(w,h)/2);
  const float hw = w*0.5f, hh = h*0.5f, cx = x+hw, cy = y+hh;
  // Per-pixel coverage is needed only in the corner columns; the middle of a row has one
  // coverage value (it depends on the row alone). Outlines (glass edges) skip the middle.
  const int edge = r + int(ring) + 2;
  const bool split = w > 2*edge && r >= ring + 1;
  auto perPixel = [&](int px0, int px1, int py, float dy) {
    for(int px=px0; px<px1; ++px) {
      const float dx = fabsf(px+0.5f-cx) - (hw-r);
      const float ox = std::max(dx,0.f), oy = std::max(dy,0.f);
      const float d = sqrtf(ox*ox+oy*oy) + std::min(std::max(dx,dy),0.f) - r;
      float cov = std::min(1.f, std::max(0.f, 0.5f-d));
      if(ring>0) cov -= std::min(1.f, std::max(0.f, 0.5f-(d+ring)));
      if(cov>0) plot(px, py, c, unsigned(alpha*cov));
    }
  };
  for(int py=y; py<y+h; ++py) {
    if(unsigned(py)>=unsigned(SCREEN_H)) continue;
    const float dy = fabsf(py+0.5f-cy) - (hh-r);
    const bool cornerRow = dy > 0;
    if(!cornerRow && ring<=0) {  // straight band: only the left/right r pixels are rounded
      span(x, py, w, c, alpha);
      continue;
    }
    if(!split) { perPixel(x, x+w, py, dy); continue; }
    if(cornerRow) {   // top/bottom band: the middle is a plain run
      const float d = dy - r;
      float mid = std::min(1.f, std::max(0.f, 0.5f-d));
      if(ring>0) mid -= std::min(1.f, std::max(0.f, 0.5f-(d+ring)));
      if(mid>0) span(x+edge, py, w-2*edge, c, unsigned(alpha*mid));
    }
    perPixel(x, x+edge, py, dy);
    perPixel(x+w-edge, x+w, py, dy);
  }
}
void circle(int cx,int cy,int r,uint16_t c,unsigned alpha) { shape(cx-r, cy-r, 2*r, 2*r, r, c, alpha); }
// ---------- glass layer cache ----------
// The background and the glass cards of a page do not change between renders, but blending
// them is most of a frame's cost. glassBegin() copies a cached "background + glass" image
// when the page's glass cards are the same as last time; glass() then only records itself.
namespace {
constexpr size_t FRAME_BYTES = size_t(SCREEN_W) * SCREEN_H * 2;
constexpr int GLASS_MAX = 24, GLASS_KEYS = 8;
struct GlassRec { int16_t x, y, w, h, r; uint16_t alpha; };
struct GlassCache { uint16_t* buf = nullptr; const uint16_t* background = nullptr; GlassRec rec[GLASS_MAX]; int n = 0; bool valid = false; };
GlassCache caches[GLASS_KEYS];
GlassRec recorded[GLASS_MAX];
int recordedN = 0, glassKey = -1;
bool skipGlass = false;
const uint16_t* glassBackground = nullptr;
void drawGlass(const GlassRec& g) {
  shape(g.x, g.y, g.w, g.h, g.r, GLASS, g.alpha);
  shape(g.x, g.y, g.w, g.h, g.r, WHITE, 36, 1.2f);  // hairline edge
}
}
// 1.2 MB PSRAM -> PSRAM: the CPU manages ~50 MB/s (24 ms); the AXI DMA copies without it.
// Falls back to memcpy if the DMA is unavailable or a copy fails.
namespace {
async_memcpy_handle_t dma = nullptr;
SemaphoreHandle_t dmaDone = nullptr;
bool dmaTried = false, dmaBroken = false;
bool IRAM_ATTR onDmaDone(async_memcpy_handle_t, async_memcpy_event_t*, void*) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(dmaDone, &woken);
  return woken == pdTRUE;
}
void copyFrame(uint16_t* dst, const uint16_t* src) {
  if(!dmaTried) {
    dmaTried = true;
    async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
    cfg.dma_burst_size = 64;
    dmaDone = xSemaphoreCreateBinary();
    if(!dmaDone || esp_async_memcpy_install_gdma_axi(&cfg, &dma) != ESP_OK) dma = nullptr;
  }
  const int toCpu = ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA;
  if(dma && !dmaBroken && !((uintptr_t(dst) | uintptr_t(src)) & 63) &&
     esp_cache_msync(dst, FRAME_BYTES, toCpu) == ESP_OK) {   // no cached line of dst may be written back over the DMA data
    xSemaphoreTake(dmaDone, 0);
    if(esp_async_memcpy(dma, dst, const_cast<uint16_t*>(src), FRAME_BYTES, onDmaDone, nullptr) == ESP_OK) {
      // Wait for this copy whatever it takes: drawing over a transfer still in flight would
      // lose pixels. A copy that is that slow means something is wrong: stop using the DMA.
      if(xSemaphoreTake(dmaDone, pdMS_TO_TICKS(2000)) != pdTRUE) dmaBroken = true;
      else { esp_cache_msync(dst, FRAME_BYTES, toCpu); return; }   // the CPU must not see stale lines
    }
  }
  memcpy(dst, src, FRAME_BYTES);
}
}

void glassBegin(const uint16_t* background, int key) {
  recordedN = 0; glassKey = key; glassBackground = background; skipGlass = false;
  if(key >= 0 && key < GLASS_KEYS && caches[key].valid && caches[key].background == background) { copyFrame(target, caches[key].buf); skipGlass = true; return; }
  if(background) memcpy(target, background, FRAME_BYTES);
  else std::fill(target, target + SCREEN_W*SCREEN_H, GLASS);
}
bool glassEndInner();
bool glassEnd() {
  const bool ok = glassEndInner();
  skipGlass = false; glassKey = -1; recordedN = 0;   // frames without glassBegin draw glass directly
  return ok;
}
bool glassEndInner() {
  if(glassKey < 0 || glassKey >= GLASS_KEYS || !glassBackground) return true;
  GlassCache& c = caches[glassKey];
  const bool same = recordedN <= GLASS_MAX && c.valid && c.background == glassBackground && c.n == recordedN &&
                    !memcmp(c.rec, recorded, recordedN * sizeof(GlassRec));
  if(same) return true;
  if(skipGlass) { c.valid = false; return false; }   // the cached glass did not match: draw again
  if(recordedN > GLASS_MAX) return true;             // too many cards to cache
  if(!c.buf) c.buf = static_cast<uint16_t*>(heap_caps_aligned_alloc(64, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if(!c.buf) return true;
  uint16_t* frame = target;
  target = c.buf;
  memcpy(c.buf, glassBackground, FRAME_BYTES);
  for(int i=0; i<recordedN; ++i) drawGlass(recorded[i]);
  target = frame;
  esp_cache_msync(c.buf, FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);   // for the DMA reads
  memcpy(c.rec, recorded, recordedN * sizeof(GlassRec)); c.n = recordedN; c.background = glassBackground; c.valid = true;
  return true;
}
void glass(int x,int y,int w,int h,int r,unsigned alpha) {
  const GlassRec g = {int16_t(x), int16_t(y), int16_t(w), int16_t(h), int16_t(r), uint16_t(alpha)};
  if(recordedN < GLASS_MAX) recorded[recordedN] = g;
  ++recordedN;
  if(!skipGlass) drawGlass(g);
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
void text(int x,int y,const char* s,uint16_t color,const Font& f,unsigned alpha) {
  while(*s) {
    const Glyph* g=findGlyph(f,nextChar(s));
    if(!g) { x+=f.height/2; continue; }
    const uint8_t* p=f.bitmap+g->offset;
    const int stride=(g->advance+1)/2;
    for(int yy=0; yy<f.height; ++yy, p+=stride)
      for(int xx=0; xx<g->advance; ++xx) {
        const unsigned a = (xx&1) ? (p[xx>>1]&15) : (p[xx>>1]>>4);
        if(a) plot(x+xx, y+yy, color, ((a==15 ? 256 : a*17) * alpha) >> 8);
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

// ---------- moon ----------
// The surface is a map in the asset blob (tools/make_moon.py: seas, ~150 named craters and
// the bright ray craters at their real positions, mipmapped). Here only the phase, the
// night side and eclipses are added.
namespace {
float smooth(float e0, float e1, float x) {
  float t = (x - e0) / (e1 - e0); t = t < 0 ? 0 : (t > 1 ? 1 : t);
  return t * t * (3 - 2 * t);
}
struct RGBf { float r, g, b; };
RGBf mix(RGBf a, RGBf b, float t) { return {a.r + (b.r-a.r)*t, a.g + (b.g-a.g)*t, a.b + (b.b-a.b)*t}; }
RGBf scale(RGBf a, float k) { return {a.r*k, a.g*k, a.b*k}; }
RGBf toRGB(uint16_t c) { return {float(c>>11)*(255.f/31), float((c>>5)&63)*(255.f/63), float(c&31)*(255.f/31)}; }
int quant(float v, int bits) { const int m = (1<<bits)-1; const int x = int(v * m / 255.f + 0.5f); return x < 0 ? 0 : (x > m ? m : x); }
uint16_t to565(RGBf c) { return uint16_t((quant(c.r,5)<<11) | (quant(c.g,6)<<5) | quant(c.b,5)); }
const RGBf UMBRA_C = {124,45,18};

// Map level whose size best matches the drawn diameter (nullptr: no map in the assets).
const MoonLevel* moonLevel(float r) {
  const MoonLevel* best = nullptr;     // smallest level >= 1.8 r, else the largest
  for(int i=0; i<MOON_LEVEL_COUNT; ++i) {
    const MoonLevel& m = MOON_MAP[i];
    const bool fits = m.size >= 1.8f * r;
    if(!best) best = &m;
    else if(fits && (best->size < 1.8f * r || m.size < best->size)) best = &m;
    else if(!fits && best->size < 1.8f * r && m.size > best->size) best = &m;
  }
  return best;
}
constexpr float RELIEF_GAIN = 1.0f / 64;     // tools/make_moon.py RELIEF_GAIN / RELIEF_SCALE
// Bilinear sample at disc coordinates (nx, ny in -1..1); slope: surface slope along x.
RGBf sampleMoon(const MoonLevel& m, float nx, float ny, float& slope) {
  const int n = m.size;
  const float u = (nx + 1) * 0.5f * n - 0.5f, v = (ny + 1) * 0.5f * n - 0.5f;
  const int x0 = std::max(0, std::min(n - 2, int(floorf(u)))), y0 = std::max(0, std::min(n - 2, int(floorf(v))));
  const float fx = std::max(0.f, std::min(1.f, u - x0)), fy = std::max(0.f, std::min(1.f, v - y0));
  const uint16_t* p = m.pixels + y0 * n + x0;
  const RGBf a = mix(toRGB(p[0]), toRGB(p[1]), fx), b = mix(toRGB(p[n]), toRGB(p[n + 1]), fx);
  const int8_t* q = m.relief + y0 * n + x0;
  const float ra = q[0] + (q[1] - q[0]) * fx, rb = q[n] + (q[n + 1] - q[n]) * fx;
  slope = (ra + (rb - ra) * fy) * RELIEF_GAIN;
  return mix(a, b, fy);
}
}

void moonDisc(float cx,float cy,float r,float age,uint16_t lit,uint16_t dark,unsigned darkAlpha,const MoonShadow* shadow) {
  // Sun direction seen from here (x right, z toward the viewer): lit on the right while waxing.
  const float theta = 2 * float(M_PI) * age / 29.530588853f;
  const float sx = sinf(theta), sz = -cosf(theta);
  const RGBf flat = toRGB(lit), darkC = toRGB(dark);
  const MoonLevel* map = moonLevel(r);
  const float edgeSoft = 1.5f / r, invR = 1.f / r;
  const int y0 = int(cy - r - 1), y1 = int(cy + r + 1), x0 = int(cx - r - 1), x1 = int(cx + r + 1);
  for(int py=y0; py<=y1; ++py)
    for(int px=x0; px<=x1; ++px) {
      const float dx = px + 0.5f - cx, dy = py + 0.5f - cy, dist = sqrtf(dx*dx + dy*dy);
      const float cov = std::min(1.f, std::max(0.f, r + 0.5f - dist));
      if(cov <= 0) continue;
      const float nx = std::max(-1.f, std::min(1.f, dx * invR)), ny = std::max(-1.f, std::min(1.f, dy * invR));
      const float rr = std::min(1.f, dist * invR), z = sqrtf(std::max(0.f, 1 - rr*rr));
      const float inc = nx * sx + z * sz;                 // cos of the sun's incidence
      const float litCov = smooth(-edgeSoft, edgeSoft * 2.5f, inc);
      float slope = 0;
      const RGBf surf = map ? sampleMoon(*map, nx, ny, slope) : flat;
      // earthshine: the night side is not black, its seas stay faintly visible (hidden under full light)
      if(cov * litCov < 0.999f) plot(px, py, to565(mix(darkC, surf, 0.08f)), unsigned(darkAlpha * cov * (1 - litCov)));
      if(litCov <= 0) continue;
      // crater relief lit by the real sun direction: long shadows near the terminator, none at full moon
      const float relief = std::max(0.45f, std::min(1.35f, (inc - slope * sx) / std::max(inc, 0.12f)));
      // low sun: the colour difference between seas and highlands fades, relief takes over
      const RGBf flatC = {200, 203, 209};               // tools/make_moon.py shade()
      RGBf col = mix(flatC, surf, 0.55f + 0.45f * smooth(0, 0.5f, inc));
      col = scale(col, (0.84f + 0.16f * smooth(0, 0.6f, inc)) * relief);
      if(shadow) {  // Earth's shadow during a lunar eclipse: copper, darker over the seas
        const float d = hypotf(nx - shadow->dx, ny - shadow->dy);
        const float pen = 1 - smooth(shadow->umbraR, shadow->penumbraR, d);
        const float umb = 1 - smooth(shadow->umbraR - 0.06f, shadow->umbraR + 0.06f, d);
        const float lum = (surf.r + surf.g + surf.b) / 3;
        col = mix(col, darkC, pen * 0.35f);
        col = mix(col, scale(UMBRA_C, 0.6f + 0.4f * smooth(170, 225, lum)), umb * 0.85f);
      }
      plot(px, py, to565(col), unsigned(256 * cov * litCov));
    }
}

void sunDisc(float cx,float cy,float r,uint16_t c,const SunCover* cover) {
  const int y0 = int(cy - r - 1), y1 = int(cy + r + 1), x0 = int(cx - r - 1), x1 = int(cx + r + 1);
  const float mr = cover ? cover->moonR * r : 0, mx = cover ? cx + cover->dx * r : 0, my = cover ? cy + cover->dy * r : 0;
  for(int py=y0; py<=y1; ++py)
    for(int px=x0; px<=x1; ++px) {
      const float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
      float cov = std::min(1.f, std::max(0.f, r + 0.5f - sqrtf(dx*dx + dy*dy)));
      if(cov <= 0) continue;
      if(cover) {  // the moon is invisible against the daytime sky: only cut it out of the sun
        const float ex = px + 0.5f - mx, ey = py + 0.5f - my;
        cov *= std::min(1.f, std::max(0.f, sqrtf(ex*ex + ey*ey) - mr + 0.5f));
      }
      if(cov > 0) plot(px, py, c, unsigned(256 * cov));
    }
}
