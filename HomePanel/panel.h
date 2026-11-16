#pragma once
#include <Arduino.h>
#include "esp_lcd_panel_ops.h"
bool panelBegin();
// Hidden frame buffer to draw into. The visible buffer is never written.
uint16_t* panelBackBuffer();
// Shows the back buffer at the next vertical blank (no copy, no tearing). Returns at once.
void panelPresent();
// Waits until the back buffer is no longer scanned out. Call before drawing into it.
void panelAcquireBack();
// Buffer currently on screen (read-only use: debug screenshots).
const uint16_t* panelFrontBuffer();
void panelBrightness(uint8_t percent);
bool touchBegin();
// -1: no new report / read failure, 0: released, 1: pressed.
int touchRead(uint16_t& x, uint16_t& y);
// Up to `max` touch points (pinch zoom). Returns the count, 0 when released, -1 when no new data.
int touchReadPoints(uint16_t* xs, uint16_t* ys, int max);
