#include "astro.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Port of the validated Python prototype (Meeus, low precision). Doubles for time
// and angles; the moon series uses the main terms of Meeus tables 47.A/47.B.

namespace {
constexpr double D2R = M_PI / 180.0;
constexpr double SYNODIC = 29.530588853;
constexpr double AU_KM = 149597870.7;

double norm360(double a) { a = fmod(a, 360.0); return a < 0 ? a + 360.0 : a; }

struct Term { int8_t d, m, mp, f; int32_t k; };
const Term MOON_L[] = {
  {0,0,1,0,6288774},{2,0,-1,0,1274027},{2,0,0,0,658314},{0,0,2,0,213618},{0,1,0,0,-185116},{0,0,0,2,-114332},
  {2,0,-2,0,58793},{2,-1,-1,0,57066},{2,0,1,0,53322},{2,-1,0,0,45758},{0,1,-1,0,-40923},{1,0,0,0,-34720},
  {0,1,1,0,-30383},{2,0,0,-2,15327},{0,0,1,2,-12528},{0,0,1,-2,10980},{4,0,-1,0,10675},{0,0,3,0,10034},
  {4,0,-2,0,8548},{2,1,-1,0,-7888},{2,1,0,0,-6766},{1,0,-1,0,-5163},{1,1,0,0,4987},{2,-1,1,0,4036}};
const Term MOON_R[] = {
  {0,0,1,0,-20905355},{2,0,-1,0,-3699111},{2,0,0,0,-2955968},{0,0,2,0,-569925},{0,1,0,0,48888},{0,0,0,2,-3149},
  {2,0,-2,0,246158},{2,-1,-1,0,-152138},{2,0,1,0,-170733},{2,-1,0,0,-204586},{0,1,-1,0,-129620},{1,0,0,0,108743},
  {0,1,1,0,104755},{2,0,0,-2,10321},{0,0,1,-2,79661},{4,0,-1,0,-34782},{0,0,3,0,-23210},{4,0,-2,0,-21636},
  {2,1,-1,0,24208},{2,1,0,0,30824},{1,0,-1,0,-8379},{1,1,0,0,-16675},{2,-1,1,0,-12831}};
const Term MOON_B[] = {
  {0,0,0,1,5128122},{0,0,1,1,280602},{0,0,1,-1,277693},{2,0,0,-1,173237},{2,0,-1,1,55413},{2,0,-1,-1,46271},
  {2,0,0,1,32573},{0,0,2,1,17198},{2,0,1,-1,9266},{0,0,2,-1,8822},{2,-1,0,-1,8216},{2,0,-2,-1,4324},{2,0,1,1,4200}};

double obliquity(double jd) { return 23.439291 - 0.0130042 * (jd - 2451545.0) / 36525.0; }

SkyPoint toEquatorial(double lonDeg, double latDeg, double distKm, double jd) {
  const double l = lonDeg * D2R, b = latDeg * D2R, e = obliquity(jd) * D2R;
  SkyPoint p;
  p.ra = atan2(sin(l) * cos(e) - tan(b) * sin(e), cos(l));
  p.dec = asin(sin(b) * cos(e) + cos(b) * sin(e) * sin(l));
  p.distKm = distKm;
  return p;
}

double sunLongitude(double jd, double* distKm) {
  const double T = (jd - 2451545.0) / 36525.0;
  const double L0 = norm360(280.46646 + 36000.76983 * T);
  const double M = norm360(357.52911 + 35999.05029 * T);
  const double C = (1.914602 - 0.004817 * T) * sin(M * D2R) + 0.019993 * sin(2 * M * D2R) + 0.000289 * sin(3 * M * D2R);
  const double e = 0.016708634 - 0.000042037 * T, v = M + C;
  if(distKm) *distKm = 1.000001018 * (1 - e * e) / (1 + e * cos(v * D2R)) * AU_KM;
  const double om = 125.04 - 1934.136 * T;
  return norm360(L0 + C - 0.00569 - 0.00478 * sin(om * D2R));
}

void moonEcliptic(double jd, double& lon, double& lat, double& dist) {
  const double T = (jd - 2451545.0) / 36525.0;
  const double Lp = norm360(218.3164477 + 481267.88123421 * T);
  const double D = norm360(297.8501921 + 445267.1114034 * T);
  const double M = norm360(357.5291092 + 35999.0502909 * T);
  const double Mp = norm360(134.9633964 + 477198.8675055 * T);
  const double F = norm360(93.2720950 + 483202.0175233 * T);
  const double A1 = norm360(119.75 + 131.849 * T), A2 = norm360(53.09 + 479264.290 * T), A3 = norm360(313.45 + 481266.484 * T);
  const double E = 1 - 0.002516 * T;
  const float Dr = float(D * D2R), Mr = float(M * D2R), Mpr = float(Mp * D2R), Fr = float(F * D2R);   // 0..2pi
  auto arg = [&](const Term& t) { return t.d * Dr + t.m * Mr + t.mp * Mpr + t.f * Fr; };   // |arg| < 26 rad: sinf is fine
  auto ecc = [&](const Term& t) { return t.m == 0 ? 1.0 : (abs(t.m) == 1 ? E : E * E); };
  double sl = 0, sr = 0, sb = 0;
  for(const Term& t : MOON_L) sl += t.k * ecc(t) * sinf(arg(t));
  for(const Term& t : MOON_R) sr += t.k * ecc(t) * cosf(arg(t));
  for(const Term& t : MOON_B) sb += t.k * ecc(t) * sinf(arg(t));
  sl += 3958 * sin(A1 * D2R) + 1962 * sin((Lp - F) * D2R) + 318 * sin(A2 * D2R);
  sb += -2235 * sin(Lp * D2R) + 382 * sin(A3 * D2R) + 175 * sin((A1 - F) * D2R) + 175 * sin((A1 + F) * D2R)
        + 127 * sin((Lp - Mp) * D2R) - 115 * sin((Lp + Mp) * D2R);
  lon = norm360(Lp + sl / 1e6);
  lat = sb / 1e6;
  dist = 385000.56 + sr / 1000;
}

double gmst(double jd) {
  const double T = (jd - 2451545.0) / 36525.0;
  return norm360(280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * T * T) * D2R;
}

double separationDeg(const SkyPoint& a, const SkyPoint& b) {
  const double c = sin(a.dec) * sin(b.dec) + cos(a.dec) * cos(b.dec) * cos(a.ra - b.ra);
  return acos(c > 1 ? 1 : (c < -1 ? -1 : c)) / D2R;
}
double wrapPi(double a) { a = fmod(a + M_PI, 2 * M_PI); return (a < 0 ? a + 2 * M_PI : a) - M_PI; }

double elongationDeg(double jd) {
  double ml, mb, md;
  moonEcliptic(jd, ml, mb, md);
  return norm360(ml - sunLongitude(jd, nullptr));
}

// Next time (after jd) the elongation reaches target (0 new moon, 180 full moon).
double nextPhase(double jd, double target) {
  // Estimate from the mean motion, then Newton steps: ~5 moon evaluations instead of ~90.
  const double rate = 360.0 / SYNODIC;   // degrees of elongation per day (mean)
  double t = jd + fmod(target - elongationDeg(jd) + 720.0, 360.0) / rate;
  for(int i = 0; i < 6; ++i) {
    const double d = fmod(elongationDeg(t) - target + 540.0, 360.0) - 180.0;
    t -= d / rate;
    if(fabs(d) < 1e-4) break;
  }
  return t;
}
// An eclipse needs the moon near a node: |ecliptic latitude| below ~1.6 degrees.
bool nearNode(double jd) {
  double lon, lat, dist;
  moonEcliptic(jd, lon, lat, dist);
  return fabs(lat) < 1.75;
}

struct Shadow { double sep, umbra, penumbra, moonR; };
Shadow earthShadow(double jd) {
  const SkyPoint s = sunPosition(jd), m = moonPosition(jd);
  SkyPoint anti = s; anti.ra = s.ra + M_PI; anti.dec = -s.dec;
  const double pm = asin(6378.14 / m.distKm) / D2R, au = s.distKm / AU_KM;
  const double ss = 959.63 / 3600 / au, ps = 8.794 / 3600 / au;
  return {separationDeg(m, anti), 1.02 * (0.99834 * pm - ss + ps), 1.02 * (0.99834 * pm + ss + ps), 0.272481 * pm};
}

// Edge of an eclipse by bisection: `inside` at the peak side, `outside` beyond the edge.
template<class F> double contact(F touching, double inside, double outside) {
  if(touching(outside)) return outside;
  for(int i=0; i<12; ++i) { const double mid = (inside + outside) / 2; (touching(mid) ? inside : outside) = mid; }
  return inside;   // 0.25 day / 4096: ~5 seconds
}

double timeToJd(time_t t) { return double(t) / 86400.0 + 2440587.5; }
time_t jdToTime(double jd) { return time_t((jd - 2440587.5) * 86400.0 + 0.5); }

void riseSet(time_t day, float lat, float lon, float h0, float (*alt)(time_t, float, float), time_t& rise, time_t& set) {
  rise = set = 0;
  float prev = alt(day, lat, lon) - h0;
  for(int i = 1; i <= 144; ++i) {          // 10-minute steps
    const time_t t = day + i * 600;
    const float cur = alt(t, lat, lon) - h0;
    if(prev < 0 && cur >= 0 && !rise) rise = t - time_t(600 * cur / (cur - prev));
    if(prev >= 0 && cur < 0 && !set) set = t - time_t(600 * cur / (cur - prev));
    prev = cur;
  }
}
}

