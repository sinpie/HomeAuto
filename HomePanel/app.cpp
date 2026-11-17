#include <Arduino.h>
#include <algorithm>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <Preferences.h>
#include <sys/time.h>
#include "board_config.h"
#include "panel.h"
#include "gfx.h"
#include "wallpad_link.h"
#include "weather.h"
#include "astro.h"
#include "geo_map.h"

// Wall panel UI: boiler, management office call, elevator call, one-way door calls.
// Boiler state is read directly from the wallpad RS485 bus via the EW11 (read-only).
// Elevator / office / door calls still come from the USB bridge (tools/ha_bridge.py).
// Unknown values are shown as unknown; no command is assumed to succeed.

namespace {
uint16_t* frame;
uint16_t* background;  // PSRAM copy of BG_IMAGE (faster than reading flash every frame)


constexpr uint32_t REPLY_TIMEOUT_MS = 6000, NOTICE_MS = 4000, RING_SHOW_MS = 60000;
constexpr int DEMO_HOME_FLOOR = 12;

enum Page { HOME, WEATHER, BOILER, CONTROL, CALLS, SETTINGS, PAGE_COUNT };
const char* pageNames[] = {"홈", "날씨", "보일러", "제어", "방문 호출", "설정"};
const IconId pageIcons[] = {ICON_HOME, ICON_CLOUD_SUN, ICON_FLAME, ICON_POWER, ICON_BELL, ICON_GEAR};
const char* doorNames[] = {"현관", "1층 현관", "지하주차장 문"};
const IconId doorIcons[] = {ICON_DOOR, ICON_BUILDING, ICON_PARKING};
const char* WEEKDAYS[7] = {"일", "월", "화", "수", "목", "금", "토"};

struct Elevator { int floor=0, dir=0; bool calling=false, pending=false; uint32_t requestedAt=0, arrivedAt=0, failedAt=0, busCallAt=0; } elev;
// Heating rooms = wallpad heating groups 1..ROOM_COUNT (confirmed on this home's bus:
// group 1 living room by a set-temperature sweep; groups 2-4 named by the owner).
constexpr int ROOM_COUNT = 4;
const char* roomNames[ROOM_COUNT] = {"거실", "안방", "컴퓨터방", "아이방"};
constexpr uint32_t ZONE_STALE_MS = 15000;
struct RoomView { bool known, on; int set, current; };
RoomView rooms[ROOM_COUNT];
WallpadSnapshot wallpad = {};
WeatherData weather = {};          // copy of the latest forecast (weather.cpp)
// Location for the sky (sun, moon, planets, eclipses) and the place label: owned by the weather
// task (saved, configured or IP), known before and without a forecast.
float skyLat = 0, skyLon = 0;
char skyPlace[48] = "";
bool skyReady() { return clockReady() && (skyLat != 0 || skyLon != 0); }
uint32_t weatherSignature = 0;
RoomView* demoRoomsPtr = nullptr;  // demo-mode rooms (editable by the demo controls)
uint32_t roomSignature = 0, lastWallpadPoll = 0;
bool wallpadLive();
const char* wallpadStatusText();
const WallpadSnapshot& devices();
bool inside(int x,int y,int bx,int by,int w,int h);
int navPages(int* out);
bool controlVisible();
uint32_t deviceSignature();
struct Office { bool calling=false, pending=false; uint32_t requestedAt=0, startedAt=0, failedAt=0; } office;
struct Ring { uint8_t door; uint64_t at; };
Ring rings[8];
int ringCount = 0, activeRing = -1;
uint32_t totalRings = 0, seenRings = 0;   // all calls ever / calls the calls page has shown
uint64_t activeRingAt = 0, lastRingAt[3] = {};

int page = HOME, brightness = 80;
bool gasConfirm = false;           // "close the gas valve?" popup
uint32_t demoGasDoneAt = 0;
bool demo = false, bridge = false, touchOk = false, dirty = true;
bool pickerTouchReset = false;   // ignore the current touch in the map picker until release
uint32_t perfSection[6];   // debug (PERF): last render background, chrome, page, overlays, present, sun & moon card
uint32_t lastBridge = 0, lastPress = 0, lastDemoTick = 0, shownMinute = 0;
int flashX, flashY, flashW, flashH, flashR; uint32_t flashUntil = 0;
String incoming;

// ---------- state helpers ----------
bool online() { return bridge && millis()-lastBridge < 8000; }
uint64_t nowMs() { return uint64_t(esp_timer_get_time() / 1000); }
bool recent(uint32_t at, uint32_t span) { return at && millis()-at < span; }
bool recentCall(uint64_t at, uint64_t span) { return at && nowMs()-at < span; }
uint32_t minuteStamp() { return clockReady() ? uint32_t(time(nullptr) / 60) : millis() / 60000; }
char noticeText[96] = ""; uint32_t noticeAt = 0;
void notice(const char* text) { snprintf(noticeText, sizeof(noticeText), "%s", text); noticeAt = millis(); }
void commandBusy() { notice("월패드 명령을 보낼 수 없습니다 · 이전 명령 처리 중이거나 연결 끊김"); }
void flash(int x,int y,int w,int h,int r=18) { flashX=x; flashY=y; flashW=w; flashH=h; flashR=r; flashUntil=millis()+160; }
void formatFloor(char* out,size_t n,int floor) {
  if(floor==0) snprintf(out, n, "--");
  else if(floor<0) snprintf(out, n, "B%d", -floor);
  else snprintf(out, n, "%d", floor);
}
void formatAgo(char* out,size_t n,uint64_t at) {
  if(!at) { snprintf(out, n, "기록 없음"); return; }
  const uint64_t s=(nowMs()-at)/1000;
  if(s<60) snprintf(out, n, "방금");
  else if(s<3600) snprintf(out, n, "%lu분 전", (unsigned long)(s/60));
  else snprintf(out, n, "%lu시간 전", (unsigned long)(s/3600));
}
void clearLiveState() { elev = Elevator(); office = Office(); }
void refreshRooms();
void setDemo(bool on) {
  // the call history while in demo is the demo's own: restored afterwards
  static Ring savedRings[8]; static int savedCount = 0; static uint64_t savedLast[3];
  static uint32_t savedTotal = 0, savedSeen = 0;
  if(on && !demo) {
    memcpy(savedRings, rings, sizeof(rings)); savedCount = ringCount; memcpy(savedLast, lastRingAt, sizeof(lastRingAt));
    savedTotal = totalRings; savedSeen = seenRings;
  } else if(!on && demo) {
    memcpy(rings, savedRings, sizeof(rings)); ringCount = savedCount; memcpy(lastRingAt, savedLast, sizeof(lastRingAt));
    totalRings = savedTotal; seenRings = savedSeen; activeRing = -1;
  }
  demo = on; bridge = false; clearLiveState();
  if(demo) elev.floor = 1;
  refreshRooms();
  Serial.println("SYNC");
  dirty = true;
}
void addRing(int door) {
  const int keep = std::min(ringCount, 7);
  std::move_backward(rings, rings+keep, rings+keep+1);
  const uint64_t now = nowMs();
  rings[0] = {uint8_t(door), now};
  ringCount = std::min(ringCount+1, 8);
  ++totalRings;
  lastRingAt[door] = now;
  activeRing = door; activeRingAt = now;
  pickerTouchReset = true;
  panelBrightness(100);  // Wake the screen for a visitor call.
  dirty = true;
}
void dismissRing() { activeRing = -1; panelBrightness(brightness); dirty = true; pickerTouchReset = true; }

// ---------- commands (sent to bridge; state changes only on its reply) ----------
void callElevator() {
  if(demo) {
    if(elev.floor==DEMO_HOME_FLOOR) elev.arrivedAt = millis();
    else elev.calling = true;
  } else {
    if(!online() || elev.floor==0 || elev.calling || elev.pending) return;
    Serial.println("CMD|E|call");
    elev.pending = true; elev.requestedAt = millis();
  }
  dirty = true;
}
void callOffice() {
  if(demo) { office.calling = true; office.startedAt = millis(); }
  else {
    if(!online() || office.calling || office.pending) return;
    Serial.println("CMD|O|call");
    office.pending = true; office.requestedAt = millis();
  }
  dirty = true;
}

// ---------- shared widgets ----------
void drawSidebar() {
  glass(12, 12, 184, 576, 26, 150);
  circle(46, 52, 22, ACCENT);
  iconCenter(ICON_HOME, S24, 46, 51, ON_ACCENT);
  text(80, 34, "우리집", TEXT, FONT_TITLE);
  int nav[PAGE_COUNT];
  const int count = navPages(nav);
  for(int k=0; k<count; ++k) {
    const int i = nav[k], y = 112 + k*66;
    const bool on = page==i;
    if(on) { shape(24, y, 160, 54, 16, WHITE, 30); shape(24, y+15, 4, 24, 2, ACCENT); }
    icon(pageIcons[i], S24, 42, y+15, on ? ACCENT : FAINT);
    text(78, y+(54-FONT_BODY.height)/2, pageNames[i], on ? TEXT : MUTED);
    if(i==CALLS && totalRings != seenRings && page!=CALLS) circle(168, y+27, 5, WARN);  // calls not seen yet
  }
  char up[40];
  const WallClock& c = devices().clock;
  struct tm now;
  if(localNow(now)) {
    snprintf(up, sizeof(up), "%d월 %d일 (%s)", now.tm_mon + 1, now.tm_mday, WEEKDAYS[now.tm_wday]);
  } else if(c.valid) {  // wallpad time (pushed to the entrance switch), advanced locally
    const uint32_t daySec = (uint32_t(c.hour)*3600 + c.minute*60 + c.second + (millis()-c.at)/1000) % 86400;
    snprintf(up, sizeof(up), "%u월 %u일 %02lu:%02lu", c.month, c.day, (unsigned long)(daySec/3600), (unsigned long)(daySec/60%60));
  } else {
    const unsigned long m = millis()/60000;
    if(m<60) snprintf(up, sizeof(up), "가동 %lu분", m); else snprintf(up, sizeof(up), "가동 %lu시간", m/60);
  }
  text(36, 548, up, FAINT, FONT_CAPTION);
}

void drawHeader() {
  text(224, 22, pageNames[page], TEXT, FONT_TITLE);
  const uint16_t dot = demo ? WARN : (wallpadLive() ? ACCENT : FAINT);
  circle(230, 79, 4, dot);
  text(242, 66, wallpadStatusText(), MUTED, FONT_CAPTION);
}

void drawElevator() {
  glass(600, 14, 400, 94, 26);
  circle(646, 61, 26, WHITE, 26);
  iconCenter(ICON_ELEVATOR, S32, 646, 61, TEXT);
  text(686, 22, "엘리베이터", MUTED, FONT_CAPTION);
  char f[12]; formatFloor(f, sizeof(f), elev.floor);
  text(686, 48, f, elev.floor ? TEXT : FAINT, FONT_DISPLAY);
  int x = 686 + textWidth(f, FONT_DISPLAY) + 4;
  text(x, 66, "층", MUTED);
  x += textWidth("층", FONT_BODY) + 6;
  if(elev.dir>0) icon(ICON_UP, S24, x, 67, ACCENT);
  else if(elev.dir<0) icon(ICON_DOWN, S24, x, 67, ACCENT);

  const char* label = "호출"; uint16_t fill = ACCENT, ink = ON_ACCENT; unsigned a = 256;
  if(recent(elev.failedAt, NOTICE_MS)) { label = "실패"; fill = DANGER; ink = DANGER; a = 48; }
  else if(elev.pending) { label = "요청 중"; fill = WHITE; ink = WARN; a = 30; }
  else if(elev.calling) { label = "호출됨"; fill = WARN; ink = ON_WARN; }
  else if(recent(elev.arrivedAt, 5000)) { label = "도착"; fill = ACCENT; ink = ON_ACCENT; }
  else if(!demo && (!online() || elev.floor==0)) { label = "미연결"; fill = WHITE; ink = FAINT; a = 26; }
  button(868, 28, 118, 66, label, fill, a, ink);
}

// ---------- weather helpers ----------

IconId weatherIcon(uint8_t code, bool day) {
  if(code == 0 || code == 1) return day ? ICON_SUN : ICON_MOON;
  if(code == 2) return day ? ICON_CLOUD_SUN : ICON_CLOUD;
  if(code == 3) return ICON_CLOUD;
  if(code == 45 || code == 48) return ICON_FOG;
  if(code >= 95) return ICON_STORM;
  if((code >= 71 && code <= 77) || code == 85 || code == 86) return ICON_SNOW;
  return ICON_RAIN;  // drizzle, rain, showers
}
// ---- layered weather icons (masks from assets/assets.bin, colored and stacked here) ----
// Geometry matches tools/make_assets.py (unit box): full body (0.50,0.50) r 0.25,
// partial body (0.36,0.36) r 0.19, twilight sun on the horizon at y 0.70, r 0.22.
constexpr uint16_t WX_SUN = rgb(0xFDB813), WX_RAYS = rgb(0xFCD34D), WX_MOON = rgb(0xDADCE0), WX_MOON_DARK = rgb(0x475569);
constexpr uint16_t WX_CLOUD_D = rgb(0xF1F5F9), WX_SHADE_D = rgb(0xCBD5E1), WX_CLOUD_N = rgb(0xCBD5E1), WX_SHADE_N = rgb(0x94A3B8);
constexpr uint16_t WX_BACK = rgb(0x94A3B8), WX_RAIN_C = rgb(0x60A5FA), WX_SNOW_C = rgb(0xE0F2FE), WX_BOLT_C = rgb(0xFACC15), WX_FOG_C = rgb(0xCBD5E1);

enum Sky { SKY_CLEAR, SKY_MOSTLY, SKY_PARTLY, SKY_OVERCAST, SKY_FOG, SKY_DRIZZLE, SKY_RAIN, SKY_HEAVY,
           SKY_SHOWER, SKY_SNOW, SKY_SLEET, SKY_STORM };
Sky skyOf(uint8_t code) {
  switch(code) {
    case 0: return SKY_CLEAR;
    case 1: return SKY_MOSTLY;
    case 2: return SKY_PARTLY;
    case 3: return SKY_OVERCAST;
    case 45: case 48: return SKY_FOG;
    case 51: case 53: case 55: return SKY_DRIZZLE;
    case 56: case 57: case 66: case 67: return SKY_SLEET;   // freezing drizzle / rain
    case 61: case 63: return SKY_RAIN;
    case 65: return SKY_HEAVY;
    case 80: case 81: case 82: return SKY_SHOWER;
    case 71: case 73: case 75: case 77: case 85: case 86: return SKY_SNOW;
    default: return code >= 95 ? SKY_STORM : SKY_OVERCAST;
  }
}
float moonAgeNow() {
  static time_t at = 0; static float age = 0;
  const time_t now = time(nullptr);
  if(now / 60 != at / 60) { age = moonAge(now); at = now; }
  return age;
}

// twilight: 0 none, 1 around sunrise, 2 around sunset (clear to partly cloudy skies only)
void drawWeatherIcon(uint8_t code, bool day, int cx, int cy, IconSize size, int twilight = 0) {
  const int n = ICON_SIZES[size], x0 = cx - n/2, y0 = cy - n/2;
  const Sky sky = skyOf(code);
  auto layer = [&](IconId id, uint16_t c) { icon(id, size, x0, y0, c); };
  const bool calm = sky == SKY_CLEAR || sky == SKY_MOSTLY || sky == SKY_PARTLY;
  if(twilight && calm) {
    layer(ICON_WX_TWILIGHT_RAYS, WX_RAYS);
    disc(x0 + 0.50f*n, y0 + 0.70f*n, 0.22f*n, WX_SUN, 256, y0 + 0.70f*n);
    layer(ICON_WX_HORIZON, WX_FOG_C);
    layer(twilight == 1 ? ICON_WX_ARROW_UP : ICON_WX_ARROW_DOWN, WX_RAYS);
    return;
  }
  auto body = [&](bool part) {
    const float bx = x0 + (part ? 0.36f : 0.50f)*n, by = y0 + (part ? 0.36f : 0.50f)*n, br = (part ? 0.19f : 0.25f)*n;
    if(day) { layer(part ? ICON_WX_RAYS_PART : ICON_WX_RAYS_FULL, WX_RAYS); disc(bx, by, br, WX_SUN); }
    else moonDisc(bx, by, part ? br : br * 1.1f, moonAgeNow(), WX_MOON, WX_MOON_DARK, 150);
  };
  const uint16_t cloud = day ? WX_CLOUD_D : WX_CLOUD_N, shade = day ? WX_SHADE_D : WX_SHADE_N;
  switch(sky) {
    case SKY_CLEAR: body(false); return;
    case SKY_MOSTLY: body(true); layer(ICON_WX_CLOUD_SMALL, cloud); return;
    case SKY_PARTLY: body(true); layer(ICON_WX_CLOUD_PART, cloud); layer(ICON_WX_CLOUD_PART_SHADE, shade); return;
    default: break;
  }
  if(sky == SKY_OVERCAST || sky == SKY_STORM || sky == SKY_HEAVY) layer(ICON_WX_CLOUD_BACK, WX_BACK);
  if(sky == SKY_SHOWER) body(true);
  layer(ICON_WX_CLOUD_MAIN, cloud);
  layer(ICON_WX_CLOUD_MAIN_SHADE, shade);
  switch(sky) {
    case SKY_FOG: layer(ICON_WX_FOG, WX_FOG_C); break;
    case SKY_DRIZZLE: layer(ICON_WX_DRIZZLE, WX_RAIN_C); break;
    case SKY_RAIN: case SKY_SHOWER: layer(ICON_WX_RAIN, WX_RAIN_C); break;
    case SKY_HEAVY: layer(ICON_WX_RAIN_HEAVY, WX_RAIN_C); break;
    case SKY_SNOW: layer(ICON_WX_SNOW, WX_SNOW_C); break;
    case SKY_SLEET: layer(ICON_WX_SLEET, WX_RAIN_C); break;
    case SKY_STORM: layer(ICON_WX_BOLT, WX_BOLT_C); break;
    default: break;
  }
}
// Index of today in weather.days (the forecast may be from yesterday until the next fetch).
int todayIndex() {
  if(!weather.firstNoon) return 0;
  const time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);
  const time_t noon = now - (t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec) + 12 * 3600;
  const long days = lround(double(noon - weather.firstNoon) / 86400.0);
  return int(std::min<long>(WX_DAYS - 1, std::max<long>(0, days)));
}
// Sunrise/sunset within +-30 minutes of a local time in minutes.
int twilightAt(int minuteOfDay) {
  const WeatherDay& d = weather.days[todayIndex()];
  if(!d.sunrise || !d.sunset) return 0;
  if(abs(minuteOfDay - d.sunrise) <= 30) return 1;
  if(abs(minuteOfDay - d.sunset) <= 30) return 2;
  return 0;
}
int twilightNow() { struct tm now; return localNow(now) ? twilightAt(now.tm_hour*60 + now.tm_min) : 0; }

