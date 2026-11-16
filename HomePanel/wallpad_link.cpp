#include "wallpad_link.h"
#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <NetworkClient.h>
#include <initializer_list>

// Samsung SDS wallpad packets (as observed on this home's bus, 9600 8E1):
//   a packet starts with a byte >= 0x80; the last byte is the XOR of the
//   preceding bytes with bit 7 cleared.
//   heating status reply: B0 7C <group> <on 0/1> <set C> <current C> FF <chk>
// The socket is only written by the command sender: the device commands built by
// the wallpad*() functions below, one at a time, on explicit request.

namespace {
// Board Ethernet (vendor ethernet example): IP101 PHY, MDC 31, MDIO 52, reset 51,
// RMII reference clock input on GPIO50.
constexpr int PHY_ADDR = 1, PHY_MDC = 31, PHY_MDIO = 52, PHY_RESET = 51;
constexpr uint32_t WIFI_RETRY_MS = 20000;   // re-issue WiFi.begin() while not associated
constexpr uint32_t SILENT_RECONNECT_MS = 30000;  // the bus talks every ~0.8 s; silence = dead TCP
constexpr uint32_t ZONE_STALE_MS = 15000;   // drop heating values not refreshed for this long
constexpr uint32_t DEVICE_STALE_MS = 30000; // other devices: drop state not refreshed for this long
constexpr uint32_t DOORBELL_REPEAT_MS = 15000;  // the same ring is repeated to every intercom header
constexpr uint32_t RSSI_PERIOD_MS = 5000;   // RSSI is a request to the C6: keep it rare
constexpr uint8_t REASON_ASSOC_LEAVE = 8;   // our own WiFi.disconnect()

portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
WallpadSnapshot shared = {};
const char* targetHost = nullptr;
uint16_t targetPort = 0;
bool useWifi = false;
const char* wifiSsid = nullptr;
const char* wifiPassword = nullptr;
volatile uint8_t wifiReason = 0;     // last disconnect reason reported by the AP / driver
volatile bool ownDisconnect = false;
volatile uint32_t wifiAttempts = 0;
uint32_t wifiStartedAt = 0;

uint8_t buf[64];
int len = 0;
uint32_t packets = 0, badBytes = 0;
// Last wallpad -> device packet: a device reply (B0 ..) is interpreted in its context.
uint8_t queryHead = 0, queryCmd = 0, queryArg = 0;
uint32_t lastRingAt[3] = {};

// Pending device command (one at a time). Sent in a free bus slot, retried until the
// addressed device answers with the expected reply command.
constexpr uint8_t CMD_MAX_ATTEMPTS = 5;
constexpr uint32_t CMD_REPLY_MS = 1500;
uint8_t cmdPacket[10];
int cmdLength = 0;
uint8_t expectReply1 = 0, expectReply2 = 0, expectGroup = 0xFF;  // 0xFF: any group
volatile bool cmdRequested = false;
uint32_t cmdSentAt = 0;
bool sendSlot = false;  // the wallpad just polled a device that does not answer: the bus is free

void startWifi() {
  ownDisconnect = true;  // the reason-8 event this causes is not the failure we want to see
  WiFi.disconnect();
  WiFi.begin(wifiSsid, wifiPassword);
  wifiStartedAt = millis();
  ++wifiAttempts;
}

// Publish state only when it changes (called from the loop every few ms).
void setState(WallpadLinkState s, uint32_t ip) {
  portENTER_CRITICAL(&lock);
  shared.state = s;
  shared.ip = ip;
  shared.wifi = useWifi;
  if(s < WALLPAD_NO_IP) shared.rssi = 0;
  portEXIT_CRITICAL(&lock);
}

void updateRssi() {
  static uint32_t last = 0;
  if(!useWifi || millis() - last < RSSI_PERIOD_MS) return;
  last = millis();
  const int8_t rssi = int8_t(WiFi.RSSI());
  portENTER_CRITICAL(&lock);
  shared.rssi = rssi;
  portEXIT_CRITICAL(&lock);
}

// Forget heating values that are no longer refreshed (also avoids millis() wrap
// making a 49.7-day-old value look fresh).
void expireZones() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if(now - last < 1000) return;
  last = now;
  auto stale = [now](bool& seen, uint32_t at, uint32_t limit) { if(seen && now - at > limit) seen = false; };
  portENTER_CRITICAL(&lock);
  for(HeatZone& z : shared.zones) stale(z.seen, z.at, ZONE_STALE_MS);
  stale(shared.gas.seen, shared.gas.at, DEVICE_STALE_MS);
  for(LightGroup& l : shared.lights) stale(l.seen, l.at, DEVICE_STALE_MS);
  for(OutletState& o : shared.outlets) stale(o.seen, o.at, DEVICE_STALE_MS);
  stale(shared.vent.seen, shared.vent.at, DEVICE_STALE_MS);
  for(EnergyState& e : shared.energy) stale(e.seen, e.at, DEVICE_STALE_MS);
  stale(shared.intercom.seen, shared.intercom.at, DEVICE_STALE_MS);
  stale(shared.entrance.seen, shared.entrance.at, DEVICE_STALE_MS);
  // A ring or call that is no longer refreshed by the wallpad is over.
  if(shared.intercom.phase != INTERCOM_IDLE && now - shared.intercom.at > 60000) shared.intercom.phase = INTERCOM_IDLE;
  portEXIT_CRITICAL(&lock);
}