double julianDay(time_t utc) { return timeToJd(utc); }

SkyPoint sunPosition(double jd) {
  double dist;
  const double lon = sunLongitude(jd, &dist);
  return toEquatorial(lon, 0, dist, jd);
}

SkyPoint moonPosition(double jd) {
  double lon, lat, dist;
  moonEcliptic(jd, lon, lat, dist);
  return toEquatorial(lon, lat, dist, jd);
}

SkyPoint topocentric(const SkyPoint& p, double jd, float lat, float lon) {
  const double phi = lat * D2R, u = atan(0.99664719 * tan(phi));
  const double rs = 0.99664719 * sin(u), rc = cos(u), sp = 6378.14 / p.distKm;
  const double H = gmst(jd) + lon * D2R - p.ra;
  const double dra = atan2(-rc * sp * sin(H), cos(p.dec) - rc * sp * cos(H));
  SkyPoint t = p;
  t.ra = p.ra + dra;
  t.dec = atan2((sin(p.dec) - rs * sp) * cos(dra), cos(p.dec) - rc * sp * cos(H));
  return t;
}

float altitudeDeg(const SkyPoint& p, double jd, float lat, float lon) {
  const double H = gmst(jd) + lon * D2R - p.ra, phi = lat * D2R;
  return float(asin(sin(phi) * sin(p.dec) + cos(phi) * cos(p.dec) * cos(H)) / D2R);
}

