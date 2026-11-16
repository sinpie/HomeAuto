#include "geo_map.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <math.h>
#include <string.h>
#include <algorithm>
#include "driver/jpeg_decode.h"
#include "miniz.h"            // ROM inflate (PNG label tiles)
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "board_config.h"

namespace {
constexpr int TILE = 256, SLOTS = 40;                       // 40 x 128 KB in PSRAM
constexpr uint32_t RETRY_MS = 10000, MAX_JPEG = 96 * 1024;
const uint16_t EMPTY = 0xE71C, GRID = 0xCE59;              // light grey placeholder

enum SlotState : uint8_t { FREE, WANTED, LOADING, READY, FAILED };
struct Slot { int8_t z; int32_t x, y; SlotState state; uint32_t used, failedAt; uint16_t* pix; };
Slot slots[SLOTS];
SemaphoreHandle_t lock;
TaskHandle_t worker;
volatile uint32_t generation = 0;
uint32_t useClock = 0;
char error[64] = "";
int lastCode = 0;
// Error text for the UI, written under the cache lock (mapError() copies it under the same lock).
template<class... A> void setError(const char* fmt, A... args) {
  if(lock) xSemaphoreTake(lock, portMAX_DELAY);
  snprintf(error, sizeof(error), fmt, args...);
  if(lock) xSemaphoreGive(lock);
}   // HTTP status of the last download (404: no label tile there)
// Timing statistics (serial PERF): totals since the last mapStats().
uint32_t statDownloadMs = 0, statDownloads = 0, statBytes = 0, statJpegUs = 0, statJpegs = 0, statPngMs = 0, statPngs = 0;

bool same(const Slot& s, int z, int x, int y) { return s.state != FREE && s.z == z && s.x == x && s.y == y; }

// HTTPS GET of one tile image into buf. Returns the size, 0 on failure.
size_t download(const char* url, uint8_t* buf, size_t cap) {
  // Kept between calls (map task only): with reuse the same TLS session serves the next
  // tile from the same host instead of a new ~1 s handshake.
  static NetworkClientSecure tls;
  static HTTPClient http;
  static bool setUp = false;
  if(!setUp) {
    setUp = true;
    tls.setInsecure();   // public map images only
    tls.setHandshakeTimeout(10);
    http.setReuse(true);
    http.setTimeout(8000);
    http.setConnectTimeout(8000);
    http.setUserAgent("HomePanel/1.0 (ESP32-P4 wall panel)");
  }
  const uint32_t t0 = millis();
  if(!http.begin(tls, url)) { setError("지도 연결 실패"); return 0; }
  int code = http.GET();
  if(code < 0) {   // e.g. the kept-alive connection was closed by the server meanwhile
    http.end(); tls.stop();
    if(!http.begin(tls, url)) { setError("지도 연결 실패"); return 0; }
    code = http.GET();
  }
  lastCode = code;
  if(code != HTTP_CODE_OK) { setError("지도 서버 오류 (%d)", code); http.end(); tls.stop(); return 0; }
  NetworkClient* stream = http.getStreamPtr();
  if(!stream) { http.end(); tls.stop(); setError("지도 받기 중단"); return 0; }
  const int total = http.getSize();
  size_t got = 0;
  const uint32_t start = millis();
  while((total < 0 ? (http.connected() || stream->available()) : got < size_t(total)) && millis() - start < 8000) {
    const size_t avail = stream->available();
    if(!avail) { delay(2); continue; }
    if(got + avail > cap) { setError("지도 타일이 너무 큼"); http.end(); tls.stop(); return 0; }
    got += stream->readBytes(buf + got, avail);
  }
  http.end();   // keeps the connection open (reuse)
  if(total > 0 && got != size_t(total)) { tls.stop(); setError("지도 받기 중단"); return 0; }
  statDownloadMs += millis() - t0; ++statDownloads; statBytes += got;
  return got;
}

// Minimal PNG reader for the 256x256 label tiles: 8-bit RGBA, RGB or palette (+tRNS),
// not interlaced. Inflate is the ROM's miniz. Output: RGBA, 4 bytes per pixel.
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
bool decodePng(const uint8_t* png, size_t n, uint8_t* zbuf, size_t zcap, uint8_t* raw, size_t rawCap, uint8_t* rgba) {
  static const uint8_t SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  if(n < 8 || memcmp(png, SIG, 8)) return false;
  uint32_t w = 0, h = 0; uint8_t depth = 0, type = 0;
  uint8_t palette[256][4] = {};
  size_t zlen = 0;
  for(size_t i = 8; i + 12 <= n; ) {
    const uint32_t len = be32(png + i); const uint8_t* tag = png + i + 4; const uint8_t* d = png + i + 8;
    if(len > n - i - 12) return false;   // also catches huge lengths that would wrap
    if(!memcmp(tag, "IHDR", 4)) { w = be32(d); h = be32(d + 4); depth = d[8]; type = d[9]; if(d[12]) return false; }
    else if(!memcmp(tag, "PLTE", 4)) { for(uint32_t k = 0; k < len / 3 && k < 256; ++k) { palette[k][0] = d[3*k]; palette[k][1] = d[3*k+1]; palette[k][2] = d[3*k+2]; palette[k][3] = 255; } }
    else if(!memcmp(tag, "tRNS", 4) && type == 3) { for(uint32_t k = 0; k < len && k < 256; ++k) palette[k][3] = d[k]; }
    else if(!memcmp(tag, "IDAT", 4)) { if(zlen + len > zcap) return false; memcpy(zbuf + zlen, d, len); zlen += len; }
    else if(!memcmp(tag, "IEND", 4)) break;
    i += 12 + len;
  }
  const int bpp = type == 6 ? 4 : type == 2 ? 3 : type == 3 ? 1 : 0;
  if(w != 256 || h != 256 || depth != 8 || !bpp) return false;
  const size_t stride = 256 * bpp, need = 256 * (stride + 1);
  if(need > rawCap || tinfl_decompress_mem_to_mem(raw, rawCap, zbuf, zlen, TINFL_FLAG_PARSE_ZLIB_HEADER) != need) return false;
  uint8_t* prev = nullptr;
  for(uint32_t y = 0; y < 256; ++y) {
    uint8_t* row = raw + y * (stride + 1);
    const uint8_t filter = row[0]; uint8_t* r = row + 1;
    for(size_t x = 0; x < stride; ++x) {
      const int a = x >= size_t(bpp) ? r[x - bpp] : 0, b = prev ? prev[x] : 0, c = (prev && x >= size_t(bpp)) ? prev[x - bpp] : 0;
      int add = 0;
      if(filter == 1) add = a; else if(filter == 2) add = b; else if(filter == 3) add = (a + b) / 2;
      else if(filter == 4) { const int p0 = a + b - c, pa = abs(p0 - a), pb = abs(p0 - b), pc = abs(p0 - c); add = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); }
      r[x] = uint8_t(r[x] + add);
    }
    for(int x = 0; x < 256; ++x) {
      uint8_t* o = rgba + (y * 256 + x) * 4;
      if(type == 6) memcpy(o, r + 4 * x, 4);
      else if(type == 2) { memcpy(o, r + 3 * x, 3); o[3] = 255; }
      else memcpy(o, palette[r[x]], 4);
    }
    prev = r;
  }
  return true;
}
// Blend a label tile onto an imagery tile. Labels exist up to level 14 in Korea: deeper
// tiles use the level-14 parent, enlarged (scale 2^(z-14)), cut to this tile's part.
void blendLabels(uint16_t* out, const uint8_t* rgba, int shift, int subX, int subY) {
  for(int y = 0; y < TILE; ++y)
    for(int x = 0; x < TILE; ++x) {
      const int sx = (subX * TILE + x) >> shift, sy = (subY * TILE + y) >> shift;
      const uint8_t* p = rgba + (sy * TILE + sx) * 4;
      if(!p[3]) continue;
      const unsigned a = p[3] + (p[3] >> 7), ia = 256 - a;   // 0..256, no divisions
      uint16_t& d = out[y * TILE + x];
      const unsigned r = ((p[0] >> 3) * a + (d >> 11) * ia) >> 8;
      const unsigned g = ((p[1] >> 2) * a + ((d >> 5) & 63) * ia) >> 8;
      const unsigned b = ((p[2] >> 3) * a + (d & 31) * ia) >> 8;
      d = uint16_t((r << 11) | (g << 5) | b);
    }
}
constexpr int LABEL_MAX_ZOOM = 14;
const char* const LABEL_LAYERS[] = {"Reference/World_Transportation", "Reference/World_Boundaries_and_Places"};
// Decoded label tiles are kept per layer: deeper satellite levels reuse the same level-14
// label tile for up to 64 children.
struct PngWork { uint8_t* file; uint8_t* z; uint8_t* raw; uint8_t* rgba[2]; int key[2][3]; bool ok[2], empty[2]; };

