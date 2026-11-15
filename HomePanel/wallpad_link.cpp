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

portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
WallpadSnapshot shared = {};
const char* targetHost = nullptr;
uint16_t targetPort = 0;
bool useWifi = false;

uint8_t buf[64];
int len = 0;
uint32_t packets = 0, badBytes = 0;

void setState(WallpadLinkState s, uint32_t ip) {
  const int8_t rssi = (useWifi && s >= WALLPAD_NO_IP) ? int8_t(WiFi.RSSI()) : 0;
  portENTER_CRITICAL(&lock);
  shared.state = s;
  shared.ip = ip;
  shared.wifi = useWifi;
  shared.rssi = rssi;
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
  for(;;) {
    if(!linkUp()) { client.stop(); setState(useWifi ? WALLPAD_NO_WIFI : WALLPAD_NO_CABLE, 0); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    const IPAddress ip = localIp();
    if(ip == IPAddress(0,0,0,0)) { client.stop(); setState(WALLPAD_NO_IP, 0); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    if(!client.connected()) {
      setState(WALLPAD_CONNECTING, uint32_t(ip));
      client.stop();
      len = 0;
      if(!client.connect(targetHost, targetPort, 3000)) { vTaskDelay(pdMS_TO_TICKS(3000)); continue; }
      client.setNoDelay(true);
    }
    setState(WALLPAD_CONNECTED, uint32_t(ip));
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

void wallpadBegin(const char* host, uint16_t port, const char* wifiSsid, const char* wifiPassword) {
  targetHost = host;
  targetPort = port;
  useWifi = wifiSsid && *wifiSsid;
  if(!host || !*host) { setState(WALLPAD_NOT_CONFIGURED, 0); return; }
  if(useWifi) {
    // WiFi runs on the ESP32-C6 coprocessor (ESP-Hosted); the driver reconnects on its own.
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSsid, wifiPassword);
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

WallpadSnapshot wallpadSnapshot() {
  portENTER_CRITICAL(&lock);
  WallpadSnapshot copy = shared;
  portEXIT_CRITICAL(&lock);
  return copy;
}
