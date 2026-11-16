#include "weather.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ETH.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <Preferences.h>

namespace {
constexpr uint32_t REFRESH_MS = 30UL * 60 * 1000, RETRY_MS = 2UL * 60 * 1000;

SemaphoreHandle_t mutex;
WeatherData data = {};
volatile WeatherState state = WX_WAIT_NETWORK;
const char* error = "";
volatile bool timeSet = false;
float lat = 0, lon = 0;
long utcOffset = 0;
char place[48] = "";

bool online() {
  return (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0,0,0,0)) ||
         (ETH.linkUp() && ETH.localIP() != IPAddress(0,0,0,0));
}

int lastHttpCode = 0;
char errorText[64] = "";

// HTTPS without certificate checks: the data is public weather, never credentials.
// (A spoofing server could only show wrong weather.)
bool httpGet(const char* url, String& body) {
  HTTPClient http;
  NetworkClientSecure tls;
  NetworkClient plain;
  http.setTimeout(10000);
  http.setUserAgent("HomePanel/1.0 (ESP32-P4 wall panel)");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  bool begun;
  http.setConnectTimeout(8000);
  if(!strncmp(url, "https://", 8)) { tls.setInsecure(); tls.setHandshakeTimeout(10); begun = http.begin(tls, url); }
  else begun = http.begin(plain, url);
  if(!begun) { lastHttpCode = -100; return false; }
  lastHttpCode = http.GET();
  const bool ok = lastHttpCode == HTTP_CODE_OK;
  if(ok) body = http.getString();
  http.end();
  return ok;
}
const char* failure(const char* what) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  snprintf(errorText, sizeof(errorText), "%s (%d)", what, lastHttpCode);
  xSemaphoreGive(mutex);
  return errorText;
}

// --- minimal JSON readers for the known response shapes ---
const char* findKey(const char* from, const char* end, const char* key) {
  char pattern[48];
  snprintf(pattern, sizeof(pattern), "\"%s\":", key);
  const char* p = strstr(from, pattern);
  return (p && (!end || p < end)) ? p + strlen(pattern) : nullptr;
}
bool number(const char* from, const char* end, const char* key, float& out) {
  const char* p = findKey(from, end, key);
  if(!p || !strncmp(p, "null", 4)) return false;
  out = strtof(p, nullptr);
  return true;
}
bool string(const char* from, const char* end, const char* key, char* out, size_t n) {
  const char* p = findKey(from, end, key);
  if(!p || *p != '"') return false;
  const char* q = p + 1;
  while(*q && *q != '"') q += (*q == '\\' && q[1]) ? 2 : 1;   // skip \" and other escapes
  if(*q != '"') return false;
  size_t len = std::min(size_t(q - p - 1), n - 1);
  while(len && (uint8_t(p[1 + len]) & 0xC0) == 0x80) --len;   // cut before a UTF-8 continuation byte
  memcpy(out, p + 1, len); out[len] = 0;
  return true;
}
// Numeric array "key":[a,b,null,..] -> values (null keeps the previous value).
int numbers(const char* from, const char* end, const char* key, float* out, int max) {
  const char* p = findKey(from, end, key);
  if(!p || *p != '[') return 0;
  ++p;
  int n = 0; float last = 0;
  while(*p && *p != ']' && n < max) {
    if(!strncmp(p, "null", 4)) { out[n++] = last; p += 4; }
    else { char* e; last = strtof(p, &e); if(e == p) break; out[n++] = last; p = e; }
    while(*p == ',' || *p == ' ') ++p;
  }
  return n;
}
// String array "key":["2026-11-16T07:12",..] -> pointers to each string start.
int strings(const char* from, const char* end, const char* key, const char** out, int max) {
  const char* p = findKey(from, end, key);
  if(!p || *p != '[') return 0;
  int n = 0;
  while(n < max) {
    p = strchr(p, '"');
    if(!p || (end && p > end)) break;
    out[n++] = p + 1;
    p = strchr(p + 1, '"');
    if(!p) break;
    ++p;
    if(*p == ']') break;
  }
  return n;
}
const char* section(const char* body, const char* name) {
  char pattern[32];
  snprintf(pattern, sizeof(pattern), "\"%s\":{", name);
  return strstr(body, pattern);
}