// One tile: imagery or street map (JPEG, hardware decode to RGB565), then for satellite
// tiles the road and place-name labels on top.
bool fetchTile(jpeg_decoder_handle_t dec, uint8_t* in, size_t inSize, PngWork& work, int z, int x, int y, uint16_t* out) {
  // The street map stops at level 13 in Korea (deeper levels are a "Map data not yet
  // available" picture); satellite imagery goes on to level 17.
  char url[160];
  const bool satellite = z > MAP_STREET_MAX_ZOOM;
  snprintf(url, sizeof(url), "https://server.arcgisonline.com/ArcGIS/rest/services/%s/MapServer/tile/%d/%d/%d",
           satellite ? "World_Imagery" : "World_Street_Map", z, y, x);
  const size_t got = download(url, in, inSize);
  if(!got) return false;
  jpeg_decode_picture_info_t info;
  if(jpeg_decoder_get_info(in, got, &info) != ESP_OK || info.width != TILE || info.height != TILE) {
    setError("지도 형식 오류"); return false;
  }
  jpeg_decode_cfg_t cfg = {};
  cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;   // little-endian RGB565, like the frame buffer
  cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  uint32_t outSize = 0;
  esp_cache_msync(out, TILE * TILE * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  const uint32_t j0 = micros();
  const esp_err_t decoded = jpeg_decoder_process(dec, &cfg, in, got, reinterpret_cast<uint8_t*>(out), TILE * TILE * 2, &outSize);
  statJpegUs += micros() - j0; ++statJpegs;
  if(decoded != ESP_OK) {
    setError("지도 해독 실패"); return false;
  }
  if(satellite && work.rgba[1]) {
    const int lz = std::min(z, LABEL_MAX_ZOOM), shift = z - lz;
    const int px = x >> shift, py = y >> shift, subX = x & ((1 << shift) - 1), subY = y & ((1 << shift) - 1);
    for(int k = 0; k < 2; ++k) {   // a missing label layer still leaves a usable tile
      const bool cached = (work.ok[k] || work.empty[k]) && work.key[k][0] == lz && work.key[k][1] == px && work.key[k][2] == py;
      if(!cached) {
        snprintf(url, sizeof(url), "https://server.arcgisonline.com/ArcGIS/rest/services/%s/MapServer/tile/%d/%d/%d", LABEL_LAYERS[k], lz, py, px);
        const size_t n = download(url, work.file, 128 * 1024);
        const uint32_t p0 = millis();
        work.ok[k] = n && decodePng(work.file, n, work.z, 128 * 1024, work.raw, 256 * 1025 + 16, work.rgba[k]);
        statPngMs += millis() - p0; ++statPngs;
        work.empty[k] = !n && lastCode == 404;   // no labels exist there: do not ask again for its children
        if(work.ok[k] || work.empty[k]) { work.key[k][0] = lz; work.key[k][1] = px; work.key[k][2] = py; }
      }
      if(work.ok[k]) blendLabels(out, work.rgba[k], shift, subX, subY);
    }
  }
  return true;
}

void task(void*) {
  jpeg_decoder_handle_t dec = nullptr;
  jpeg_decode_engine_cfg_t ecfg = {};
  ecfg.timeout_ms = 200;
  jpeg_decode_memory_alloc_cfg_t inCfg = {};
  inCfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
  size_t inSize = 0;
  uint8_t* in = static_cast<uint8_t*>(jpeg_alloc_decoder_mem(MAX_JPEG, &inCfg, &inSize));
  PngWork work = {};   // label decoding buffers in PSRAM (about 0.8 MB)
  auto ps = [](size_t n) { return static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); };
  work.file = ps(128 * 1024); work.z = ps(128 * 1024); work.raw = ps(256 * 1025 + 16);
  work.rgba[0] = ps(256 * 256 * 4); work.rgba[1] = ps(256 * 256 * 4);
  for(auto& k : work.key) k[0] = -1;
  if(!work.file || !work.z || !work.raw || !work.rgba[0]) work.rgba[1] = nullptr;   // no labels, map still works
  if(jpeg_new_decoder_engine(&ecfg, &dec) != ESP_OK || !in) {
    setError("JPEG 디코더 시작 실패");
    worker = nullptr;
    vTaskDelete(nullptr);
  }
  for(;;) {
    // most recently wanted tile first (the one on screen now)
    int pick = -1;
    xSemaphoreTake(lock, portMAX_DELAY);
    for(int i=0; i<SLOTS; ++i)
      if(slots[i].state == WANTED && (pick < 0 || int32_t(slots[i].used - slots[pick].used) > 0)) pick = i;
    Slot job = {};
    if(pick >= 0) { slots[pick].state = LOADING; job = slots[pick]; }
    xSemaphoreGive(lock);
    if(pick < 0) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)); continue; }
    const bool ok = fetchTile(dec, in, inSize, work, job.z, job.x, job.y, job.pix);
    xSemaphoreTake(lock, portMAX_DELAY);
    slots[pick].state = ok ? READY : FAILED;
    slots[pick].failedAt = millis();
    xSemaphoreGive(lock);
    if(ok) setError("%s", "");
    ++generation;
  }
}