bool linkUp() { return useWifi ? WiFi.status() == WL_CONNECTED : ETH.linkUp(); }
IPAddress localIp() { return useWifi ? WiFi.localIP() : ETH.localIP(); }

bool checksumOk(const uint8_t* p, int n) {
  uint8_t x = 0;
  for(int i=0; i<n-1; ++i) x ^= p[i];
  return (x & 0x7F) == p[n-1];
}

uint32_t bcd3(const uint8_t* d) {
  uint32_t v = 0;
  for(int i=0; i<3; ++i) v = v*100 + (d[i]>>4)*10 + (d[i]&15);
  return v;
}

bool isIntercom(uint8_t head) { return head >= 0xA1 && head <= 0xA6; }

// Wallpad -> device packet. Records the query context and the events the wallpad
// announces to intercoms and entrance switches.
void wallpadPacket(const uint8_t* p, int n, uint32_t now) {
  queryHead = p[0]; queryCmd = p[1]; queryArg = n > 2 ? p[2] : 0;
  const uint8_t head = p[0], cmd = p[1];
  sendSlot = cmd == 0x5A && head != 0xAE;

  if(isIntercom(head) && n == 4) {
    IntercomState& ic = shared.intercom;
    switch(cmd) {
      case 0x31: case 0x32: {  // doorbell: 31 home door, 32 common door (repeated to every intercom)
        const uint8_t door = cmd == 0x31 ? 1 : 2;
        if(!lastRingAt[door] || now - lastRingAt[door] > DOORBELL_REPEAT_MS) {
          ++shared.doorbells;
          shared.lastDoorbell = door;
        }
        lastRingAt[door] = now;
        ic.phase = INTERCOM_RINGING; ic.ringDoor = door; ic.at = now;
        break;
      }
      case 0x3E: ic.phase = INTERCOM_IDLE; ic.at = now; break;                         // bell off
      case 0x42: if(ic.phase != INTERCOM_CALL) { ic.phase = INTERCOM_BUSY; ic.at = now; } break;  // other intercom busy
      case 0x35: case 0x36: case 0x38: ic.phase = INTERCOM_CALL; ic.at = now; break;   // video / accepted / video call
      case 0x3B:                                                                        // door opened (GY 00 home, 01 common)
        ++shared.doorOpens;
        shared.lastDoorOpen = p[2] == 0 ? 1 : 2;
        break;
    }
    return;
  }

  EntranceState& en = shared.entrance;
  if(head == 0xAD) {  // old entrance switch
    if(cmd == 0x56 && n == 4 && p[2] <= 1) en.gasClosed = p[2];           // gas state pushed to the switch
    else if(cmd == 0x55 && n == 4) en.gasClosed = 1;                       // gas close confirmed
    else if(cmd == 0x2F && n == 4) ++shared.elevatorCalls;                 // elevator call confirmed
    else if(cmd == 0x54 && n == 4) en.lightsAllOff = p[2] == 0;            // batch light switch confirmed
    return;
  }
  if(head == 0xCC) {  // new entrance switch
    if(cmd == 0x01 && n >= 10 && p[2] == 0x16) {                           // clock push: YY MM DD HH NN SS
      if(p[4] >= 1 && p[4] <= 12 && p[5] >= 1 && p[5] <= 31 && p[6] < 24 && p[7] < 60 && p[8] < 60)
        shared.clock = {true, p[3], p[4], p[5], p[6], p[7], p[8], now};
    }
    else if(cmd == 0x0B && n >= 5 && p[3] <= 1) en.gasClosed = p[3] == 0 ? 1 : 0;  // CC 0B 04 YY: 00 closed
    else if(cmd == 0x12) ++shared.elevatorArrivals;                        // elevator arrived
    else if(cmd == 0x10) ++shared.elevatorCalls;                           // elevator call confirmed
    else if(cmd == 0x08 && n >= 5 && p[3] <= 1) en.away = p[3] == 0;       // away mode: 00 set, 01 released
    else if(cmd == 0x13 || cmd == 0x02) en.gasClosed = 1;                  // gas close confirmed
  }
}