// Location estimate from the public IP (often only the ISP's city).
bool ipLookup(float& la, float& lo, long& offset, char* name, size_t n, bool report = true) {
  String body;
  if(!httpGet("http://ip-api.com/json/?fields=status,city,regionName,lat,lon,offset", body)) { if(report) error = failure("위치 조회 실패"); return false; }
  const char* b = body.c_str();
  char status[16] = "";
  float off;
  if(!string(b, nullptr, "status", status, sizeof(status)) || strcmp(status, "success") ||
     !number(b, nullptr, "lat", la) || !number(b, nullptr, "lon", lo)) { if(report) error = "위치 응답 오류"; return false; }
  offset = number(b, nullptr, "offset", off) ? long(off) : 9 * 3600;
  char city[32] = "", region[32] = "";
  string(b, nullptr, "city", city, sizeof(city));
  string(b, nullptr, "regionName", region, sizeof(region));
  snprintf(name, n, "%s%s%s", city, (*city && *region) ? ", " : "", region);
  return true;
}
bool locate() {
  float la, lo; long offset = utcOffset; char name[48];
  if(!ipLookup(la, lo, offset, name, sizeof(name))) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);   // the UI reads these every 250 ms
  utcOffset = offset;
  lat = roundf(la * 100) / 100;  // ~1 km is enough for a forecast
  lon = roundf(lo * 100) / 100;
  snprintf(place, sizeof(place), "%s", name);
  xSemaphoreGive(mutex);
  return true;
}

// Place name for a point picked on the map: OpenStreetMap Nominatim reverse geocoding
// (one request per change, as its usage policy asks). "OO시 OO구" style.
void dropSuffix(char* s, const char* suffix) {
  const size_t n = strlen(s), m = strlen(suffix);
  if(n > m && !strcmp(s + n - m, suffix)) s[n - m] = 0;
}
bool placeName(float la, float lo, char* out, size_t n) {
  char url[200];
  snprintf(url, sizeof(url), "https://nominatim.openstreetmap.org/reverse?format=jsonv2&lat=%.4f&lon=%.4f&zoom=14&accept-language=ko", la, lo);
  String body;
  if(!httpGet(url, body)) return false;
  const char* a = strstr(body.c_str(), "\"address\":{");
  if(!a) return false;
  const char* end = strchr(a, '}');
  char city[48] = "", district[48] = "";
  for(const char* key : {"city", "county", "province", "state"}) if(string(a, end, key, city, sizeof(city))) break;
  for(const char* key : {"borough", "city_district", "town", "suburb", "village"}) if(string(a, end, key, district, sizeof(district))) break;
  if(*district) for(const char* s : {"특별자치시", "특별시", "광역시"}) dropSuffix(city, s);
  char name[100];
  snprintf(name, sizeof(name), "%s%s%s", city, (*city && *district) ? " " : "", district);
  if(!*name || strlen(name) >= n) return false;   // nothing usable, or would be cut mid-character
  snprintf(out, n, "%s", name);
  return true;
}