bool started = false;
void start() {
  if(started) return;   // once: a failed worker is not restarted on every frame
  started = true;
  lock = xSemaphoreCreateMutex();
  jpeg_decode_memory_alloc_cfg_t outCfg = {};
  outCfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
  for(Slot& s : slots) {
    size_t got = 0;
    s = {};
    s.pix = static_cast<uint16_t*>(jpeg_alloc_decoder_mem(TILE * TILE * 2, &outCfg, &got));
    if(!s.pix || got < TILE * TILE * 2) { setError("지도 메모리 부족"); s.pix = nullptr; }
  }
  // 24 KB: the ROM inflate used for the PNG labels keeps an ~11 KB decompressor on the stack
  // (a 12 KB stack overflowed on the device).
  xTaskCreatePinnedToCore(task, "map", 24576, nullptr, 1, &worker, 0);   // core 0: keep the UI core free
}

// Slot for a tile: existing one, or the least recently used idle slot (marked WANTED).
Slot* request(int z, int x, int y) {
  Slot* lru = nullptr;
  for(Slot& s : slots) {
    if(!s.pix) continue;
    if(same(s, z, x, y)) {
      s.used = ++useClock;
      if(s.state == FAILED && millis() - s.failedAt > RETRY_MS) s.state = WANTED;
      return &s;
    }
    if(s.state == LOADING) continue;
    if(!lru || s.state == FREE || (lru->state != FREE && int32_t(s.used - lru->used) < 0)) lru = &s;
  }
  if(!lru) return nullptr;
  lru->z = int8_t(z); lru->x = x; lru->y = y; lru->state = WANTED; lru->used = ++useClock;
  return lru;
}
}

