#include "assets.h"
#include <string.h>
#include "esp_partition.h"
#include "board_config.h"
#include "font_data.h"
#include "assets_data.h"

const uint16_t* BG_IMAGE = nullptr;
const uint8_t* ICON_BITMAP = nullptr;
const uint32_t* ICON_OFFSETS = nullptr;
MoonLevel MOON_MAP[MOON_MAX_LEVELS] = {};
int MOON_LEVEL_COUNT = 0;
Font FONT_CAPTION = {}, FONT_BODY = {}, FONT_TITLE = {}, FONT_DISPLAY = {}, FONT_TEXT = {};

namespace {
struct Header { char magic[4]; uint32_t version, total, count; };
struct Entry { uint32_t type, offset, size; };
enum : uint32_t { SECTION_BACKGROUND = 1, SECTION_ICONS = 2, SECTION_FONT = 3, SECTION_MOON = 4 };
const char* error = "not loaded";

uint16_t u16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
uint32_t u32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

bool fail(const char* why) { error = why; return false; }
}

const char* assetsError() { return error; }

bool assetsBegin() {
  const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, nullptr);
  if(!part) return fail("no ffat partition");
  Header h;
  if(esp_partition_read(part, 0, &h, sizeof(h)) != ESP_OK) return fail("partition read failed");
  if(memcmp(h.magic, "HPA1", 4) != 0) return fail("assets not uploaded");
  if(h.version != 1 || h.total < sizeof(h) || h.total > part->size || h.count > 32) return fail("bad asset header");

  const void* mapped = nullptr;
  esp_partition_mmap_handle_t handle;
  if(esp_partition_mmap(part, 0, h.total, ESP_PARTITION_MMAP_DATA, &mapped, &handle) != ESP_OK) return fail("mmap failed");
  const uint8_t* base = static_cast<const uint8_t*>(mapped);
  const uint8_t* table = base + sizeof(Header);
  Font* fonts[] = {&FONT_CAPTION, &FONT_BODY, &FONT_TITLE, &FONT_DISPLAY, &FONT_TEXT};

  if(sizeof(Header) + 12u*h.count > h.total) return fail("bad section table");
  for(uint32_t i=0; i<h.count; ++i) {
    const Entry e = {u32(table + 12*i), u32(table + 12*i + 4), u32(table + 12*i + 8)};
    if(e.offset < sizeof(Header) || e.offset > h.total || e.size > h.total - e.offset || (e.offset & 3) || e.size < 8)
      return fail("bad section table");   // compared without overflow; every section has an 8-byte head
    const uint8_t* s = base + e.offset;
    if(e.type == SECTION_BACKGROUND) {
      if(u16(s) == SCREEN_W && u16(s+2) == SCREEN_H && e.size >= 8 + 2u*SCREEN_W*SCREEN_H)
        BG_IMAGE = reinterpret_cast<const uint16_t*>(s + 8);
    } else if(e.type == SECTION_ICONS) {
      const uint16_t icons = u16(s), sizes = u16(s+2);
      bool sameSizes = sizes == ICON_SIZE_COUNT;
      for(int k=0; sameSizes && k<ICON_SIZE_COUNT; ++k) sameSizes = s[4+k] == ICON_SIZES[k];
      const uint32_t head = 8 + 4u*icons*sizes;
      if(icons == ICON_COUNT && sameSizes && head <= e.size) {
        // Every icon image must lie inside the section (a bad offset would read past the map).
        const uint32_t* offsets = reinterpret_cast<const uint32_t*>(s + 8);
        const uint32_t bitmapSize = e.size - head;
        bool inside = true;
        for(int id=0; inside && id<icons; ++id)
          for(int k=0; inside && k<sizes; ++k) {
            const uint32_t n = ICON_SIZES[k];
            inside = offsets[id*sizes+k] <= bitmapSize && (n+1)/2*n <= bitmapSize - offsets[id*sizes+k];
          }
        if(!inside) return fail("bad icon offsets");
        ICON_OFFSETS = offsets;
        ICON_BITMAP = s + head;
      }
    } else if(e.type == SECTION_MOON) {
      const uint16_t levels = u16(s);
      if(levels <= MOON_MAX_LEVELS && 4 + 12u*levels <= e.size) {
        int n = 0;
        for(int k=0; k<levels; ++k) {
          const uint16_t size = u16(s + 4 + 12*k);
          const uint32_t off = u32(s + 8 + 12*k), rel = u32(s + 12 + 12*k);
          if(size < 2 || size > 1024 || (off & 1) || off > e.size || 2u*size*size > e.size - off ||
             rel > e.size || 1u*size*size > e.size - rel || (k && size >= MOON_MAP[n-1 < 0 ? 0 : n-1].size)) { n = 0; break; }
          MOON_MAP[n++] = {size, reinterpret_cast<const uint16_t*>(s + off), reinterpret_cast<const int8_t*>(s + rel)};
        }
        MOON_LEVEL_COUNT = n;
      }
    } else if(e.type == SECTION_FONT) {
      const uint16_t id = u16(s), height = u16(s+2);
      const uint32_t glyphs = u32(s+4);
      if(id < 5 && glyphs < 65536 && 8 + 12u*glyphs <= e.size) {
        const Glyph* table = reinterpret_cast<const Glyph*>(s + 8);
        const uint32_t bitmapSize = e.size - (8 + 12u*glyphs);
        // Glyphs must be inside the section and sorted by code (binary search).
        for(uint32_t g=0; g<glyphs; ++g) {
          if(table[g].offset > bitmapSize || (table[g].advance+1u)/2*height > bitmapSize - table[g].offset) return fail("bad font offsets");
          if(g && table[g].code <= table[g-1].code) return fail("font not sorted");
        }
        Font& f = *fonts[id];
        f.glyphs = table;
        f.count = uint16_t(glyphs);
        f.height = uint8_t(height);
        f.bitmap = s + 8 + 12u*glyphs;
      }
    }
  }
  if(!BG_IMAGE) return fail("background missing");
  if(!ICON_BITMAP) return fail("icons missing or icon list changed");
  if(!FONT_BODY.count || !FONT_CAPTION.count || !FONT_TITLE.count || !FONT_DISPLAY.count) return fail("fonts missing");
  error = "ok";
  return true;
}
