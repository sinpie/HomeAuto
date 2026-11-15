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

enum Page { HOME, BOILER, CALLS, SETTINGS, PAGE_COUNT };
const char* pageNames[] = {"홈", "보일러", "방문 호출", "설정"};
const IconId pageIcons[] = {ICON_HOME, ICON_FLAME, ICON_BELL, ICON_GEAR};
const char* doorNames[] = {"현관", "1층 현관", "지하주차장 문"};
const IconId doorIcons[] = {ICON_DOOR, ICON_BUILDING, ICON_PARKING};

struct Elevator { int floor=0, dir=0; bool calling=false, pending=false; uint32_t requestedAt=0, arrivedAt=0, failedAt=0; } elev;
// Heating rooms = wallpad heating groups 1..ROOM_COUNT. Group 1 is the living room
// (confirmed by a set-temperature sweep); the other names are placeholders.
constexpr int ROOM_COUNT = 4;
const char* roomNames[ROOM_COUNT] = {"거실", "방 2", "방 3", "방 4"};
constexpr uint32_t ZONE_STALE_MS = 15000;
struct RoomView { bool known, on; int set, current; };
RoomView rooms[ROOM_COUNT];
WallpadSnapshot wallpad = {};
uint32_t roomSignature = 0, lastWallpadPoll = 0;
bool wallpadLive();
const char* wallpadStatusText();
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
  for(int i=0; i<PAGE_COUNT; ++i) {
    const int y = 112 + i*66;
    const bool on = page==i;
    if(on) { shape(24, y, 160, 54, 16, WHITE, 30); shape(24, y+15, 4, 24, 2, ACCENT); }
    icon(pageIcons[i], S24, 42, y+15, on ? ACCENT : FAINT);
    text(78, y+(54-FONT_BODY.height)/2, pageNames[i], on ? TEXT : MUTED);
  }
  if(ringCount && page!=CALLS && rings[0].at > 0) circle(168, 112+CALLS*66+27, 5, WARN);
  char up[32]; const unsigned long m = millis()/60000;
  if(m<60) snprintf(up, sizeof(up), "가동 %lu분", m); else snprintf(up, sizeof(up), "가동 %lu시간", m/60);
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
  if(sig != roomSignature) { roomSignature = sig; dirty = true; }
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

void drawCalls() {
  glass(224, 126, 776, 406, 26);
  icon(ICON_BELL, S24, 256, 150, MUTED);
  text(290, 148, "출입문 호출 기록", TEXT);
  rightText(968, 150, "일방 호출 · 최근 5건", FAINT, FONT_CAPTION);
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
  if(x<196 && y>=112 && y<112+PAGE_COUNT*66) { page = (y-112)/66; return; }
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