void azAlt(const SkyPoint& p, double jd, float lat, float lon, float& az, float& alt) {
  const double H = gmst(jd) + lon * D2R - p.ra, phi = lat * D2R;
  alt = float(asin(sin(phi) * sin(p.dec) + cos(phi) * cos(p.dec) * cos(H)) / D2R);
  az = float(norm360(atan2(sin(H), cos(H) * sin(phi) - tan(p.dec) * cos(phi)) / D2R + 180));   // Meeus: from south
}
namespace {
// Keplerian elements at J2000 and their rates per century: a, e, I, L, long. perihelion, long. node
struct Orbit { float el[6], rate[6]; };
const Orbit ORBITS[PLANETS + 1] = {
  {{0.38709927f, 0.20563593f, 7.00497902f, 252.25032350f, 77.45779628f, 48.33076593f},
   {0.00000037f, 0.00001906f, -0.00594749f, 149472.67411175f, 0.16047689f, -0.12534081f}},   // Mercury
  {{0.72333566f, 0.00677672f, 3.39467605f, 181.97909950f, 131.60246718f, 76.67984255f},
   {0.00000390f, -0.00004107f, -0.00078890f, 58517.81538729f, 0.00268329f, -0.27769418f}},   // Venus
  {{1.52371034f, 0.09339410f, 1.84969142f, -4.55343205f, -23.94362959f, 49.55953891f},
   {0.00001847f, 0.00007882f, -0.00813131f, 19140.30268499f, 0.44441088f, -0.29257343f}},    // Mars
  {{5.20288700f, 0.04838624f, 1.30439695f, 34.39644051f, 14.72847983f, 100.47390909f},
   {-0.00011607f, -0.00013253f, -0.00183714f, 3034.74612775f, 0.21252668f, 0.20469106f}},    // Jupiter
  {{9.53667594f, 0.05386179f, 2.48599187f, 49.95424423f, 92.59887831f, 113.66242448f},
   {-0.00125060f, -0.00050991f, 0.00193609f, 1222.49362201f, -0.41897216f, -0.28867794f}},   // Saturn
  {{1.00000261f, 0.01671123f, -0.00001531f, 100.46457166f, 102.93768193f, 0.0f},
   {0.00000562f, -0.00004392f, -0.01294668f, 35999.37244981f, 0.32327364f, 0.0f}}};          // Earth-Moon barycentre
// Heliocentric ecliptic position (AU). The mean longitude is reduced in double (large rate).
void heliocentric(const Orbit& o, double T, float& x, float& y, float& z) {
  const float a = o.el[0] + o.rate[0] * float(T), e = o.el[1] + o.rate[1] * float(T);
  const float I = (o.el[2] + o.rate[2] * float(T)) * float(D2R);
  const double L = norm360(o.el[3] + o.rate[3] * T);
  const float wb = o.el[4] + o.rate[4] * float(T), Om = o.el[5] + o.rate[5] * float(T);
  const float M = float(norm360(L - wb) * D2R), w = (wb - Om) * float(D2R), O = Om * float(D2R);
  float E = M + e * sinf(M);
  for(int i = 0; i < 5; ++i) E -= (E - e * sinf(E) - M) / (1 - e * cosf(E));
  const float xp = a * (cosf(E) - e), yp = a * sqrtf(1 - e * e) * sinf(E);
  const float cw = cosf(w), sw = sinf(w), cO = cosf(O), sO = sinf(O), cI = cosf(I), sI = sinf(I);
  x = (cw * cO - sw * sO * cI) * xp + (-sw * cO - cw * sO * cI) * yp;
  y = (cw * sO + sw * cO * cI) * xp + (-sw * sO + cw * cO * cI) * yp;
  z = sw * sI * xp + cw * sI * yp;
}
}
void planetsAzAlt(time_t utc, float lat, float lon, float az[PLANETS], float alt[PLANETS]) {
  const double jd = timeToJd(utc), T = (jd - 2451545.0) / 36525.0;
  float ex, ey, ez;
  heliocentric(ORBITS[PLANETS], T, ex, ey, ez);
  for(int i = 0; i < PLANETS; ++i) {
    float px, py, pz;
    heliocentric(ORBITS[i], T, px, py, pz);
    const float gx = px - ex, gy = py - ey, gz = pz - ez;
    const float dist = sqrtf(gx * gx + gy * gy + gz * gz);
    const double lonJ2000 = atan2f(gy, gx) / D2R, latDeg = asinf(gz / dist) / D2R;
    const double lonDeg = norm360(lonJ2000 + 1.396971 * T + 0.0003086 * T * T);   // precession to the date
    azAlt(toEquatorial(lonDeg, latDeg, dist * AU_KM, jd), jd, lat, lon, az[i], alt[i]);
  }
}

