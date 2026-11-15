#pragma once
#include <Arduino.h>
#include "esp_lcd_panel_ops.h"
bool panelBegin();
// Hidden frame buffer to draw into. The visible buffer is never written.
uint16_t* panelBackBuffer();
// Shows the back buffer at the next vertical blank (no copy, no tearing).
void panelPresent();
// Buffer currently on screen (read-only use: debug screenshots).
const uint16_t* panelFrontBuffer();
void panelBrightness(uint8_t percent);
bool touchBegin();
// -1: no new report / read failure, 0: released, 1: pressed.
int touchRead(uint16_t& x, uint16_t& y);
