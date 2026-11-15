#include "wallpad_link.h"
#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <NetworkClient.h>

// Samsung SDS wallpad packets (as observed on this home's bus, 9600 8E1):
//   a packet starts with a byte >= 0x80; the last byte is the XOR of the
//   preceding bytes with bit 7 cleared.
//   heating status reply: B0 7C <group> <on 0/1> <set C> <current C> FF <chk>
// Nothing is ever written to the TCP socket: receive only.

namespace {
// Board Ethernet (vendor ethernet example): IP101 PHY, MDC 31, MDIO 52, reset 51,
// RMII reference clock input on GPIO50.
constexpr int PHY_ADDR = 1, PHY_MDC = 31, PHY_MDIO = 52, PHY_RESET = 51;
constexpr uint32_t WIFI_RETRY_MS = 20000;   // re-issue WiFi.begin() while not associated
constexpr uint32_t SILENT_RECONNECT_MS = 30000;  // the bus talks every ~0.8 s; silence = dead TCP
constexpr uint32_t ZONE_STALE_MS = 15000;   // drop heating values not refreshed for this long
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
  portENTER_CRITICAL(&lock);
  for(HeatZone& z : shared.zones)
    if(z.seen && now - z.at > ZONE_STALE_MS) z.seen = false;
  portEXIT_CRITICAL(&lock);
}

bool linkUp() { return useWifi ? WiFi.status() == WL_CONNECTED : ETH.linkUp(); }
IPAddress localIp() { return useWifi ? WiFi.localIP() : ETH.localIP(); }

bool checksumOk(const uint8_t* p, int n) {
  uint8_t x = 0;
  for(int i=0; i<n-1; ++i) x ^= p[i];
  return (x & 0x7F) == p[n-1];
}

void handlePacket(const uint8_t* p, int n) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&lock);
  ++packets;
  shared.packets = packets;
  shared.lastPacketAt = now;
  // Heating reply to status / on-off / set-temperature: B0 7C|7D|7F GR TG SET CUR FF PT
  if(n==8 && p[0]==0xB0 && (p[1]==0x7C || p[1]==0x7D || p[1]==0x7F) && p[2]>=1 && p[2]<=HEAT_ZONES && p[3]<=1) {
    HeatZone& z = shared.zones[p[2]-1];
    z.seen = true; z.on = p[3]==1; z.set = p[4]; z.current = p[5]; z.at = now;
  }
  portEXIT_CRITICAL(&lock);
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
void parse() {
  static const int lengths[] = {4, 5, 8, 6, 7};
  while(len > 0) {
    if(buf[0] < 0x80) { drop(1); countBad(1); continue; }
    bool found = false;
    for(int L : lengths) {
      if(len >= L+1 && buf[L] >= 0x80 && checksumOk(buf, L)) { handlePacket(buf, L); drop(L); found = true; break; }
    }
    if(found) continue;
    if(len >= 13) { drop(1); countBad(1); continue; }
    return;  // wait for more bytes
  }
}

// The last packet of a burst has no following header byte; accept it on idle.
void flushIdle() {
  static const int lengths[] = {4, 5, 8, 6, 7};
  for(int L : lengths)
    if(len == L && buf[0] >= 0x80 && checksumOk(buf, L)) { handlePacket(buf, L); len = 0; return; }
  if(len >= 13) { countBad(len); len = 0; }
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

WallpadSnapshot wallpadSnapshot() {
  portENTER_CRITICAL(&lock);
  WallpadSnapshot copy = shared;
  portEXIT_CRITICAL(&lock);
  return copy;
}