void sunAzAlt(time_t utc, float lat, float lon, float& az, float& alt) {
  const double jd = timeToJd(utc);
  azAlt(sunPosition(jd), jd, lat, lon, az, alt);
}
void moonAzAlt(time_t utc, float lat, float lon, float& az, float& alt) {
  const double jd = timeToJd(utc);
  azAlt(topocentric(moonPosition(jd), jd, lat, lon), jd, lat, lon, az, alt);
}

float sunAltitude(time_t utc, float lat, float lon) {
  const double jd = timeToJd(utc);
  return altitudeDeg(sunPosition(jd), jd, lat, lon);
}
float moonAltitude(time_t utc, float lat, float lon) {
  const double jd = timeToJd(utc);
  return altitudeDeg(topocentric(moonPosition(jd), jd, lat, lon), jd, lat, lon);
}
float moonAge(time_t utc) { return float(elongationDeg(timeToJd(utc)) / 360.0 * SYNODIC); }
float moonIllumination(time_t utc) { return float((1 - cos(elongationDeg(timeToJd(utc)) * D2R)) / 2); }

void sunRiseSet(time_t day, float lat, float lon, time_t& rise, time_t& set) {
  riseSet(day, lat, lon, -0.833f, sunAltitude, rise, set);
}
void moonRiseSet(time_t day, float lat, float lon, time_t& rise, time_t& set) {
  riseSet(day, lat, lon, -0.833f, moonAltitude, rise, set);
}