void startClock() {
  // POSIX TZ has the sign inverted: UTC+9 is "UTC-9".
  char tz[24];
  const long h = utcOffset / 3600, m = labs(utcOffset % 3600) / 60;
  if(m) snprintf(tz, sizeof(tz), "UTC%+ld:%02ld", -h, m); else snprintf(tz, sizeof(tz), "UTC%+ld", -h);
  configTzTime(tz, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
}

int minutesOf(const char* iso) {  // "YYYY-MM-DDTHH:MM"
  return atoi(iso + 11) * 60 + atoi(iso + 14);
}

bool fetch() {
  char url[512];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
    "&hourly=temperature_2m,weather_code,precipitation_probability"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,uv_index_max"
    "&timezone=auto&forecast_days=7&wind_speed_unit=ms", lat, lon);
  String body;
  if(!httpGet(url, body)) { error = failure("날씨 서버 연결 실패"); return false; }
  const char* b = body.c_str();
  const char* cur = section(b, "current");
  const char* hourly = section(b, "hourly");
  const char* daily = section(b, "daily");
  if(!cur || !hourly || !daily) { error = "날씨 응답 오류"; return false; }
  const char* curEnd = strchr(cur, '}');

  WeatherData next = {};
  float v;
  if(!number(cur, curEnd, "temperature_2m", next.temp)) { error = "날씨 응답 오류"; return false; }
  number(cur, curEnd, "apparent_temperature", next.feels);
  if(number(cur, curEnd, "relative_humidity_2m", v)) next.humidity = uint8_t(v);
  if(number(cur, curEnd, "weather_code", v)) next.code = uint8_t(v);
  number(cur, curEnd, "wind_speed_10m", next.wind);
  next.isDay = number(cur, curEnd, "is_day", v) ? v > 0.5f : true;

  static float t[WX_HOURS], c[WX_HOURS], pr[WX_HOURS];
  memset(t, 0, sizeof(t)); memset(c, 0, sizeof(c)); memset(pr, 0, sizeof(pr));
  const int nh = numbers(hourly, daily, "temperature_2m", t, WX_HOURS);
  numbers(hourly, daily, "weather_code", c, WX_HOURS);
  numbers(hourly, daily, "precipitation_probability", pr, WX_HOURS);
  for(int i=0; i<nh; ++i) next.hours[i] = {int8_t(lroundf(t[i])), uint8_t(c[i]), uint8_t(pr[i])};

  static float dc[WX_DAYS], dmax[WX_DAYS], dmin[WX_DAYS], dp[WX_DAYS], duv[WX_DAYS];
  memset(dc, 0, sizeof(dc)); memset(dmax, 0, sizeof(dmax)); memset(dmin, 0, sizeof(dmin)); memset(dp, 0, sizeof(dp)); memset(duv, 0, sizeof(duv));
  const char* dates[WX_DAYS]; const char* rise[WX_DAYS]; const char* set[WX_DAYS];
  const int nd = strings(daily, nullptr, "time", dates, WX_DAYS);
  numbers(daily, nullptr, "weather_code", dc, WX_DAYS);
  numbers(daily, nullptr, "temperature_2m_max", dmax, WX_DAYS);
  numbers(daily, nullptr, "temperature_2m_min", dmin, WX_DAYS);
  numbers(daily, nullptr, "precipitation_probability_max", dp, WX_DAYS);
  numbers(daily, nullptr, "uv_index_max", duv, WX_DAYS);
  const int nr = strings(daily, nullptr, "sunrise", rise, WX_DAYS);
  const int ns = strings(daily, nullptr, "sunset", set, WX_DAYS);
  for(int i=0; i<nd; ++i) {
    struct tm d = {};
    d.tm_year = atoi(dates[i]) - 1900; d.tm_mon = atoi(dates[i] + 5) - 1; d.tm_mday = atoi(dates[i] + 8); d.tm_hour = 12;
    const time_t noon = mktime(&d);  // fills tm_wday
    if(i == 0) next.firstNoon = noon;
    WeatherDay& w = next.days[i];
    w = {uint8_t(d.tm_mon + 1), uint8_t(d.tm_mday), uint8_t(d.tm_wday), uint8_t(dc[i]),
         int8_t(lroundf(dmax[i])), int8_t(lroundf(dmin[i])), uint8_t(dp[i]),
         uint16_t(i < nr ? minutesOf(rise[i]) : 0), uint16_t(i < ns ? minutesOf(set[i]) : 0), uint8_t(lroundf(duv[i]))};
  }
  next.valid = nd > 0 && nh > 0;
  next.fetchedAt = millis();
  next.lat = lat; next.lon = lon;
  memcpy(next.place, place, sizeof(next.place));
  if(!next.valid) return false;   // a cut-off answer must not replace the forecast on screen
  xSemaphoreTake(mutex, portMAX_DELAY);
  data = next;
  xSemaphoreGive(mutex);
  return true;
}

// Place search: the UI posts a query, the task fetches up to 4 results.
char searchQuery[192] = "";
uint32_t searchSeq = 0;      // newest query; older answers are dropped
volatile bool searchRequested = false;
volatile SearchState searchState = SEARCH_IDLE;
PlaceResult searchResults[4];
int searchCount = 0;

