#include "weather.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ETH.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

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
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  bool begun;
  if(!strncmp(url, "https://", 8)) { tls.setInsecure(); begun = http.begin(tls, url); }
  else begun = http.begin(plain, url);
  if(!begun) { lastHttpCode = -100; return false; }
  lastHttpCode = http.GET();
  const bool ok = lastHttpCode == HTTP_CODE_OK;
  if(ok) body = http.getString();
  http.end();
  return ok;
}
const char* failure(const char* what) {
  snprintf(errorText, sizeof(errorText), "%s (%d)", what, lastHttpCode);
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
  const char* q = strchr(p + 1, '"');
  if(!q) return false;
  const size_t len = std::min(size_t(q - p - 1), n - 1);
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

bool locate() {
  String body;
  if(!httpGet("http://ip-api.com/json/?fields=status,city,regionName,lat,lon,offset", body)) { error = failure("위치 조회 실패"); return false; }
  const char* b = body.c_str();
  char status[16] = "";
  float la, lo, off;
  if(!string(b, nullptr, "status", status, sizeof(status)) || strcmp(status, "success") ||
     !number(b, nullptr, "lat", la) || !number(b, nullptr, "lon", lo)) { error = "위치 응답 오류"; return false; }
  lat = roundf(la * 100) / 100;  // ~1 km is enough for a forecast
  lon = roundf(lo * 100) / 100;
  utcOffset = number(b, nullptr, "offset", off) ? long(off) : 9 * 3600;
  char city[32] = "", region[32] = "";
  string(b, nullptr, "city", city, sizeof(city));
  string(b, nullptr, "regionName", region, sizeof(region));
  snprintf(place, sizeof(place), "%s%s%s", city, (*city && *region) ? ", " : "", region);
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
  const int nh = numbers(hourly, daily, "temperature_2m", t, WX_HOURS);
  numbers(hourly, daily, "weather_code", c, WX_HOURS);
  numbers(hourly, daily, "precipitation_probability", pr, WX_HOURS);
  for(int i=0; i<nh; ++i) next.hours[i] = {int8_t(lroundf(t[i])), uint8_t(c[i]), uint8_t(pr[i])};

  static float dc[WX_DAYS], dmax[WX_DAYS], dmin[WX_DAYS], dp[WX_DAYS], duv[WX_DAYS];
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
    mktime(&d);  // fills tm_wday
    WeatherDay& w = next.days[i];
    w = {uint8_t(d.tm_mon + 1), uint8_t(d.tm_mday), uint8_t(d.tm_wday), uint8_t(dc[i]),
         int8_t(lroundf(dmax[i])), int8_t(lroundf(dmin[i])), uint8_t(dp[i]),
         uint16_t(i < nr ? minutesOf(rise[i]) : 0), uint16_t(i < ns ? minutesOf(set[i]) : 0), uint8_t(lroundf(duv[i]))};
  }
  next.valid = nd > 0 && nh > 0;
  next.fetchedAt = millis();
  next.lat = lat; next.lon = lon;
  memcpy(next.place, place, sizeof(next.place));
  xSemaphoreTake(mutex, portMAX_DELAY);
  data = next;
  xSemaphoreGive(mutex);
  return next.valid;
}

void task(void*) {
  bool located = *place != 0;  // configured location: no IP lookup
  if(located) startClock();
  uint32_t nextAt = 0;
  for(;;) {
    if(!timeSet) {
      struct tm now;
      if(getLocalTime(&now, 0) && now.tm_year > 120) timeSet = true;
    }
    if(!online()) { if(!data.valid) state = WX_WAIT_NETWORK; vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    if(int32_t(millis() - nextAt) < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    if(!located) {
      state = WX_LOCATING;
      if(!locate()) { state = WX_ERROR; nextAt = millis() + RETRY_MS; continue; }
      located = true;
      startClock();
    }
    state = WX_FETCHING;
    if(fetch()) { state = WX_READY; error = ""; nextAt = millis() + REFRESH_MS; }
    else { state = data.valid ? WX_READY : WX_ERROR; nextAt = millis() + RETRY_MS; }
  }
}
}

void weatherBegin(float configLat, float configLon, const char* configPlace, long utcOffsetSec) {
  if(configPlace && *configPlace) {
    lat = roundf(configLat * 100) / 100;
    lon = roundf(configLon * 100) / 100;
    snprintf(place, sizeof(place), "%s", configPlace);
    utcOffset = utcOffsetSec;
  }
  mutex = xSemaphoreCreateMutex();
  xTaskCreate(task, "weather", 8192, nullptr, 2, nullptr);
}
WeatherState weatherState() { return state; }
const char* weatherError() { return error; }
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

// NOAA-style approximation, good to a fraction of a degree.
float sunElevation(float latDeg, float lonDeg, time_t utc) {
  const double d = utc / 86400.0 - 10957.5;  // days since J2000.0
  const double g = fmod(357.529 + 0.98560028 * d, 360) * M_PI / 180;
  const double q = fmod(280.459 + 0.98564736 * d, 360);
  const double L = (q + 1.915 * sin(g) + 0.020 * sin(2 * g)) * M_PI / 180;
  const double e = (23.439 - 0.00000036 * d) * M_PI / 180;
  const double ra = atan2(cos(e) * sin(L), cos(L));
  const double dec = asin(sin(e) * sin(L));
  const double gmst = fmod(18.697374558 + 24.06570982441908 * d, 24) * 15 * M_PI / 180;
  const double ha = gmst + lonDeg * M_PI / 180 - ra;
  const double la = latDeg * M_PI / 180;
  return float(asin(sin(la) * sin(dec) + cos(la) * cos(dec) * cos(ha)) * 180 / M_PI);
}

void moonPhase(time_t utc, float& age, float& illumination) {
  const double synodic = 29.530588853;
  const double days = (utc - 947182440.0) / 86400.0;  // new moon 2000-01-06 18:14 UTC
  age = float(fmod(fmod(days, synodic) + synodic, synodic));
  illumination = float((1 - cos(2 * M_PI * age / synodic)) / 2);
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
