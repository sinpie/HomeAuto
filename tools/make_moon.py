"""Moon surface map for the panel (a drawing, not a photo).

Renders the near side as seen from Earth (north up, Mare Crisium on the right) as a
colour map with four tones: highlands, seas, darker seas and the brightest young
craters with their rays. Seas and craters are at their real positions with their
real sizes (radius in degrees = diameter km / 2 / 1737 km). Crater coordinates are
rounded values (about 1 degree), good enough for a picture.

The firmware (gfx.cpp moonDisc) only adds the phase, the night side and eclipses on
top of this map; the map itself is packed into assets/assets.bin by assets_pack.py.

  py -3 tools/make_moon.py          # preview -> tools/preview/moon_map.png
"""
import math
from pathlib import Path
import numpy as np

D = math.pi / 180
KM = 1 / 1737.4 / D / 2                  # diameter km -> radius degrees
ROOT = Path(__file__).resolve().parents[1]
MOON_SIZE = 256                          # largest map in the asset (mipmaps down to 16)

# ---- seas: overlapping circles (lat, lon, radius deg) blended as metaballs ----
MARIA = [
    # Mare Imbrium: round basin; Sinus Iridum bay to the north-west; Archimedes plain inside
    (33, -16, 15), (38, -22, 9), (27, -8, 8), (30, -27, 8), (21, -14, 6), (44, -31, 4.2),
    # Oceanus Procellarum: the vast western sea, from Sinus Roris down to Mare Humorum
    (50, -52, 4), (44, -48, 5), (36, -55, 7), (27, -58, 9), (17, -60, 10), (7, -57, 9), (-3, -52, 8),
    (-11, -46, 6), (22, -47, 8), (12, -46, 7), (2, -42, 6), (30, -45, 6), (17, -38, 5),
    # Mare Frigoris: narrow wavy band in the north
    (53, -45, 3.5), (56, -38, 3.2), (58.5, -30, 4.2), (59, -22, 3.6), (57.5, -15, 2.8), (56, -8, 3.2),
    (57, 0, 4.4), (56, 8, 3.8), (54.5, 15, 2.8), (55.5, 22, 3.4), (57, 30, 2.8), (55, 38, 2.4),
    # Palus Putredinis, Palus Nebularum (Apennine side of Imbrium)
    (27, 0, 4), (37, 1, 3),
    # Mare Serenitatis (round), joined to Imbrium through the Caucasus gap; Lacus Somniorum, Lacus Mortis
    (28, 17.5, 10.5), (31, 9, 2.6), (38, 29, 4.5), (35, 33, 3), (45, 27, 2.5),
    # Mare Vaporum, Sinus Medii, Sinus Aestuum: the chain south of the Apennines
    (13, 4, 4.2), (16, 0, 2.8), (2, 1, 3), (11, -8, 3.5), (13, -12, 2.5),
    # Mare Tranquillitatis: irregular, toward Serenitatis (north-west) and Fecunditatis (south-east)
    (8.5, 31, 9.5), (14, 24, 5.5), (19, 22, 3.5), (3, 37, 5.5), (12, 37, 4), (2, 25, 4.5),
    # Sinus Asperitatis and Mare Nectaris (round)
    (-5, 27, 3), (-15, 34.5, 5.2),
    # Mare Crisium: separate oval, longer east-west
    (17, 59, 7.5), (17, 54, 4.5), (17, 64, 4.5),
    # Mare Fecunditatis: large, irregular, reaching south
    (-4, 52, 6.5), (-11, 51, 7), (-18, 50, 4.5), (-7, 45, 4), (0, 57, 3.5),
    # Mare Insularum, Cognitum, Nubium (irregular), Humorum (round)
    (7.5, -31, 7.5), (4, -22, 4), (-10, -23, 5), (-17, -13, 6.5), (-23, -18, 7), (-26, -11, 4),
    (-14, -8, 4), (-24.4, -38.6, 6.2),
    # dark-floored craters and plains: Grimaldi, Riccioli, Plato
    (-5.2, -68.6, 2.6), (-3.0, -74.6, 1.6), (51.6, -9.4, 1.5),
    # eastern limb seas (seen edge-on): Spumans, Undarum, Marginis, Smythii, Humboldtianum, Australe
    (1, 65, 2.5), (7, 69, 3), (13, 86, 4.5), (-1, 87, 5), (57, 81, 4.5), (-46, 91, 6),
]
DARK_MARIA = [   # darkest parts: Tranquillitatis, Crisium, western Procellarum, south Imbrium, Humorum, ...
    (8, 30, 8), (3, 36, 4.5), (17, 59, 6.5), (30, -58, 6), (18, -62, 7), (6, -58, 6),
    (26, -20, 6), (-24.4, -38.6, 4), (-6, 52, 4), (14, 5, 3),
]

