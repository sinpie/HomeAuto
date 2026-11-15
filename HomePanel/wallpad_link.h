#pragma once
#include <stdint.h>

// Read-only link to the wallpad RS485 bus through an EW11 (RS485 -> TCP server),
// over WiFi (ESP32-C6 coprocessor) when an SSID is configured, otherwise over the
// board's Ethernet port. This module never transmits to the bus.

constexpr int HEAT_ZONES = 8;  // the wallpad polls heating groups 1..8

struct HeatZone {
  bool seen;         // a status reply was received
  bool on;           // heating on
  uint8_t set;       // set temperature, degrees C
  uint8_t current;   // current temperature, degrees C
  uint32_t at;       // millis() of the last reply
};

enum WallpadLinkState : uint8_t {
  WALLPAD_NOT_CONFIGURED,  // no EW11 address set (local_config.h)
  WALLPAD_NO_ETHERNET,  // Ethernet driver failed to start
  WALLPAD_NO_CABLE,     // no Ethernet link
  WALLPAD_NO_WIFI,      // WiFi not (yet) associated
  WALLPAD_NO_IP,        // waiting for DHCP
  WALLPAD_CONNECTING,   // connecting to the EW11
  WALLPAD_CONNECTED,    // TCP connected (data may still be absent)
};

struct WallpadSnapshot {
  WallpadLinkState state;
  bool wifi;            // true: WiFi, false: Ethernet
  int8_t rssi;          // WiFi signal (dBm), 0 when unknown
  uint32_t ip;          // our IPv4 address (network byte order as IPAddress uses)
  uint32_t packets;     // checksum-valid packets
  uint32_t badBytes;    // bytes that did not form a valid packet
  uint32_t lastPacketAt;
  HeatZone zones[HEAT_ZONES];
};

// wifiSsid empty -> Ethernet. The password is only passed to the WiFi driver.
void wallpadBegin(const char* host, uint16_t port, const char* wifiSsid, const char* wifiPassword);
WallpadSnapshot wallpadSnapshot();

// Debug (serial "NET", "NET SCAN"): link details and the last WiFi disconnect reason.
// With scan, also reports how many networks are visible and whether the configured
// SSID is among them (other names are never printed). The scan blocks for seconds.
class Print;
void wallpadDiagnose(Print& out, const char* wifiSsid, bool scan);