// Worst weather and average temperature over today's hours [from, to). False: no data
// (the forecast is too old to cover today).
bool summarize(int from, int to, uint8_t& code, int& temp) {
  int worst = -1, sum = 0, n = 0;
  const int base = todayIndex() * 24;
  for(int h=from + base; h<to + base && h<WX_HOURS; ++h) {
    const WeatherHour& w = weather.hours[h];
    if(weatherSeverity(w.code) > worst) { worst = weatherSeverity(w.code); code = w.code; }
    sum += w.temp; ++n;
  }
  temp = n ? (sum + (sum >= 0 ? n/2 : -n/2)) / n : 0;
  return n > 0;
}
bool isDayHour(int hourOfDay) {
  const WeatherDay& d = weather.days[todayIndex()];
  const int m = hourOfDay * 60 + 30;
  return d.sunrise && d.sunset ? (m >= d.sunrise && m < d.sunset) : (hourOfDay >= 7 && hourOfDay < 18);
}
// "맑은 밤" instead of "맑음" after sunset (the icon is a moon then).
const char* conditionText() {
  if(!weather.isDay && weather.code <= 1) return "맑은 밤";
  return weatherText(weather.code);
}
const char* weatherStatusText() {
  switch(weatherState()) {
    case WX_WAIT_NETWORK: return "인터넷 연결 대기";
    case WX_LOCATING: return "위치 확인 중";
    case WX_FETCHING: return "날씨 받는 중";
    case WX_ERROR: return weatherError();
    default: return "";
  }
}

// Rooms come from the wallpad link (or fixed sample values in demo mode).
void refreshRooms() {
  static RoomView demoRooms[ROOM_COUNT] = {{true,true,24,22},{true,false,18,25},{true,false,23,23},{true,true,27,23}};
  demoRoomsPtr = demoRooms;
  const uint32_t now = millis();
  uint32_t sig = demo ? 1 : 2;
  for(int i=0; i<ROOM_COUNT; ++i) {
    if(demo) rooms[i] = demoRooms[i];
    else {
      const HeatZone& z = wallpad.zones[i];
      const bool fresh = z.seen && now - z.at < ZONE_STALE_MS;
      rooms[i] = {fresh, fresh && z.on, fresh ? z.set : 0, fresh ? z.current : 0};
    }
    sig = sig*131 + (rooms[i].known<<1 | rooms[i].on);
    sig = sig*131 + rooms[i].set;
    sig = sig*131 + rooms[i].current;
  }
  sig = sig*131 + wallpad.state;
  sig = sig*131 + deviceSignature();
  if(page == CONTROL && !controlVisible()) { page = HOME; gasConfirm = false; }
  if(sig != roomSignature) { roomSignature = sig; dirty = true; }
}
// Devices other than heating: the live snapshot, or a fixed sample in demo mode.
WallpadSnapshot& demoDevices() {
  static WallpadSnapshot sample = [] {
    WallpadSnapshot d = {};
    d.gas = {true, 1, 0};
    d.lights[0] = {true, 3, 0b011, 1};
    d.lights[1] = {true, 2, 0b00, 1};
    d.outlets[0] = {true, 0x01, 85, 1};
    d.outlets[1] = {true, 0x11, 3, 1};
    d.vent = {true, true, 2, false, 1};
    d.energy[0] = {true, 1234, 1};
    d.energy[1] = {true, 1250, 1};
    d.energy[2] = {true, 456, 1};
    d.entrance = {true, 1, 0, 0, 1};
    return d;
  }();
  return sample;
}
const WallpadSnapshot& devices() { return demo ? demoDevices() : wallpad; }

// State of the device command addressed to (kind, target), for card feedback.
CommandState commandFor(CommandKind kind, uint8_t target, CommandKind alsoKind = CMD_NONE) {
  if(demo) return CMD_IDLE;
  const CommandStatus& c = wallpad.command;
  if((c.kind != kind && c.kind != alsoKind) || c.target != target) return CMD_IDLE;
  if(c.state == CMD_SENDING) return CMD_SENDING;
  if(c.state == CMD_FAILED && millis() - c.at < 5000) return CMD_FAILED;
  if(c.state == CMD_DONE && millis() - c.at < 2500) return CMD_DONE;
  return CMD_IDLE;
}
// Overrides a card's status text while a command for it is running or just failed.
bool commandStatus(CommandState st, char* status, size_t n, uint16_t& color, const char* doneText) {
  if(st == CMD_SENDING) { snprintf(status, n, "요청 중"); color = WARN; return true; }
  if(st == CMD_FAILED) { snprintf(status, n, "응답 없음"); color = DANGER; return true; }
  if(st == CMD_DONE && doneText) { snprintf(status, n, "%s", doneText); color = ACCENT; return true; }
  return false;
}
enum CardKind : uint8_t { CARD_GAS, CARD_LIGHT, CARD_VENT, CARD_OUTLET, CARD_METER, CARD_ENTRANCE };
struct ControlCard { CardKind kind; uint8_t index; };
constexpr int MAX_CARDS = 1 + LIGHT_GROUPS + 1 + OUTLETS + 3 + 1;
// One card per device, in display order; a card exists only while its device answers.
int controlCards(ControlCard* out) {
  const WallpadSnapshot& d = devices();
  int n = 0;
  if(d.gas.seen) out[n++] = {CARD_GAS, 0};
  for(int i=0; i<LIGHT_GROUPS; ++i) if(d.lights[i].seen) out[n++] = {CARD_LIGHT, uint8_t(i)};
  if(d.vent.seen) out[n++] = {CARD_VENT, 0};
  for(int i=0; i<OUTLETS; ++i) if(d.outlets[i].seen) out[n++] = {CARD_OUTLET, uint8_t(i)};
  for(int i=0; i<3; ++i) if(d.energy[i].seen) out[n++] = {CARD_METER, uint8_t(i)};
  if(d.entrance.seen) out[n++] = {CARD_ENTRANCE, 0};
  return n;
}
bool controlVisible() { ControlCard c[MAX_CARDS]; return controlCards(c) > 0; }
// Sidebar entries: the control tab is listed only while some device answers.
int navPages(int* out) {
  int n = 0;
  for(int i=0; i<PAGE_COUNT; ++i) if(i != CONTROL || controlVisible()) out[n++] = i;
  return n;
}
uint32_t deviceSignature() {
  const WallpadSnapshot& d = devices();
  uint32_t sig = 7;
  auto mix = [&](uint32_t v) { sig = sig*131 + v; };
  mix(d.gas.seen); mix(d.gas.closedAt ? uint32_t(1 + (nowMs() - d.gas.closedAt) / 60000) : 0);
  mix(wallpad.command.state); mix(wallpad.command.kind); mix(wallpad.command.target); mix(wallpad.command.attempts);
  for(const LightGroup& l : d.lights) { mix(l.seen); mix(l.count); mix(l.bits); }
  for(const OutletState& o : d.outlets) { mix(o.seen); mix(o.mode); mix(o.watts); }
  mix(d.vent.seen); mix(d.vent.on); mix(d.vent.speed); mix(d.vent.automatic);
  for(const EnergyState& e : d.energy) { mix(e.seen); mix(e.value); }
  mix(d.entrance.seen); mix(uint8_t(d.entrance.gasClosed)); mix(uint8_t(d.entrance.away)); mix(uint8_t(d.entrance.lightsAllOff));
  mix(d.intercom.seen); mix(d.intercom.phase);
  mix(d.clock.valid ? d.clock.minute : 99);
  return sig;
}

bool wallpadLive() { return !demo && wallpad.state==WALLPAD_CONNECTED && wallpad.lastPacketAt && millis()-wallpad.lastPacketAt < 5000; }
const char* wallpadStatusText() {
  if(demo) return "체험 모드";
  switch(wallpad.state) {
    case WALLPAD_NOT_CONFIGURED: return "EW11 주소 설정 필요";
    case WALLPAD_NO_ETHERNET: return "이더넷 장치 오류";
    case WALLPAD_NO_CABLE: return "LAN 케이블 연결 필요";
    case WALLPAD_NO_WIFI: return "WiFi 연결 중";
    case WALLPAD_NO_IP: return "네트워크 주소 받는 중";
    case WALLPAD_CONNECTING: return "월패드 연결 중";
    default: return wallpadLive() ? "월패드 연결됨" : "월패드 신호 없음";
  }
}
int heatingCount() { int n=0; for(const RoomView& r : rooms) n += r.known && r.on; return n; }
bool anyRoomKnown() { for(const RoomView& r : rooms) if(r.known) return true; return false; }
void formatDegrees(char* out,size_t n,const RoomView& r,bool setPoint) {
  if(!r.known) snprintf(out, n, "--°"); else snprintf(out, n, "%d°", setPoint ? r.set : r.current);
}

void drawNotice() {   // short message at the bottom (3 s)
  if(!noticeAt) return;
  const int w = textWidth(noticeText, FONT_CAPTION) + 40, x = 224 + (776 - w) / 2;
  shape(x, 540, w, 40, 20, GLASS, 240);
  shape(x, 540, w, 40, 20, WARN, 200, 1.5f);
  text(x + 20, 560 - FONT_CAPTION.height / 2, noticeText, WARN, FONT_CAPTION);
}
void drawFlash() {
  if(flashUntil) shape(flashX-3, flashY-3, flashW+6, flashH+6, flashR+3, ACCENT, 220, 3);
}

// ---------- pages ----------
void drawClockCard(int x,int y,int w,int h) {
  glass(x, y, w, h, 26);
  struct tm now;
  if(localNow(now)) {
    char t[8], date[48];
    snprintf(t, sizeof(t), "%02d:%02d", now.tm_hour, now.tm_min);
    snprintf(date, sizeof(date), "%d월 %d일 %s요일", now.tm_mon + 1, now.tm_mday, WEEKDAYS[now.tm_wday]);
    text(x+24, y+22, now.tm_hour < 12 ? "오전" : "오후", MUTED, FONT_CAPTION);
    text(x+22, y+48, t, TEXT, FONT_DISPLAY);
    text(x+24, y+112, date, TEXT);
  } else {
    text(x+24, y+22, "현재 시각", MUTED, FONT_CAPTION);
    text(x+22, y+48, "--:--", FAINT, FONT_DISPLAY);
    text(x+24, y+112, "인터넷 시간 동기화 중", FAINT);
  }
  if(*skyPlace) {
    icon(ICON_BUILDING, S24, x+22, y+h-42, FAINT);
    text(x+52, y+h-42, skyPlace, FAINT, FONT_CAPTION);
  }
}

void fitText(int x, int y, int w, const char* s, uint16_t c, const Font& f);
const Eclipse* skyEvent();   // eclipse announced now (demo: the replayed one), see skyNow()
const Eclipse* skyEventAt(time_t& t);   // the same, with the snapshot's time
void drawEclipseHalf(const Eclipse& e, int x, int y, int w, int h, time_t now);

void drawWeatherCard(int x,int y,int w,int h) {
  glass(x, y, w, h, 26);
  time_t eventTime = 0;
  if(const Eclipse* e = skyReady() ? skyEventAt(eventTime) : nullptr) {
    // an eclipse this week: the weather in short on the left, the eclipse on the right
    if(weather.valid) {
      drawWeatherIcon(weather.code, weather.isDay, x+62, y+60, S56, twilightNow());
      char t[12]; snprintf(t, sizeof(t), "%d°", int(lroundf(weather.temp)));
      text(x+24, y+98, t, TEXT, FONT_TITLE);
      text(x+24, y+138, conditionText(), TEXT, FONT_CAPTION);
      char hl[32]; snprintf(hl, sizeof(hl), "최고 %d° 최저 %d°", weather.days[todayIndex()].tmax, weather.days[todayIndex()].tmin);
      text(x+24, y+164, hl, MUTED, FONT_CAPTION);
    } else {
      iconCenter(ICON_CLOUD_SUN, S56, x+62, y+60, FAINT);
      text(x+24, y+110, "날씨", TEXT, FONT_CAPTION);
      fitText(x+24, y+136, 128, weatherStatusText(), FAINT, FONT_CAPTION);
    }
    shape(x + 160, y + 24, 1, h - 48, 0, WHITE, 28);
    drawEclipseHalf(*e, x + 176, y, w - 176, h, eventTime);
    return;
  }
  if(!weather.valid) {
    iconCenter(ICON_CLOUD_SUN, S56, x+60, y+66, FAINT);
    text(x+110, y+42, "날씨", TEXT);
    text(x+110, y+72, weatherStatusText(), FAINT, FONT_CAPTION);
    return;
  }
  drawWeatherIcon(weather.code, weather.isDay, x+58, y+60, S80, twilightNow());
  char t[12]; snprintf(t, sizeof(t), "%d°", int(lroundf(weather.temp)));
  text(x+102, y+30, t, TEXT, FONT_DISPLAY);
  const int tw = textWidth(t, FONT_DISPLAY);
  text(x+110+tw, y+34, conditionText(), TEXT);
  char feel[48]; snprintf(feel, sizeof(feel), "체감 %d° · 습도 %u%%", int(lroundf(weather.feels)), weather.humidity);
  text(x+110+tw, y+64, feel, MUTED, FONT_CAPTION);
  // morning / afternoon / evening
  static const char* names[3] = {"아침", "점심", "저녁"};
  static const int ranges[3][2] = {{6, 10}, {11, 15}, {17, 21}};
  const int colW = (w - 32) / 3;
  for(int i=0; i<3; ++i) {
    uint8_t code = 0; int temp = 0;
    const bool known = summarize(ranges[i][0], ranges[i][1], code, temp);
    const int cx = x + 16 + i*colW;
    shape(cx + 4, y+106, colW - 8, h - 120, 16, WHITE, 18);
    centerText(cx + 4, y+112, colW - 8, names[i], MUTED, FONT_CAPTION);
    if(!known) { centerText(cx + 4, y+140, colW - 8, "--", FAINT); continue; }
    char tt[8]; snprintf(tt, sizeof(tt), "%d°", temp);
    const int rowW = 32 + 6 + textWidth(tt, FONT_BODY), rx = cx + 4 + (colW - 8 - rowW) / 2;
    drawWeatherIcon(code, i < 2, rx + 16, y+152, S32);
    text(rx + 38, y+140, tt, TEXT);
  }
}