# ---- named craters: (name, lat, lon, diameter km) ----
CRATERS = [
    ('Plato', 51.6, -9.4, 101), ('Archimedes', 29.7, -4.0, 81), ('Aristillus', 33.9, 1.2, 55), ('Autolycus', 30.7, 1.5, 39),
    ('Cassini', 40.2, 4.6, 57), ('Eudoxus', 44.3, 16.3, 67), ('Aristoteles', 50.2, 17.4, 87), ('Timocharis', 26.7, -13.1, 33),
    ('Lambert', 25.8, -21.0, 30), ('Euler', 23.3, -29.2, 27), ('Eratosthenes', 14.5, -11.3, 58), ('Pytheas', 20.5, -20.6, 20),
    ('Reinhold', 3.3, -22.8, 42), ('Lansberg', -0.3, -26.6, 39), ('Encke', 4.6, -36.6, 28), ('Marius', 11.9, -50.8, 41),
    ('Herodotus', 23.2, -49.7, 35), ('Seleucus', 21.0, -66.6, 43), ('Krafft', 16.6, -72.6, 51), ('Cavalerius', 5.1, -66.8, 57),
    ('Hevelius', 2.2, -67.6, 115), ('Grimaldi', -5.2, -68.6, 173), ('Riccioli', -3.3, -74.6, 139), ('Flamsteed', -4.5, -44.3, 20),
    ('Letronne', -10.8, -42.5, 119), ('Billy', -13.8, -50.1, 46), ('Gassendi', -17.5, -39.9, 101), ('Mersenius', -21.5, -49.2, 84),
    ('Byrgius', -24.7, -65.3, 87), ('Darwin', -20.2, -69.5, 120), ('Vieta', -29.2, -56.3, 87), ('Doppelmayer', -28.5, -41.4, 63),
    ('Schickard', -44.3, -54.6, 206), ('Wargentin', -49.6, -60.2, 84), ('Bailly', -66.5, -69.1, 301), ('Zucchius', -61.4, -50.3, 64),
    ('Schiller', -51.8, -40.0, 179), ('Hainzel', -41.3, -33.5, 70), ('Scheiner', -60.5, -27.8, 110), ('Clavius', -58.4, -14.4, 225),
    ('Longomontanus', -49.6, -21.7, 145), ('Wilhelm', -43.1, -20.4, 106), ('Heinsius', -39.5, -17.7, 64), ('Maginus', -50.5, -6.3, 194),
    ('Moretus', -70.6, -5.5, 111), ('Orontius', -40.3, -4.0, 105), ('Capuanus', -34.1, -26.7, 59), ('Campanus', -28.0, -27.8, 46),
    ('Mercator', -29.3, -26.1, 46), ('Bullialdus', -20.7, -22.2, 61), ('Pitatus', -29.9, -13.5, 106), ('Fra Mauro', -6.1, -17.0, 101),
    ('Bonpland', -8.3, -17.4, 60), ('Parry', -7.9, -15.8, 47), ('Gambart', 1.0, -15.2, 25), ('Mosting', -0.7, -5.9, 25),
    ('Herschel', -5.7, -2.1, 40), ('Ptolemaeus', -9.3, -1.9, 153), ('Alphonsus', -13.4, -3.2, 108), ('Arzachel', -18.2, -1.9, 96),
    ('Thebit', -22.0, -4.0, 57), ('Purbach', -25.5, -2.3, 115), ('Regiomontanus', -28.3, -1.0, 108), ('Walter', -33.0, 0.7, 132),
    ('Deslandres', -32.5, -5.2, 256), ('Albategnius', -11.2, 4.1, 114), ('Hipparchus', -5.1, 5.2, 138), ('Werner', -28.0, 3.3, 70),
    ('Aliacensis', -30.6, 5.2, 80), ('Stofler', -41.1, 6.0, 126), ('Licetus', -47.1, 6.7, 75), ('Cuvier', -50.3, 9.9, 75),
    ('Maurolycus', -41.8, 14.0, 114), ('Gemma Frisius', -34.2, 13.3, 88), ('Barocius', -44.9, 16.8, 82), ('Baco', -51.0, 19.1, 69),
    ('Sacrobosco', -23.7, 16.7, 98), ('Abulfeda', -13.8, 13.9, 62), ('Catharina', -18.0, 23.6, 100), ('Cyrillus', -13.2, 24.0, 98),
    ('Theophilus', -11.4, 26.4, 100), ('Piccolomini', -29.7, 32.2, 88), ('Fracastorius', -21.5, 33.2, 112), ('Rheita', -37.1, 47.2, 70),
    ('Janssen', -44.9, 41.0, 199), ('Metius', -40.3, 43.3, 87), ('Fabricius', -42.9, 42.0, 78), ('Pitiscus', -50.4, 30.9, 82),
    ('Hommel', -54.6, 33.0, 125), ('Vlacq', -53.3, 38.8, 89), ('Rosenberger', -55.4, 43.1, 95), ('Steinheil', -48.6, 46.5, 67),
    ('Stevinus', -32.5, 54.2, 74), ('Furnerius', -36.0, 60.6, 135), ('Snellius', -29.3, 55.7, 82), ('Petavius', -25.3, 60.4, 177),
    ('Vendelinus', -16.4, 61.6, 131), ('Langrenus', -8.9, 61.1, 132), ('Goclenius', -10.0, 45.0, 72), ('Gutenberg', -8.6, 41.2, 74),
    ('Neander', -31.3, 39.9, 50), ('Reichenbach', -30.3, 48.0, 71), ('Taruntius', 5.6, 46.5, 56), ('Apollonius', 4.5, 61.1, 53),
    ('Firmicus', 7.3, 63.4, 56), ('Condorcet', 12.1, 69.6, 74), ('Cleomedes', 27.7, 56.0, 125), ('Macrobius', 21.3, 46.0, 63),
    ('Romer', 25.4, 36.4, 39), ('Littrow', 21.5, 31.4, 30), ('Plinius', 15.4, 23.7, 43), ('Julius Caesar', 9.0, 15.4, 90),
    ('Agrippa', 4.1, 10.5, 44), ('Godin', 1.8, 10.2, 34), ('Triesnecker', 4.2, 3.6, 26), ('Manilius', 14.5, 9.1, 38),
    ('Menelaus', 16.3, 16.0, 26), ('Posidonius', 31.8, 29.9, 95), ('Geminus', 34.5, 56.7, 85), ('Messala', 39.2, 60.5, 125),
    ('Gauss', 35.7, 79.0, 177), ('Hahn', 31.3, 73.6, 84), ('Franklin', 38.8, 47.7, 56), ('Cepheus', 40.8, 45.8, 39),
    ('Atlas', 46.7, 44.4, 87), ('Hercules', 46.7, 39.1, 69), ('Burg', 45.0, 28.2, 40), ('Mercurius', 46.6, 66.2, 67),
    ('Endymion', 53.9, 57.0, 123), ('Archytas', 58.7, 5.0, 32), ('W. Bond', 65.4, 4.5, 156), ('Barrow', 71.3, 7.7, 93),
    ('Meton', 73.6, 18.8, 130), ('Goldschmidt', 73.2, -3.8, 113), ('Anaxagoras', 73.4, -10.1, 50), ('Philolaus', 72.1, -32.4, 70),
    ('Fontenelle', 63.4, -18.9, 38), ('J. Herschel', 62.0, -42.0, 165), ('Pythagoras', 63.5, -63.0, 142), ('Harpalus', 52.6, -43.4, 39),
    ('Mairan', 41.6, -43.4, 40), ('Le Verrier', 40.3, -20.6, 20), ('Helicon', 40.4, -23.1, 24), ('Birt', -22.4, -8.5, 16),
]
# The brightest young craters (fourth tone). rays: (count, min len, max len deg, strength)
BRIGHT = {
    'Tycho':       dict(lat=-43.3, lon=-11.4, km=85, rays=(30, 12, 60, 0.8), halo=4.0, collar=True, seed=1),
    'Copernicus':  dict(lat=9.6,   lon=-20.1, km=93, rays=(40, 5, 22, 0.55), halo=3.6, collar=False, seed=2),
    'Kepler':      dict(lat=8.1,   lon=-38.0, km=31, rays=(26, 4, 16, 0.55), halo=4.5, collar=False, seed=3),
    'Aristarchus': dict(lat=23.7,  lon=-47.4, km=40, rays=(12, 3, 10, 0.4), halo=3.0, collar=False, seed=4, floor=1.0),
    'Proclus':     dict(lat=16.1,  lon=46.8,  km=28, rays=(14, 4, 14, 0.4), halo=2.5, collar=False, seed=5),
    'Anaxagoras':  dict(lat=73.4,  lon=-10.1, km=50, rays=(12, 4, 14, 0.35), halo=2.5, collar=False, seed=6),
}

