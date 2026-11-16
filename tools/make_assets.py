"""Generate the HomePanel background picture and icon set (original artwork).

Everything is drawn here with Pillow; no third-party image or icon files are used.
Output: assets/background.png (1024x600), then assets/assets.bin via assets_pack.py.
Icon preview sheet: tools/preview/icons.png
Run:  py -3 tools/make_assets.py
      py -3 tools/make_assets.py --photo my_photo.jpg   # use a photo as the background
A photo is center-cropped to 1024x600, slightly darkened at the bottom/left for text contrast.
Use only photos you own or whose license allows this use.
"""
import math
import random
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
W, H = 1024, 600
ICON_SIZES = (24, 32, 56, 80)
SS = 8  # icon supersampling


# ---------------- background: apartment complex at night ----------------
def lerp(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def hexrgb(value):
    return ((value >> 16) & 255, (value >> 8) & 255, value & 255)


def _add(base, layer):
    from PIL import ImageChops
    return ImageChops.add(base, layer)


def skyline(draw, rng, base_y, color, lit, window, density, min_h, max_h, min_w, max_w, lit_ratio):
    x = -20
    while x < W + 20:
        bw = rng.randint(min_w, max_w)
        bh = rng.randint(min_h, max_h)
        top = base_y - bh
        draw.rectangle([x, top, x + bw, H], fill=color)
        # rooftop details
        if rng.random() < 0.35:
            draw.rectangle([x + bw*0.3, top - 8, x + bw*0.45, top], fill=color)
        cols = max(1, (bw - 10) // window[0])
        for row in range(int(bh // window[1])):
            for col in range(cols):
                if rng.random() < density:
                    wx = x + 7 + col * window[0]
                    wy = top + 8 + row * window[1]
                    on = rng.random() < lit_ratio
                    tone = lit[rng.randrange(len(lit))] if on else lerp(color, (60, 72, 110), 0.35)
                    draw.rectangle([wx, wy, wx + window[0]*0.55, wy + window[1]*0.5], fill=tone)
        x += bw + rng.randint(2, 14)


def background():
    rng = random.Random(7)
    sky = Image.new('RGB', (W, H))
    top, mid, horizon = hexrgb(0x070B1F), hexrgb(0x111C44), hexrgb(0x2A2F63)
    px = sky.load()
    for y in range(H):
        t = y / (H*0.72)
        color = lerp(top, mid, min(1, t*1.4)) if t < 0.7 else lerp(mid, horizon, min(1, (t-0.7)/0.3))
        for x in range(W):
            px[x, y] = color
    sky = _add(sky, _blob((W, H), (760, 520), 420, hexrgb(0x5B3C8C), 0.55))   # violet city glow
    sky = _add(sky, _blob((W, H), (180, 470), 360, hexrgb(0x1E5A7A), 0.45))   # teal haze
    sky = _add(sky, _blob((W, H), (880, 92), 120, hexrgb(0x9FB8E8), 0.35))    # moon halo
    draw = ImageDraw.Draw(sky)
    for _ in range(170):                                                      # stars
        x, y = rng.randrange(W), int(rng.random()**1.6 * 300)
        v = rng.randint(90, 230)
        r = 1 if rng.random() < 0.85 else 2
        draw.ellipse([x, y, x + r, y + r], fill=(v, v, min(255, v + 20)))
    moon = Image.new('L', (W, H))
    ImageDraw.Draw(moon).ellipse([856, 68, 904, 116], fill=255)
    ImageDraw.Draw(moon).ellipse([870, 60, 918, 108], fill=0)                 # crescent: subtract a disc
    moon = moon.filter(ImageFilter.GaussianBlur(0.7))
    sky = Image.composite(Image.new('RGB', (W, H), hexrgb(0xE9EEF9)), sky, moon)
    draw = ImageDraw.Draw(sky)

    far = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    skyline(ImageDraw.Draw(far), rng, 470, hexrgb(0x18214A), [hexrgb(0x46548C)], (14, 18), 0.6, 120, 250, 50, 110, 0.4)
    far = far.filter(ImageFilter.GaussianBlur(1.2))
    sky.paste(far, (0, 0), far)
    near = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    warm = [hexrgb(0xF6C978), hexrgb(0xFFD99A), hexrgb(0xF2B866), hexrgb(0xBFD4FF)]
    skyline(ImageDraw.Draw(near), rng, 560, hexrgb(0x0C1230), warm, (18, 22), 0.75, 170, 330, 80, 150, 0.28)
    sky.paste(near, (0, 0), near)
    # bottom fade for readability
    fade = Image.new('L', (W, H))
    fp = fade.load()
    for y in range(H):
        a = int(max(0, (y - 380) / 220) ** 1.4 * 200)
        for x in range(W):
            fp[x, y] = a
    sky = Image.composite(Image.new('RGB', (W, H), hexrgb(0x050814)), sky, fade)
    return sky


def _blob(size, center, radius, color, strength):
    layer = Image.new('RGB', size, (0, 0, 0))
    ImageDraw.Draw(layer).ellipse([center[0]-radius, center[1]-radius, center[0]+radius, center[1]+radius],
                                  fill=tuple(int(c*strength) for c in color))
    return layer.filter(ImageFilter.GaussianBlur(radius*0.5))


def photo_background(path, center_y=0.5):
    from PIL import ImageEnhance, ImageOps
    image = ImageOps.exif_transpose(Image.open(path)).convert('RGB')
    image = ImageOps.fit(image, (W, H), Image.LANCZOS, centering=(0.5, center_y))
    image = ImageEnhance.Brightness(image).enhance(0.78)
    image = ImageEnhance.Color(image).enhance(0.92)
    # Darken toward the bottom and the sidebar so glass cards and text stay readable.
    shade = Image.new('L', (W, H))
    sp = shade.load()
    for y in range(H):
        for x in range(W):
            sp[x, y] = int(min(150, max(0, (y - 260) / 340) * 150 + max(0, (220 - x) / 220) * 70))
    return Image.composite(Image.new('RGB', (W, H), (4, 7, 18)), image, shade)


def to_rgb565(image):
    data = []
    for r, g, b in image.getdata():
        data.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return data


# ---------------- icons (stroke style, rounded caps) ----------------
class Pen:
    def __init__(self, size):
        self.n = size * SS
        self.img = Image.new('L', (self.n, self.n))
        self.d = ImageDraw.Draw(self.img)
        self.w = max(2, round(self.n * 0.085))

    def p(self, x, y):
        return (x * self.n, y * self.n)

    def line(self, *pts, width=None, closed=False):
        width = width or self.w
        pts = [self.p(*q) for q in pts]
        if closed:
            pts = pts + [pts[0]]
        self.d.line(pts, fill=255, width=width, joint='curve')
        for x, y in pts:
            r = width / 2
            self.d.ellipse([x - r, y - r, x + r, y + r], fill=255)

    def arc(self, cx, cy, r, start, end, width=None, steps=48):
        pts = [(cx + r*math.cos(math.radians(start + (end-start)*i/steps)),
                cy + r*math.sin(math.radians(start + (end-start)*i/steps))) for i in range(steps + 1)]
        self.line(*pts, width=width)

    def fill(self, *pts):
        self.d.polygon([self.p(*q) for q in pts], fill=255)

    def disc(self, cx, cy, r, value=255):
        self.d.ellipse([*self.p(cx - r, cy - r), *self.p(cx + r, cy + r)], fill=value)

    def rrect(self, x0, y0, x1, y1, r, width=None):
        self.d.rounded_rectangle([*self.p(x0, y0), *self.p(x1, y1)], radius=r*self.n, outline=255, width=width or self.w)

    def done(self, size):
        return self.img.resize((size, size), Image.LANCZOS)


def icon_home(p):
    p.line((0.14, 0.48), (0.5, 0.16), (0.86, 0.48))
    p.line((0.24, 0.40), (0.24, 0.84), (0.76, 0.84), (0.76, 0.40))
    p.line((0.42, 0.84), (0.42, 0.62), (0.58, 0.62), (0.58, 0.84))


def icon_flame(p):
    def drop(cx, top, bottom, width, steps=48):
        mid, half = (top + bottom) / 2, (bottom - top) / 2
        return [(cx + width * math.sin(t) * math.sin(t / 2), mid - half * math.cos(t))
                for t in (2 * math.pi * i / steps for i in range(steps + 1))]
    p.line(*drop(0.5, 0.12, 0.88, 0.40))
    p.line(*drop(0.5, 0.56, 0.88, 0.17))


def icon_bell(p):
    p.arc(0.5, 0.44, 0.24, 180, 360)
    p.line((0.26, 0.44), (0.26, 0.64), (0.16, 0.74), (0.84, 0.74), (0.74, 0.64), (0.74, 0.44))
    p.arc(0.5, 0.80, 0.08, 20, 160)
    p.line((0.5, 0.12), (0.5, 0.20))


def icon_gear(p):
    pts = []
    teeth = 8
    for i in range(teeth * 4):
        a = 2 * math.pi * i / (teeth * 4) - math.pi / (teeth * 4)
        r = 0.40 if i % 4 in (1, 2) else 0.31
        pts.append((0.5 + r * math.cos(a), 0.5 + r * math.sin(a)))
    p.line(*pts, closed=True)
    p.arc(0.5, 0.5, 0.11, 0, 360)


def icon_elevator(p):
    p.rrect(0.14, 0.12, 0.86, 0.88, 0.12)
    p.fill((0.36, 0.30), (0.48, 0.46), (0.24, 0.46))
    p.fill((0.64, 0.70), (0.76, 0.54), (0.52, 0.54))
    p.line((0.5, 0.22), (0.5, 0.78), width=max(1, p.w // 2))


def icon_headset(p):
    p.arc(0.5, 0.52, 0.31, 180, 360)
    p.d.rounded_rectangle([*p.p(0.13, 0.46), *p.p(0.31, 0.74)], radius=0.06*p.n, fill=255)
    p.d.rounded_rectangle([*p.p(0.69, 0.46), *p.p(0.87, 0.74)], radius=0.06*p.n, fill=255)
    p.arc(0.56, 0.70, 0.22, 10, 90)
    p.disc(0.54, 0.92, 0.06)


def icon_door(p):
    p.line((0.22, 0.86), (0.22, 0.14), (0.78, 0.14), (0.78, 0.86))
    p.line((0.12, 0.86), (0.88, 0.86))
    p.disc(0.65, 0.52, 0.06)


def icon_building(p):
    p.line((0.20, 0.88), (0.20, 0.12), (0.80, 0.12), (0.80, 0.88))
    p.line((0.10, 0.88), (0.90, 0.88))
    for y in (0.28, 0.46):
        for x in (0.36, 0.64):
            p.disc(x, y, 0.055)
    p.line((0.42, 0.88), (0.42, 0.66), (0.58, 0.66), (0.58, 0.88))


def icon_parking(p):
    p.rrect(0.12, 0.12, 0.88, 0.88, 0.18)
    p.line((0.38, 0.72), (0.38, 0.28), (0.54, 0.28))
    p.arc(0.54, 0.40, 0.12, -90, 90)
    p.line((0.54, 0.52), (0.38, 0.52))


def icon_power(p):
    p.arc(0.5, 0.54, 0.30, -60, 240)
    p.line((0.5, 0.14), (0.5, 0.48))


def icon_thermo(p):
    p.arc(0.5, 0.70, 0.15, -60, 240)
    p.line((0.425, 0.57), (0.425, 0.22))
    p.arc(0.5, 0.22, 0.075, 180, 360)
    p.line((0.575, 0.22), (0.575, 0.57))
    p.disc(0.5, 0.70, 0.07)
    p.line((0.5, 0.66), (0.5, 0.40))


def icon_plus(p):
    p.line((0.5, 0.2), (0.5, 0.8))
    p.line((0.2, 0.5), (0.8, 0.5))


def icon_minus(p):
    p.line((0.2, 0.5), (0.8, 0.5))


def icon_chevron(p):
    p.line((0.38, 0.22), (0.66, 0.5), (0.38, 0.78))


def icon_check(p):
    p.line((0.2, 0.52), (0.42, 0.74), (0.82, 0.30))


def icon_up(p):
    p.fill((0.5, 0.18), (0.86, 0.72), (0.14, 0.72))


def icon_down(p):
    p.fill((0.5, 0.82), (0.86, 0.28), (0.14, 0.28))


def icon_sun(p):
    p.arc(0.5, 0.5, 0.16, 0, 360)
    for i in range(8):
        a = math.radians(i * 45)
        p.line((0.5 + 0.28*math.cos(a), 0.5 + 0.28*math.sin(a)), (0.5 + 0.38*math.cos(a), 0.5 + 0.38*math.sin(a)))



# ---- weather icons ----
def cloud_outline(p, dx=0.0, dy=0.0, k=1.0):
    """A cloud: flat base with three bumps, drawn as one outline."""
    def q(x, y):
        return (0.5 + (x - 0.5) * k + dx, 0.5 + (y - 0.5) * k + dy)
    pts = [q(0.22, 0.72)]
    for cx, cy, r, a0, a1 in ((0.24, 0.60, 0.12, 90, 270), (0.42, 0.44, 0.17, 180, 340), (0.70, 0.56, 0.16, 250, 450)):
        for i in range(17):
            a = math.radians(a0 + (a1 - a0) * i / 16)
            pts.append(q(cx + r * math.cos(a), cy + r * math.sin(a)))
    pts.append(q(0.22, 0.72))
    p.line(*pts)


def icon_cloud(p):
    cloud_outline(p)


def icon_cloud_sun(p):
    p.arc(0.66, 0.30, 0.13, 150, 400)
    for a in (-90, -45, 0, 45):
        r = math.radians(a)
        p.line((0.66 + 0.19*math.cos(r), 0.30 + 0.19*math.sin(r)), (0.66 + 0.26*math.cos(r), 0.30 + 0.26*math.sin(r)))
    cloud_outline(p, -0.06, 0.08, 0.86)


def icon_rain(p):
    cloud_outline(p, 0, -0.12, 0.9)
    for x in (0.34, 0.52, 0.70):
        p.line((x, 0.72), (x - 0.06, 0.88))


def icon_snow(p):
    cloud_outline(p, 0, -0.12, 0.9)
    for x in (0.32, 0.52, 0.72):
        p.disc(x, 0.82, 0.045)


def icon_storm(p):
    cloud_outline(p, 0, -0.14, 0.9)
    p.fill((0.52, 0.60), (0.36, 0.80), (0.50, 0.80), (0.42, 0.96), (0.66, 0.72), (0.52, 0.72), (0.60, 0.60))


def icon_fog(p):
    cloud_outline(p, 0, -0.14, 0.9)
    p.line((0.18, 0.74), (0.82, 0.74))
    p.line((0.26, 0.88), (0.74, 0.88))


def icon_moon(p):
    """Filled crescent (a disc minus an offset disc) with a small four-point star."""
    p.disc(0.44, 0.54, 0.34)
    p.disc(0.62, 0.40, 0.30, value=0)
    cx, cy, r, k = 0.76, 0.20, 0.11, 0.035
    p.fill((cx, cy - r), (cx + k, cy - k), (cx + r, cy), (cx + k, cy + k),
           (cx, cy + r), (cx - k, cy + k), (cx - r, cy), (cx - k, cy - k))


def icon_drop(p):
    pts = []
    for i in range(41):
        t = 2 * math.pi * i / 40
        pts.append((0.5 + 0.26 * math.sin(t) * math.sin(t / 2), 0.54 - 0.34 * math.cos(t)))
    p.line(*pts)


def icon_wind(p):
    p.line((0.14, 0.40), (0.62, 0.40))
    p.arc(0.62, 0.30, 0.10, 90, -180)
    p.line((0.14, 0.58), (0.74, 0.58))
    p.arc(0.74, 0.68, 0.10, -90, 180)
    p.line((0.14, 0.76), (0.46, 0.76))


def icon_sunrise(p):
    p.arc(0.5, 0.70, 0.20, 180, 360)
    p.line((0.12, 0.70), (0.88, 0.70))
    p.line((0.5, 0.40), (0.5, 0.14))
    p.line((0.40, 0.24), (0.5, 0.14), (0.60, 0.24))
    p.line((0.20, 0.84), (0.80, 0.84))


def icon_sunset(p):
    p.arc(0.5, 0.70, 0.20, 180, 360)
    p.line((0.12, 0.70), (0.88, 0.70))
    p.line((0.5, 0.14), (0.5, 0.40))
    p.line((0.40, 0.30), (0.5, 0.40), (0.60, 0.30))
    p.line((0.20, 0.84), (0.80, 0.84))


# ---- layered weather icons (filled masks; the firmware colors and stacks them) ----
# Shared geometry (unit box), mirrored in app.cpp drawWeatherIcon():
#   full body  : center (0.50, 0.50), disc r 0.25, rays 0.33..0.45
#   part body  : center (0.36, 0.36), disc r 0.19, rays 0.25..0.33
#   twilight   : half sun on the horizon at y 0.70, center x 0.50, r 0.22
def rays(p, cx, cy, r0, r1, n=8, width=None):
    for i in range(n):
        a = 2 * math.pi * i / n
        p.line((cx + r0 * math.cos(a), cy + r0 * math.sin(a)), (cx + r1 * math.cos(a), cy + r1 * math.sin(a)),
               width=width or round(p.w * 1.1))


def cloud_fill(p, x0, y0, x1, y1, value=255):
    """Filled cloud in the box (x0,y0)-(x1,y1): flat rounded base with two bumps."""
    w, h = x1 - x0, y1 - y0
    base_top = y0 + h * 0.48
    p.d.rounded_rectangle([*p.p(x0, base_top), *p.p(x1, y1)], radius=(y1 - base_top) * p.n / 2, fill=value)
    p.disc(x0 + w * 0.34, y0 + h * 0.56, h * 0.36, value)
    p.disc(x0 + w * 0.62, y0 + h * 0.42, h * 0.42, value)


def wx_rays_full(p):
    rays(p, 0.50, 0.50, 0.335, 0.445)


def wx_rays_part(p):
    rays(p, 0.36, 0.36, 0.255, 0.335, width=round(p.w * 0.95))


def wx_cloud_main(p):     # overcast / precipitation cloud, upper part of the box
    cloud_fill(p, 0.10, 0.20, 0.90, 0.66)


def wx_cloud_main_shade(p):  # lower band of the main cloud (darker tone)
    cloud_fill(p, 0.10, 0.20, 0.90, 0.66)
    p.d.rectangle([*p.p(0, 0), *p.p(1, 0.52)], fill=0)


def wx_cloud_back(p):     # second cloud behind (overcast depth)
    cloud_fill(p, 0.26, 0.10, 0.94, 0.48)


def wx_cloud_part(p):     # partly cloudy: big cloud in front, lower right
    cloud_fill(p, 0.26, 0.40, 0.94, 0.82)


def wx_cloud_part_shade(p):
    cloud_fill(p, 0.26, 0.40, 0.94, 0.82)
    p.d.rectangle([*p.p(0, 0), *p.p(1, 0.70)], fill=0)


def wx_cloud_small(p):    # mostly clear: small cloud, lower right
    cloud_fill(p, 0.38, 0.50, 0.92, 0.82)


def drop(p, x, y, length=0.15):
    p.line((x, y), (x - length * 0.35, y + length), width=round(p.w * 1.05))


def wx_rain(p):
    for x in (0.34, 0.52, 0.70):
        drop(p, x, 0.74)


def wx_rain_heavy(p):
    for x, y in ((0.26, 0.72), (0.42, 0.78), (0.58, 0.72), (0.74, 0.78)):
        drop(p, x, y, 0.16)


def wx_drizzle(p):
    for x, y in ((0.32, 0.76), (0.50, 0.82), (0.68, 0.76), (0.41, 0.92), (0.59, 0.92)):
        p.disc(x, y, 0.035)


def flake(p, x, y, r):
    for a in (0, 60, 120):
        t = math.radians(a)
        p.line((x - r * math.cos(t), y - r * math.sin(t)), (x + r * math.cos(t), y + r * math.sin(t)), width=max(2, round(p.w * 0.5)))


def wx_snow(p):
    for x, y in ((0.32, 0.80), (0.52, 0.88), (0.72, 0.80)):
        flake(p, x, y, 0.065)


def wx_sleet(p):
    drop(p, 0.34, 0.74)
    flake(p, 0.52, 0.86, 0.06)
    drop(p, 0.72, 0.74)


def wx_bolt(p):
    p.fill((0.55, 0.58), (0.38, 0.80), (0.50, 0.80), (0.42, 0.98), (0.68, 0.72), (0.55, 0.72), (0.63, 0.58))


def wx_fog(p):
    p.line((0.16, 0.76), (0.84, 0.76), width=round(p.w * 1.1))
    p.line((0.26, 0.90), (0.74, 0.90), width=round(p.w * 1.1))


def wx_twilight_rays(p):  # rays of a sun sitting on the horizon (upper half only)
    for i in range(5):
        a = math.pi + math.pi * (i + 1) / 6
        p.line((0.50 + 0.30 * math.cos(a), 0.70 + 0.30 * math.sin(a)),
               (0.50 + 0.40 * math.cos(a), 0.70 + 0.40 * math.sin(a)), width=round(p.w * 1.05))


def wx_horizon(p):
    p.line((0.10, 0.74), (0.90, 0.74), width=round(p.w * 1.1))


def wx_arrow_up(p):
    p.fill((0.50, 0.80), (0.60, 0.92), (0.40, 0.92))


def wx_arrow_down(p):
    p.fill((0.50, 0.94), (0.60, 0.82), (0.40, 0.82))

ICONS = [('HOME', icon_home), ('FLAME', icon_flame), ('BELL', icon_bell), ('GEAR', icon_gear),
         ('ELEVATOR', icon_elevator), ('HEADSET', icon_headset), ('DOOR', icon_door), ('BUILDING', icon_building),
         ('PARKING', icon_parking), ('POWER', icon_power), ('THERMO', icon_thermo), ('PLUS', icon_plus),
         ('MINUS', icon_minus), ('CHEVRON', icon_chevron), ('CHECK', icon_check), ('UP', icon_up),
         ('DOWN', icon_down), ('SUN', icon_sun),
         ('CLOUD', icon_cloud), ('CLOUD_SUN', icon_cloud_sun), ('RAIN', icon_rain), ('SNOW', icon_snow),
         ('STORM', icon_storm), ('FOG', icon_fog), ('MOON', icon_moon), ('DROP', icon_drop), ('WIND', icon_wind),
         ('SUNRISE', icon_sunrise), ('SUNSET', icon_sunset),
         ('WX_RAYS_FULL', wx_rays_full), ('WX_RAYS_PART', wx_rays_part),
         ('WX_CLOUD_MAIN', wx_cloud_main), ('WX_CLOUD_MAIN_SHADE', wx_cloud_main_shade), ('WX_CLOUD_BACK', wx_cloud_back),
         ('WX_CLOUD_PART', wx_cloud_part), ('WX_CLOUD_PART_SHADE', wx_cloud_part_shade), ('WX_CLOUD_SMALL', wx_cloud_small),
         ('WX_RAIN', wx_rain), ('WX_RAIN_HEAVY', wx_rain_heavy), ('WX_DRIZZLE', wx_drizzle), ('WX_SNOW', wx_snow),
         ('WX_SLEET', wx_sleet), ('WX_BOLT', wx_bolt), ('WX_FOG', wx_fog),
         ('WX_TWILIGHT_RAYS', wx_twilight_rays), ('WX_HORIZON', wx_horizon), ('WX_ARROW_UP', wx_arrow_up),
         ('WX_ARROW_DOWN', wx_arrow_down)]


def pack4(image):
    out = bytearray()
    for y in range(image.height):
        row = [image.getpixel((x, y)) * 15 // 255 for x in range(image.width)]
        if len(row) % 2:
            row.append(0)
        out.extend((row[i] << 4) | row[i+1] for i in range(0, len(row), 2))
    return out


def render_icons(sheet_path=None):
    """Returns (bitmap, table): 4-bit alpha icons; table[icon][size] = offset into bitmap."""
    sheet = Image.new('L', (len(ICONS) * 88, 88 * len(ICON_SIZES)))
    bitmap, table = bytearray(), []
    for index, (name, draw) in enumerate(ICONS):
        offsets = []
        for row, size in enumerate(ICON_SIZES):
            pen = Pen(size)
            draw(pen)
            image = pen.done(size)
            sheet.paste(image, (index * 88, row * 88))
            offsets.append(len(bitmap))
            bitmap.extend(pack4(image))
        table.append(offsets)
    if sheet_path:
        sheet.save(sheet_path)
    return bytes(bitmap), table


def main():
    import argparse
    import assets_pack
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--photo', type=Path, help='photo to use as the background instead of the drawn one')
    parser.add_argument('--center-y', type=float, default=0.5, help='vertical crop position 0 (top) .. 1 (bottom)')
    args = parser.parse_args()
    bg = photo_background(args.photo, args.center_y) if args.photo else background()
    assets_pack.BACKGROUND.parent.mkdir(exist_ok=True)
    bg.save(assets_pack.BACKGROUND)
    print(f'background saved to {assets_pack.BACKGROUND}')
    assets_pack.pack()


if __name__ == '__main__':
    main()
