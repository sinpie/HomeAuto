#pragma once
#include <stdint.h>
#include <stddef.h>

// Street map for the location picker: Esri World Street Map tiles (256x256 JPEG, satellite
// imagery above MAP_STREET_MAX_ZOOM;
// Web Mercator) fetched over HTTPS by a background task and decoded by the ESP32-P4
// JPEG engine into a PSRAM tile cache. Only map tiles around the shown area are
// requested; nothing is sent besides the tile numbers.

constexpr int MAP_MIN_ZOOM = 5, MAP_MAX_ZOOM = 17;
constexpr int MAP_STREET_MAX_ZOOM = 13;   // above: satellite imagery

// Web Mercator world pixels at a zoom level (256 * 2^zoom pixels around the world).
void mapWorldPixel(double lat, double lon, int zoom, double& px, double& py);
void mapLatLon(double px, double py, int zoom, double& lat, double& lon);

// Draws the map into an SCREEN_W-wide RGB565 frame with world pixel (cx, cy) at the
// centre of the rectangle. Missing tiles are queued and drawn as a plain grid.
// Returns the number of tiles still missing.
int mapDraw(uint16_t* frame, int x, int y, int w, int h, double cx, double cy, int zoom);
void mapBegin();            // allocate the tile cache and start the worker (at boot)
uint32_t mapGeneration();   // changes whenever a tile arrives (redraw then)
void mapStats(char* out, size_t n);   // debug timing line since the last call (serial PERF)
const char* mapError();     // last fetch/decode problem, "" if none
