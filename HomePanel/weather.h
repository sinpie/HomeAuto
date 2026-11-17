#pragma once
#include <stdint.h>
#include <time.h>
#include <stddef.h>
#include "astro.h"

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
  time_t firstNoon;                // local noon of days[0]: hours[0] is that day's 00:00
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

// Location picked on the panel (saved in flash, wins over local_config.h): the task
// looks up its place name, refetches the weather and recomputes the eclipses.
void weatherSetLocation(float lat, float lon);
bool weatherLocationPending();
void weatherRequestIpLocation();                 // IP-based estimate for the location picker
bool weatherIpLocation(float& lat, float& lon);  // false until it arrived
bool weatherIpFailed();                          // the last IP lookup failed (offline, server error)
void weatherLocation(float& lat, float& lon, char* place, size_t n);
// Place search for the location picker (OpenStreetMap Nominatim, Korean names first).
struct PlaceResult { char name[160]; float lat, lon; };
enum SearchState : uint8_t { SEARCH_IDLE, SEARCH_BUSY, SEARCH_DONE, SEARCH_FAILED };
void weatherSearch(const char* query);              // UTF-8; replaces a running search
SearchState weatherSearchResults(PlaceResult* out, int max, int& count);
// Upcoming eclipses for the location (astro.h); empty until the first search finished.
int weatherEclipses(Eclipse* out, int max);

// One pass of the sun or the moon across the sky, from its rise to its set (-0.833 degrees:
// refraction and the disc's edge), sampled at PASS_POINTS evenly spaced times. The pass is the
// one the body is on at the given time, else its next one, so it is always whole.
constexpr int PASS_POINTS = 72;
struct SkyPass {
  bool valid;
  time_t from;                 // time it was computed for: valid for from <= t <= set
  time_t rise, set;
  float az[PASS_POINTS], alt[PASS_POINTS];
};
// A fraction of a second of math (searches the rise and set, then samples): core 0 or rarely.
bool computePass(bool moon, time_t t, float lat, float lon, SkyPass& out);
// The passes for now, kept up to date by the weather task (false until the first ones exist).
bool weatherPasses(SkyPass& sun, SkyPass& moon, float& lat, float& lon);

// Moon phase name for an age in days (astro.h moonAge).
const char* moonPhaseName(float age);