void mapWorldPixel(double lat, double lon, int zoom, double& px, double& py) {
  const double n = TILE * double(1 << zoom), s = sin(lat * M_PI / 180);
  px = (lon + 180) / 360 * n;
  py = (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * n;
}
void mapLatLon(double px, double py, int zoom, double& lat, double& lon) {
  const double n = TILE * double(1 << zoom);
  lon = px / n * 360 - 180;
  lat = atan(sinh(M_PI * (1 - 2 * py / n))) * 180 / M_PI;
}

int mapDraw(uint16_t* frame, int x, int y, int w, int h, double cx, double cy, int zoom) {
  start();
  const int tiles = 1 << zoom;
  const double left = cx - w / 2.0, top = cy - h / 2.0;
  const int tx0 = int(floor(left / TILE)), ty0 = int(floor(top / TILE));
  const int tx1 = int(floor((left + w - 1) / TILE)), ty1 = int(floor((top + h - 1) / TILE));
  int missing = 0;
  bool asked = false;
  xSemaphoreTake(lock, portMAX_DELAY);
  const uint32_t epoch = useClock;   // anything WANTED but not touched below is no longer on screen
  // pass 1: refresh the visible tiles so pass 2 never evicts one of them
  for(int ty=ty0; ty<=ty1; ++ty) for(int tx=tx0; tx<=tx1; ++tx) {
    if(ty < 0 || ty >= tiles) continue;
    const int wx = ((tx % tiles) + tiles) % tiles;
    for(Slot& s : slots) if(same(s, zoom, wx, ty)) s.used = ++useClock;
  }
  struct Part { int sx, sy; const uint16_t* pix; };
  Part parts[48]; int count = 0;
  for(int ty=ty0; ty<=ty1; ++ty) for(int tx=tx0; tx<=tx1 && count < 48; ++tx) {
    const int sx = x + int(floor(tx * double(TILE) - left)), sy = y + int(floor(ty * double(TILE) - top));
    Slot* s = nullptr;
    if(ty >= 0 && ty < tiles) {
      s = request(zoom, ((tx % tiles) + tiles) % tiles, ty);
      if(s && s->state == WANTED) asked = true;
    }
    const bool ready = s && s->state == READY;
    if(!ready && ty >= 0 && ty < tiles) ++missing;
    parts[count++] = {sx, sy, ready ? s->pix : nullptr};
  }
  for(Slot& sl : slots) if(sl.state == WANTED && int32_t(sl.used - epoch) <= 0) sl.state = FREE;
  // READY pixels are only changed after an eviction, and only this (UI) thread evicts:
  // the copy needs no lock, so the tile task is not held up for ~1 MB of copying.
  xSemaphoreGive(lock);
  for(int k=0; k<count; ++k) {
    const int sx = parts[k].sx, sy = parts[k].sy;
    const uint16_t* pix = parts[k].pix;
    const bool ready = pix != nullptr;
    for(int row=0; row<TILE; ++row) {
      const int py = sy + row;
      if(py < y || py >= y + h) continue;
      const int a = std::max(sx, x), b = std::min(sx + TILE, x + w);
      if(a >= b) continue;
      uint16_t* dst = frame + py * SCREEN_W + a;
      if(ready) memcpy(dst, pix + row * TILE + (a - sx), (b - a) * 2);
      else {
        std::fill(dst, dst + (b - a), (row % 64) ? EMPTY : GRID);
        for(int c = a; c < b; ++c) if((c - sx) % 64 == 0) frame[py * SCREEN_W + c] = GRID;
      }
    }
  }
  if(asked && worker) xTaskNotifyGive(worker);
  return missing;
}

void mapBegin() { start(); }
uint32_t mapGeneration() { return generation; }
void mapStats(char* out, size_t n) {
  snprintf(out, n, "MAP|downloads|%lu|avg_ms|%lu|kb|%lu|jpeg|%lu|avg_us|%lu|png|%lu|avg_ms|%lu",
           (unsigned long)statDownloads, (unsigned long)(statDownloads ? statDownloadMs / statDownloads : 0),
           (unsigned long)(statBytes / 1024), (unsigned long)statJpegs, (unsigned long)(statJpegs ? statJpegUs / statJpegs : 0),
           (unsigned long)statPngs, (unsigned long)(statPngs ? statPngMs / statPngs : 0));
  statDownloadMs = statDownloads = statBytes = statJpegUs = statJpegs = statPngMs = statPngs = 0;
}
const char* mapError() {   // UI only: a stable copy (the tile task rewrites error)
  static char copy[64];
  if(!lock) return "";
  xSemaphoreTake(lock, portMAX_DELAY);
  snprintf(copy, sizeof(copy), "%s", error);
  xSemaphoreGive(lock);
  return copy;
}