void drawHome() {
  drawClockCard(224, 126, 300, 206);
  drawWeatherCard(540, 126, 460, 206);

  // Boiler (from the wallpad bus)
  glass(224, 348, 384, 184, 26);
  const int heatingRooms = heatingCount();
  iconChip(248, 368, ICON_FLAME, heatingRooms ? HEAT : WHITE, heatingRooms ? 256 : 30, heatingRooms ? ON_HEAT : TEXT);
  text(316, 368, "보일러", TEXT);
  char summary[40];
  if(!anyRoomKnown()) snprintf(summary, sizeof(summary), "%s", wallpadStatusText());
  else if(heatingRooms) snprintf(summary, sizeof(summary), "%d개 방 난방 중", heatingRooms);
  else snprintf(summary, sizeof(summary), "모두 꺼짐");
  text(316, 396, summary, heatingRooms ? HEAT : (anyRoomKnown() ? MUTED : FAINT), FONT_CAPTION);
  icon(ICON_CHEVRON, S24, 572, 378, FAINT);
  for(int i=0; i<ROOM_COUNT; ++i) {  // one small column per room
    const int cx = 248 + i*84;
    const RoomView& r = rooms[i];
    char t[12]; formatDegrees(t, sizeof(t), r, false);
    text(cx, 444, roomNames[i], MUTED, FONT_CAPTION);
    text(cx, 470, t, r.known ? (r.on ? HEAT : TEXT) : FAINT, FONT_TITLE);
  }

  // Door calls
  glass(624, 348, 376, 184, 26);
  icon(ICON_BELL, S24, 648, 368, MUTED);
  text(682, 366, "방문 호출", TEXT);
  icon(ICON_CHEVRON, S24, 952, 368, FAINT);
  for(int i=0; i<3; ++i) {
    const int y = 410 + i*38;
    const bool fresh = recentCall(lastRingAt[i], 10*60000);
    icon(doorIcons[i], S24, 648, y, fresh ? WARN : MUTED);
    text(682, y-2, doorNames[i], TEXT, FONT_CAPTION);
    char ago[24]; formatAgo(ago, sizeof(ago), lastRingAt[i]);
    rightText(976, y-2, ago, fresh ? WARN : FAINT, FONT_CAPTION);
  }
  text(224, 554, demo ? "체험 모드: 실제 기기와 연결되지 않은 화면입니다"
                       : (!weather.valid ? "" : (*weatherError() && millis() - weather.fetchedAt > 3UL * 3600 * 1000
                                                ? "날씨 갱신 실패 · 표시 중인 날씨는 3시간 이상 지난 정보입니다"
                                                : "날씨: Open-Meteo · 위치는 설정 탭에서 바꿀 수 있습니다")),
       demo ? WARN : FAINT, FONT_CAPTION);
}

// ---------- weather tab ----------
// ---- sun, moon and eclipses (astro.cpp) ----
Eclipse eclipses[8];
int eclipseCount = 0;
uint32_t eclipseSignature = 0;
// Demo: replays two real eclipses as seen from here, 12 s each.
constexpr time_t DEMO_LUNAR_PEAK = 1772537760;   // 2026-03-03 total lunar eclipse (UTC)
constexpr time_t DEMO_SOLAR_PEAK = 2072306905;   // 2035-09-02 solar eclipse, deep partial in central Korea (UTC)
time_t demoSkyTime(const char*& name) {
  const int ms = int(millis() % 24000);
  if(ms < 12000) { name = "개기월식"; return DEMO_LUNAR_PEAK + time_t(ms - 6000) * (170 * 60) / 6000; }
  name = "부분일식"; return DEMO_SOLAR_PEAK + time_t(ms - 18000) * (80 * 60) / 6000;
}
const char* eclipseName(const Eclipse& e) {
  if(e.solar) return e.kind == ECLIPSE_TOTAL ? "개기일식" : (e.kind == ECLIPSE_ANNULAR ? "금환일식" : "부분일식");
  return e.kind == ECLIPSE_TOTAL ? "개기월식" : (e.kind == ECLIPSE_PARTIAL ? "부분월식" : "반영월식");
}
// Next eclipse that can be seen from here (lunar: moon above the horizon at maximum).
const Eclipse* nextEclipse(time_t now) {
  for(int i=0; i<eclipseCount; ++i)
    if(eclipses[i].end > now && (eclipses[i].solar || eclipses[i].visible)) return &eclipses[i];
  return nullptr;
}
constexpr time_t COUNTDOWN_S = 7 * 86400;    // countdown shown from a week before the start
// The eclipse to announce now: from a week before its start until its end.
const Eclipse* activeEclipse(time_t now) {
  const Eclipse* e = nextEclipse(now);
  return e && e->start - now <= COUNTDOWN_S ? e : nullptr;
}
void formatWhen(char* out, size_t n, time_t utc, time_t now) {   // "20:40" today, else "3/3 20:40"
  struct tm a, b; localtime_r(&utc, &a); localtime_r(&now, &b);
  if(a.tm_yday == b.tm_yday && a.tm_year == b.tm_year) snprintf(out, n, "%02d:%02d", a.tm_hour, a.tm_min);
  else snprintf(out, n, "%d월 %d일 %02d:%02d", a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min);
}
void formatCountdown(char* out, size_t n, long left) {
  const long d = left / 86400, h = left / 3600 % 24, m = left / 60 % 60, s = left % 60;
  if(d) snprintf(out, n, "%ld일 %02ld:%02ld:%02ld", d, h, m, s); else snprintf(out, n, "%02ld:%02ld:%02ld", h, m, s);
}
// Home card, right part: name, countdown or progress, its shape (live, or at maximum
// before it starts), contact times and magnitude.
void drawEclipseHalf(const Eclipse& e, int x, int y, int w, int h, time_t now) {
  const bool running = now >= e.start;
  text(x, y + 18, eclipseName(e), WARN);
  char line[48];
  if(running) snprintf(line, sizeof(line), "진행 중");
  else { char cd[24]; formatCountdown(cd, sizeof(cd), long(e.start - now)); snprintf(line, sizeof(line), "시작까지 %s", cd); }
  text(x, y + 48, line, running ? WARN : MUTED, FONT_CAPTION);
  const time_t t = running ? now : e.peak;
  const float lat = skyLat, lon = skyLon;
  const int bx = x + 40, by = y + 128, br = 32;
  if(e.solar) {
    SunCover cover;
    const bool on = solarCover(t, lat, lon, cover.dx, cover.dy, cover.moonR);
    if(!on) disc(bx, by, br + 10, WX_SUN, 36);
    sunDisc(bx, by, br, WX_SUN, on ? &cover : nullptr);
  } else {
    MoonShadow sh;
    const bool on = lunarShadow(t, sh.dx, sh.dy, sh.umbraR, sh.penumbraR);
    disc(bx, by, br + 9, WX_MOON, 16);
    moonDisc(bx, by, br, moonAge(t), WX_MOON, WX_MOON_DARK, 200, on ? &sh : nullptr);
  }
  centerText(x, y + 170, 80, running ? "지금" : "최대 때", FAINT, FONT_CAPTION);
  const int tx = x + 92;
  const char* labels[3] = {"시작", "최대", "종료"};
  const time_t times[3] = {e.start, e.peak, e.end};
  for(int i=0; i<3; ++i) {
    char when[32]; formatWhen(when, sizeof(when), times[i], now);
    text(tx, y + 84 + i * 26, labels[i], MUTED, FONT_CAPTION);
    text(tx + 40, y + 84 + i * 26, when, TEXT, FONT_CAPTION);
  }
  snprintf(line, sizeof(line), "식분 %.2f", e.magnitude);
  text(tx, y + 162, line, MUTED, FONT_CAPTION);
}
// Demo: the two replayed eclipses, computed once (contact times included).
const Eclipse* demoEvent(time_t t) {
  static Eclipse events[2];
  static bool ok[2];
  static float atLat = 1e9f, atLon = 1e9f;
  if(atLat != skyLat || atLon != skyLon) {
    atLat = skyLat; atLon = skyLon;
    ok[0] = eclipseNear(DEMO_LUNAR_PEAK, false, atLat, atLon, events[0]);
    ok[1] = eclipseNear(DEMO_SOLAR_PEAK, true, atLat, atLon, events[1]);
  }
  const int i = t < (DEMO_LUNAR_PEAK + DEMO_SOLAR_PEAK) / 2 ? 0 : 1;
  return ok[i] ? &events[i] : nullptr;
}
// The sky right now (demo: the replay time), computed once per second and shared by the
// home card and the weather page.
struct SkyNow {
  time_t t = 0; bool demo = false; float lat = 0, lon = 0;
  const Eclipse* event = nullptr;
  float sunAz = 0, sunAlt = 0, moonAz = 0, moonAlt = 0, age = 0, illum = 0;
  bool solarOn = false, lunarOn = false;
  SunCover cover = {}; MoonShadow shadow = {};
  float planetAz[PLANETS] = {}, planetAlt[PLANETS] = {};
};
const SkyNow& skyNow() {
  static SkyNow s;
  const char* name;
  const time_t t = demo ? demoSkyTime(name) : time(nullptr);
  if(t == s.t && demo == s.demo && s.lat == skyLat && s.lon == skyLon) return s;
  s.t = t; s.demo = demo; s.lat = skyLat; s.lon = skyLon;
  sunAzAlt(t, s.lat, s.lon, s.sunAz, s.sunAlt);
  moonAzAlt(t, s.lat, s.lon, s.moonAz, s.moonAlt);
  s.age = moonAge(t); s.illum = moonIllumination(t);
  s.solarOn = solarCover(t, s.lat, s.lon, s.cover.dx, s.cover.dy, s.cover.moonR) && s.sunAlt > -1;
  s.lunarOn = lunarShadow(t, s.shadow.dx, s.shadow.dy, s.shadow.umbraR, s.shadow.penumbraR);
  s.event = demo ? demoEvent(t) : activeEclipse(t);
  planetsAzAlt(t, s.lat, s.lon, s.planetAz, s.planetAlt);
  return s;
}
const Eclipse* skyEvent() { return skyNow().event; }
const Eclipse* skyEventAt(time_t& t) { const SkyNow& s = skyNow(); t = s.t; return s.event; }

time_t localMidnight(time_t utc) {
  struct tm t; localtime_r(&utc, &t);
  return utc - (t.tm_hour*3600 + t.tm_min*60 + t.tm_sec);
}
void formatClock(char* out, size_t n, time_t utc) {
  if(!utc) { snprintf(out, n, "--:--"); return; }
  struct tm t; localtime_r(&utc, &t);
  snprintf(out, n, "%02d:%02d", t.tm_hour, t.tm_min);
}
// Rise and set times; `ref` is the shown time: a time on another day gets "+1" / "-1".
// The row is moved left when the marks would pass `right`.
void riseSetRow(int x, int y, time_t rise, time_t set, uint16_t riseC, uint16_t setC, time_t ref = 0, int right = SCREEN_W) {
  char tr[8], ts[8], mr[8] = "", ms[8] = "";
  formatClock(tr, sizeof(tr), rise); formatClock(ts, sizeof(ts), set);
  auto mark = [&](time_t when, char* out) {
    if(!when || !ref) return;
    const long days = long((localMidnight(when) - localMidnight(ref)) / 86400);
    if(days) snprintf(out, 8, "%+ld", days);
  };
  mark(rise, mr); mark(set, ms);
  auto width = [&](const char* t, const char* m) { return 26 + textWidth(t, FONT_CAPTION) + (*m ? 2 + textWidth(m, FONT_CAPTION) : 0); };
  const int w1 = width(tr, mr), gap = 8;
  const int second = std::max(78, w1 + gap);
  const int total = second + width(ts, ms);
  if(x + total > right) x = right - total;
  auto one = [&](int ix, IconId id, uint16_t c, const char* t, const char* m) {
    icon(id, S24, ix, y, c);
    text(ix + 26, y, t, TEXT, FONT_CAPTION);
    if(*m) text(ix + 26 + textWidth(t, FONT_CAPTION) + 2, y - 4, m, FAINT, FONT_CAPTION);
  };
  one(x, ICON_SUNRISE, riseC, tr, mr);
  one(x + second, ICON_SUNSET, setC, ts, ms);
}

// The pass a body is on at time t (else its next one). For the current time it comes from the
// weather task (computed on core 0); for demo times it is computed here once and kept.
const SkyPass& passFor(bool moon, time_t t, float lat, float lon) {
  static SkyPass shared[2];
  static float sharedLat = 0, sharedLon = 0;
  static uint32_t polledAt = 0;
  static SkyPass kept[2][4];                    // demo: a few passes per body
  static float keptLat[2][4], keptLon[2][4];
  static int next[2] = {0, 0};
  auto fits = [&](const SkyPass& p, float la, float lo, time_t slack) {
    return p.valid && la == lat && lo == lon && p.from <= t && t <= p.set + slack;
  };
  if(!demo) {
    if(millis() - polledAt > 1000 || !polledAt) { polledAt = millis(); weatherPasses(shared[0], shared[1], sharedLat, sharedLon); }
    // just after the set the task is about to publish the next pass: keep the old one meanwhile
    if(fits(shared[moon], sharedLat, sharedLon, 180)) return shared[moon];
  }
  for(int i = 0; i < 4; ++i) if(fits(kept[moon][i], keptLat[moon][i], keptLon[moon][i], 0)) return kept[moon][i];
  const int i = next[moon]; next[moon] = (i + 1) % 4;
  computePass(moon, t, lat, lon, kept[moon][i]);   // demo times, or before the task has one
  keptLat[moon][i] = lat; keptLon[moon][i] = lon;
  return kept[moon][i];
}

// Fraction of a unit disc covered by a disc of radius r at distance d (circle overlap area).
float covered(float r, float d) {
  if(d >= 1 + r) return 0;
  if(d <= fabsf(r - 1)) return r < 1 ? r * r : 1;
  const float a = acosf((d*d + 1 - r*r) / (2*d)), b = acosf((d*d + r*r - 1) / (2*d*r));
  return (a + r*r*b - 0.5f * sqrtf(std::max(0.f, (-d + 1 + r) * (d + 1 - r) * (d - 1 + r) * (d + 1 + r)))) / float(M_PI);
}