// Device -> wallpad reply (B0 ..), read in the context of the preceding query.
void deviceReply(const uint8_t* p, int n, uint32_t now) {
  sendSlot = false;
  CommandStatus& cmd = shared.command;
  if(cmd.state == CMD_SENDING && n >= 3 && (p[1] == expectReply1 || p[1] == expectReply2) &&
     (expectGroup == 0xFF || p[2] == expectGroup)) {
    cmd.state = CMD_DONE;
    cmd.at = now;
    if(cmd.kind == CMD_GAS_CLOSE) { shared.gas.seen = true; shared.gas.at = now; shared.gas.closedAt = now; return; }
    // Our own command is not echoed back, so read the reply in its context.
    queryHead = cmdPacket[0]; queryCmd = cmdPacket[1]; queryArg = cmdPacket[2];
  }
  // Heating: B0 7C|7D|7F GR TG SET CUR FF PT (carries its own group id)
  if(n==8 && (p[1]==0x7C || p[1]==0x7D || p[1]==0x7F) && p[2]>=1 && p[2]<=HEAT_ZONES && p[3]<=1) {
    HeatZone& z = shared.zones[p[2]-1];
    z.seen = true; z.on = p[3]==1; z.set = p[4]; z.current = p[5]; z.at = now;
    return;
  }
  if(isIntercom(queryHead)) {  // an intercom answers
    IntercomState& ic = shared.intercom;
    ic.seen = true;
    if(p[1] == 0x42 || p[1] == 0x36) { ic.phase = INTERCOM_CALL; ic.at = now; }
    else if(ic.at == 0) ic.at = now;
    return;
  }
  switch(queryHead) {
    case 0xAD: case 0xCC: {  // an entrance switch answers
      EntranceState& en = shared.entrance;
      en.seen = true; en.at = now;
      if(queryHead == 0xAD && queryCmd == 0x52 && p[1] == 0x52 && n == 4) en.lightsAllOff = p[2] == 0;
      break;
    }
    case 0xAB:  // gas valve: AB 41 00 -> B0 41 XX (00 open, 01 closed); AB 78 (close) -> B0 78
      // AB 41 -> B0 41 XX: the controller is present (XX did not follow the valve here)
      if(n==4 && queryCmd==0x41 && p[1]==0x41) { shared.gas.seen = true; shared.gas.at = now; }
      else if(n==4 && queryCmd==0x78 && p[1]==0x78) { shared.gas.seen = true; shared.gas.at = now; shared.gas.closedAt = now; }
      break;
    case 0xAC:  // lights
      if(n==5 && queryCmd==0x79 && p[1]==0x79) {
        // AC 79 00 GR -> B0 79 RN BM (RN = count<<4 | room); older: AC 79 GR 00 -> B0 79 GR TG
        int index; uint8_t count, bits;
        if(queryArg == 0) { index = (p[2] & 15) - 1; count = p[2] >> 4; bits = p[3]; }
        else { index = p[2] - 1; count = 1; bits = p[3] ? 1 : 0; }
        if(index >= 0 && index < LIGHT_GROUPS && count >= 1 && count <= 8) shared.lights[index] = {true, count, bits, now};
      } else if(n==5 && queryCmd==0x7A && (p[1]==0x79 || p[1]==0x7A)) {
        // reply to a light command AC 7A GR TG: GR 00 = all lights
        if(p[2] == 0 && p[3] == 0) {
          for(LightGroup& l : shared.lights) if(l.seen) { l.bits = 0; l.at = now; }
        } else if(p[2] >= 1 && p[2] <= LIGHT_GROUPS) {
          LightGroup& l = shared.lights[p[2]-1];
          if(!l.seen || l.count == 1) l = {true, 1, uint8_t(p[3] ? 1 : 0), now};
        }
      }
      break;
    case 0xC6:  // outlets: C6 4A GR .. -> B0 4A GR XX WW WW 00 ZZ 00 PT (XX 01 on, 11 standby-cut armed, 10 cut)
      if(n==10 && p[2]>=1 && p[2]<=OUTLETS) {
        OutletState& o = shared.outlets[p[2]-1];
        if(queryCmd==0x4A && p[1]==0x4A) o = {true, p[3], uint16_t(p[4]<<8 | p[5]), now};
        else if(queryCmd==0x6E && p[1]==0x6E && o.seen) { o.mode = p[3] ? 0x01 : 0x10; o.at = now; }    // power on/off
        else if(queryCmd==0x4B && p[1]==0x4B && o.seen) { o.mode = p[3] ? 0x11 : 0x01; o.at = now; }    // standby cut
      }
      break;
    case 0xC2:  // ventilation
      if(n==6 && queryCmd==0x4E && p[1]==0x4E && p[2]<=3) {
        // B0 4E XX MM YY: XX 03/02/01 = speed 1/2/3, 00 off; MM 01 auto/night; YY 00 = on
        const uint8_t speed = p[2] ? uint8_t(4 - p[2]) : 0;
        shared.vent = {true, p[4]==0, speed, p[3]==1, now};
      } else if(n==6 && queryCmd==0x4F && p[1]==0x4F && shared.vent.seen) {
        // command reply B0 4F ZZ: 06 off, 05 on, 04 auto, 03/02/01 speed 1/2/3
        VentState& v = shared.vent;
        switch(p[2]) {
          case 0x06: v.on = false; break;
          case 0x05: v.on = true; break;
          case 0x04: v.on = true; v.automatic = true; break;
          case 0x01: case 0x02: case 0x03: v.on = true; v.automatic = false; v.speed = uint8_t(4 - p[2]); break;
        }
        v.at = now;
      }
      break;
    case 0xAA:  // energy meter: AA 6F YY -> B0 6F YY ZZ ZZ ZZ (decimal digits; 00 power W, 01 gas, 02 water)
      if(n==7 && queryCmd==0x6F && p[1]==0x6F && p[2]<=2) shared.energy[p[2]] = {true, bcd3(p+3), now};
      break;
  }
}