void urlEncode(const char* in, char* out, size_t n) {
  size_t o = 0;
  for(const uint8_t* p = reinterpret_cast<const uint8_t*>(in); *p && o + 4 < n; ++p) {
    if(isalnum(*p) || *p == '-' || *p == '.' || *p == '_') out[o++] = char(*p);
    else o += snprintf(out + o, n - o, "%%%02X", *p);
  }
  out[o] = 0;
}
// Nominatim jsonv2: [{"lat":"37.2","lon":"127.0",...,"display_name":"..."}, ...]
void runSearch() {
  char q[192], enc[600], url[720];
  xSemaphoreTake(mutex, portMAX_DELAY);
  snprintf(q, sizeof(q), "%s", searchQuery);
  const uint32_t seq = searchSeq;
  xSemaphoreGive(mutex);
  urlEncode(q, enc, sizeof(enc));
  snprintf(url, sizeof(url), "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=4&accept-language=ko&q=%s", enc);
  String body;
  PlaceResult found[4];
  int n = 0;
  const bool ok = httpGet(url, body);
  if(ok) {
    const char* p = body.c_str();
    while(n < 4 && (p = strstr(p, "\"place_id\""))) {
      const char* next = strstr(p + 1, "\"place_id\"");
      float la, lo; char tmp[24];
      PlaceResult r = {};
      if(string(p, next, "lat", tmp, sizeof(tmp)) && (la = strtof(tmp, nullptr), true) &&
         string(p, next, "lon", tmp, sizeof(tmp)) && (lo = strtof(tmp, nullptr), true) &&
         string(p, next, "display_name", r.name, sizeof(r.name))) {
        r.lat = la; r.lon = lo; found[n++] = r;
      }
      p += 10;
    }
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  if(seq == searchSeq) {   // still the newest query
    memcpy(searchResults, found, sizeof(found)); searchCount = n;
    searchState = ok ? SEARCH_DONE : SEARCH_FAILED;
  }
  xSemaphoreGive(mutex);
}

SkyDay sky = {};          // published copy (mutex)
bool skyReady = false;
SkyDay skyWork;           // the task's working copy (static: ~2.4 KB)

// Requests from the UI (location picker), handled by the task.
volatile bool ipRequested = false, ipKnown = false, ipFailed = false, newLocation = false;
uint32_t pickSeq = 0;      // bumped by every weatherSetLocation (mutex)
float ipLat = 0, ipLon = 0, picked[2] = {0, 0};   // written and read under the mutex
// Upcoming eclipses for the current location, recomputed daily and on a location change.
Eclipse eclipses[8];
int eclipseCount = 0;
uint32_t eclipsesAt = 0;
bool eclipsesStale = true;

void saveLocation() {
  Preferences prefs;
  if(!prefs.begin("location", false)) return;
  prefs.putFloat("lat", lat);
  prefs.putFloat("lon", lon);
  prefs.putString("place", place);
  prefs.end();
}

void task(void*) {
  bool located = *place != 0;  // configured or saved location: no IP lookup
  startClock();                // time does not need a location (offset: configured, else +9 h)
  uint32_t nextAt = 0, retryMs = RETRY_MS;
  bool fetchTried = false;
  for(;;) {
    if(!timeSet) {
      struct tm now;
      if(getLocalTime(&now, 0) && now.tm_year > 120) timeSet = true;
    }
    if(timeSet && (lat != 0 || lon != 0)) {   // today's sun/moon paths: no network needed
      const time_t now = time(nullptr);
      struct tm lt; localtime_r(&now, &lt);
      const time_t day = now - (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
      if(!skyReady || sky.day != day || sky.lat != lat || sky.lon != lon) {
        computeSkyDay(day, lat, lon, skyWork);
        xSemaphoreTake(mutex, portMAX_DELAY);
        sky = skyWork; skyReady = true;
        xSemaphoreGive(mutex);
      }
    }
    if(!online()) {
      if(!data.valid) state = WX_WAIT_NETWORK;
      if(searchRequested) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        searchRequested = false; searchState = SEARCH_FAILED;
        xSemaphoreGive(mutex);
      }
      if(ipRequested) { ipRequested = false; ipFailed = true; }
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    if(searchRequested) { searchRequested = false; runSearch(); }
    if(ipRequested) {
      ipRequested = false;
      float la, lo; long off; char name[48];
      const bool ok = ipLookup(la, lo, off, name, sizeof(name), false);   // quiet: keeps the weather card's message
      xSemaphoreTake(mutex, portMAX_DELAY);
      if(ok) { ipLat = la; ipLon = lo; ipKnown = true; }
      ipFailed = !ok;
      xSemaphoreGive(mutex);
    }
    if(newLocation) {
      xSemaphoreTake(mutex, portMAX_DELAY);
      const float pickedLat = picked[0], pickedLon = picked[1];
      const uint32_t seq = pickSeq;
      xSemaphoreGive(mutex);
      char name[48];
      snprintf(name, sizeof(name), "%.3f, %.3f", pickedLat, pickedLon);
      placeName(pickedLat, pickedLon, name, sizeof(name));   // keeps the coordinates if the lookup fails
      xSemaphoreTake(mutex, portMAX_DELAY);
      if(seq != pickSeq) { xSemaphoreGive(mutex); continue; }   // a newer pick arrived: handle that one
      lat = pickedLat; lon = pickedLon;
      snprintf(place, sizeof(place), "%s", name);
      if(seq == pickSeq) newLocation = false;   // a newer pick runs on the next pass
      xSemaphoreGive(mutex);
      saveLocation();
      if(!located) { located = true; startClock(); }
      eclipsesStale = true;
      nextAt = millis();
    }
    // after the first forecast (seconds of work; the weather comes first)
    const bool skyInputs = timeSet && (lat != 0 || lon != 0) && (data.valid || fetchTried);
    if(skyInputs && (eclipsesStale || millis() - eclipsesAt > 24UL * 3600 * 1000)) {
      Eclipse found[8];
      const int n = findEclipses(time(nullptr), lat, lon, found, 8);   // a few seconds
      xSemaphoreTake(mutex, portMAX_DELAY);
      memcpy(eclipses, found, sizeof(found)); eclipseCount = n;
      xSemaphoreGive(mutex);
      eclipsesAt = millis(); eclipsesStale = false;
    }

    if(int32_t(millis() - nextAt) < 0) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    if(!located) {
      state = WX_LOCATING;
      if(!locate()) { state = WX_ERROR; nextAt = millis() + RETRY_MS; continue; }
      located = true;
      eclipsesStale = true;
      startClock();
    }
    state = WX_FETCHING;
    fetchTried = true;   // the sky work below may start even if this fails (server down)
    if(fetch()) { state = WX_READY; error = ""; nextAt = millis() + REFRESH_MS; retryMs = RETRY_MS; }
    else {
      state = data.valid ? WX_READY : WX_ERROR;
      nextAt = millis() + retryMs;
      retryMs = std::min<uint32_t>(retryMs * 2, REFRESH_MS);
    }
  }
}
}

void weatherBegin(float configLat, float configLon, const char* configPlace, long utcOffsetSec) {
  utcOffset = utcOffsetSec ? utcOffsetSec : 9 * 3600;
  Preferences prefs;   // a location picked on the panel wins over local_config.h
  if(prefs.begin("location", true) && prefs.isKey("lat")) {
    lat = prefs.getFloat("lat"); lon = prefs.getFloat("lon");
    snprintf(place, sizeof(place), "%s", prefs.getString("place", "").c_str());
    if(!*place) snprintf(place, sizeof(place), "%.3f, %.3f", lat, lon);
  } else if(configPlace && *configPlace) {
    lat = roundf(configLat * 100) / 100;
    lon = roundf(configLon * 100) / 100;
    snprintf(place, sizeof(place), "%s", configPlace);
  }
  prefs.end();
  mutex = xSemaphoreCreateMutex();
  if(!mutex) return;   // out of memory: no weather task
  // Priority 1 like the UI loop: the eclipse search takes seconds and must not stall the screen.
  xTaskCreatePinnedToCore(task, "weather", 12288, nullptr, 1, nullptr, 0);   // core 0: the UI loop runs on core 1
}
void weatherSetLocation(float la, float lo) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  picked[0] = la; picked[1] = lo; ++pickSeq; newLocation = true;
  xSemaphoreGive(mutex);
}
bool weatherLocationPending() { return newLocation; }
void weatherRequestIpLocation() { ipFailed = false; ipRequested = true; }
bool weatherIpFailed() { return ipFailed; }
bool weatherIpLocation(float& la, float& lo) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  const bool known = ipKnown;
  la = ipLat; lo = ipLon;
  xSemaphoreGive(mutex);
  return known;
}
void weatherLocation(float& la, float& lo, char* name, size_t n) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  la = lat; lo = lon; snprintf(name, n, "%s", place);
  xSemaphoreGive(mutex);
}
void weatherSearch(const char* query) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  snprintf(searchQuery, sizeof(searchQuery), "%s", query);
  ++searchSeq;
  searchState = SEARCH_BUSY;
  searchRequested = true;
  xSemaphoreGive(mutex);
}
SearchState weatherSearchResults(PlaceResult* out, int max, int& count) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  count = std::min(max, searchCount);
  memcpy(out, searchResults, count * sizeof(PlaceResult));
  xSemaphoreGive(mutex);
  return searchState;
}
void computeSkyDay(time_t day, float la, float lo, SkyDay& out) {
  out.day = day; out.lat = la; out.lon = lo;
  for(int i = 0; i < SKY_STEPS; ++i) {
    const time_t t = day + SKY_START_OFFSET + i * 600;
    sunAzAlt(t, la, lo, out.sunAz[i], out.sunAlt[i]);
    moonAzAlt(t, la, lo, out.moonAz[i], out.moonAlt[i]);
    if(i % 48 == 47) vTaskDelay(1);   // seconds of math: let other work run
  }
  sunRiseSet(day, la, lo, out.sunRise, out.sunSet);
  moonRiseSet(day, la, lo, out.moonRise, out.moonSet);
}
bool weatherSkyDay(SkyDay& out) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  const bool ok = skyReady;
  if(ok) out = sky;
  xSemaphoreGive(mutex);
  return ok;
}
int weatherEclipses(Eclipse* out, int max) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  const int n = std::min(max, eclipseCount);
  memcpy(out, eclipses, n * sizeof(Eclipse));
  xSemaphoreGive(mutex);
  return n;
}
WeatherState weatherState() { return state; }
const char* weatherError() {   // UI only: a stable copy (the task rewrites errorText)
  static char copy[64];
  xSemaphoreTake(mutex, portMAX_DELAY);
  snprintf(copy, sizeof(copy), "%s", error);
  xSemaphoreGive(mutex);
  return copy;
}
bool weatherCopy(WeatherData& out) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  out = data;
  xSemaphoreGive(mutex);
  return out.valid;
}
bool clockReady() { return timeSet; }
bool localNow(struct tm& out) { return timeSet && getLocalTime(&out, 0); }