void drawSunMoonCard(int x,int y,int w,int h);
void drawSunMoon(int x,int y,int w,int h) {
  const uint32_t t0 = micros();
  drawSunMoonCard(x, y, w, h);
  perfSection[5] = micros() - t0;
}
void drawSunMoonCard(int x,int y,int w,int h) {
  glass(x, y, w, h, 26);
  text(x+24, y+18, "해와 달", TEXT);
  const SkyNow& sky = skyNow();
  const float lat = skyLat, lon = skyLon;
  const time_t t = sky.t;
  char badge[64] = "";
  const char* tag = demo ? "체험 · " : "";
  if(const Eclipse* e = sky.event) {
    if(t >= e->start) snprintf(badge, sizeof(badge), "%s%s 진행 중", tag, eclipseName(*e));
    else { char cd[24]; formatCountdown(cd, sizeof(cd), long(e->start - t)); snprintf(badge, sizeof(badge), "%s%s까지 %s", tag, eclipseName(*e), cd); }
  } else if(demo) { char clk[8]; formatClock(clk, sizeof(clk), t); snprintf(badge, sizeof(badge), "체험 · %s", clk); }
  if(*badge) rightText(x + w - 20, y + 22, badge, WARN, FONT_CAPTION);
  const int half = w / 2;
  shape(x + half, y + 58, 1, h - 76, 0, WHITE, 28);

  // current state of both bodies, eclipses included
  const float sunAz = sky.sunAz, sunAlt = sky.sunAlt, moonAz = sky.moonAz, moonAlt = sky.moonAlt;
  const float age = sky.age, illum = sky.illum;
  const SunCover cover = sky.cover; const bool solar = sky.solarOn;
  const MoonShadow sh = sky.shadow; const bool lunar = sky.lunarOn;

  // left: the sky. Paths of today as dotted arcs (east left, west right), bodies where they are now
  const int hx0 = x + 34, hx1 = x + half - 28, horizon = y + 118, top = y + 56;
  auto skyX = [&](float az) { return hx0 + (hx1 - hx0) * (std::max(45.f, std::min(315.f, az)) - 45.f) / 270.f; };
  auto skyY = [&](float alt) { return horizon - (horizon - top) * std::min(alt, 80.f) / 80.f; };
  span(hx0 - 6, horizon, hx1 - hx0 + 12, FAINT, 120);
  text(x + 14, horizon - FONT_CAPTION.height / 2, "동", FAINT, FONT_CAPTION);
  text(hx1 + 8, horizon - FONT_CAPTION.height / 2, "서", FAINT, FONT_CAPTION);
  // One whole pass per body (from its rise to its set): the pass it is on now, else the next one.
  // The times under the chart are that pass's rise and set ("+1" when on the next day).
  const SkyPass& sunP = passFor(false, localMidnight(t) + 12 * 3600, lat, lon);   // today's sun
  const SkyPass& moonP = passFor(true, t, lat, lon);
  auto drawPass = [&](const SkyPass& p, uint16_t c) {
    if(!p.valid) return;
    for(int i = 0; i < PASS_POINTS; ++i) if(p.alt[i] >= 0) plot(int(skyX(p.az[i])), int(skyY(p.alt[i])), c, 150);
  };
  drawPass(sunP, WARN);
  drawPass(moonP, MUTED);
  const time_t sunRise = sunP.valid ? sunP.rise : 0, sunSet = sunP.valid ? sunP.set : 0;
  const time_t moonRise = moonP.valid ? moonP.rise : 0, moonSet = moonP.valid ? moonP.set : 0;
  // planets above the horizon, faded in and out with the darkness of the sky: the sun's depth
  // below the horizon (0 at the horizon .. 1 at -12 degrees) or, during a solar eclipse, how
  // much of the sun is hidden (0 at half .. 1 at all). Brighter planets show up earlier.
  {
    auto ramp = [](float a, float b, float v) { float t = (v - a) / (b - a); t = t < 0 ? 0 : (t > 1 ? 1 : t); return t * t * (3 - 2 * t); };
    const float hidden = solar ? covered(cover.moonR, hypotf(cover.dx, cover.dy)) : 0.f;
    const float darkness = std::max(ramp(0, 12, -sunAlt), sunAlt > -1 ? ramp(0.5f, 1.f, hidden) : 0.f);
    static const char* const NAMES[PLANETS] = {"수", "금", "화", "목", "토"};
    static const uint16_t COLORS[PLANETS] = {rgb(0xB8B8B8), rgb(0xFFF4C8), rgb(0xFF8A5C), rgb(0xF2D9B0), rgb(0xE8C77A)};
    static const float BRIGHT[PLANETS] = {0.35f, 1.0f, 0.5f, 0.8f, 0.4f};   // Mercury .. Saturn, typical
    static const int ORDER[PLANETS] = {1, 3, 2, 4, 0};   // Venus, Jupiter, Mars, Saturn, Mercury
    int labelX[PLANETS], labelY[PLANETS], labels = 0;
    for(int k=0; k<PLANETS; ++k) {
      const int i = ORDER[k];
      if(sky.planetAlt[i] < 0) continue;
      const float start = 0.45f - 0.35f * BRIGHT[i];                        // Venus first, faint ones later
      const unsigned alpha = unsigned(256 * ramp(start, start + 0.3f, darkness));
      if(!alpha) continue;
      const float px = skyX(sky.planetAz[i]), py = skyY(sky.planetAlt[i]);
      // hidden behind the sun or the moon as drawn on the chart (disc + 3 px; the sun's glow too)
      auto behind = [&](float az, float alt, float r) { return alt >= 0 && hypotf(px - skyX(az), py - skyY(alt)) < r + 3; };
      if(behind(sunAz, sunAlt, solar ? 7.f : 11.f) || behind(moonAz, moonAlt, 7.f)) continue;
      disc(px, py, i == 1 || i == 3 ? 3.2f : 2.5f, COLORS[i], alpha);
      const int lx = int(px) + 4, ly = int(py) - FONT_CAPTION.height + 4;
      bool clear = true;
      for(int j=0; j<labels; ++j) if(abs(lx - labelX[j]) < 22 && abs(ly - labelY[j]) < 20) clear = false;
      if(!clear) continue;   // two planets close together: one label
      labelX[labels] = lx; labelY[labels] = ly; ++labels;
      text(lx, ly, NAMES[i], COLORS[i], FONT_CAPTION, alpha);   // label fades too
    }
  }
  if(moonAlt >= 0) moonDisc(skyX(moonAz), skyY(moonAlt), 7, age, WX_MOON, WX_MOON_DARK, 200, lunar ? &sh : nullptr);
  if(sunAlt >= 0) {
    if(!solar) disc(skyX(sunAz), skyY(sunAlt), 11, WX_SUN, 40);
    sunDisc(skyX(sunAz), skyY(sunAlt), 7, WX_SUN, solar ? &cover : nullptr);
  }
  char line[48];
  snprintf(line, sizeof(line), "해 %d° · 달 %d°", int(lroundf(sunAlt)), int(lroundf(moonAlt)));
  centerText(x + 8, horizon + 8, half - 16, line, MUTED, FONT_CAPTION);
  riseSetRow(x + 22, y + h - 32, sunRise, sunSet, WARN, HEAT, t, x + half - 6);

  // right: one big body. An eclipse in progress first, then the sun while it is up, else the moon
  const bool showSun = solar || (!(lunar && moonAlt > -1) && sunAlt >= 0);
  const int bx = x + half + 50, by = y + 96, br = 32;
  const int tx = bx + br + 16;
  if(showSun) {
    if(!solar) disc(bx, by, br + 10, WX_SUN, 36);       // soft glow (not while eclipsed)
    sunDisc(bx, by, br, WX_SUN, solar ? &cover : nullptr);
    text(tx, y + 62, "태양", TEXT, FONT_CAPTION);
    if(solar) {
      const float hidden = covered(cover.moonR, hypotf(cover.dx, cover.dy));   // share of the sun's area
      snprintf(line, sizeof(line), "가림 %d%%", int(lroundf(hidden * 100)));
      text(tx, y + 88, line, WARN, FONT_CAPTION);
    }
    snprintf(line, sizeof(line), "고도 %d°", int(lroundf(sunAlt)));
    text(tx, y + 114, line, MUTED, FONT_CAPTION);
  } else {
    disc(bx, by, br + 9, WX_MOON, 16);
    moonDisc(bx, by, br, age, WX_MOON, WX_MOON_DARK, 200, lunar ? &sh : nullptr);
    text(tx, y + 62, lunar ? "월식" : moonPhaseName(age), lunar ? WARN : TEXT, FONT_CAPTION);
    // phase brightness is always 100% at a lunar eclipse: show the share inside the umbra instead
    if(lunar) {
      const float inUmbra = covered(sh.umbraR, hypotf(sh.dx, sh.dy));
      if(inUmbra > 0.005f) snprintf(line, sizeof(line), "가림 %d%%", int(lroundf(inUmbra * 100)));
      else snprintf(line, sizeof(line), "반영 속");
    } else snprintf(line, sizeof(line), "밝기 %d%%", int(lroundf(illum * 100)));
    text(tx, y + 88, line, lunar ? WARN : MUTED, FONT_CAPTION);
    snprintf(line, sizeof(line), "고도 %d°", int(lroundf(moonAlt)));   // below the horizon: negative, dimmed
    text(tx, y + 114, line, moonAlt >= 0 ? MUTED : FAINT, FONT_CAPTION);
  }
  riseSetRow(x + half + 14, y + h - 32, moonRise, moonSet, MUTED, FAINT, t, x + w - 10);
}

void drawWeather() {
  if(!weather.valid) {   // no forecast (yet): the sky does not depend on it
    glass(224, 126, 380, 196, 26);
    iconCenter(ICON_CLOUD_SUN, S56, 414, 200, FAINT);
    centerText(224, 248, 380, "날씨 정보 없음", MUTED);
    if(skyReady()) drawSunMoon(618, 126, 382, 196);
    else { glass(618, 126, 382, 196, 26); centerText(618, 210, 382, "시간·위치 확인 중", FAINT); }
    glass(224, 336, 776, 204, 22);
    centerText(224, 424, 776, weatherStatusText(), MUTED);
    return;
  }
  struct tm now;
  const bool timeOk = localNow(now);
  // current conditions
  glass(224, 126, 380, 196, 26);
  drawWeatherIcon(weather.code, weather.isDay, 284, 188, S80, twilightNow());
  char t[16]; snprintf(t, sizeof(t), "%d°", int(lroundf(weather.temp)));
  text(332, 146, t, TEXT, FONT_DISPLAY);
  text(332, 200, conditionText(), TEXT);
  char line[64];
  const int today0 = todayIndex();
  const WeatherDay& today = weather.days[today0];
  snprintf(line, sizeof(line), "최고 %d° · 최저 %d° · 체감 %d°", today.tmax, today.tmin, int(lroundf(weather.feels)));
  text(248, 248, line, MUTED, FONT_CAPTION);
  icon(ICON_DROP, S24, 248, 280, MUTED);
  snprintf(line, sizeof(line), "%u%%", weather.humidity); text(276, 280, line, TEXT, FONT_CAPTION);
  icon(ICON_WIND, S24, 344, 280, MUTED);
  snprintf(line, sizeof(line), "%.1fm/s", weather.wind); text(372, 280, line, TEXT, FONT_CAPTION);
  icon(ICON_SUN, S24, 470, 280, MUTED);
  if(today.uv == 255) snprintf(line, sizeof(line), "UV --"); else snprintf(line, sizeof(line), "UV %u", today.uv);
  text(498, 280, line, TEXT, FONT_CAPTION);

  if(skyReady()) drawSunMoon(618, 126, 382, 196);
  else { glass(618, 126, 382, 196, 26); centerText(618, 210, 382, "시간·위치 확인 중", FAINT); }

  // hourly: next 12 hours
  glass(224, 336, 776, 98, 22);
  const int startHour = today0 * 24 + (timeOk ? now.tm_hour : 0);
  for(int i=0; i<12; ++i) {
    const int hIndex = startHour + i;
    if(hIndex >= WX_HOURS) break;
    const WeatherHour& wh = weather.hours[hIndex];
    const int cx = 236 + i*63;
    char hh[8];
    if(i == 0) snprintf(hh, sizeof(hh), "지금"); else snprintf(hh, sizeof(hh), "%d시", hIndex % 24);
    centerText(cx, 344, 56, hh, i ? MUTED : TEXT, FONT_CAPTION);
    drawWeatherIcon(wh.code, isDayHour(hIndex % 24), cx + 28, 384, S32, i ? twilightAt((hIndex % 24)*60 + 30) : twilightNow());
    char tt[8]; snprintf(tt, sizeof(tt), "%d°", wh.temp);
    centerText(cx, 400, 56, tt, TEXT, FONT_BODY);
  }

  // weekly
  glass(224, 446, 776, 94, 22);
  for(int i=0; i + today0 < WX_DAYS; ++i) {
    const WeatherDay& d = weather.days[i + today0];
    const int cx = 232 + i*110;
    char dn[16];
    if(i == 0) snprintf(dn, sizeof(dn), "오늘"); else snprintf(dn, sizeof(dn), "%s %d", WEEKDAYS[d.weekday], d.day);
    centerText(cx, 452, 100, dn, i ? (d.weekday == 0 ? DANGER : MUTED) : TEXT, FONT_CAPTION);
    drawWeatherIcon(d.code, true, cx + 28, 492, S32);
    char tt[8];
    snprintf(tt, sizeof(tt), "%d°", d.tmax); text(cx + 54, 476, tt, TEXT, FONT_CAPTION);
    snprintf(tt, sizeof(tt), "%d°", d.tmin); text(cx + 54, 500, tt, MUTED, FONT_CAPTION);
    if(d.pop >= 30) { char pp[8]; snprintf(pp, sizeof(pp), "%u%%", d.pop); centerText(cx + 4, 512, 48, pp, rgb(0x7DD3FC), FONT_CAPTION); }
  }
  const uint32_t age = (millis() - weather.fetchedAt) / 60000;
  char ago[24];
  if(age < 60) snprintf(ago, sizeof(ago), "%lu분 전", (unsigned long)age);
  else if(age < 48 * 60) snprintf(ago, sizeof(ago), "%lu시간 전", (unsigned long)(age / 60));
  else snprintf(ago, sizeof(ago), "%lu일 전", (unsigned long)(age / 1440));
  snprintf(line, sizeof(line), "%s · Open-Meteo · %s 갱신%s", weather.place, ago, *weatherError() ? " · 갱신 실패" : "");
  text(224, 554, line, FAINT, FONT_CAPTION);
}

