#pragma once
#include <stdint.h>

// Read-only link to the wallpad RS485 bus through an EW11 (RS485 -> TCP server),
// over the board's Ethernet port. This module never transmits to the bus.

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
  WALLPAD_NO_IP,        // waiting for DHCP
  WALLPAD_CONNECTING,   // connecting to the EW11
  WALLPAD_CONNECTED,    // TCP connected (data may still be absent)
};

struct WallpadSnapshot {
  WallpadLinkState state;
  uint32_t ip;          // our IPv4 address (network byte order as IPAddress uses)
  uint32_t packets;     // checksum-valid packets
  uint32_t badBytes;    // bytes that did not form a valid packet
  uint32_t lastPacketAt;
  HeatZone zones[HEAT_ZONES];
};

void wallpadBegin(const char* host, uint16_t port);
WallpadSnapshot wallpadSnapshot();