const char* weatherText(uint8_t code) {
  if(code == 0) return "맑음";
  if(code == 1) return "대체로 맑음";
  if(code == 2) return "구름 조금";
  if(code == 3) return "흐림";
  if(code == 45 || code == 48) return "안개";
  if(code >= 51 && code <= 57) return "이슬비";
  if(code >= 61 && code <= 67) return "비";
  if(code >= 71 && code <= 77) return "눈";
  if(code >= 80 && code <= 82) return "소나기";
  if(code == 85 || code == 86) return "눈 소나기";
  if(code >= 95) return "뇌우";
  return "알 수 없음";
}
int weatherSeverity(uint8_t code) {
  if(code >= 95) return 8;
  if(code >= 71 && code <= 86 && !(code >= 80 && code <= 82)) return 7;
  if((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return 6;
  if(code >= 51 && code <= 57) return 5;
  if(code == 45 || code == 48) return 4;
  return code;  // 0..3
}

const char* moonPhaseName(float age) {
  if(age < 1.85f) return "삭";
  if(age < 5.54f) return "초승달";
  if(age < 9.23f) return "상현달";
  if(age < 12.92f) return "차오르는 달";
  if(age < 16.61f) return "보름달";
  if(age < 20.30f) return "기우는 달";
  if(age < 23.99f) return "하현달";
  if(age < 27.68f) return "그믐달";
  return "삭";
}
