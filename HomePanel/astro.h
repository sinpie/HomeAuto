#pragma once
#include <stdint.h>
#include <time.h>

// Sun and moon positions, rise/set, phase and eclipses, computed on the device
// (Meeus, "Astronomical Algorithms", low-precision series: sun ~0.01 deg, moon
// ~0.05 deg). Checked against NASA eclipse data: lunar eclipse maxima within ~3
// minutes, magnitudes within ~0.02; local solar eclipse magnitudes within ~0.1.

struct SkyPoint { double ra, dec, distKm; };   // radians, radians, km

double julianDay(time_t utc);
SkyPoint sunPosition(double jd);               // geocentric
SkyPoint moonPosition(double jd);              // geocentric
SkyPoint topocentric(const SkyPoint& p, double jd, float lat, float lon);
float altitudeDeg(const SkyPoint& p, double jd, float lat, float lon);

float sunAltitude(time_t utc, float lat, float lon);
// Naked-eye planets (JPL approximate elements, valid 1800-2050, a few arcminutes; precessed
// from J2000 to the date like the sun and moon):
// Mercury, Venus, Mars, Jupiter, Saturn. Azimuth/altitude in degrees.
constexpr int PLANETS = 5;
void planetsAzAlt(time_t utc, float lat, float lon, float az[PLANETS], float alt[PLANETS]);
// Azimuth (degrees from north through east) and altitude; the moon is topocentric.
void sunAzAlt(time_t utc, float lat, float lon, float& az, float& alt);
void moonAzAlt(time_t utc, float lat, float lon, float& az, float& alt);
float moonAltitude(time_t utc, float lat, float lon);   // topocentric
// Moon age in days from the true sun-moon elongation (0 new .. 14.77 full .. 29.53).
float moonAge(time_t utc);
float moonIllumination(time_t utc);                     // 0..1

// Rise and set within [dayStartUtc, dayStartUtc + 24 h). 0 when it does not happen.
void sunRiseSet(time_t dayStartUtc, float lat, float lon, time_t& rise, time_t& set);
void moonRiseSet(time_t dayStartUtc, float lat, float lon, time_t& rise, time_t& set);

enum EclipseKind : uint8_t { ECLIPSE_NONE, ECLIPSE_PENUMBRAL, ECLIPSE_PARTIAL, ECLIPSE_ANNULAR, ECLIPSE_TOTAL };
struct Eclipse {
  bool solar;
  EclipseKind kind;
  bool visible;        // lunar: moon above the horizon at maximum; solar: only local ones are found
  time_t peak;         // UTC of the maximum
  time_t start, end;   // first and last contact (lunar: penumbra; solar: as seen from here)
  float magnitude;
};
// Eclipses from `from` over the next ~3 years, in time order. Solar eclipses only
// when visible from (lat, lon). Takes a few seconds: call from a background task.
int findEclipses(time_t from, float lat, float lon, Eclipse* out, int max);
// The eclipse at the new (solar) or full (lunar) moon within 3 days of utc, if any.
bool eclipseNear(time_t utc, bool solar, float lat, float lon, Eclipse& out);

// Live eclipse geometry for drawing, in units of the body's radius, as seen facing
// south from the northern hemisphere (x to the west = right, y down = south).
// Lunar: Earth's umbra/penumbra centre relative to the moon centre.
bool lunarShadow(time_t utc, float& dx, float& dy, float& umbraR, float& penumbraR);
// Solar: the moon's centre relative to the sun centre, as seen from (lat, lon).
bool solarCover(time_t utc, float lat, float lon, float& dx, float& dy, float& moonR);
