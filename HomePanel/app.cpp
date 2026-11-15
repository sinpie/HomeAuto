#include <Arduino.h>
#include <algorithm>
#include "esp_heap_caps.h"
#include "board_config.h"
#include "panel.h"
#include "font_data.h"
#include "assets_data.h"
#include "wallpad_link.h"

// Wall panel UI: boiler, management office call, elevator call, one-way door calls.
// Boiler state is read directly from the wallpad RS485 bus via the EW11 (read-only).
// Elevator / office / door calls still come from the USB bridge (tools/ha_bridge.py).
// Unknown values are shown as unknown; no command is assumed to succeed.

namespace {
uint16_t* frame;
uint16_t* background;  // PSRAM copy of BG_IMAGE (faster than reading flash every frame)

constexpr uint16_t rgb(unsigned c) { return ((c>>8)&0xF800)|((c>>5)&0x07E0)|((c>>3)&0x001F); }
constexpr uint16_t WHITE=rgb(0xFFFFFF), BLACK=rgb(0x000000), GLASS=rgb(0x0B1230);
constexpr uint16_t TEXT=rgb(0xF8FAFC), MUTED=rgb(0xB6C2D9), FAINT=rgb(0x8C99B5);
constexpr uint16_t ACCENT=rgb(0x34D399), ON_ACCENT=rgb(0x052E21), WARN=rgb(0xFBBF24), ON_WARN=rgb(0x3B2503);
constexpr uint16_t DANGER=rgb(0xF87171), HEAT=rgb(0xFB923C), ON_HEAT=rgb(0x3A1503);

constexpr int UNKNOWN_TEMP = -9999;
constexpr uint32_t REPLY_TIMEOUT_MS = 6000, NOTICE_MS = 4000, RING_SHOW_MS = 60000;
constexpr int DEMO_HOME_FLOOR = 12;
enum IconSize { S24, S32, S56 };

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
struct Ring { uint8_t door; uint32_t at; };
Ring rings[8];
int ringCount = 0, activeRing = -1;
uint32_t activeRingAt = 0, lastRingAt[3] = {};

int page = HOME, brightness = 80;
bool demo = false, bridge = false, touchOk = false, dirty = true;
uint32_t lastBridge = 0, lastPress = 0, lastDemoTick = 0, shownMinute = 0;
int flashX, flashY, flashW, flashH, flashR; uint32_t flashUntil = 0;
String incoming;

// ---------- pixels ----------
inline uint16_t blend(uint16_t bg, uint16_t fg, unsigned a) {
  // a: 0..256. Channels blended separately (a packed red/blue multiply overflows into red).
  const unsigned ia = 256-a;
  const unsigned r = ((fg>>11)*a + (bg>>11)*ia) >> 8;
  const unsigned g = (((fg>>5)&63)*a + ((bg>>5)&63)*ia) >> 8;
  const unsigned b = ((fg&31)*a + (bg&31)*ia) >> 8;
  return uint16_t((r<<11) | (g<<5) | b);
}
inline void plot(int x,int y,uint16_t c,unsigned a) {
  if(unsigned(x)>=unsigned(SCREEN_W) || unsigned(y)>=unsigned(SCREEN_H) || !a) return;
  uint16_t& d = frame[y*SCREEN_W+x];
  d = a>=256 ? c : blend(d, c, a);
}
void span(int x,int y,int w,uint16_t c,unsigned a) {
  if(unsigned(y)>=unsigned(SCREEN_H)) return;
  const int x0 = std::max(0,x), x1 = std::min(SCREEN_W, x+w);
  uint16_t* p = frame + y*SCREEN_W;
  if(a>=256) std::fill(p+x0, p+x1, c);
  else for(int i=x0; i<x1; ++i) p[i] = blend(p[i], c, a);
}

// Anti-aliased rounded rectangle (signed distance). ring>0 draws an outline of that width.
void shape(int x,int y,int w,int h,int r,uint16_t c,unsigned alpha=256,float ring=0) {
  r = std::min(r, std::min(w,h)/2);
  const float hw = w*0.5f, hh = h*0.5f, cx = x+hw, cy = y+hh;
  for(int py=y; py<y+h; ++py) {
    if(unsigned(py)>=unsigned(SCREEN_H)) continue;
    const float dy = fabsf(py+0.5f-cy) - (hh-r);
    const bool cornerRow = dy > 0;
    if(!cornerRow && ring<=0) {  // straight band: only the left/right r pixels are rounded
      span(x, py, w, c, alpha);
      continue;
    }
    for(int px=x; px<x+w; ++px) {
      const float dx = fabsf(px+0.5f-cx) - (hw-r);
      const float ox = std::max(dx,0.f), oy = std::max(dy,0.f);
      const float d = sqrtf(ox*ox+oy*oy) + std::min(std::max(dx,dy),0.f) - r;
      float cov = std::min(1.f, std::max(0.f, 0.5f-d));
      if(ring>0) cov -= std::min(1.f, std::max(0.f, 0.5f-(d+ring)));
      if(cov>0) plot(px, py, c, unsigned(alpha*cov));
    }
  }
}
void circle(int cx,int cy,int r,uint16_t c,unsigned alpha=256) { shape(cx-r, cy-r, 2*r, 2*r, r, c, alpha); }
void glass(int x,int y,int w,int h,int r,unsigned alpha=168) {
  shape(x, y, w, h, r, GLASS, alpha);
  shape(x, y, w, h, r, WHITE, 36, 1.2f);  // hairline edge
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
void text(int x,int y,const char* s,uint16_t color,const Font& f=FONT_BODY) {
  while(*s) {
    const Glyph* g=findGlyph(f,nextChar(s));
    if(!g) { x+=f.height/2; continue; }
    const uint8_t* p=f.bitmap+g->offset;
    const int stride=(g->advance+1)/2;
    for(int yy=0; yy<f.height; ++yy, p+=stride)
      for(int xx=0; xx<g->advance; ++xx) {
        const unsigned a = (xx&1) ? (p[xx>>1]&15) : (p[xx>>1]>>4);
        if(a) plot(x+xx, y+yy, color, a==15 ? 256 : a*17);
      }
    x+=g->advance;
  }
}
void centerText(int x,int y,int w,const char* s,uint16_t c,const Font& f=FONT_BODY) { text(x+(w-textWidth(s,f))/2, y, s, c, f); }
void rightText(int right,int y,const char* s,uint16_t c,const Font& f=FONT_BODY) { text(right-textWidth(s,f), y, s, c, f); }

// ---------- icons ----------
void icon(IconId id,IconSize size,int x,int y,uint16_t color) {
  const int n = ICON_SIZES[size], stride = (n+1)/2;
  const uint8_t* p = ICON_BITMAP + ICON_OFFSET[id][size];
  for(int yy=0; yy<n; ++yy, p+=stride)
    for(int xx=0; xx<n; ++xx) {
      const unsigned a = (xx&1) ? (p[xx>>1]&15) : (p[xx>>1]>>4);
      if(a) plot(x+xx, y+yy, color, a==15 ? 256 : a*17);
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
void button(int x,int y,int w,int h,const char* label,uint16_t fill,unsigned alpha,uint16_t ink,int iconId=-1) {
  shape(x, y, w, h, std::min(20, h/2), fill, alpha);
  const int tw = textWidth(label, FONT_BODY) + (iconId>=0 ? 32 : 0);
  int tx = x + (w-tw)/2;
  if(iconId>=0) { iconCenter(IconId(iconId), S24, tx+12, y+h/2, ink); tx += 32; }
  text(tx, y+(h-FONT_BODY.height)/2, label, ink);
}

// ---------- state helpers ----------
bool online() { return bridge && millis()-lastBridge < 8000; }
bool recent(uint32_t at, uint32_t span) { return at && millis()-at < span; }
void flash(int x,int y,int w,int h,int r=18) { flashX=x; flashY=y; flashW=w; flashH=h; flashR=r; flashUntil=millis()+160; }
void formatTemp(char* out,size_t n,int x10) {
  if(x10==UNKNOWN_TEMP) { snprintf(out, n, "--.-°"); return; }
  snprintf(out, n, "%s%d.%d°", x10<0?"-":"", abs(x10)/10, abs(x10)%10);
}
void formatFloor(char* out,size_t n,int floor) {
  if(floor==0) snprintf(out, n, "--");
  else if(floor<0) snprintf(out, n, "B%d", -floor);
  else snprintf(out, n, "%d", floor);
}
void formatAgo(char* out,size_t n,uint32_t at) {
  if(!at) { snprintf(out, n, "기록 없음"); return; }
  const uint32_t s=(millis()-at)/1000;
  if(s<60) snprintf(out, n, "방금");
  else if(s<3600) snprintf(out, n, "%lu분 전", (unsigned long)(s/60));
  else snprintf(out, n, "%lu시간 전", (unsigned long)(s/3600));
}
void clearLiveState() { elev = Elevator(); office = Office(); }
void setDemo(bool on) {
  demo = on; bridge = false; clearLiveState();
  if(demo) elev.floor = 1;
  Serial.println("SYNC");
  dirty = true;
}
void addRing(int door) {
  const int keep = std::min(ringCount, 7);
  std::move_backward(rings, rings+keep, rings+keep+1);
  rings[0] = {uint8_t(door), millis()};
  ringCount = std::min(ringCount+1, 8);
  lastRingAt[door] = millis();
  activeRing = door; activeRingAt = millis();
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
    const bool fresh = recent(lastRingAt[i], 10*60000);
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
  char note[80];
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
    snprintf(link, sizeof(link), "%s · %s → EW11 · 패킷 %lu", wallpadLive() ? "수신 중" : "신호 없음",
             ip.toString().c_str(), (unsigned long)wallpad.packets);
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
  if(background) memcpy(frame, background, SCREEN_W*SCREEN_H*2);
  else memcpy(frame, BG_IMAGE, SCREEN_W*SCREEN_H*2);
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
    if(inside(x,y,332,198,584,48)) { brightness = std::min(100, std::max(10, ((x-332)*100/584+5)/10*10)); panelBrightness(brightness); }
    if(inside(x,y,256,422,256,56)) { flash(256,422,256,56); setDemo(!demo); }
    if(demo && inside(x,y,528,422,256,56)) addRing(ringCount % 3);
  }
}

// ---------- serial: bridge protocol + debug ----------
void sendScreenshot() {
  // Debug: raw RGB565 little-endian of the visible frame.
  Serial.printf("SHOT|%d|%d\n", SCREEN_W, SCREEN_H);
  Serial.flush();
  const uint8_t* p = reinterpret_cast<const uint8_t*>(panelFrontBuffer());
  const size_t total = SCREEN_W*SCREEN_H*2;
  for(size_t sent=0; sent<total; ) {
    const size_t n = Serial.write(p+sent, std::min<size_t>(4096, total-sent));
    if(!n) delay(1);
    sent += n;
  }
  Serial.flush();
}

void serialLine(const String& line) {
  const char* l = line.c_str();
  int a, b, c; char extra;
  if(line=="SHOT") { sendScreenshot(); return; }
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
  if(activeRing>=0 && now-activeRingAt > RING_SHOW_MS) dismissRing();
  if(flashUntil && now > flashUntil) { flashUntil = 0; dirty = true; }
  if(bridge && !online()) { bridge = false; clearLiveState(); dirty = true; }
  if(now/60000 != shownMinute) dirty = true;  // uptime and "N분 전" labels
}
}

void setup() {
  Serial.begin(115200);
  incoming.reserve(128);
  if(!panelBegin()) { Serial.println("ERROR|LCD init failed"); return; }
  background = static_cast<uint16_t*>(heap_caps_malloc(SCREEN_W*SCREEN_H*2, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(background) memcpy(background, BG_IMAGE, SCREEN_W*SCREEN_H*2);
  frame = panelBackBuffer();
  touchOk = touchBegin();
  wallpadBegin(WALLPAD_HOST, WALLPAD_PORT);
  render();
  panelBrightness(brightness);
  Serial.println("HELLO|HomePanel|4");
}

void loop() {
  while(Serial.available()) {
    char c = Serial.read();
    if(c=='\n') { serialLine(incoming); incoming = ""; }
    else if(c!='\r') { if(incoming.length()<127) incoming += c; else incoming = ""; }
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
  uint16_t x=0, y=0;
  const int report = touchOk ? touchRead(x, y) : -1;
  if(report==0) held = false;
  if(report==1 && !held && millis()-lastPress > 180) { held = true; lastPress = millis(); tap(x, y); }
  // Redraw only on change; a static screen is never rewritten.
  if(dirty) render();
  delay(12);
}