namespace {
// One lunar eclipse around a full moon (closest approach within +-3 h).
bool lunarNear(double full, float lat, float lon, Eclipse& out) {
  double best = 1e9, bestJd = full; Shadow bs = {};
  for(int i = -36; i <= 36; ++i) {
    const double jd = full + i * 5.0 / 1440;
    const Shadow s = earthShadow(jd);
    if(s.sep < best) { best = s.sep; bestJd = jd; bs = s; }
  }
  const EclipseKind kind = best < bs.umbra - bs.moonR ? ECLIPSE_TOTAL : best < bs.umbra + bs.moonR ? ECLIPSE_PARTIAL
                           : best < bs.penumbra + bs.moonR ? ECLIPSE_PENUMBRAL : ECLIPSE_NONE;
  if(kind == ECLIPSE_NONE) return false;
  auto touching = [](double jd) { const Shadow s = earthShadow(jd); return s.sep < s.penumbra + s.moonR; };
  const double a = contact(touching, bestJd, bestJd - 0.25), b = contact(touching, bestJd, bestJd + 0.25);
  // umbral magnitude; a penumbral eclipse reports its penumbral magnitude instead
  const double mag = kind == ECLIPSE_PENUMBRAL ? (bs.penumbra + bs.moonR - best) / (2 * bs.moonR)
                                               : (bs.umbra + bs.moonR - best) / (2 * bs.moonR);
  out = {false, kind, moonAltitude(jdToTime(bestJd), lat, lon) > 0, jdToTime(bestJd), jdToTime(a), jdToTime(b), float(mag)};
  return true;
}
// One solar eclipse around a new moon as seen from (lat, lon), while the sun is up.
bool solarNear(double fresh, float lat, float lon, Eclipse& out) {
  double best = 1e9, bestJd = 0, ss = 0, sm = 0;
  for(int i = -60; i <= 60; ++i) {
    const double jd = fresh + i * 4.0 / 1440;
    const SkyPoint s = sunPosition(jd), m = topocentric(moonPosition(jd), jd, lat, lon);
    const double d = separationDeg(s, m);
    const double sunR = 959.63 / 3600 / (s.distKm / AU_KM), moonR = asin(1737.4 / m.distKm) / D2R;
    if(altitudeDeg(s, jd, lat, lon) > -0.8f && d < sunR + moonR && d < best) { best = d; bestJd = jd; ss = sunR; sm = moonR; }
  }
  if(!bestJd) return false;
  const EclipseKind kind = (sm > ss && best < sm - ss) ? ECLIPSE_TOTAL : (sm < ss && best < ss - sm) ? ECLIPSE_ANNULAR : ECLIPSE_PARTIAL;
  auto touching = [&](double jd) {
    const SkyPoint s = sunPosition(jd), m = topocentric(moonPosition(jd), jd, lat, lon);
    return separationDeg(s, m) < 959.63 / 3600 / (s.distKm / AU_KM) + asin(1737.4 / m.distKm) / D2R;
  };
  const double a = contact(touching, bestJd, bestJd - 0.2), b = contact(touching, bestJd, bestJd + 0.2);
  out = {true, kind, true, jdToTime(bestJd), jdToTime(a), jdToTime(b), float((ss + sm - best) / (2 * ss))};
  return true;
}
}