void handlePacket(const uint8_t* p, int n) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&lock);
  ++packets;
  shared.packets = packets;
  shared.lastPacketAt = now;
  if(p[0] == 0xB0) deviceReply(p, n, now); else wallpadPacket(p, n, now);
  portEXIT_CRITICAL(&lock);
}

// Packet length expected from context (outlet packets are 10 bytes, fan 6, meter 7).
int expectedLength() {
  if(len < 2) return 0;
  const uint8_t head = buf[0], cmd = buf[1];
  if(head == 0xC6 && (cmd == 0x4A || cmd == 0x4B || cmd == 0x6E)) return 10;
  if(head == 0xC2 && (cmd == 0x4E || cmd == 0x4F)) return 6;
  if(head == 0xAA && cmd == 0x6F) return 4;
  if(head == 0xCC && cmd == 0x01) return 23;      // clock push: CC 01 16 YY MM DD HH NN SS U1 00x15... PT
  if(head == 0xB0) {
    if(queryHead == 0xC6 && (cmd == 0x4A || cmd == 0x4B || cmd == 0x6E)) return 10;
    if(queryHead == 0xC2 && (cmd == 0x4E || cmd == 0x4F)) return 6;
    if(queryHead == 0xAA && cmd == 0x6F) return 7;
  }
  return 0;
}