# Small unnamed craterlets on the sea floors (positions are illustrative, not catalogued).
_crng = np.random.default_rng(11)
SEA_CRATERLETS = []
while len(SEA_CRATERLETS) < 90:
    _la, _lo = float(_crng.uniform(-30, 55)), float(_crng.uniform(-75, 70))
    if any(math.hypot(_la - a, (_lo - b) * math.cos(_la * D)) < r * 0.8 for a, b, r in MARIA):
        SEA_CRATERLETS.append((_la, _lo, float(_crng.uniform(0.25, 0.7))))
# Highland craters by the real size distribution. Robbins (2019) lists about 83,000 craters
# of 5 km or more on the whole moon (cumulative count ~ D^-2), so about 21,000 of 10 km or
# more; roughly half are on the near side and ~70% of that is highland: ~7,000. 10 km is
# about one map pixel, the smallest that shows. Positions are illustrative (not the catalogue).
_hrng = np.random.default_rng(23)
HIGHLAND_CRATERS = []
while len(HIGHLAND_CRATERS) < 7000:
    _la = math.degrees(math.asin(_hrng.uniform(-0.995, 0.995))); _lo = float(_hrng.uniform(-90, 90))
    if any(math.hypot(_la - a, (_lo - b) * math.cos(_la * D)) < r * 1.05 for a, b, r in MARIA):
        continue
    _km = min(100.0, 10 * (1 - _hrng.uniform()) ** -0.5)       # N(>D) ~ D^-2 from 10 km
    HIGHLAND_CRATERS.append((_la, _lo, _km * KM))
