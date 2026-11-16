#pragma once
#include <stdint.h>
#include <time.h>

// Internet time and weather.
//  - Location: configured in local_config.h (WEATHER_LAT/LON/PLACE); otherwise
//    estimated from the public IP (ip-api.com, often only the ISP's city), rounded to ~1 km.
//  - Time: NTP, time zone from the same lookup.
//  - Weather: Open-Meteo forecast (no API key), refreshed every 30 minutes.
// Runs in its own task once WiFi or Ethernet has an IP address.

constexpr int WX_HOURS = 48;  // hourly values from today 00:00 local time
constexpr int WX_DAYS = 7;

struct WeatherHour { int8_t temp; uint8_t code; uint8_t pop; };   // °C, WMO code, rain chance %
struct WeatherDay {
  uint8_t month, day, weekday;     // weekday 0 = Sunday
  uint8_t code;                    // WMO weather code
  int8_t tmax, tmin;
  uint8_t pop;                     // max rain chance %
  uint16_t sunrise, sunset;        // minutes after local midnight
  uint8_t uv;
};
struct WeatherData {
  bool valid;
  uint32_t fetchedAt;              // millis() of the last successful update
  char place[48];                  // "city, region" from the IP lookup
  float lat, lon;
  float temp, feels, wind;         // °C, °C, m/s
  uint8_t humidity, code;
  bool isDay;
  WeatherHour hours[WX_HOURS];
  WeatherDay days[WX_DAYS];
};

enum WeatherState : uint8_t { WX_WAIT_NETWORK, WX_LOCATING, WX_FETCHING, WX_READY, WX_ERROR };

// place empty -> estimate the location from the public IP. utcOffsetSec: time zone
// for a configured place (the IP lookup provides it otherwise).
void weatherBegin(float lat, float lon, const char* place, long utcOffsetSec);
WeatherState weatherState();
const char* weatherError();
bool weatherCopy(WeatherData& out);   // false until the first forecast arrived
bool clockReady();                    // NTP time is set
bool localNow(struct tm& out);        // local time, false if not set

// WMO weather code -> Korean text / severity (higher = worse) for summaries.
const char* weatherText(uint8_t code);
int weatherSeverity(uint8_t code);

// Sun elevation in degrees for the given location and UTC time.
float sunElevation(float lat, float lon, time_t utc);
// Moon: age in days (0..29.5) and illuminated fraction (0..1) at a UTC time.
void moonPhase(time_t utc, float& age, float& illumination);
const char* moonPhaseName(float age);