// ---------- location picker (map) ----------
int pickerMissing = 0;   // tiles not drawn yet (retried every 10 s by a redraw)
void pickerNormalize();
struct Picker { bool open = false, moved = false, waitIp = false; int zoom = 13; double cx = 0, cy = 0; uint32_t gen = 0; } picker;
// Keep the map centre inside the world: wrap east-west, clamp north-south (Mercator limit).
void pickerNormalize() {
  const double world = ldexp(256.0, picker.zoom);
  picker.cx = fmod(fmod(picker.cx, world) + world, world);
  picker.cy = std::min(world - 1, std::max(0.0, picker.cy));
}
void pickerCenter(double lat, double lon) {
  lat = std::min(85.0, std::max(-85.0, lat));   // log(tan) is infinite at the poles
  mapWorldPixel(lat, lon, picker.zoom, picker.cx, picker.cy);
  pickerNormalize();
}
void openPicker() {
  pickerTouchReset = true;   // the tap that opened the picker is still on the glass
  float la, lo; char name[8];
  weatherLocation(la, lo, name, sizeof(name));
  picker = Picker();
  picker.open = true; picker.waitIp = true;   // starts at the IP estimate once it arrives
  if(la == 0 && lo == 0) { la = 37.5665f; lo = 126.978f; picker.zoom = 11; }   // nothing known: Seoul
  pickerCenter(la, lo);
  weatherRequestIpLocation();
}
void pickerZoom(int d) {
  const int z = std::min(MAP_MAX_ZOOM, std::max(MAP_MIN_ZOOM, picker.zoom + d));
  if(z == picker.zoom) return;
  const double k = ldexp(1.0, z - picker.zoom);
  picker.cx *= k; picker.cy *= k; picker.zoom = z; pickerNormalize();
}
void drawSearch();
bool searchOpen();
void drawPicker() {
  picker.gen = mapGeneration();
  pickerMissing = mapDraw(frame, 0, 0, SCREEN_W, SCREEN_H, picker.cx, picker.cy, picker.zoom);
  // pin at the map centre
  circle(512, 303, 15, BLACK, 70);
  circle(512, 300, 15, WHITE);
  circle(512, 300, 11, ACCENT);
  circle(512, 300, 4, WHITE);
  if(searchOpen()) { drawSearch(); return; }
  glass(12, 12, 1000, 84, 24, 225);
  button(24, 26, 120, 56, "취소", WHITE, 34, TEXT);
  centerText(156, 22, 588, "위치 설정", TEXT, FONT_TITLE);
  double lat, lon;
  mapLatLon(picker.cx, picker.cy, picker.zoom, lat, lon);
  char line[72];
  snprintf(line, sizeof(line), "끌어서 이동 · 두 손가락으로 확대 · %.4f, %.4f", lat, lon);
  centerText(156, 62, 588, line, MUTED, FONT_CAPTION);
  button(756, 26, 112, 56, "검색", WHITE, 34, TEXT, ICON_SEARCH);
  button(880, 26, 120, 56, "OK", ACCENT, 256, ON_ACCENT, ICON_CHECK);
  for(int i=0; i<2; ++i) {
    const int cy = 452 + i*72;
    circle(968, cy, 30, GLASS, 215);
    shape(938, cy - 30, 60, 60, 30, WHITE, 40, 1.2f);
    iconCenter(i ? ICON_MINUS : ICON_PLUS, S32, 968, cy, TEXT);
  }
  float il, io;
  const bool ipReady = weatherIpLocation(il, io);
  button(24, 520, 160, 56, ipReady ? "IP 위치" : (weatherIpFailed() ? "IP 확인 실패" : "IP 확인 중"), GLASS, 215, ipReady ? TEXT : FAINT);
  button(196, 520, 160, 56, "현재 설정", GLASS, 215, TEXT);
  const char* err = mapError();
  snprintf(line, sizeof(line), "%s%s지도 © Esri", err, *err ? " · " : "");
  const int lw = textWidth(line, FONT_CAPTION);
  shape(1010 - lw - 16, 574, lw + 16, 24, 8, GLASS, 190);
  text(1010 - lw - 8, 576, line, *err ? WARN : MUTED, FONT_CAPTION);
}
// ---------- on-screen keyboard (location search) ----------
// Two-set Korean (두벌식) and QWERTY layouts with a fixed digit row. Keystrokes are kept
// as jamo / ASCII code points and composed into syllables on every change, so
// backspace removes one jamo at a time like a phone keyboard.
namespace hangul {
const char16_t CHO[] = u"ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ";
const char16_t JUNG[] = u"ㅏㅐㅑㅒㅓㅔㅕㅖㅗㅘㅙㅚㅛㅜㅝㅞㅟㅠㅡㅢㅣ";
const char16_t JONG[] = u" ㄱㄲㄳㄴㄵㄶㄷㄹㄺㄻㄼㄽㄾㄿㅀㅁㅂㅄㅅㅆㅇㅈㅊㅋㅌㅍㅎ";   // index 0 = none
struct Pair { char16_t a, b, ab; };
const Pair VOWELS[] = {{u'ㅗ',u'ㅏ',u'ㅘ'},{u'ㅗ',u'ㅐ',u'ㅙ'},{u'ㅗ',u'ㅣ',u'ㅚ'},{u'ㅜ',u'ㅓ',u'ㅝ'},{u'ㅜ',u'ㅔ',u'ㅞ'},{u'ㅜ',u'ㅣ',u'ㅟ'},{u'ㅡ',u'ㅣ',u'ㅢ'}};
const Pair FINALS[] = {{u'ㄱ',u'ㅅ',u'ㄳ'},{u'ㄴ',u'ㅈ',u'ㄵ'},{u'ㄴ',u'ㅎ',u'ㄶ'},{u'ㄹ',u'ㄱ',u'ㄺ'},{u'ㄹ',u'ㅁ',u'ㄻ'},{u'ㄹ',u'ㅂ',u'ㄼ'},
                       {u'ㄹ',u'ㅅ',u'ㄽ'},{u'ㄹ',u'ㅌ',u'ㄾ'},{u'ㄹ',u'ㅍ',u'ㄿ'},{u'ㄹ',u'ㅎ',u'ㅀ'},{u'ㅂ',u'ㅅ',u'ㅄ'}};
int indexOf(const char16_t* table, uint32_t c, int from = 0) {
  for(int i=from; table[i]; ++i) if(table[i] == c) return i;
  return -1;
}
uint32_t combine(const Pair* pairs, int n, uint32_t a, uint32_t b) {
  for(int i=0; i<n; ++i) if(pairs[i].a == a && pairs[i].b == b) return pairs[i].ab;
  return 0;
}
const Pair* split(uint32_t ab) {
  for(const Pair& p : FINALS) if(p.ab == ab) return &p;
  return nullptr;
}
void put(char*& o, char* end, uint32_t c) {
  if(c < 0x80) { if(o + 1 < end) *o++ = char(c); }
  else if(c < 0x800) { if(o + 2 < end) { *o++ = char(0xC0 | (c >> 6)); *o++ = char(0x80 | (c & 63)); } }
  else if(o + 3 < end) { *o++ = char(0xE0 | (c >> 12)); *o++ = char(0x80 | ((c >> 6) & 63)); *o++ = char(0x80 | (c & 63)); }
}
// Keystrokes -> UTF-8 text.
void compose(const uint32_t* keys, int n, char* out, size_t size) {
  char* o = out; char* end = out + size;
  int cho = -1, jung = -1, jong = 0;
  auto flush = [&]() {
    if(cho >= 0 && jung >= 0) put(o, end, 0xAC00 + (cho * 21 + jung) * 28 + jong);
    else if(cho >= 0) put(o, end, CHO[cho]);
    else if(jung >= 0) put(o, end, JUNG[jung]);
    cho = -1; jung = -1; jong = 0;
  };
  for(int i=0; i<n; ++i) {
    const uint32_t k = keys[i];
    const int c = indexOf(CHO, k), v = indexOf(JUNG, k);
    if(c >= 0) {                                     // consonant
      if(cho >= 0 && jung >= 0) {
        if(!jong) { const int j = indexOf(JONG, k, 1); if(j > 0) { jong = j; continue; } }
        else if(const uint32_t ab = combine(FINALS, 11, JONG[jong], k)) { jong = indexOf(JONG, ab, 1); continue; }
      }
      flush(); cho = c;
    } else if(v >= 0) {                              // vowel
      if(jong) {                                     // the final consonant moves to the next syllable
        uint32_t next = JONG[jong];
        if(const Pair* p = split(next)) { jong = indexOf(JONG, p->a, 1); next = p->b; } else jong = 0;
        flush(); cho = indexOf(CHO, next); jung = v;
      } else if(jung >= 0) {
        if(const uint32_t ab = combine(VOWELS, 7, JUNG[jung], k)) jung = indexOf(JUNG, ab);
        else { flush(); jung = v; }
      } else jung = v;
    } else { flush(); put(o, end, k); }
  }
  flush();
  *o = 0;
}
}

enum KeyCode : uint32_t { KEY_SHIFT = 1, KEY_BACK = 2, KEY_LANG = 3, KEY_SEARCH = 4 };
struct Keyboard { bool open = false, hangul = true, shift = false; uint32_t keys[48]; int count = 0; } kb;
bool searchOpen() { return kb.open; }
PlaceResult results[4];
int resultCount = 0;
SearchState searchState = SEARCH_IDLE;
constexpr int KB_TOP = 276, KB_ROW = 62, KB_KEY_H = 54, KB_MARGIN = 12, KB_GAP = 8;
constexpr float KB_UNIT = (SCREEN_W - 2 * KB_MARGIN - 9 * KB_GAP) / 10.f;