HIGH = np.array([218, 220, 224.]); MARE = np.array([174, 177, 183.]); MARE_SHORE = np.array([188, 191, 196.]); MARE_DARK = np.array([152, 156, 164.])
FLOOR = np.array([200, 204, 212.]); RIM = np.array([238, 240, 244.]); WHITE = np.array([253, 253, 253.])
COLLAR = np.array([186, 190, 198.])


def vec(lat, lon):
    return np.array([math.cos(lat * D) * math.sin(lon * D), math.sin(lat * D), math.cos(lat * D) * math.cos(lon * D)])


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


_rng = np.random.default_rng(7)
WAVES = []
for f in (5, 7, 9, 13, 17, 23, 31, 43):
    k = _rng.normal(size=3)
    WAVES.append((k / np.linalg.norm(k) * f, _rng.uniform(0, 6.28)))


FINE = []                                                 # higher frequencies: coastlines, sea texture
for f in (37, 53, 71, 97, 131, 173):
    k = _rng.normal(size=3)
    FINE.append((k / np.linalg.norm(k) * f, _rng.uniform(0, 6.28)))


def noise(P, waves=WAVES):
    n = np.zeros(P.shape[:2])
    for k, ph in waves:
        n += np.sin(P @ k + ph)
    return n * (2 / len(waves))


def field(P, blobs, blend=1.55):
    f = np.zeros(P.shape[:2])
    for la, lo, rad in blobs:
        d2 = 2 * (1 - P @ vec(la, lo)); R = rad * blend * D
        f += np.clip(1 - d2 / (R * R), 0, None) ** 2
    return f


