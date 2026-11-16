#include <Arduino.h>
#include <algorithm>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "board_config.h"
#include "panel.h"
#include "gfx.h"
#include "wallpad_link.h"

// Wall panel UI: boiler, management office call, elevator call, one-way door calls.
// Boiler state is read directly from the wallpad RS485 bus via the EW11 (read-only).
// Elevator / office / door calls still come from the USB bridge (tools/ha_bridge.py).
// Unknown values are shown as unknown; no command is assumed to succeed.

namespace {
uint16_t* frame;
uint16_t* background;  // PSRAM copy of BG_IMAGE (faster than reading flash every frame)


constexpr uint32_t REPLY_TIMEOUT_MS = 6000, NOTICE_MS = 4000, RING_SHOW_MS = 60000;
constexpr int DEMO_HOME_FLOOR = 12;

enum Page { HOME, BOILER, CONTROL, CALLS, SETTINGS, PAGE_COUNT };
const char* pageNames[] = {"홈", "보일러", "제어", "방문 호출", "설정"};
const IconId pageIcons[] = {ICON_HOME, ICON_FLAME, ICON_POWER, ICON_BELL, ICON_GEAR};
const char* doorNames[] = {"현관", "1층 현관", "지하주차장 문"};
const IconId doorIcons[] = {ICON_DOOR, ICON_BUILDING, ICON_PARKING};

struct Elevator { int floor=0, dir=0; bool calling=false, pending=false; uint32_t requestedAt=0, arrivedAt=0, failedAt=0; } elev;
// Heating rooms = wallpad heating groups 1..ROOM_COUNT (confirmed on this home's bus:
// group 1 living room by a set-temperature sweep; groups 2-4 named by the owner).
constexpr int ROOM_COUNT = 4;
const char* roomNames[ROOM_COUNT] = {"거실", "안방", "방1", "방2"};
constexpr uint32_t ZONE_STALE_MS = 15000;
struct RoomView { bool known, on; int set, current; };
RoomView rooms[ROOM_COUNT];
WallpadSnapshot wallpad = {};
uint32_t roomSignature = 0, lastWallpadPoll = 0;
bool wallpadLive();
const char* wallpadStatusText();
const WallpadSnapshot& devices();
int navPages(int* out);
bool controlVisible();
uint32_t deviceSignature();
struct Office { bool calling=false, pending=false; uint32_t requestedAt=0, startedAt=0, failedAt=0; } office;
struct Ring { uint8_t door; uint64_t at; };
Ring rings[8];
int ringCount = 0, activeRing = -1;
uint64_t activeRingAt = 0, lastRingAt[3] = {};

int page = HOME, brightness = 80;
bool demo = false, bridge = false, touchOk = false, dirty = true;
uint32_t lastBridge = 0, lastPress = 0, lastDemoTick = 0, shownMinute = 0;
int flashX, flashY, flashW, flashH, flashR; uint32_t flashUntil = 0;
String incoming;

// ---------- state helpers ----------
bool online() { return bridge && millis()-lastBridge < 8000; }
uint64_t nowMs() { return uint64_t(esp_timer_get_time() / 1000); }
bool recent(uint32_t at, uint32_t span) { return at && millis()-at < span; }
bool recentCall(uint64_t at, uint64_t span) { return at && nowMs()-at < span; }
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
  lastRingAt[door] = now;
  activeRing = door; activeRingAt = now;
  panelBrightness(100);  // Wake the screen for a visitor call.
  dirty = true;
}
void dismissRing() { activeRing = -1; panelBrightness(brightness); dirty = true; }

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
    if(i==CALLS && ringCount && page!=CALLS) circle(168, y+27, 5, WARN);  // unseen call history
  }
  char up[40];
  const WallClock& c = devices().clock;
  if(c.valid) {  // wallpad time (pushed to the entrance switch), advanced locally
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

// Rooms come from the wallpad link (or fixed sample values in demo mode).
void refreshRooms() {
  static const RoomView demoRooms[ROOM_COUNT] = {{true,true,24,22},{true,false,18,25},{true,false,23,23},{true,true,27,23}};
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
  if(page == CONTROL && !controlVisible()) page = HOME;
  if(sig != roomSignature) { roomSignature = sig; dirty = true; }
}
// Devices other than heating: the live snapshot, or a fixed sample in demo mode.
const WallpadSnapshot& devices() {
  static WallpadSnapshot sample = [] {
    WallpadSnapshot d = {};
    d.gas = {true, true, 1};
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
  return demo ? sample : wallpad;
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
  mix(d.gas.seen); mix(d.gas.closed);
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

void drawFlash() {
  if(flashUntil) shape(flashX-3, flashY-3, flashW+6, flashH+6, flashR+3, ACCENT, 220, 3);
}

// ---------- pages ----------
void drawHome() {
  // Boiler (read-only, from the wallpad bus)
  glass(224, 126, 384, 206, 26);
  const int heatingRooms = heatingCount();
  iconChip(248, 150, ICON_FLAME, heatingRooms ? HEAT : WHITE, heatingRooms ? 256 : 30, heatingRooms ? ON_HEAT : TEXT);
  text(316, 150, "보일러", TEXT);
  char summary[40];
  if(!anyRoomKnown()) snprintf(summary, sizeof(summary), "%s", wallpadStatusText());
  else if(heatingRooms) snprintf(summary, sizeof(summary), "%d개 방 난방 중", heatingRooms);
  else snprintf(summary, sizeof(summary), "모두 꺼짐");
  text(316, 178, summary, heatingRooms ? HEAT : (anyRoomKnown() ? MUTED : FAINT), FONT_CAPTION);
  icon(ICON_CHEVRON, S24, 572, 162, FAINT);
  char t[16]; formatDegrees(t, sizeof(t), rooms[0], false);
  text(248, 216, "거실 현재 온도", FAINT, FONT_CAPTION);
  text(246, 242, t, rooms[0].known ? TEXT : FAINT, FONT_DISPLAY);
  char tt[24]; formatDegrees(tt, sizeof(tt), rooms[0], true);
  rightText(584, 244, rooms[0].known && rooms[0].on ? "난방 중 · 설정" : "설정 온도", rooms[0].known && rooms[0].on ? HEAT : FAINT, FONT_CAPTION);
  rightText(584, 270, tt, rooms[0].known ? MUTED : FAINT);

  // Management office
  glass(624, 126, 376, 206, 26);
  iconChip(648, 150, ICON_HEADSET, WHITE, 30, TEXT);
  text(716, 150, "관리실 통화", TEXT);
  text(716, 178, "관리실을 호출합니다", MUTED, FONT_CAPTION);
  const char* label = "관리실 호출"; uint16_t fill = ACCENT, ink = ON_ACCENT; unsigned a = 256;
  if(recent(office.failedAt, NOTICE_MS)) { label = "호출 실패"; fill = DANGER; ink = DANGER; a = 48; }
  else if(office.pending) { label = "요청 중"; fill = WHITE; ink = WARN; a = 30; }
  else if(office.calling) { label = "호출 중"; fill = WARN; ink = ON_WARN; }
  else if(!demo && !online()) { label = "연결 필요"; fill = WHITE; ink = FAINT; a = 26; }
  button(648, 248, 328, 60, label, fill, a, ink, ICON_HEADSET);

  // Door call overview: one tile per door, last call time
  glass(224, 348, 776, 184, 26);
  icon(ICON_BELL, S24, 248, 366, MUTED);
  text(282, 364, "방문 호출", TEXT);
  rightText(944, 366, "전체 보기", MUTED, FONT_CAPTION);
  icon(ICON_CHEVRON, S24, 948, 366, MUTED);
  for(int i=0; i<3; ++i) {
    const int x = 248 + i*248;
    const bool fresh = recentCall(lastRingAt[i], 10*60000);
    shape(x, 412, 232, 96, 18, WHITE, fresh ? 34 : 20);
    circle(x+38, 460, 24, fresh ? WARN : WHITE, fresh ? 256 : 30);
    iconCenter(doorIcons[i], S32, x+38, 460, fresh ? ON_WARN : TEXT);
    text(x+74, 428, doorNames[i], TEXT);
    char ago[24]; formatAgo(ago, sizeof(ago), lastRingAt[i]);
    text(x+74, 458, ago, fresh ? WARN : FAINT, FONT_CAPTION);
  }
  text(224, 554, demo ? "체험 모드: 실제 기기와 연결되지 않은 화면입니다"
                       : (online() ? "USB 브리지 연결됨" : "PC 브리지를 연결하면 실제 상태가 표시됩니다"),
       demo ? WARN : FAINT, FONT_CAPTION);
}

void roundButton(int cx,int cy,int r,IconId id) {
  circle(cx, cy, r, WHITE, 34);
  iconCenter(id, S32, cx, cy, TEXT);
}

void drawBoiler() {
  for(int i=0; i<ROOM_COUNT; ++i) {
    const int x = 224 + (i%2)*396, y = 126 + (i/2)*204, w = 380, h = 190;
    const RoomView& r = rooms[i];
    glass(x, y, w, h, 26);
    text(x+24, y+20, roomNames[i], TEXT);
    const char* state = !r.known ? "미확인" : (r.on ? "난방 중" : "꺼짐");
    const int sw = textWidth(state, FONT_CAPTION) + 28;
    shape(x+w-24-sw, y+18, sw, 32, 16, r.known && r.on ? HEAT : WHITE, r.known && r.on ? 256 : 30);
    centerText(x+w-24-sw, y+22, sw, state, r.known && r.on ? ON_HEAT : (r.known ? MUTED : FAINT), FONT_CAPTION);
    char cur[16]; formatDegrees(cur, sizeof(cur), r, false);
    text(x+24, y+66, "현재", FAINT, FONT_CAPTION);
    text(x+22, y+92, cur, r.known ? TEXT : FAINT, FONT_DISPLAY);
    char set[16]; formatDegrees(set, sizeof(set), r, true);
    rightText(x+w-24, y+96, "설정", FAINT, FONT_CAPTION);
    rightText(x+w-24, y+122, set, r.known ? MUTED : FAINT, FONT_TITLE);
  }
  char note[128];
  if(demo) snprintf(note, sizeof(note), "체험 모드: 예시 값입니다");
  else if(wallpadLive()) snprintf(note, sizeof(note), "월패드에서 읽은 값 · 조작은 기존 온도조절기에서 하세요");
  else snprintf(note, sizeof(note), "%s · EW11 %s", wallpadStatusText(), WALLPAD_HOST);
  text(224, 548, note, demo ? WARN : FAINT, FONT_CAPTION);
}

// Control cards: all the same size (smaller than the boiler cards), 3 x 3 grid.
constexpr int CARD_W = 249, CARD_H = 124, CARD_GAP = 14, CARD_COLS = 3, CARD_ROWS = 3;

void controlCard(int x,int y,const ControlCard& card) {
  const WallpadSnapshot& d = devices();
  glass(x, y, CARD_W, CARD_H, 22);
  char title[24], status[32], detail[40] = "";
  IconId glyph = ICON_POWER;
  bool active = false;
  uint16_t statusColor = MUTED;
  int dots = 0; uint8_t dotBits = 0;
  switch(card.kind) {
    case CARD_GAS:
      glyph = ICON_FLAME;
      snprintf(title, sizeof(title), "가스 밸브");
      snprintf(status, sizeof(status), "%s", d.gas.closed ? "잠김" : "열림");
      snprintf(detail, sizeof(detail), "원격은 잠그기만 가능");
      statusColor = d.gas.closed ? ACCENT : WARN;
      active = !d.gas.closed;
      break;
    case CARD_LIGHT: {
      const LightGroup& g = d.lights[card.index];
      glyph = ICON_SUN;
      snprintf(title, sizeof(title), "조명 %d", card.index + 1);
      const int on = __builtin_popcount(g.bits & ((1u << g.count) - 1));
      if(on) snprintf(status, sizeof(status), "%d/%d 켜짐", on, g.count); else snprintf(status, sizeof(status), "꺼짐");
      active = on > 0;
      statusColor = active ? HEAT : MUTED;
      dots = g.count; dotBits = g.bits;
      break;
    }
    case CARD_VENT:
      glyph = ICON_GEAR;
      snprintf(title, sizeof(title), "환기");
      if(!d.vent.on) snprintf(status, sizeof(status), "꺼짐");
      else if(d.vent.automatic) snprintf(status, sizeof(status), "자동");
      else snprintf(status, sizeof(status), "%u단", d.vent.speed);
      snprintf(detail, sizeof(detail), "%s", d.vent.automatic ? "자동·취침 모드" : "수동 모드");
      active = d.vent.on;
      statusColor = active ? HEAT : MUTED;
      break;
    case CARD_OUTLET: {
      const OutletState& o = d.outlets[card.index];
      snprintf(title, sizeof(title), "콘센트 %d", card.index + 1);
      snprintf(status, sizeof(status), "%s", o.mode == 0x10 ? "차단됨" : (o.mode == 0x11 ? "대기전력 차단" : "켜짐"));
      snprintf(detail, sizeof(detail), "%uW 사용 중", o.watts);
      active = o.mode != 0x10;
      statusColor = active ? HEAT : MUTED;
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
  if(dots) {  // one dot per switch in the light group
    for(int b=0; b<dots; ++b) {
      const int cx = x + 26 + b*22, cy = y + 98;
      if(dotBits & (1u << b)) circle(cx, cy, 7, HEAT); else shape(cx-7, cy-7, 14, 14, 7, MUTED, 200, 1.5f);
    }
  } else if(*detail) text(x+18, y+86, detail, FAINT, FONT_CAPTION);
}

void drawControl() {
  ControlCard cards[MAX_CARDS];
  const int n = controlCards(cards);
  const int shown = std::min(n, CARD_COLS * CARD_ROWS);
  for(int i=0; i<shown; ++i)
    controlCard(224 + (i % CARD_COLS)*(CARD_W + CARD_GAP), 126 + (i / CARD_COLS)*(CARD_H + CARD_GAP), cards[i]);
  char note[96];
  if(demo) snprintf(note, sizeof(note), "체험 모드: 예시 값입니다");
  else if(n > shown) snprintf(note, sizeof(note), "장치 %d개 중 %d개 표시 · 조작 기능은 아직 꺼져 있습니다", n, shown);
  else snprintf(note, sizeof(note), "월패드 버스에서 읽은 상태 · 조작 기능은 아직 꺼져 있습니다");
  text(224, 548, note, demo ? WARN : FAINT, FONT_CAPTION);
}

void drawCalls() {
  glass(224, 126, 776, 406, 26);
  icon(ICON_BELL, S24, 256, 150, MUTED);
  text(290, 148, "출입문 호출 기록", TEXT);
  const IntercomState& ic = devices().intercom;
  const char* phase = "일방 호출 · 최근 5건";
  if(ic.phase == INTERCOM_RINGING) phase = ic.ringDoor == 1 ? "현관 벨 울리는 중" : "공동현관 벨 울리는 중";
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
  glass(224, 126, 776, 406, 26);
  icon(ICON_SUN, S24, 256, 152, MUTED);
  text(290, 150, "화면 밝기", TEXT);
  char b[12]; snprintf(b, sizeof(b), "%d%%", brightness); rightText(968, 150, b, ACCENT);
  roundButton(286, 222, 28, ICON_MINUS);
  shape(332, 218, 584, 8, 4, WHITE, 40);
  const int fill = 584*brightness/100;
  shape(332, 218, fill, 8, 4, ACCENT);
  circle(332+fill, 222, 13, WHITE);
  roundButton(938, 222, 28, ICON_PLUS);
  divider(256, 276, 712);
  icon(ICON_CHECK, S24, 256, 298, touchOk ? ACCENT : WARN);
  text(290, 296, "터치", TEXT);
  rightText(968, 298, touchOk ? "정상" : "확인 필요", touchOk ? ACCENT : WARN, FONT_CAPTION);
  divider(256, 344, 712);
  icon(ICON_BUILDING, S24, 256, 362, wallpadLive() ? ACCENT : MUTED);
  text(290, 360, "월패드", TEXT);
  char link[96];
  if(wallpad.state==WALLPAD_CONNECTED) {
    const IPAddress ip(wallpad.ip);
    char medium[24];
    if(wallpad.wifi) snprintf(medium, sizeof(medium), "WiFi %ddBm", wallpad.rssi); else snprintf(medium, sizeof(medium), "유선");
    snprintf(link, sizeof(link), "%s · %s %s · 패킷 %lu", wallpadLive() ? "수신 중" : "신호 없음",
             medium, ip.toString().c_str(), (unsigned long)wallpad.packets);
  } else snprintf(link, sizeof(link), "%s", wallpadStatusText());
  rightText(968, 362, link, wallpadLive() ? ACCENT : MUTED, FONT_CAPTION);
  divider(256, 404, 712);
  button(256, 422, 256, 56, demo ? "체험 종료" : "체험 시작", demo ? WARN : WHITE, demo ? 256 : 34, demo ? ON_WARN : TEXT);
  if(demo) button(528, 422, 256, 56, "방문 호출 시험", WHITE, 34, TEXT, ICON_BELL);
  text(256, 492, "체험 모드는 화면에서만 동작하며 실제 기기를 제어하지 않습니다", FAINT, FONT_CAPTION);
}

void drawRingOverlay() {
  for(int i=0; i<SCREEN_W*SCREEN_H; ++i) frame[i] = blend(frame[i], BLACK, 150);
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

void render() {
  frame = panelBackBuffer();
  gfxSetTarget(frame);
  if(background) memcpy(frame, background, SCREEN_W*SCREEN_H*2);
  else if(BG_IMAGE) memcpy(frame, BG_IMAGE, SCREEN_W*SCREEN_H*2);
  else std::fill(frame, frame+SCREEN_W*SCREEN_H, GLASS);  // assets not uploaded
  drawSidebar();
  drawHeader();
  drawElevator();
  if(page==HOME) drawHome();
  else if(page==BOILER) drawBoiler();
  else if(page==CONTROL) drawControl();
  else if(page==CALLS) drawCalls();
  else drawSettings();
  if(activeRing>=0) drawRingOverlay();
  drawFlash();
  panelPresent();
  shownMinute = millis()/60000;
  dirty = false;
}

// ---------- input ----------
bool inside(int x,int y,int bx,int by,int w,int h) { return x>=bx && x<bx+w && y>=by && y<by+h; }
bool insideCircle(int x,int y,int cx,int cy,int r) { return (x-cx)*(x-cx)+(y-cy)*(y-cy) <= r*r; }

void tap(int x,int y) {
  dirty = true;
  if(activeRing>=0) { if(inside(x,y,382,412,260,68)) { flash(382,412,260,68); dismissRing(); } return; }
  if(inside(x,y,868,28,118,66)) { flash(868,28,118,66); callElevator(); return; }
  if(x<196 && y>=112) {
    int nav[PAGE_COUNT];
    const int count = navPages(nav), k = (y-112)/66;
    if(k < count) page = nav[k];
    return;
  }
  if(page==HOME) {
    if(inside(x,y,224,126,384,206)) page = BOILER;
    else if(inside(x,y,648,248,328,60)) { flash(648,248,328,60); callOffice(); }
    else if(inside(x,y,224,348,776,184)) page = CALLS;
  } else if(page==SETTINGS) {
    if(insideCircle(x,y,286,222,36)) { flash(258,194,56,56,28); brightness = std::max(10, brightness-10); panelBrightness(brightness); }
    if(insideCircle(x,y,938,222,36)) { flash(910,194,56,56,28); brightness = std::min(100, brightness+10); panelBrightness(brightness); }
    else if(inside(x,y,332,198,584,48)) { brightness = std::min(100, std::max(10, ((x-332)*100/584+5)/10*10)); panelBrightness(brightness); }
    if(inside(x,y,256,422,256,56)) { flash(256,422,256,56); setDemo(!demo); }
    if(demo && inside(x,y,528,422,256,56)) addRing(ringCount % 3);
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
    if(millis() - progressAt > 3000) return;  // host stopped reading: give up, don't hang the UI
    delay(1);
  }
  Serial.flush();
}

void serialLine(const String& line) {
  const char* l = line.c_str();
  int a, b, c; char extra;
  if(line=="SHOT") { sendScreenshot(); return; }
  if(line=="NET") { wallpadDiagnose(Serial, WIFI_SSID, false); return; }
  if(line=="NET SCAN") { wallpadDiagnose(Serial, WIFI_SSID, true); return; }
  if(line=="DEV") {
    const WallpadSnapshot w = wallpadSnapshot();
    Serial.printf("DEV|gas|%d|%d\n", w.gas.seen, w.gas.closed);
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
  if(sscanf(l, "TAP|%d|%d%c", &a, &b, &extra)==2) { tap(a, b); return; }
  if(demo) return;
  if(line=="PING") { lastBridge = millis(); bridge = true; dirty = true; return; }
  if(!online()) return;
  if(sscanf(l, "ELEV|%d|%d|%d%c", &a, &b, &c, &extra)==3 && a>=-9 && a<=199 && b>=-1 && b<=1 && (c==0||c==1)) {
    if(elev.calling && !c && a==elev.floor) elev.arrivedAt = millis();
    elev.floor = a; elev.dir = b; elev.calling = c;
    if(c) elev.pending = false;
  } else if(line=="ELEVERR") { elev.pending = false; elev.failedAt = millis(); }
  else if(sscanf(l, "OFFICE|%d%c", &a, &extra)==1 && (a==0||a==1)) {
    if(a && !office.calling) office.startedAt = millis();
    office.calling = a; if(a) office.pending = false;
  } else if(line=="OFFICEERR") { office.pending = false; office.failedAt = millis(); }
  else if(sscanf(l, "RING|%d%c", &a, &extra)==1 && a>=0 && a<3) addRing(a);
  else return;
  dirty = true;
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
  if(activeRing>=0 && nowMs()-activeRingAt > RING_SHOW_MS) dismissRing();
  if(flashUntil && int32_t(now - flashUntil) >= 0) { flashUntil = 0; dirty = true; }
  if(bridge && !online()) { bridge = false; clearLiveState(); dirty = true; }
  if(now/60000 != shownMinute) dirty = true;  // uptime and "N분 전" labels
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
  render();
  panelBrightness(brightness);
  Serial.println("HELLO|HomePanel|5");
}

void loop() {
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
      if(wallpad.elevatorCalls != seenCalls) { elev.calling = true; elev.pending = false; dirty = true; }
      if(wallpad.elevatorArrivals != seenArrivals) { elev.calling = false; elev.dir = 0; elev.arrivedAt = millis(); dirty = true; }
      if(wallpad.doorOpens != seenOpens) dirty = true;
    }
    seenBells = wallpad.doorbells; seenOpens = wallpad.doorOpens;
    seenCalls = wallpad.elevatorCalls; seenArrivals = wallpad.elevatorArrivals;
    refreshRooms();
    static uint32_t shownPackets = 0;  // settings page shows a live packet counter
    if(page==SETTINGS && wallpad.packets/10 != shownPackets/10) { shownPackets = wallpad.packets; dirty = true; }
  }
  static bool held = false;
  static uint32_t lastPressReport = 0;
  uint16_t x=0, y=0;
  const int report = touchOk ? touchRead(x, y) : -1;
  if(report==0) held = false;
  if(report==1) lastPressReport = millis();
  else if(held && millis() - lastPressReport > 400) held = false;
  if(report==1 && !held && millis()-lastPress > 180) { held = true; lastPress = millis(); tap(x, y); }
  // Redraw only on change; a static screen is never rewritten.
  if(dirty) render();
  delay(12);
}