void drop(int n) {
  memmove(buf, buf+n, len-n);
  len -= n;
}

void countBad(int n) {
  badBytes += n;
  portENTER_CRITICAL(&lock);
  shared.badBytes = badBytes;
  portEXIT_CRITICAL(&lock);
}

// Split the byte stream. A packet is accepted when its checksum is valid and the
// next byte starts a new packet (or the line went idle, see flushIdle()).
const int LENGTHS[] = {4, 5, 8, 6, 7, 9, 10};

void parse() {
  while(len > 0) {
    if(buf[0] < 0x80) { drop(1); countBad(1); continue; }
    const int hint = expectedLength();
    if(hint && len < hint+1) return;  // wait: a shorter prefix could pass the checksum by chance
    if(hint && buf[hint] >= 0x80 && checksumOk(buf, hint)) { handlePacket(buf, hint); drop(hint); continue; }
    bool found = false;
    for(int L : LENGTHS) {
      if(len >= L+1 && buf[L] >= 0x80 && checksumOk(buf, L)) { handlePacket(buf, L); drop(L); found = true; break; }
    }
    if(found) continue;
    if(len >= (hint > 12 ? hint + 1 : 13)) { drop(1); countBad(1); continue; }
    return;  // wait for more bytes
  }
}

// The last packet of a burst has no following header byte; accept it on idle.
void flushIdle() {
  const int hint = expectedLength();
  if(hint && len == hint && buf[0] >= 0x80 && checksumOk(buf, hint)) { handlePacket(buf, hint); len = 0; return; }
  for(int L : LENGTHS)
    if(len == L && buf[0] >= 0x80 && checksumOk(buf, L)) { handlePacket(buf, L); len = 0; return; }
  if(len >= 24) { countBad(len); len = 0; }
}

void feed(uint8_t b) {
  if(len == sizeof(buf)) { countBad(1); drop(1); }
  buf[len++] = b;
  parse();
}