def local_frame(la, lo):
    east = np.array([math.cos(lo * D), 0, -math.sin(lo * D)])
    north = np.array([-math.sin(la * D) * math.sin(lo * D), math.cos(la * D), -math.sin(la * D) * math.cos(lo * D)])
    return east, north


def angle_deg(P, c):
    return np.sqrt(np.clip(2 * (1 - P @ c), 0, None)) / D


def crater_height(d, rad, peak=False):
    """Height (moon radii) of a crater: bowl or flat floor, raised rim, optional central peak.
    The firmware lights this with the real sun direction, so shadows follow the phase."""
    R = rad * D                                           # crater radius in moon radii
    t = d / rad
    depth, rim = 0.10 * R, 0.03 * R
    h = np.where(t < 1, depth * (np.maximum(t, 0.45) ** 2 - 1), 0.0)
    h = h + rim * np.exp(-((t - 1) / 0.22) ** 2)
    if peak:
        h = h + 0.35 * depth * np.exp(-(t / 0.13) ** 2)
    return h


def crater(col, H, P, mare, la, lo, rad, px, flooded=0.0):
    """Ordinary crater: slightly darker floor in the colour map, its shape in the height map.
    flooded (0..1): lava-filled craters in the seas are shallow and faint."""
    c = vec(la, lo)
    if c[2] <= 0:
        return col, H
    N = col.shape[0]
    cx, cy = (c[0] + 1) / 2 * N, (1 - c[1]) / 2 * N
    half = int(rad * 1.7 * D * N / 2) + 3
    x0, x1 = max(0, int(cx) - half), min(N, int(cx) + half + 1)
    y0, y1 = max(0, int(cy) - half), min(N, int(cy) + half + 1)
    if x0 >= x1 or y0 >= y1:
        return col, H
    w = (slice(y0, y1), slice(x0, x1))
    d = angle_deg(P[w], c)
    keep = 1 - 0.75 * flooded
    m = 1 - smooth(rad - px, rad + px, d)
    col[w] = col[w] * (1 - 0.03 * keep * m)[..., None]
    H[w] = H[w] + keep * crater_height(d, rad, peak=rad > 1.5 and flooded < 0.5)
    return col, H


def mare_at(la, lo):
    """Sea coverage (0..1) at one point, from the same field as the map."""
    P = vec(la, lo)[None, None, :]
    return float(smooth(0.32, 0.37, field(P, MARIA) * (1 + 0.25 * noise(P)))[0, 0])


def bright_crater(col, H, P, mare, spec, px):
    la, lo, rad = spec['lat'], spec['lon'], spec['km'] * KM
    c = vec(la, lo); d = angle_deg(P, c)
    east, north = local_frame(la, lo)
    bearing = np.degrees(np.arctan2(P @ east, P @ north))
    rng = np.random.default_rng(spec['seed'])
    count, lmin, lmax, strength = spec['rays']
    # rays: irregular, broken streaks of different length, width and brightness
    ray = np.zeros(P.shape[:2])
    for _ in range(count):
        b0 = rng.uniform(0, 360); length = rng.uniform(lmin, lmax) * rng.uniform(0.6, 1.0)
        width = rng.uniform(0.35, 1.1) * (1 + length / 60); amp = rng.uniform(0.35, 1.0)
        curve = rng.uniform(-0.15, 0.15)
        db = (bearing - b0 - curve * d + 180) % 360 - 180
        across = np.abs(np.sin(np.radians(db))) * d
        front = np.cos(np.radians(db)) > 0
        breakup = 0.55 + 0.45 * np.sin(d * rng.uniform(0.4, 1.2) + rng.uniform(0, 6.28))
        s = (1 - smooth(width * 0.25, width, across)) * (1 - smooth(length * 0.35, length, d)) * front * breakup * amp
        ray = np.maximum(ray, s)
    ray *= smooth(rad * 1.2, rad * 4.0, d)               # rays fade into the ejecta near the crater
    col = col + (WHITE - col) * (ray * strength * 0.9)[..., None]
    # bright ejecta blanket with a ragged edge
    rag = noise(P, [(k * 3, ph) for k, ph in WAVES[3:]])
    halo = 1 - smooth(rad * 1.0, rad * spec['halo'] * (1 + 0.18 * rag), d)
    col = col + (WHITE - col) * (halo * 0.95)[..., None]
    if spec.get('collar'):                               # Tycho's dark ring just outside the rim
        ring = (1 - smooth(0, rad * 0.35, np.abs(d - rad * 1.35))) * 0.35
        col = col + (COLLAR - col) * ring[..., None]
    # the crater itself: bright rim, grey floor (Aristarchus: bright floor), tiny central peak
    m = 1 - smooth(rad - px, rad + px, d)
    rim = 1 - smooth(0, max(rad * 0.2, px * 0.8), np.abs(d - rad * 0.95))
    floor = FLOOR if not spec.get('floor') else WHITE
    col = col + (floor - col) * m[..., None]
    col = col + (WHITE - col) * (rim * 0.8)[..., None]
    peak = 1 - smooth(0, max(rad * 0.12, px * 0.6), d)
    col = col + (WHITE - col) * (peak * 0.6)[..., None]
    return col, H + crater_height(d, rad, peak=True)