// Calls f(label, code, x, y, w, h) for every key of the current layout.
template<class F> void forEachKey(F f) {
  static const char* const KO[2][3] = {{"ㅂㅈㄷㄱㅅㅛㅕㅑㅐㅔ", "ㅁㄴㅇㄹㅎㅗㅓㅏㅣ", "ㅋㅌㅊㅍㅠㅜㅡ"},
                                       {"ㅃㅉㄸㄲㅆㅛㅕㅑㅒㅖ", "ㅁㄴㅇㄹㅎㅗㅓㅏㅣ", "ㅋㅌㅊㅍㅠㅜㅡ"}};
  static const char* const EN[2][3] = {{"qwertyuiop", "asdfghjkl", "zxcvbnm"}, {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"}};
  const char* const* rows = kb.hangul ? KO[kb.shift] : EN[kb.shift];
  auto keyRow = [&](const char* chars, float x, int y) {
    while(*chars) {
      const char* start = chars;
      const uint32_t code = nextChar(chars);
      char label[8]; const size_t len = size_t(chars - start);
      memcpy(label, start, len); label[len] = 0;
      f(label, code, int(x), y, int(KB_UNIT), KB_KEY_H);
      x += KB_UNIT + KB_GAP;
    }
  };
  const float step = KB_UNIT + KB_GAP, wide = 1.5f * KB_UNIT + 0.5f * KB_GAP;
  keyRow("1234567890", KB_MARGIN, KB_TOP);
  keyRow(rows[0], KB_MARGIN, KB_TOP + KB_ROW);
  keyRow(rows[1], KB_MARGIN + step / 2, KB_TOP + 2 * KB_ROW);
  const int y3 = KB_TOP + 3 * KB_ROW, y4 = KB_TOP + 4 * KB_ROW;
  f("", KEY_SHIFT, KB_MARGIN, y3, int(wide), KB_KEY_H);
  keyRow(rows[2], KB_MARGIN + wide + KB_GAP, y3);
  f("", KEY_BACK, int(SCREEN_W - KB_MARGIN - wide), y3, int(wide), KB_KEY_H);
  f(kb.hangul ? "ABC" : "한글", KEY_LANG, KB_MARGIN, y4, int(wide), KB_KEY_H);
  f("-", '-', int(KB_MARGIN + wide + KB_GAP), y4, int(KB_UNIT), KB_KEY_H);
  f("띄어쓰기", ' ', int(KB_MARGIN + wide + KB_GAP + step), y4, int(5 * KB_UNIT + 4 * KB_GAP), KB_KEY_H);
  f(",", ',', int(KB_MARGIN + wide + KB_GAP + 6 * step), y4, int(KB_UNIT), KB_KEY_H);
  f("검색", KEY_SEARCH, int(SCREEN_W - KB_MARGIN - wide), y4, int(wide), KB_KEY_H);
}

// Text cut to a width (".." when shortened).
void fitText(int x, int y, int w, const char* s, uint16_t c, const Font& f) {
  if(textWidth(s, f) <= w) { text(x, y, s, c, f); return; }
  char buf[128]; size_t n = 0;
  const char* p = s;
  while(*p) {
    const char* q = p; nextChar(q);
    if(n + size_t(q - p) + 3 > sizeof(buf)) break;
    char trial[128]; memcpy(trial, buf, n); memcpy(trial + n, p, q - p); memcpy(trial + n + (q - p), "..", 3);
    if(textWidth(trial, f) > w) break;
    memcpy(buf + n, p, q - p); n += q - p; p = q;
  }
  memcpy(buf + n, "..", 3);
  text(x, y, buf, c, f);
}
const Font& textFont() { return FONT_TEXT.count ? FONT_TEXT : FONT_CAPTION; }

void drawSearch() {
  char query[160];
  hangul::compose(kb.keys, kb.count, query, sizeof(query));
  glass(12, 12, 1000, 84, 24, 230);
  button(24, 26, 120, 56, "닫기", WHITE, 34, TEXT);
  shape(156, 24, 712, 60, 16, WHITE, 36);
  shape(156, 24, 712, 60, 16, ACCENT, 200, 1.5f);
  icon(ICON_SEARCH, S24, 172, 42, MUTED);
  const Font& tf = textFont();
  if(*query) {
    const int room = 640;
    const char* shown = query;                            // drop leading characters until the end fits
    while(*shown && textWidth(shown, tf) > room) nextChar(shown);
    const int tw = textWidth(shown, tf);
    text(206, 54 - tf.height / 2, shown, TEXT, tf);
    shape(206 + tw + 2, 38, 2, 32, 1, ACCENT);
  } else {
    text(206, 54 - FONT_CAPTION.height / 2, "주소나 장소 이름 (예: 서울시청)", FAINT, FONT_CAPTION);
    shape(204, 38, 2, 32, 1, ACCENT);
  }
  button(880, 26, 120, 56, "검색", ACCENT, 256, ON_ACCENT, ICON_SEARCH);

  // results
  if(searchState != SEARCH_IDLE) {
    glass(12, 104, 1000, 164, 22, 230);
    if(searchState == SEARCH_BUSY) centerText(12, 172, 1000, "찾는 중", MUTED, FONT_CAPTION);
    else if(searchState == SEARCH_FAILED) centerText(12, 172, 1000, "검색 실패 (인터넷 연결 확인)", WARN, FONT_CAPTION);
    else if(!resultCount) centerText(12, 172, 1000, "결과가 없습니다", MUTED, FONT_CAPTION);
    for(int i=0; i<std::min(resultCount, 3); ++i) {
      const int ry = 108 + i * 52;
      if(i) divider(36, ry, 952);
      circle(48, ry + 26, 7, ACCENT);
      fitText(68, ry + 26 - tf.height / 2, 920, results[i].name, TEXT, tf);
    }
  }
  // keyboard: opaque, so its image depends only on the layout state; drawn once per state
  const int kbTop = KB_TOP - 12;
  const size_t kbBytes = size_t(SCREEN_H - kbTop) * SCREEN_W * 2;
  static uint16_t* kbImage = nullptr;
  static int kbState = -1;
  const int state = (kb.hangul ? 2 : 0) | (kb.shift ? 1 : 0);
  if(kbImage && kbState == state) { memcpy(frame + kbTop * SCREEN_W, kbImage, kbBytes); return; }
  shape(0, kbTop, SCREEN_W, SCREEN_H - kbTop, 0, GLASS, 256);
  forEachKey([&](const char* label, uint32_t code, int x, int y, int w, int h) {
    const bool special = code < 32;
    const bool accent = code == KEY_SEARCH || (code == KEY_SHIFT && kb.shift);
    shape(x, y, w, h, 12, accent ? ACCENT : WHITE, accent ? 256 : (special ? 22 : 40));
    const uint16_t ink = accent ? ON_ACCENT : TEXT;
    if(code == KEY_SHIFT) iconCenter(ICON_SHIFT, S24, x + w/2, y + h/2, ink);
    else if(code == KEY_BACK) iconCenter(ICON_BACKSPACE, S32, x + w/2, y + h/2, ink);
    else if(code == KEY_SEARCH) { iconCenter(ICON_SEARCH, S24, x + w/2 - 30, y + h/2, ink); text(x + w/2 - 12, y + (h - FONT_BODY.height)/2, label, ink); }
    else {   // jamo and letters large, the space and language keys small
      const Font& f = code == ' ' || code == KEY_LANG ? FONT_CAPTION : (code >= 0x3131 ? FONT_TITLE : FONT_BODY);
      centerText(x, y + (h - f.height) / 2, w, label, ink, f);
    }
  });
  if(!kbImage) kbImage = static_cast<uint16_t*>(heap_caps_malloc(kbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if(kbImage) { memcpy(kbImage, frame + kbTop * SCREEN_W, kbBytes); kbState = state; }
}

void startSearch() {
  char query[160];
  hangul::compose(kb.keys, kb.count, query, sizeof(query));
  if(!*query) return;
  weatherSearch(query);
  searchState = SEARCH_BUSY; resultCount = 0;
}
void searchTap(int x, int y) {
  if(inside(x,y,24,26,120,56)) { flash(24,26,120,56); kb.open = false; return; }
  if(inside(x,y,880,26,120,56)) { flash(880,26,120,56); startSearch(); return; }
  if(searchState == SEARCH_DONE)
    for(int i=0; i<std::min(resultCount, 3); ++i)
      if(inside(x,y,12,108 + i*52,1000,52)) {
        flash(12, 108 + i*52, 1000, 52, 0);
        picker.zoom = 15;
        pickerCenter(results[i].lat, results[i].lon);
        picker.moved = true; picker.waitIp = false; kb.open = false;
        return;
      }
  forEachKey([&](const char*, uint32_t code, int kx, int ky, int w, int h) {
    if(!inside(x, y, kx, ky, w, h)) return;
    flash(kx, ky, w, h, 12);
    if(code == KEY_SHIFT) kb.shift = !kb.shift;
    else if(code == KEY_BACK) { if(kb.count) --kb.count; }
    else if(code == KEY_LANG) { kb.hangul = !kb.hangul; kb.shift = false; }
    else if(code == KEY_SEARCH) startSearch();
    else if(kb.count < int(sizeof(kb.keys) / sizeof(kb.keys[0]))) { kb.keys[kb.count++] = code; kb.shift = false; }
  });
}

bool insideCircle(int x,int y,int cx,int cy,int r);
void pickerTap(int x, int y) {
  if(kb.open) { searchTap(x, y); return; }
  if(inside(x,y,756,26,112,56)) { flash(756,26,112,56); kb.open = true; return; }
  if(inside(x,y,24,26,120,56)) { flash(24,26,120,56); picker.open = false; return; }
  if(inside(x,y,880,26,120,56)) {
    double lat, lon;
    mapLatLon(picker.cx, picker.cy, picker.zoom, lat, lon);
    lon = fmod(fmod(lon + 180, 360) + 360, 360) - 180;
    weatherSetLocation(float(lat), float(lon));
    picker.open = false;
    return;
  }
  if(insideCircle(x,y,968,452,36)) { flash(938,422,60,60,30); pickerZoom(1); return; }
  if(insideCircle(x,y,968,524,36)) { flash(938,494,60,60,30); pickerZoom(-1); return; }
  if(inside(x,y,24,520,160,56)) {
    flash(24,520,160,56);
    float la, lo;
    if(weatherIpLocation(la, lo)) pickerCenter(la, lo);
    picker.moved = false;   // the answer may arrive later: let it move the map
    weatherRequestIpLocation(); picker.waitIp = true;
    return;
  }
  if(inside(x,y,196,520,160,56)) {
    flash(196,520,160,56);
    float la, lo; char name[8];
    weatherLocation(la, lo, name, sizeof(name));
    if(la != 0 || lo != 0) pickerCenter(la, lo);
    picker.moved = true; picker.waitIp = false;
  }
}
void tap(int x,int y);
// Drag moves the map, two fingers zoom (one map level per 1.5x pinch, around the
// fingers' midpoint), a touch that does not move is a tap (sent on release).
void pickerZoomAt(int d, int mx, int my) {
  const int z = std::min(MAP_MAX_ZOOM, std::max(MAP_MIN_ZOOM, picker.zoom + d));
  if(z == picker.zoom) return;
  const double k = ldexp(1.0, z - picker.zoom), ox = mx - SCREEN_W / 2, oy = my - SCREEN_H / 2;
  picker.cx = (picker.cx + ox) * k - ox; picker.cy = (picker.cy + oy) * k - oy; picker.zoom = z; pickerNormalize();
}
void pickerTouch(int n, const uint16_t* xs, const uint16_t* ys) {
  static bool down = false, dragging = false, pinched = false;
  static int sx, sy; static double scx, scy, pinchStart; static uint32_t lastSeen;
  static bool ignoring = false, cancelled = false;
  if(pickerTouchReset) { pickerTouchReset = false; down = dragging = pinched = false; ignoring = n > 0; }
  if(ignoring) { if(n == 0 || (n < 0 && millis() - lastSeen > 400)) ignoring = false; else { if(n > 0) lastSeen = millis(); return; } }
  if(n >= 2) { lastSeen = millis(); if(kb.open) { down = pinched = true; return; } }   // no tap from a two-finger touch
  if(n >= 2 && !kb.open) {
    const double dist = hypot(double(xs[0]) - xs[1], double(ys[0]) - ys[1]);
    const int mx = (xs[0] + xs[1]) / 2, my = (ys[0] + ys[1]) / 2;
    if(!pinched) { pinched = true; down = true; dragging = true; pinchStart = std::max(dist, 20.0); return; }
    if(dist > pinchStart * 1.5) { pickerZoomAt(1, mx, my); pinchStart = dist; dirty = true; }
    else if(dist < pinchStart / 1.5) { pickerZoomAt(-1, mx, my); pinchStart = dist; dirty = true; }
    picker.moved = true; picker.waitIp = false;
  } else if(n == 1) {
    lastSeen = millis();
    const int x = xs[0], y = ys[0];
    if(pinched) return;                                   // one finger left after a pinch: wait for release
    if(!down) { down = true; dragging = false; cancelled = false; sx = x; sy = y; scx = picker.cx; scy = picker.cy; return; }
    if(!dragging && abs(x - sx) + abs(y - sy) > 12) {
      if(!kb.open && sy > 100) dragging = true;
      else cancelled = true;                              // slid off a button or key
    }
    if(dragging) {
      picker.cx = scx - (x - sx); picker.cy = scy - (y - sy); pickerNormalize();
      picker.moved = true; picker.waitIp = false; dirty = true;
    }
  } else if(down && (n == 0 || millis() - lastSeen > 400)) {
    down = false;
    if(!dragging && !pinched && !cancelled) tap(sx, sy);
    cancelled = false;
    pinched = false;
  }
}

void roundButton(int cx,int cy,int r,IconId id) {
  circle(cx, cy, r, WHITE, 34);
  iconCenter(id, S32, cx, cy, TEXT);
}

// Control cards: all the same size (smaller than the boiler cards), 3 x 3 grid.
constexpr int CARD_W = 249, CARD_H = 124, CARD_GAP = 14, CARD_COLS = 3, CARD_ROWS = 3;

// Boiler room cards: power switch, current temperature, set temperature -/+.
constexpr int ROOM_W = 380, ROOM_H = 190;
int roomX(int i) { return 224 + (i%2)*396; }
int roomY(int i) { return 126 + (i/2)*204; }

void drawBoiler() {
  for(int i=0; i<ROOM_COUNT; ++i) {
    const int x = roomX(i), y = roomY(i), w = ROOM_W;
    const RoomView& r = rooms[i];
    glass(x, y, w, ROOM_H, 26);
    text(x+24, y+18, roomNames[i], TEXT);
    char status[32]; uint16_t statusColor = r.known ? (r.on ? HEAT : MUTED) : FAINT;
    snprintf(status, sizeof(status), "%s", !r.known ? "미확인" : (r.on ? "난방 중" : "꺼짐"));
    commandStatus(commandFor(CMD_HEAT_POWER, i + 1, CMD_HEAT_SET), status, sizeof(status), statusColor, nullptr);
    text(x+24, y+48, status, statusColor, FONT_CAPTION);
    // power switch
    const int sx = x + w - 24 - 72, sy = y + 18;
    if(r.known) {
      shape(sx, sy, 72, 40, 20, r.on ? HEAT : WHITE, r.on ? 256 : 44);
      circle(r.on ? sx+52 : sx+20, sy+20, 15, r.on ? WHITE : MUTED);
    } else shape(sx, sy, 72, 40, 20, WHITE, 20);
    // current temperature
    char cur[16]; formatDegrees(cur, sizeof(cur), r, false);
    text(x+24, y+90, "현재", FAINT, FONT_CAPTION);
    text(x+22, y+116, cur, r.known ? TEXT : FAINT, FONT_DISPLAY);
    // set temperature with -/+
    char set[16]; formatDegrees(set, sizeof(set), r, true);
    centerText(x+w-150, y+90, 108, "설정", FAINT, FONT_CAPTION);
    centerText(x+w-150, y+122, 108, set, r.known ? TEXT : FAINT, FONT_TITLE);
    if(r.known) {
      circle(x+w-172, y+140, 22, WHITE, 34); iconCenter(ICON_MINUS, S24, x+w-172, y+140, TEXT);
      circle(x+w-46, y+140, 22, WHITE, 34); iconCenter(ICON_PLUS, S24, x+w-46, y+140, TEXT);
    }
  }
  char note[128];
  if(demo) snprintf(note, sizeof(note), "체험 모드: 예시 값입니다");
  else if(wallpadLive()) snprintf(note, sizeof(note), "월패드에서 읽은 값 · 켜기/끄기와 설정 온도를 바꿀 수 있습니다");
  else snprintf(note, sizeof(note), "%s · EW11 %s", wallpadStatusText(), WALLPAD_HOST);
  text(224, 548, note, demo ? WARN : FAINT, FONT_CAPTION);
}

bool boilerTap(int x,int y) {
  for(int i=0; i<ROOM_COUNT; ++i) {
    const int rx = roomX(i), ry = roomY(i), w = ROOM_W;
    if(!inside(x, y, rx, ry, w, ROOM_H)) continue;
    const RoomView& r = rooms[i];
    if(!r.known) return true;
    const uint8_t group = uint8_t(i + 1);
    if(inside(x, y, rx+w-24-72-12, ry+6, 96, 64)) {                  // power switch
      flash(rx+w-24-72, ry+18, 72, 40, 20);
      if(demo && demoRoomsPtr) demoRoomsPtr[i].on = !r.on; else if(!wallpadHeatingPower(group, !r.on)) commandBusy();
    } else if(inside(x, y, rx+w-172-32, ry+108, 64, 64)) {          // set -1
      flash(rx+w-194, ry+118, 44, 44, 22);
      const int t = std::max(10, r.set - 1);
      if(demo && demoRoomsPtr) demoRoomsPtr[i].set = t; else if(!wallpadHeatingSet(group, uint8_t(t))) commandBusy();
    } else if(inside(x, y, rx+w-46-32, ry+108, 64, 64)) {           // set +1
      flash(rx+w-68, ry+118, 44, 44, 22);
      const int t = std::min(30, r.set + 1);
      if(demo && demoRoomsPtr) demoRoomsPtr[i].set = t; else if(!wallpadHeatingSet(group, uint8_t(t))) commandBusy();
    }
    return true;
  }
  return false;
}

void controlCard(int x,int y,const ControlCard& card) {
  const WallpadSnapshot& d = devices();
  glass(x, y, CARD_W, CARD_H, 22);
  char title[24], status[32], detail[40] = "";
  IconId glyph = ICON_POWER;
  bool active = false;
  uint16_t statusColor = MUTED;
  int dots = 0; uint8_t dotBits = 0;
  bool gasButton = false;
  const char* action1 = nullptr;  // right-hand button label
  const char* action2 = nullptr;  // left-hand button label (ventilation)
  switch(card.kind) {
    case CARD_GAS: {
      glyph = ICON_FLAME;
      snprintf(title, sizeof(title), "가스 밸브");
      // The controller does not report the valve position: show the last confirmed close.
      if(d.gas.closedAt) {
        const uint32_t mins = uint32_t((nowMs() - d.gas.closedAt) / 60000);
        if(mins == 0) snprintf(status, sizeof(status), "방금 잠금 확인");
        else if(mins < 60) snprintf(status, sizeof(status), "%lu분 전 잠금", (unsigned long)mins);
        else snprintf(status, sizeof(status), "%lu시간 전 잠금", (unsigned long)(mins / 60));
        statusColor = ACCENT;
      } else { snprintf(status, sizeof(status), "상태 미확인"); statusColor = FAINT; }
      if(demo && recent(demoGasDoneAt, 2500)) { snprintf(status, sizeof(status), "잠금 확인"); statusColor = ACCENT; }
      commandStatus(commandFor(CMD_GAS_CLOSE, 0), status, sizeof(status), statusColor, "잠금 확인");
      gasButton = true;
      break;
    }
    case CARD_LIGHT: {
      const LightGroup& g = d.lights[card.index];
      glyph = ICON_SUN;
      snprintf(title, sizeof(title), "조명 %d", card.index + 1);
      const int on = __builtin_popcount(g.bits & ((1u << g.count) - 1));
      if(on) snprintf(status, sizeof(status), "%d/%d 켜짐", on, g.count); else snprintf(status, sizeof(status), "꺼짐");
      active = on > 0;
      statusColor = active ? HEAT : MUTED;
      dots = std::min<int>(g.count, 4); dotBits = g.bits;
      action1 = on ? "끄기" : "켜기";
      commandStatus(commandFor(CMD_LIGHT, card.index + 1), status, sizeof(status), statusColor, nullptr);
      break;
    }
    case CARD_VENT:
      glyph = ICON_GEAR;
      snprintf(title, sizeof(title), "환기");
      if(!d.vent.on) snprintf(status, sizeof(status), "꺼짐");
      else if(d.vent.automatic) snprintf(status, sizeof(status), "자동");
      else snprintf(status, sizeof(status), "%u단", d.vent.speed);
      active = d.vent.on;
      statusColor = active ? HEAT : MUTED;
      action2 = d.vent.on ? "끄기" : "켜기";
      action1 = "풍량";
      for(uint8_t code=1; code<=6; ++code)
        if(commandStatus(commandFor(CMD_VENT, code), status, sizeof(status), statusColor, nullptr)) break;
      break;
    case CARD_OUTLET: {
      const OutletState& o = d.outlets[card.index];
      snprintf(title, sizeof(title), "콘센트 %d", card.index + 1);
      snprintf(status, sizeof(status), "%s", o.mode == 0x10 ? "차단됨" : (o.mode == 0x11 ? "대기전력 차단" : "켜짐"));
      snprintf(detail, sizeof(detail), "%uW", o.watts);
      active = o.mode != 0x10;
      statusColor = active ? HEAT : MUTED;
      action1 = active ? "끄기" : "켜기";
      commandStatus(commandFor(CMD_OUTLET, card.index + 1), status, sizeof(status), statusColor, nullptr);
      break;
    }
    case CARD_METER: {
      static const char* names[3] = {"전기", "가스", "수도"};
      const EnergyState& e = d.energy[card.index];
      glyph = ICON_BUILDING;
      snprintf(title, sizeof(title), "%s 사용량", names[card.index]);
      if(card.index == 0) snprintf(status, sizeof(status), "%luW", (unsigned long)e.value);
      else snprintf(status, sizeof(status), "%lu.%02lu", (unsigned long)(e.value/100), (unsigned long)(e.value%100));
      snprintf(detail, sizeof(detail), "에너지 미터");
      statusColor = TEXT;
      break;
    }
    case CARD_ENTRANCE: {
      const EntranceState& en = d.entrance;
      glyph = ICON_DOOR;
      snprintf(title, sizeof(title), "현관 스위치");
      snprintf(status, sizeof(status), "%s", en.away == 1 ? "외출 설정" : (en.away == 0 ? "외출 해제" : "연결됨"));
      snprintf(detail, sizeof(detail), "가스 %s · 일괄소등 %s",
               en.gasClosed < 0 ? "-" : (en.gasClosed ? "잠김" : "열림"),
               en.lightsAllOff < 0 ? "-" : (en.lightsAllOff ? "켜짐" : "꺼짐"));
      active = en.away == 1;
      statusColor = active ? HEAT : MUTED;
      break;
    }
  }
  const uint16_t chipFill = card.kind == CARD_GAS ? statusColor : HEAT;
  iconChip(x+16, y+16, glyph, active ? chipFill : WHITE, active ? 256 : 30, active ? ON_HEAT : TEXT);
  text(x+80, y+16, title, TEXT);
  text(x+80, y+44, status, statusColor, FONT_CAPTION);
  if(dots) {  // one dot per switch in the light group (first four)
    for(int b=0; b<dots; ++b) {
      const int cx = x + 26 + b*22, cy = y + 98;
      if(dotBits & (1u << b)) circle(cx, cy, 7, HEAT); else shape(cx-7, cy-7, 14, 14, 7, MUTED, 200, 1.5f);
    }
  } else if(gasButton) {
    text(x+18, y+86, "잠그기만 가능", FAINT, FONT_CAPTION);
  } else if(*detail && !action2) text(x+18, y+86, detail, FAINT, FONT_CAPTION);
  if(gasButton) button(x+CARD_W-16-100, y+72, 100, 40, "잠그기", WHITE, 40, TEXT);
  if(action1) button(x+CARD_W-16-100, y+72, 100, 40, action1, WHITE, 40, TEXT);
  if(action2) button(x+16, y+72, 100, 40, action2, WHITE, 40, TEXT);
}

// Next ventilation step: speed 1 -> 2 -> 3 -> auto -> speed 1 (codes 03, 02, 01, 04).
uint8_t nextVentCode(const VentState& v) {
  if(!v.on || v.automatic) return 0x03;
  return v.speed == 1 ? 0x02 : (v.speed == 2 ? 0x01 : 0x04);
}

// Control-card buttons. The touch area is the bottom band of the card (44 px+ target).
bool controlTap(int x,int y) {
  ControlCard cards[MAX_CARDS];
  const int n = std::min(controlCards(cards), CARD_COLS * CARD_ROWS);
  for(int i=0; i<n; ++i) {
    const int cx = 224 + (i % CARD_COLS)*(CARD_W + CARD_GAP), cy = 126 + (i / CARD_COLS)*(CARD_H + CARD_GAP);
    if(!inside(x, y, cx, cy + 66, CARD_W, CARD_H - 66)) continue;
    const bool right = x >= cx + CARD_W/2;
    const ControlCard& c = cards[i];
    const WallpadSnapshot& d = devices();
    const bool hasRight = c.kind == CARD_GAS || c.kind == CARD_LIGHT || c.kind == CARD_OUTLET || c.kind == CARD_VENT;
    const bool hasLeft = c.kind == CARD_VENT;
    if(right && hasRight) flash(cx+CARD_W-16-100, cy+72, 100, 40, 20);
    else if(!right && hasLeft) flash(cx+16, cy+72, 100, 40, 20);
    switch(c.kind) {
      case CARD_GAS:
        if(right) gasConfirm = true;
        return true;
      case CARD_LIGHT: {
        if(!right) return true;
        const LightGroup& g = d.lights[c.index];
        const bool on = (g.bits & ((1u << g.count) - 1)) != 0;
        if(demo) demoDevices().lights[c.index].bits = on ? 0 : uint8_t((1u << g.count) - 1);
        else if(!wallpadLight(c.index + 1, !on)) commandBusy();
        return true;
      }
      case CARD_OUTLET: {
        if(!right) return true;
        const bool on = d.outlets[c.index].mode != 0x10;
        if(demo) demoDevices().outlets[c.index].mode = on ? 0x10 : 0x01;
        else if(!wallpadOutletPower(c.index + 1, !on)) commandBusy();
        return true;
      }
      case CARD_VENT: {
        const VentState& v = d.vent;
        const uint8_t code = right ? nextVentCode(v) : (v.on ? 0x06 : 0x05);
        if(demo) {
          VentState& dv = demoDevices().vent;
          if(code == 0x06) dv.on = false;
          else if(code == 0x05) dv.on = true;
          else if(code == 0x04) { dv.on = true; dv.automatic = true; }
          else { dv.on = true; dv.automatic = false; dv.speed = uint8_t(4 - code); }
        } else if(!wallpadVent(code)) commandBusy();
        return true;
      }
      default:
        return true;
    }
  }
  return false;
}

void drawGasConfirm() {
  dimFrame();
  shape(292, 150, 440, 300, 32, GLASS, 236);
  shape(292, 150, 440, 300, 32, WHITE, 60, 1.5f);
  circle(512, 214, 34, WARN);
  iconCenter(ICON_FLAME, S32, 512, 214, ON_WARN);
  centerText(292, 262, 440, "가스 밸브를 잠글까요?", TEXT, FONT_TITLE);
  centerText(292, 306, 440, "원격으로는 다시 열 수 없습니다", MUTED, FONT_CAPTION);
  button(320, 366, 180, 60, "취소", WHITE, 40, TEXT);
  button(524, 366, 180, 60, "잠그기", WARN, 256, ON_WARN);
}

void drawControl() {
  ControlCard cards[MAX_CARDS];
  const int n = controlCards(cards);
  const int shown = std::min(n, CARD_COLS * CARD_ROWS);
  for(int i=0; i<shown; ++i)
    controlCard(224 + (i % CARD_COLS)*(CARD_W + CARD_GAP), 126 + (i / CARD_COLS)*(CARD_H + CARD_GAP), cards[i]);
  char note[96];
  if(demo) snprintf(note, sizeof(note), "체험 모드: 예시 값입니다");
  else if(n > shown) snprintf(note, sizeof(note), "장치 %d개 중 %d개 표시 · 명령 결과는 장치 응답으로 확인합니다", n, shown);
  else snprintf(note, sizeof(note), "월패드 버스에서 읽은 상태 · 명령 결과는 장치 응답으로 확인합니다");
  text(224, 548, note, demo ? WARN : FAINT, FONT_CAPTION);
}

void drawCalls() {
  seenRings = totalRings;
  glass(224, 126, 776, 406, 26);
  icon(ICON_BELL, S24, 256, 150, MUTED);
  text(290, 148, "출입문 호출 기록", TEXT);
  const IntercomState& ic = devices().intercom;
  const char* phase = "일방 호출 · 최근 5건";
  if(ic.phase == INTERCOM_RINGING) phase = ic.ringDoor == 1 ? "현관 벨 울리는 중" : "1층 현관 벨 울리는 중";
  else if(ic.phase == INTERCOM_CALL) phase = "인터폰 통화 중";
  else if(ic.phase == INTERCOM_BUSY) phase = "다른 인터폰 사용 중";
  rightText(968, 150, phase, ic.phase == INTERCOM_IDLE ? FAINT : WARN, FONT_CAPTION);
  if(!ringCount) { centerText(224, 314, 776, "호출 기록이 없습니다", FAINT); return; }
  for(int i=0; i<std::min(ringCount,5); ++i) {
    const int y = 192 + i*66;
    divider(256, y, 712);
    circle(282, y+33, 24, WHITE, 30);
    iconCenter(doorIcons[rings[i].door], S32, 282, y+33, TEXT);
    text(322, y+20, doorNames[rings[i].door], TEXT);
    char ago[24]; formatAgo(ago, sizeof(ago), rings[i].at);
    rightText(968, y+22, ago, MUTED, FONT_CAPTION);
  }
}

void drawSettings() {
  glass(224, 126, 776, 420, 26);
  icon(ICON_SUN, S24, 256, 152, MUTED);
  text(290, 150, "화면 밝기", TEXT);
  char b[12]; snprintf(b, sizeof(b), "%d%%", brightness); rightText(968, 150, b, ACCENT);
  roundButton(286, 212, 28, ICON_MINUS);
  shape(332, 208, 584, 8, 4, WHITE, 40);
  const int fill = 584*brightness/100;
  shape(332, 208, fill, 8, 4, ACCENT);
  circle(332+fill, 212, 13, WHITE);
  roundButton(938, 212, 28, ICON_PLUS);
  // rows: 256..322 location, 322..388 touch, 388..454 wallpad (centres 289, 355, 421)
  divider(256, 256, 712);
  icon(ICON_HOME, S24, 256, 289 - 12, MUTED);
  text(290, 289 - FONT_BODY.height / 2, "위치", TEXT);
  float la, lo; char place[48];
  weatherLocation(la, lo, place, sizeof(place));
  const bool pending = weatherLocationPending();
  rightText(852, 289 - FONT_CAPTION.height / 2, pending ? "위치 확인 중" : (*place ? place : "인터넷 주소로 추정"), pending ? WARN : MUTED, FONT_CAPTION);
  button(868, 265, 100, 48, "변경", WHITE, 34, TEXT);
  divider(256, 322, 712);
  icon(ICON_CHECK, S24, 256, 355 - 12, touchOk ? ACCENT : WARN);
  text(290, 355 - FONT_BODY.height / 2, "터치", TEXT);
  rightText(968, 355 - FONT_CAPTION.height / 2, touchOk ? "정상" : "확인 필요", touchOk ? ACCENT : WARN, FONT_CAPTION);
  divider(256, 388, 712);
  icon(ICON_BUILDING, S24, 256, 421 - 12, wallpadLive() ? ACCENT : MUTED);
  text(290, 421 - FONT_BODY.height / 2, "월패드", TEXT);
  char link[96];
  if(wallpad.state==WALLPAD_CONNECTED) {
    const IPAddress ip(wallpad.ip);
    char medium[24];
    if(wallpad.wifi) snprintf(medium, sizeof(medium), "WiFi %ddBm", wallpad.rssi); else snprintf(medium, sizeof(medium), "유선");
    snprintf(link, sizeof(link), "%s · %s %s · 패킷 %lu", wallpadLive() ? "수신 중" : "신호 없음",
             medium, ip.toString().c_str(), (unsigned long)wallpad.packets);
  } else snprintf(link, sizeof(link), "%s", wallpadStatusText());
  rightText(968, 421 - FONT_CAPTION.height / 2, link, wallpadLive() ? ACCENT : MUTED, FONT_CAPTION);
  divider(256, 454, 712);
  button(256, 470, 256, 56, demo ? "체험 종료" : "체험 시작", demo ? WARN : WHITE, demo ? 256 : 34, demo ? ON_WARN : TEXT);
  if(demo) button(528, 470, 256, 56, "방문 호출 시험", WHITE, 34, TEXT, ICON_BELL);
  text(224, 554, "체험 모드는 화면에서만 동작하며 실제 기기를 제어하지 않습니다", FAINT, FONT_CAPTION);
}

void drawRingOverlay() {
  dimFrame();
  shape(262, 88, 500, 424, 34, GLASS, 236);
  shape(262, 88, 500, 424, 34, WARN, 200, 2);
  circle(512, 178, 52, WARN, 40);
  circle(512, 178, 40, WARN);
  iconCenter(doorIcons[activeRing], S56, 512, 178, ON_WARN);
  centerText(262, 252, 500, "방문 호출", WARN, FONT_CAPTION);
  centerText(262, 282, 500, doorNames[activeRing], TEXT, FONT_TITLE);
  char ago[24], line[48]; formatAgo(ago, sizeof(ago), activeRingAt);
  snprintf(line, sizeof(line), "일방 호출 · %s", ago);
  centerText(262, 330, 500, line, MUTED, FONT_CAPTION);
  button(382, 412, 260, 68, "확인", ACCENT, 256, ON_ACCENT, ICON_CHECK);
}

// Debug timing (serial PERF): render time and the longest gap between loop passes.
uint32_t perfRenderUs = 0, perfRenderMaxUs = 0, perfLoopGapMaxMs = 0, perfLoopAt = 0, perfRenders = 0;
void renderFrame();
void render() {
  const uint32_t t0 = micros();
  renderFrame();
  perfRenderUs = micros() - t0;
  perfRenderMaxUs = std::max(perfRenderMaxUs, perfRenderUs);
  ++perfRenders;
}
void renderFrame() {
  panelAcquireBack();   // the previous front buffer may still be on screen for a moment
  frame = panelBackBuffer();
  gfxSetTarget(frame);
  if(picker.open) {
    drawPicker();
    if(activeRing>=0) drawRingOverlay();
    drawFlash();
    panelPresent();
    shownMinute = minuteStamp();
    dirty = false;
    return;
  }
  uint32_t t = micros();
  // background + this page's glass cards come from a per-page cache (see gfx.h glassBegin)
  glassBegin(background ? background : BG_IMAGE, page);
  perfSection[0] = micros() - t; t = micros();
  drawSidebar();
  drawHeader();
  drawElevator();
  perfSection[1] = micros() - t; t = micros();
  if(page==HOME) drawHome();
  else if(page==WEATHER) drawWeather();
  else if(page==BOILER) drawBoiler();
  else if(page==CONTROL) drawControl();
  else if(page==CALLS) drawCalls();
  else drawSettings();
  if(!glassEnd()) { renderFrame(); return; }   // cards changed: redraw with real glass once
  perfSection[2] = micros() - t; t = micros();
  drawNotice();
  if(gasConfirm) drawGasConfirm();
  if(activeRing>=0) drawRingOverlay();
  drawFlash();
  perfSection[3] = micros() - t; t = micros();
  panelPresent();
  perfSection[4] = micros() - t;
  shownMinute = minuteStamp();
  dirty = false;
}

// ---------- input ----------
bool inside(int x,int y,int bx,int by,int w,int h) { return x>=bx && x<bx+w && y>=by && y<by+h; }
bool insideCircle(int x,int y,int cx,int cy,int r) { return (x-cx)*(x-cx)+(y-cy)*(y-cy) <= r*r; }

void tap(int x,int y) {
  dirty = true;
  if(activeRing>=0) { if(inside(x,y,382,412,260,68)) { flash(382,412,260,68); dismissRing(); } return; }
  if(picker.open) { pickerTap(x, y); return; }
  if(gasConfirm) {
    if(inside(x,y,320,366,180,60)) { flash(320,366,180,60); gasConfirm = false; }
    else if(inside(x,y,524,366,180,60)) {
      flash(524,366,180,60);
      gasConfirm = false;
      if(demo) { demoGasDoneAt = millis(); demoDevices().gas.closedAt = nowMs(); } else if(!wallpadCloseGas()) commandBusy();
    }
    return;
  }
  if(inside(x,y,868,28,118,66)) { flash(868,28,118,66); callElevator(); return; }
  if(x<196 && y>=112) {
    int nav[PAGE_COUNT];
    const int count = navPages(nav), k = (y-112)/66;
    if(k < count) page = nav[k];
    return;
  }
  if(page==CONTROL && controlTap(x, y)) return;
  if(page==BOILER && boilerTap(x, y)) return;
  if(page==HOME) {
    if(inside(x,y,540,126,460,206)) page = WEATHER;
    else if(inside(x,y,224,348,384,184)) page = BOILER;
    else if(inside(x,y,624,348,376,184)) page = CALLS;
  } else if(page==SETTINGS) {
    const int before = brightness;
    if(insideCircle(x,y,286,212,36)) { flash(258,184,56,56,28); brightness = std::max(10, brightness-10); panelBrightness(brightness); }
    if(insideCircle(x,y,938,212,36)) { flash(910,184,56,56,28); brightness = std::min(100, brightness+10); panelBrightness(brightness); }
    else if(inside(x,y,332,188,584,48)) { brightness = std::min(100, std::max(10, ((x-332)*100/584+5)/10*10)); panelBrightness(brightness); }
    if(brightness != before) { Preferences p; if(p.begin("panel", false)) { p.putUChar("bright", uint8_t(brightness)); p.end(); } }
    if(inside(x,y,860,258,116,64)) { flash(868,266,100,48); openPicker(); }
    if(inside(x,y,256,470,256,56)) { flash(256,470,256,56); setDemo(!demo); }
    if(demo && inside(x,y,528,470,256,56)) addRing(int(totalRings % 3));
  }
}

// ---------- serial: bridge protocol + debug ----------
void sendScreenshot() {
  // Debug: raw RGB565 little-endian of the visible frame.
  if(!frame) { Serial.println("ERROR|no display"); return; }
  Serial.printf("SHOT|%d|%d\n", SCREEN_W, SCREEN_H);
  Serial.flush();
  const uint8_t* p = reinterpret_cast<const uint8_t*>(panelFrontBuffer());
  const size_t total = SCREEN_W*SCREEN_H*2;
  uint32_t progressAt = millis();
  for(size_t sent=0; sent<total; ) {
    const size_t n = Serial.write(p+sent, std::min<size_t>(4096, total-sent));
    if(n) { sent += n; progressAt = millis(); continue; }
    if(millis() - progressAt > 10000) return;  // host stopped reading: give up, don't hang the UI
    delay(1);
  }
  Serial.flush();
}

void serialLine(const String& line) {
  const char* l = line.c_str();
  int a, b, c; char extra;
  if(line=="SHOT") { sendScreenshot(); perfLoopAt = millis(); return; }
  if(line=="NET") { wallpadDiagnose(Serial, WIFI_SSID, false); return; }
  if(line=="NET SCAN") { wallpadDiagnose(Serial, WIFI_SSID, true); return; }
  if(line=="DEV") {
    const WallpadSnapshot w = wallpadSnapshot();
    Serial.printf("DEV|gas|%d|closed_ms_ago|%lu\n", w.gas.seen, w.gas.closedAt ? (unsigned long)(nowMs() - w.gas.closedAt) : 0UL);
    Serial.printf("DEV|command|state|%d|kind|%d|target|%u|attempts|%u\n", w.command.state, w.command.kind, w.command.target, w.command.attempts);
    for(int i=0; i<LIGHT_GROUPS; ++i) if(w.lights[i].seen) Serial.printf("DEV|light|%d|%u|%02X\n", i+1, w.lights[i].count, w.lights[i].bits);
    for(int i=0; i<OUTLETS; ++i) if(w.outlets[i].seen) Serial.printf("DEV|outlet|%d|%02X|%u\n", i+1, w.outlets[i].mode, w.outlets[i].watts);
    Serial.printf("DEV|vent|%d|%d|%u|%d\n", w.vent.seen, w.vent.on, w.vent.speed, w.vent.automatic);
    for(int i=0; i<3; ++i) if(w.energy[i].seen) Serial.printf("DEV|energy|%d|%lu\n", i, (unsigned long)w.energy[i].value);
    Serial.printf("DEV|intercom|%d|%d|bells|%lu|opens|%lu\n", w.intercom.seen, w.intercom.phase, (unsigned long)w.doorbells, (unsigned long)w.doorOpens);
    Serial.printf("DEV|entrance|%d|gas|%d|away|%d|alloff|%d\n", w.entrance.seen, w.entrance.gasClosed, w.entrance.away, w.entrance.lightsAllOff);
    Serial.printf("DEV|elevator|calls|%lu|arrivals|%lu|clock|%d\n", (unsigned long)w.elevatorCalls, (unsigned long)w.elevatorArrivals, w.clock.valid);
    Serial.println("DEV|done");
    return;
  }
  if(line=="ASSETS") { Serial.printf("ASSETS|%s\n", assetsError()); return; }
  if(line=="PERF") {
    auto stack = [](const char* name) { TaskHandle_t t = xTaskGetHandle(name); return t ? int(uxTaskGetStackHighWaterMark(t)) : -1; };
    Serial.printf("PERF|render_us|%lu|max|%lu|renders|%lu|loop_gap_max_ms|%lu\n", (unsigned long)perfRenderUs,
                  (unsigned long)perfRenderMaxUs, (unsigned long)perfRenders, (unsigned long)perfLoopGapMaxMs);
    Serial.printf("PERF|heap_internal|%u|min|%u|psram|%u|largest_internal|%u\n",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    Serial.printf("PERF|last_us|background|%lu|chrome|%lu|page|%lu|overlay|%lu|present|%lu|sunmoon|%lu\n",
                  (unsigned long)perfSection[0], (unsigned long)perfSection[1], (unsigned long)perfSection[2],
                  (unsigned long)perfSection[3], (unsigned long)perfSection[4], (unsigned long)perfSection[5]);
    Serial.printf("PERF|stack_free|loop|%d|weather|%d|map|%d|wallpad|%d\n", int(uxTaskGetStackHighWaterMark(nullptr)),
                  stack("weather"), stack("map"), stack("wallpad"));
    char mapLine[160]; mapStats(mapLine, sizeof(mapLine)); Serial.println(mapLine);
    perfRenderMaxUs = 0; perfLoopGapMaxMs = 0; perfRenders = 0;
    perfLoopAt = millis();   // the PERF print itself is not a stall
    return;
  }
  if(!frame) return;   // LCD init failed: nothing below may run (the tasks were not started)
  if(sscanf(l, "TAP|%d|%d%c", &a, &b, &extra)==2) { tap(a, b); return; }
  if(sscanf(l, "PAN|%d|%d%c", &a, &b, &extra)==2 && picker.open) {   // debug: move the map like a drag, one frame
    picker.cx -= a; picker.cy -= b; pickerNormalize(); picker.moved = true; render(); perfLoopAt = millis(); return;
  }
  if(demo) return;
  if(line=="PING") { lastBridge = millis(); if(!bridge) dirty = true; bridge = true; return; }
  if(!online()) return;
  const Elevator elevBefore = elev; const Office officeBefore = office;
  if(sscanf(l, "ELEV|%d|%d|%d%c", &a, &b, &c, &extra)==3 && a>=-9 && a<=199 && b>=-1 && b<=1 && (c==0||c==1)) {
    if(elev.calling && !c && a==elev.floor) elev.arrivedAt = millis();
    elev.floor = a; elev.dir = b; elev.calling = c;
    if(c) elev.pending = false;
  } else if(line=="ELEVERR") { elev.pending = false; elev.failedAt = millis(); }
  else if(sscanf(l, "OFFICE|%d%c", &a, &extra)==1 && (a==0||a==1)) {
    if(a && !office.calling) office.startedAt = millis();
    office.calling = a; if(a) office.pending = false;
  } else if(line=="OFFICEERR") { office.pending = false; office.failedAt = millis(); }
  else if(sscanf(l, "RING|%d%c", &a, &extra)==1 && a>=0 && a<3) { addRing(a); return; }
  else return;
  if(memcmp(&elevBefore, &elev, sizeof(elev)) || memcmp(&officeBefore, &office, sizeof(office))) dirty = true;
}

void demoTick() {
  if(!demo || millis()-lastDemoTick < 1200) return;
  lastDemoTick = millis();
  if(elev.calling) {
    elev.dir = elev.floor < DEMO_HOME_FLOOR ? 1 : -1;
    elev.floor += elev.dir;
    if(elev.floor==0) elev.floor += elev.dir;  // no floor 0
    if(elev.floor==DEMO_HOME_FLOOR) { elev.calling = false; elev.dir = 0; elev.arrivedAt = millis(); }
    dirty = true;
  }
  if(office.calling && millis()-office.startedAt > 5000) { office.calling = false; dirty = true; }
}

void checkTimeouts() {
  const uint32_t now = millis();
  if(elev.pending && now-elev.requestedAt > REPLY_TIMEOUT_MS) { elev.pending = false; elev.failedAt = now; dirty = true; }
  if(office.pending && now-office.requestedAt > REPLY_TIMEOUT_MS) { office.pending = false; office.failedAt = now; dirty = true; }
  for(uint32_t* at : {&elev.failedAt, &office.failedAt})
    if(*at && now-*at > NOTICE_MS) { *at = 0; dirty = true; }
  if(elev.arrivedAt && now-elev.arrivedAt > 5000) { elev.arrivedAt = 0; dirty = true; }
  if(elev.busCallAt && now - elev.busCallAt > 180000) {   // no arrival seen on the bus
    elev.busCallAt = 0;
    if(!demo && !online() && elev.calling) { elev.calling = false; dirty = true; }
  }
  if(activeRing>=0 && nowMs()-activeRingAt > RING_SHOW_MS) dismissRing();
  if(flashUntil && int32_t(now - flashUntil) >= 0) { flashUntil = 0; dirty = true; }
  if(noticeAt && now - noticeAt > 3000) { noticeAt = 0; dirty = true; }
  if(bridge && !online()) { bridge = false; clearLiveState(); dirty = true; }
  if(minuteStamp() != shownMinute) dirty = true;  // clock, uptime and "N분 전" labels
}
}

void setup() {
  Serial.begin(115200);
  incoming.reserve(128);
  if(!panelBegin()) { Serial.println("ERROR|LCD init failed"); return; }
  // Background, icons and fonts come from the asset partition (tools/upload_p4.py).
  if(!assetsBegin()) Serial.printf("ERROR|assets|%s\n", assetsError());
  if(BG_IMAGE) {
    background = static_cast<uint16_t*>(heap_caps_malloc(SCREEN_W*SCREEN_H*2, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(background) memcpy(background, BG_IMAGE, SCREEN_W*SCREEN_H*2);
  }
  frame = panelBackBuffer();
  touchOk = touchBegin();
  wallpadBegin(WALLPAD_HOST, WALLPAD_PORT, WIFI_SSID, WIFI_PASSWORD);
  weatherBegin(WEATHER_LAT, WEATHER_LON, WEATHER_PLACE, WEATHER_UTC_OFFSET);
  mapBegin();   // tile cache and worker ready before the location picker opens
  { Preferences p; if(p.begin("panel", true)) { brightness = std::min(100, std::max(10, int(p.getUChar("bright", 80)))); p.end(); } }
  render();
  panelBrightness(brightness);
  Serial.println("HELLO|HomePanel|5");
}

void loop() {
  if(perfLoopAt) perfLoopGapMaxMs = std::max(perfLoopGapMaxMs, millis() - perfLoopAt);
  perfLoopAt = millis();
  while(Serial.available()) {
    char c = Serial.read();
    static bool overflow = false;
    if(c=='\n') { if(!overflow) serialLine(incoming); incoming = ""; overflow = false; }
    else if(c!='\r') { if(incoming.length()<127) incoming += c; else { incoming = ""; overflow = true; } }
  }
  if(!frame) { delay(100); return; }
  checkTimeouts();
  demoTick();
  if(millis()-lastWallpadPoll > 250) {
    lastWallpadPoll = millis();
    wallpad = wallpadSnapshot();
    static uint32_t seenBells = 0, seenOpens = 0, seenCalls = 0, seenArrivals = 0;
    if(!demo) {
      if(wallpad.doorbells != seenBells) addRing(wallpad.lastDoorbell == 1 ? 0 : 1);  // home door / common door
      if(wallpad.elevatorCalls != seenCalls) { elev.calling = true; elev.pending = false; elev.busCallAt = millis(); dirty = true; }
      if(wallpad.elevatorArrivals != seenArrivals) { elev.calling = false; elev.dir = 0; elev.arrivedAt = millis(); dirty = true; }
      if(wallpad.doorOpens != seenOpens) dirty = true;
    }
    seenBells = wallpad.doorbells; seenOpens = wallpad.doorOpens;
    seenCalls = wallpad.elevatorCalls; seenArrivals = wallpad.elevatorArrivals;
    refreshRooms();
    WeatherData wx;
    weatherCopy(wx);
    const uint32_t wsig = wx.fetchedAt * 31 + weatherState();
    if(wsig != weatherSignature) { weather = wx; weatherSignature = wsig; dirty = true; }
    {
      float la, lo; char place[48];
      weatherLocation(la, lo, place, sizeof(place));
      if(la != skyLat || lo != skyLon || strcmp(place, skyPlace)) {
        skyLat = la; skyLon = lo; snprintf(skyPlace, sizeof(skyPlace), "%s", place); dirty = true;
      }
    }
    static bool clockFromBus = false;   // no NTP (time still 1970): take the wallpad's clock once
    // The CC 01 clock's byte encoding (binary or BCD) was never seen on this bus: accept only
    // a plausible year, so a BCD year (0x26 read as 38) cannot set the clock to 2038.
    if(!clockFromBus && time(nullptr) < 1700000000 && !demo && wallpad.clock.valid &&
       wallpad.clock.year >= 24 && wallpad.clock.year <= 40) {
      clockFromBus = true;
      struct tm t = {};
      t.tm_year = 100 + wallpad.clock.year; t.tm_mon = wallpad.clock.month - 1; t.tm_mday = wallpad.clock.day;
      t.tm_hour = wallpad.clock.hour; t.tm_min = wallpad.clock.minute;
      t.tm_sec = wallpad.clock.second + int((millis() - wallpad.clock.at) / 1000);
      const time_t local = mktime(&t);   // local time (the zone is set at boot)
      if(local > 1700000000) { const timeval tv = {local, 0}; settimeofday(&tv, nullptr); dirty = true; }
    }
    Eclipse found[8];
    const int ne = weatherEclipses(found, 8);
    uint32_t esig = 2166136261u + ne;   // FNV-1a over the whole list
    for(const uint8_t* b = reinterpret_cast<const uint8_t*>(found); b < reinterpret_cast<const uint8_t*>(found + ne); ++b) esig = (esig ^ *b) * 16777619u;
    if(esig != eclipseSignature) { memcpy(eclipses, found, sizeof(found)); eclipseCount = ne; eclipseSignature = esig; dirty = true; }
    if(picker.open) {
      float la, lo;
      if(picker.waitIp && !picker.moved && weatherIpLocation(la, lo)) { pickerCenter(la, lo); picker.waitIp = false; dirty = true; }
      if(mapGeneration() != picker.gen) dirty = true;
      static uint32_t retryAt = 0;
      if(pickerMissing && millis() - retryAt > 10000) { retryAt = millis(); dirty = true; }
      static bool ipFailedShown = false;
      if(weatherIpFailed() != ipFailedShown) { ipFailedShown = weatherIpFailed(); dirty = true; }
    }
    static uint32_t settingsTick = 0;   // live values on the settings page: at most once a second
    const bool settingsDue = page==SETTINGS && millis() - settingsTick > 1000;
    static bool wasPending = false;
    if(settingsDue && (weatherLocationPending() || wasPending)) { dirty = true; settingsTick = millis(); }
    wasPending = weatherLocationPending();
    if(picker.open && kb.open && searchState != SEARCH_IDLE) {
      PlaceResult r[4]; int n = 0;
      const SearchState st = weatherSearchResults(r, 4, n);
      if(st != searchState || (st == SEARCH_DONE && n != resultCount)) {
        searchState = st; resultCount = st == SEARCH_DONE ? n : 0;
        memcpy(results, r, sizeof(r)); dirty = true;
      }
    }
    static uint32_t shownPackets = 0;  // settings page shows a live packet counter
    if(settingsDue && wallpad.packets/10 != shownPackets/10) { shownPackets = wallpad.packets; dirty = true; settingsTick = millis(); }
  }
  static bool held = false;
  static uint32_t lastPressReport = 0;
  if(picker.open && activeRing < 0) {   // the map takes two-finger gestures
    uint16_t xs[2], ys[2];
    pickerTouch(touchOk ? touchReadPoints(xs, ys, 2) : -1, xs, ys);
    if(dirty) render();
    delay(12);
    return;
  }
  uint16_t x=0, y=0;
  const int report = touchOk ? touchRead(x, y) : -1;
  static uint32_t lastSkyFrame = 0;   // demo: animate the eclipse in the sun & moon card
  if(demo && !picker.open && skyReady() && millis() - lastSkyFrame > 200 && (page==WEATHER || (page==HOME && skyEvent()))) {
    lastSkyFrame = millis(); dirty = true;   // demo: the replayed eclipse moves
  }
  static time_t shownSecond = 0;      // eclipse countdown: redraw the weather page every second
  if(!demo && (page==WEATHER || page==HOME) && !picker.open && skyReady() && time(nullptr) != shownSecond) {
    shownSecond = time(nullptr);
    if(activeEclipse(shownSecond)) dirty = true;   // cheap: no sky recomputation for the test
  }
  if(report==0) held = false;
  if(report==1) lastPressReport = millis();
  else if(held && millis() - lastPressReport > 400) held = false;
  if(report==1 && !held && millis()-lastPress > 180) { held = true; lastPress = millis(); tap(x, y); }
  // Redraw only on change; a static screen is never rewritten.
  if(dirty) render();
  delay(12);
}