void linkTask(void*) {
  NetworkClient client;
  uint32_t lastByte = 0;
  WallpadLinkState state = WALLPAD_NOT_CONFIGURED;
  uint32_t stateIp = 0;
  auto publish = [&](WallpadLinkState s, uint32_t ip) {
    if(s != state || ip != stateIp) { state = s; stateIp = ip; setState(s, ip); }
  };
  for(;;) {
    expireZones();
    if(!linkUp()) {
      client.stop();
      publish(useWifi ? WALLPAD_NO_WIFI : WALLPAD_NO_CABLE, 0);
      // A single WiFi.begin() at boot can be lost (coprocessor link still starting,
      // AP busy). Retry periodically until associated.
      if(useWifi && millis() - wifiStartedAt > WIFI_RETRY_MS) startWifi();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    const IPAddress ip = localIp();
    if(ip == IPAddress(0,0,0,0)) { client.stop(); publish(WALLPAD_NO_IP, 0); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    updateRssi();
    if(client.connected() && millis() - lastByte > SILENT_RECONNECT_MS) client.stop();  // EW11 gone
    if(!client.connected()) {
      publish(WALLPAD_CONNECTING, uint32_t(ip));
      client.stop();
      len = 0;
      if(!client.connect(targetHost, targetPort, 3000)) { vTaskDelay(pdMS_TO_TICKS(3000)); continue; }
      client.setNoDelay(true);
      lastByte = millis();
    }
    publish(WALLPAD_CONNECTED, uint32_t(ip));
    if(cmdRequested) {
      portENTER_CRITICAL(&lock);
      const bool waiting = shared.command.state == CMD_SENDING;
      const uint8_t attempts = shared.command.attempts;
      portEXIT_CRITICAL(&lock);
      if(!waiting) cmdRequested = false;  // answered (done) by the parser
      else if(attempts == 0 || millis() - cmdSentAt > CMD_REPLY_MS) {
        if(attempts >= CMD_MAX_ATTEMPTS) {
          portENTER_CRITICAL(&lock);
          shared.command.state = CMD_FAILED;
          shared.command.at = millis();
          portEXIT_CRITICAL(&lock);
          cmdRequested = false;
        } else if(sendSlot) {
          client.write(cmdPacket, cmdLength);
          cmdSentAt = millis();
          sendSlot = false;
          portENTER_CRITICAL(&lock);
          ++shared.command.attempts;
          portEXIT_CRITICAL(&lock);
        }
      }
    }
    const int available = client.available();
    if(available > 0) {
      uint8_t chunk[128];
      const int n = client.read(chunk, std::min(available, int(sizeof(chunk))));
      for(int i=0; i<n; ++i) feed(chunk[i]);
      lastByte = millis();
    } else {
      if(len && millis()-lastByte > 30) flushIdle();
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}
}

void wallpadBegin(const char* host, uint16_t port, const char* ssid, const char* password) {
  targetHost = host;
  targetPort = port;
  useWifi = ssid && *ssid;
  shared.entrance.gasClosed = shared.entrance.away = shared.entrance.lightsAllOff = -1;  // unknown until announced
  if(!host || !*host) { setState(WALLPAD_NOT_CONFIGURED, 0); return; }
  if(useWifi) {
    // WiFi runs on the ESP32-C6 coprocessor (ESP-Hosted).
    wifiSsid = ssid;
    wifiPassword = password;
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
      const uint8_t reason = info.wifi_sta_disconnected.reason;
      if(reason == REASON_ASSOC_LEAVE && ownDisconnect) { ownDisconnect = false; return; }
      wifiReason = reason;
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    startWifi();
    // The first begin() is usually lost while the ESP-Hosted link to the C6 is still
    // starting (seen on this board: the retry connected). Retry it after ~6 s.
    wifiStartedAt = millis() - (WIFI_RETRY_MS - 6000);
    setState(WALLPAD_NO_WIFI, 0);
  } else {
    if(!ETH.begin(ETH_PHY_IP101, PHY_ADDR, PHY_MDC, PHY_MDIO, PHY_RESET, EMAC_CLK_EXT_IN)) {
      setState(WALLPAD_NO_ETHERNET, 0);
      return;
    }
    setState(WALLPAD_NO_CABLE, 0);
  }
  xTaskCreate(linkTask, "wallpad", 6144, nullptr, 4, nullptr);
}

void wallpadDiagnose(Print& out, const char* targetSsid, bool scan) {
  const WallpadSnapshot s = wallpadSnapshot();
  out.printf("NET|mode|%s\n", useWifi ? "wifi" : "ethernet");
  out.printf("NET|state|%d|packets|%lu|bad|%lu\n", s.state, (unsigned long)s.packets, (unsigned long)s.badBytes);
  if(!useWifi) {
    out.printf("NET|eth|link|%d|ip|%s\n", ETH.linkUp(), ETH.localIP().toString().c_str());
    out.println("NET|done");
    return;
  }
  out.printf("NET|wifi|status|%d|ip|%s|rssi|%d\n", int(WiFi.status()), WiFi.localIP().toString().c_str(), int(s.rssi));
  // Reason codes (esp_wifi_types.h): 15 4-way handshake timeout / 204 handshake timeout
  // usually mean a wrong password; 2 auth expired; 201 no AP found; 202 auth fail;
  // 203 association fail; 205 connection fail.
  out.printf("NET|wifi|attempts|%lu|last_disconnect_reason|%u\n", (unsigned long)wifiAttempts, unsigned(wifiReason));
  if(scan) {
    // Blocking scan; may abort a connection attempt in progress. Use only for diagnosis.
    const int n = WiFi.scanNetworks();
    out.printf("NET|scan|count|%d\n", n);
    for(int i=0; i<n; ++i)
      if(WiFi.SSID(i) == targetSsid)
        out.printf("NET|scan|target|rssi|%d|channel|%d|auth|%d\n", int(WiFi.RSSI(i)), int(WiFi.channel(i)), int(WiFi.encryptionType(i)));
    WiFi.scanDelete();
  }
  out.println("NET|done");
}

// Queue one command packet (checksum appended). Only the builders below call this.
bool startCommand(CommandKind kind, uint8_t target, std::initializer_list<uint8_t> body,
                  uint8_t reply1, uint8_t reply2, uint8_t group) {
  if(body.size() + 1 > sizeof(cmdPacket)) return false;
  bool ok = false;
  portENTER_CRITICAL(&lock);
  if(shared.state == WALLPAD_CONNECTED && shared.command.state != CMD_SENDING) {
    int n = 0; uint8_t x = 0;
    for(uint8_t b : body) { cmdPacket[n++] = b; x ^= b; }
    cmdPacket[n++] = x & 0x7F;
    cmdLength = n;
    expectReply1 = reply1; expectReply2 = reply2; expectGroup = group;
    shared.command = {CMD_SENDING, kind, target, 0, millis()};
    ok = true;
  }
  portEXIT_CRITICAL(&lock);
  if(ok) cmdRequested = true;
  return ok;
}

bool wallpadCloseGas() {
  return startCommand(CMD_GAS_CLOSE, 0, {0xAB, 0x78, 0x00}, 0x78, 0x78, 0xFF);
}
bool wallpadHeatingPower(uint8_t group, bool on) {
  if(group < 1 || group > HEAT_ZONES) return false;
  return startCommand(CMD_HEAT_POWER, group, {0xAE, 0x7D, group, uint8_t(on), 0, 0, 0}, 0x7D, 0x7D, group);
}
bool wallpadHeatingSet(uint8_t group, uint8_t celsius) {
  if(group < 1 || group > HEAT_ZONES || celsius < 10 || celsius > 30) return false;
  return startCommand(CMD_HEAT_SET, group, {0xAE, 0x7F, group, celsius, 0, 0, 0}, 0x7F, 0x7F, group);
}
bool wallpadLight(uint8_t group, bool on) {
  if(group > LIGHT_GROUPS || (group == 0 && on)) return false;
  return startCommand(CMD_LIGHT, group, {0xAC, 0x7A, group, uint8_t(on)}, 0x79, 0x7A, group);
}
bool wallpadOutletPower(uint8_t outlet, bool on) {
  if(outlet < 1 || outlet > OUTLETS) return false;
  return startCommand(CMD_OUTLET, outlet, {0xC6, 0x6E, outlet, uint8_t(on), 0, 0, 0, 0, 0}, 0x6E, 0x6E, outlet);
}
bool wallpadVent(uint8_t code) {
  if(code < 1 || code > 6) return false;
  return startCommand(CMD_VENT, code, {0xC2, 0x4F, code, 0, 0}, 0x4F, 0x4F, 0xFF);
}

WallpadSnapshot wallpadSnapshot() {
  portENTER_CRITICAL(&lock);
  WallpadSnapshot copy = shared;
  portEXIT_CRITICAL(&lock);
  return copy;
}