def albedo(n, ss=1):
    """(RGB map, relief map) of the disc, n x n pixels. Relief = slope of the height map
    toward the west limb (x), in height per moon radius (outside the disc: unused)."""
    N = n * ss
    o = (np.arange(N) + 0.5) / N * 2 - 1
    X, Y = np.meshgrid(o, o)
    r2 = np.clip(X * X + Y * Y, 0, 1)
    Z = np.sqrt(1 - r2); P = np.stack([X, -Y, Z], -1)
    nrm = np.linalg.norm(P, axis=-1, keepdims=True); P = P / nrm
    px = 2 / N / D * 1.2                                  # one map pixel at the disc centre, degrees
    nz = noise(P)
    fine = noise(P, FINE)
    fm = field(P, MARIA) * (1 + 0.25 * nz + 0.10 * fine)      # coastlines: broad bays plus small jags
    mare = smooth(0.32, 0.37, fm)                        # crisp coastline
    # gradients inside the seas: lighter along the shore, deepening inward
    depth = smooth(0.40, 1.1, fm)
    sea = MARE_SHORE + (MARE - MARE_SHORE) * depth[..., None]
    col = HIGH + (sea - HIGH) * mare[..., None]
    col = col * (1 + 0.035 * nz + 0.02 * fine * (0.4 + 0.6 * mare))[..., None]   # seas get fine texture too
    # darker regions fade in gradually instead of as a second step
    dark = smooth(0.05, 0.9, field(P, DARK_MARIA) * (1 + 0.3 * nz)) * mare
    # Serenitatis: lighter middle, dark band along its edge
    ds = angle_deg(P, vec(28, 17.5))
    dark = np.maximum(dark, (1 - smooth(0, 5, np.abs(ds - 10))) * 0.4 * mare)
    lightmid = (1 - smooth(2, 9, ds)) * 0.35
    # Imbrium: darker toward the south-west (younger lava)
    sw = np.clip(-(P @ np.array([0.70, 0.71, 0.0]) - (vec(33, -16) @ np.array([0.70, 0.71, 0.0]))) * 4, 0, 1)
    dark = np.maximum(dark, sw * (1 - smooth(10, 20, angle_deg(P, vec(33, -16)))) * 0.7 * mare)
    col = col + (MARE_DARK - col) * dark[..., None]
    col = col + (MARE_SHORE - col) * (lightmid * mare)[..., None]
    H = np.zeros(P.shape[:2])
    for name, la, lo, km in sorted(CRATERS, key=lambda c: -c[3]):   # big ones first
        col, H = crater(col, H, P, mare, la, lo, km * KM, px, flooded=mare_at(la, lo))
    for la, lo, rad in HIGHLAND_CRATERS:                  # many small highland craters
        col, H = crater(col, H, P, mare, la, lo, rad, px)
    for la, lo, rad in SEA_CRATERLETS:                    # small fresh craters on the sea floors
        col, H = crater(col, H, P, mare, la, lo, rad, px)
    for spec in BRIGHT.values():
        col, H = bright_crater(col, H, P, mare, spec, px)
    col = np.clip(col, 0, 255)
    G = np.gradient(H, axis=1) * (N / 2)                  # dh/dx per moon radius
    if ss > 1:
        col = col.reshape(n, ss, n, ss, 3).mean(axis=(1, 3))
        G = G.reshape(n, ss, n, ss).mean(axis=(1, 3))
    return col, G


