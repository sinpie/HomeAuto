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

// Other wallpad devices (Samsung SDS packet formats, n-andflash/ha_addons DOCS_PACKETS).
// A device reply (B0 ...) is read in the context of the wallpad query just before it.
// "seen" = a reply arrived within the stale window; nothing is ever sent.
constexpr int LIGHT_GROUPS = 8, OUTLETS = 8;

struct GasState { bool seen; bool closed; uint32_t at; };                  // AB 41 -> B0 41 XX (01 closed)
struct LightGroup { bool seen; uint8_t count; uint8_t bits; uint32_t at; }; // AC 79 -> B0 79 RN BM / GR TG
struct OutletState { bool seen; uint8_t mode; uint16_t watts; uint32_t at; };  // C6 4A -> B0 4A GR XX WW WW ..
struct VentState { bool seen; bool on; uint8_t speed; bool automatic; uint32_t at; };  // C2 4E -> B0 4E XX MM YY
struct EnergyState { bool seen; uint32_t value; uint32_t at; };            // AA 6F YY -> B0 6F YY ZZ ZZ ZZ

// Intercom (headers A1..A6): what the wallpad tells the intercoms.
enum IntercomPhase : uint8_t { INTERCOM_IDLE, INTERCOM_RINGING, INTERCOM_CALL, INTERCOM_BUSY };
struct IntercomState {
  bool seen;            // the wallpad talks to an intercom that answers
  IntercomPhase phase;
  uint8_t ringDoor;     // 1 home door, 2 common door (while ringing)
  uint32_t at;
};
// Entrance switch (AD old / CC new): events the wallpad confirms to it.
struct EntranceState {
  bool seen;            // an entrance switch answers
  int8_t gasClosed;     // gas state pushed to the switch: -1 unknown, 0 open, 1 closed
  int8_t away;          // away mode: -1 unknown, 0 released, 1 set
  int8_t lightsAllOff;  // batch light switch: -1 unknown, 0 normal, 1 all off
  uint32_t at;
};
// Wallpad clock pushed to the new entrance switch: CC 01 16 YY MM DD HH NN SS ..
struct WallClock { bool valid; uint8_t year, month, day, hour, minute, second; uint32_t at; };

struct WallpadSnapshot {
  WallpadLinkState state;
  bool wifi;            // true: WiFi, false: Ethernet
  int8_t rssi;          // WiFi signal (dBm), 0 when unknown
  uint32_t ip;          // our IPv4 address (network byte order as IPAddress uses)
  uint32_t packets;     // checksum-valid packets
  uint32_t badBytes;    // bytes that did not form a valid packet
  uint32_t lastPacketAt;
  HeatZone zones[HEAT_ZONES];
  GasState gas;
  LightGroup lights[LIGHT_GROUPS];
  OutletState outlets[OUTLETS];
  VentState vent;
  EnergyState energy[3];     // 0 electricity (W), 1 gas, 2 water (meter digits, scale is home-specific)
  // Doorbell rings sent by the wallpad to intercom devices (An 31 = home door, An 32 = common door).
  uint32_t doorbells;        // increments once per ring
  uint8_t lastDoorbell;      // 1 home door, 2 common door
  uint32_t doorOpens;        // door opened during a call (An 3B GY confirmed by the wallpad)
  uint8_t lastDoorOpen;      // 1 home door, 2 common door
  IntercomState intercom;
  EntranceState entrance;
  uint32_t elevatorCalls;    // elevator call confirmed by the wallpad (AD 2F / CC 10)
  uint32_t elevatorArrivals; // elevator arrived (CC 12)
  WallClock clock;
};

// wifiSsid empty -> Ethernet. The password is only passed to the WiFi driver.
void wallpadBegin(const char* host, uint16_t port, const char* wifiSsid, const char* wifiPassword);
WallpadSnapshot wallpadSnapshot();

// Debug (serial "NET", "NET SCAN"): link details and the last WiFi disconnect reason.
// With scan, also reports how many networks are visible and whether the configured
// SSID is among them (other names are never printed). The scan blocks for seconds.
class Print;
void wallpadDiagnose(Print& out, const char* wifiSsid, bool scan);
