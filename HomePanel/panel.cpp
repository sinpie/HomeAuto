#include "panel.h"
#include "board_config.h"
#include "src/lcd/esp_lcd_jd9165.h"
#include "esp_ldo_regulator.h"
#include <Wire.h>
#include "esp_lcd_mipi_dsi.h"
#include "freertos/semphr.h"
#include <algorithm>

static esp_lcd_panel_handle_t lcd;
static void* frameBuffers[2];
static int frontBuffer = 0;
static SemaphoreHandle_t refreshDone;
static esp_ldo_channel_handle_t phyPower;
static uint8_t touchAddress;
static uint16_t touchWidth = SCREEN_W, touchHeight = SCREEN_H;

static bool IRAM_ATTR onRefreshDone(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(refreshDone, &woken);
  return woken == pdTRUE;
}

bool panelBegin() {
  pinMode(LCD_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(LCD_BACKLIGHT_PIN, LOW);
  esp_ldo_channel_config_t power = {};
  power.chan_id = 3;
  power.voltage_mv = 2500;
  if (esp_ldo_acquire_channel(&power, &phyPower) != ESP_OK) return false;
  esp_lcd_dsi_bus_handle_t bus;
  esp_lcd_dsi_bus_config_t busCfg = JD9165_PANEL_BUS_DSI_2CH_CONFIG();
  if (esp_lcd_new_dsi_bus(&busCfg, &bus) != ESP_OK) return false;
  esp_lcd_panel_io_handle_t io;
  esp_lcd_dbi_io_config_t ioCfg = JD9165_PANEL_IO_DBI_CONFIG();
  if (esp_lcd_new_panel_io_dbi(bus, &ioCfg, &io) != ESP_OK) return false;
  esp_lcd_dpi_panel_config_t dpi = JD9165_1024_600_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
#if HOME_PANEL_NEW
  dpi.video_timing.hsync_pulse_width = 24;
  dpi.video_timing.hsync_back_porch = 136;
  dpi.video_timing.vsync_pulse_width = 2;
  dpi.video_timing.vsync_back_porch = 21;
#endif
  // Two driver frame buffers: draw into the hidden one and flip at vsync.
  // Avoids a full-screen PSRAM copy while the panel DMA is reading.
  dpi.num_fbs = 2;
  dpi.flags.use_dma2d = false;
  jd9165_vendor_config_t vendor = {};
  vendor.mipi_config.dsi_bus = bus;
  vendor.mipi_config.dpi_config = &dpi;
  esp_lcd_panel_dev_config_t cfg = {};
  cfg.reset_gpio_num = LCD_RESET_PIN;
  cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  cfg.bits_per_pixel = 16;
  cfg.vendor_config = &vendor;
  if (esp_lcd_new_panel_jd9165(io, &cfg, &lcd) != ESP_OK) return false;
  if (esp_lcd_panel_reset(lcd) != ESP_OK) return false;
  if (esp_lcd_panel_init(lcd) != ESP_OK) return false;
  if (esp_lcd_dpi_panel_get_frame_buffer(lcd, 2, &frameBuffers[0], &frameBuffers[1]) != ESP_OK) return false;
  refreshDone = xSemaphoreCreateBinary();
  if (!refreshDone) return false;
  esp_lcd_dpi_panel_event_callbacks_t callbacks = {};
  callbacks.on_refresh_done = onRefreshDone;
  if (esp_lcd_dpi_panel_register_event_callbacks(lcd, &callbacks, nullptr) != ESP_OK) return false;
  return true;
}

uint16_t* panelBackBuffer() {
  return static_cast<uint16_t*>(frameBuffers[frontBuffer ^ 1]);
}

const uint16_t* panelFrontBuffer() {
  return static_cast<const uint16_t*>(frameBuffers[frontBuffer]);
}

void panelPresent() {
  xSemaphoreTake(refreshDone, 0);
  // Passing a driver frame buffer only writes back the cache and flips at vsync.
  ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(lcd, 0, 0, SCREEN_W, SCREEN_H, panelBackBuffer()));
  frontBuffer ^= 1;
  // Wait until the old front buffer is no longer scanned before reusing it.
  for (int i = 0; i < 2; ++i) xSemaphoreTake(refreshDone, pdMS_TO_TICKS(40));
}

void panelBrightness(uint8_t percent) {
  static bool attached = false;
  if (!attached) attached = ledcAttach(LCD_BACKLIGHT_PIN, 20000, 8);
  if (attached) ledcWrite(LCD_BACKLIGHT_PIN, uint32_t(constrain(percent, 10, 100)) * 255 / 100);
  else digitalWrite(LCD_BACKLIGHT_PIN, HIGH);
}

static bool readRegister(uint16_t reg, uint8_t* data, uint8_t count) {
  Wire.beginTransmission(touchAddress);
  Wire.write(uint8_t(reg >> 8));
  Wire.write(uint8_t(reg));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(touchAddress, count) != count) return false;
  for (uint8_t i=0; i<count; ++i) data[i] = Wire.read();
  return true;
}

bool touchBegin() {
  // The I_W_Y vendor example has no touch reset/interrupt GPIO.
  if (!Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN, 400000)) return false;
  Wire.setTimeOut(30);
  for (uint8_t address : {uint8_t(0x5D), uint8_t(0x14)}) {
    touchAddress = address;
    uint8_t id[4], size[4];
    if (!readRegister(0x8140, id, 4)) continue;
    if (id[0] != '9' || id[1] != '1' || id[2] != '1') continue;
    if (!readRegister(0x8048, size, 4)) return false;
    touchWidth = size[0] | (uint16_t(size[1]) << 8);
    touchHeight = size[2] | (uint16_t(size[3]) << 8);
    if (!touchWidth || !touchHeight) return false;
    return true;
  }
  touchAddress = 0;
  return false;
}

int touchRead(uint16_t& x, uint16_t& y) {
  uint8_t status, point[8];
  if (!touchAddress || !readRegister(0x814E, &status, 1) || !(status & 0x80)) return -1;
  uint8_t count = status & 0x0F;
  bool ok = !count || (count <= 5 && readRegister(0x814F, point, 8));
  Wire.beginTransmission(touchAddress);
  Wire.write(uint8_t(0x81)); Wire.write(uint8_t(0x4E)); Wire.write(uint8_t(0));
  if (Wire.endTransmission() || !ok) return -1;
  if (!count) return 0;
  uint16_t rawX = point[1] | (uint16_t(point[2]) << 8);
  uint16_t rawY = point[3] | (uint16_t(point[4]) << 8);
  uint16_t rw = touchWidth, rh = touchHeight;
  if (TOUCH_SWAP_XY) { std::swap(rawX, rawY); std::swap(rw, rh); }
  x = min(uint32_t(SCREEN_W-1), uint32_t(rawX) * SCREEN_W / rw);
  y = min(uint32_t(SCREEN_H-1), uint32_t(rawY) * SCREEN_H / rh);
  if (TOUCH_MIRROR_X) x = SCREEN_W-1-x;
  if (TOUCH_MIRROR_Y) y = SCREEN_H-1-y;
  return 1;
}