RELIEF_SCALE = 64                                         # int8 relief = slope * RELIEF_SCALE


def mipmaps():
    """[(size, RGB565 little-endian bytes, int8 relief bytes)] from MOON_SIZE down to 16."""
    img, rel = albedo(MOON_SIZE, ss=2)
    out, size = [], MOON_SIZE
    while size >= 16:
        q = np.clip(img, 0, 255).astype(np.uint16)
        v = ((q[..., 0] >> 3) << 11) | ((q[..., 1] >> 2) << 5) | (q[..., 2] >> 3)
        r8 = np.clip(np.round(rel * RELIEF_SCALE), -127, 127).astype(np.int8)
        out.append((size, v.astype('<u2').tobytes(), r8.tobytes()))
        img = img.reshape(size // 2, 2, size // 2, 2, 3).mean(axis=(1, 3))
        rel = rel.reshape(size // 2, 2, size // 2, 2).mean(axis=(1, 3)); size //= 2
    return out


# ---- preview: same lighting as the firmware ----
NIGHT = np.array([51, 65, 85.])


RELIEF_GAIN = 1.0                                         # same constant as gfx.cpp


def shade(col, G, age, ss):
    n = col.shape[0]
    o = (np.arange(n) + 0.5) / n * 2 - 1
    X, Y = np.meshgrid(o, o)
    r2 = X * X + Y * Y; Z = np.sqrt(np.clip(1 - r2, 0, 1))
    theta = 2 * math.pi * age / 29.530588853
    inc = X * math.sin(theta) - Z * math.cos(theta)
    aa = 3 / n
    lit = smooth(-aa, aa * 2.5, inc)
    # relief: tilt the surface normal by the slope; shadows grow toward the terminator
    inc2 = inc - G * math.sin(theta) * RELIEF_GAIN
    relief = np.clip(inc2 / np.maximum(inc, 0.12), 0.45, 1.35)
    # low sun: the colour difference between seas and highlands fades, relief takes over
    flat = np.array([200., 203., 209.])               # same constant as gfx.cpp
    albedo_k = 0.55 + 0.45 * smooth(0.0, 0.5, inc)
    col = flat + (col - flat) * albedo_k[..., None]
    day = col * ((0.84 + 0.16 * smooth(0, 0.6, inc)) * relief)[..., None]
    night = NIGHT + (col - NIGHT) * 0.08
    out = night + (day - night) * lit[..., None]
    cov = smooth(1 + aa, 1 - aa, np.sqrt(r2))
    return np.concatenate([out, (cov * 255)[..., None]], -1)


def preview(path):
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype(r'C:\Windows\Fonts\NotoSansKR-VF.ttf', 15)
    img = Image.new('RGBA', (1000, 420), (15, 23, 42, 255)); d = ImageDraw.Draw(img)
    big = albedo(280, ss=2)
    def put(m, age, cx, cy):
        rgba = shade(m[0], m[1], age, 1).clip(0, 255).astype(np.uint8)
        m = Image.fromarray(rgba, 'RGBA'); img.alpha_composite(m, (int(cx - m.width / 2), int(cy - m.height / 2)))
    put(big, 10.5, 170, 180)
    small = {s: albedo(s, ss=3) for s in (88, 68, 24)}
    for i, age in enumerate([4.5, 7.4, 11, 14.8, 18.5, 22.1]): put(small[88], age, 400 + i * 100, 90)
    for i, age in enumerate([7.4, 14.8, 22.1]): put(small[68], age, 430 + i * 90, 240); put(small[24], age, 720 + i * 50, 240)
    d.text((400, 300), '카드 크기 (지름 68px) / 날씨 아이콘 크기 (지름 24px)', font=font, fill=(148, 163, 184))
    img.convert('RGB').save(path)


if __name__ == '__main__':
    out = ROOT / 'tools' / 'preview' / 'moon_map.png'
    out.parent.mkdir(exist_ok=True)
    preview(out)
    print(out)