int findEclipses(time_t from, float lat, float lon, Eclipse* out, int max) {
  int n = 0;
  double t = timeToJd(from) - 1;
  const double end = t + 3 * 365.25;
  double full = nextPhase(t, 180), fresh = nextPhase(t, 0);
  while(std::min(full, fresh) < end && n < max) {
    vTaskDelay(1);   // seconds of work in total: let the idle task (watchdog) and the UI run
    if(full < fresh) { if(nearNode(full) && lunarNear(full, lat, lon, out[n])) ++n; full = nextPhase(full + 1, 180); }
    else { if(nearNode(fresh) && solarNear(fresh, lat, lon, out[n])) ++n; fresh = nextPhase(fresh + 1, 0); }
  }
  return n;
}

bool eclipseNear(time_t utc, bool solar, float lat, float lon, Eclipse& out) {
  const double jd = timeToJd(utc);
  const double phase = nextPhase(jd - 3, solar ? 0 : 180);
  if(fabs(phase - jd) > 3) return false;   // the next phase is a month away: no eclipse "near" utc
  return solar ? solarNear(phase, lat, lon, out) : lunarNear(phase, lat, lon, out);
}

bool lunarShadow(time_t utc, float& dx, float& dy, float& umbraR, float& penumbraR) {
  const double jd = timeToJd(utc);
  const SkyPoint s = sunPosition(jd), m = moonPosition(jd);
  const Shadow sh = earthShadow(jd);
  if(sh.sep > sh.penumbra + sh.moonR) return false;
  const double ara = s.ra + M_PI, adec = -s.dec;
  dx = float(-(wrapPi(ara - m.ra) * cos(m.dec)) / D2R / sh.moonR);   // east (larger RA) to the left
  dy = float(-((adec - m.dec) / D2R) / sh.moonR);                    // north up
  umbraR = float(sh.umbra / sh.moonR);
  penumbraR = float(sh.penumbra / sh.moonR);
  return true;
}

bool solarCover(time_t utc, float lat, float lon, float& dx, float& dy, float& moonR) {
  const double jd = timeToJd(utc);
  const SkyPoint s = sunPosition(jd), m = topocentric(moonPosition(jd), jd, lat, lon);
  const double sunR = 959.63 / 3600 / (s.distKm / AU_KM), mr = asin(1737.4 / m.distKm) / D2R;
  if(separationDeg(s, m) > sunR + mr) return false;
  dx = float(-(wrapPi(m.ra - s.ra) * cos(s.dec)) / D2R / sunR);
  dy = float(-((m.dec - s.dec) / D2R) / sunR);
  moonR = float(mr / sunR);
  return true;
}
