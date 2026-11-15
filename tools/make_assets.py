"""Generate the HomePanel background picture and icon set (original artwork).

Everything is drawn here with Pillow; no third-party image or icon files are used.
Output: HomePanel/assets_data.h  (RGB565 background, 4-bit alpha icons)
Preview PNGs: tools/preview/  (not used by the firmware)
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
ICON_SIZES = (24, 32, 56)
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


ICONS = [('HOME', icon_home), ('FLAME', icon_flame), ('BELL', icon_bell), ('GEAR', icon_gear),
         ('ELEVATOR', icon_elevator), ('HEADSET', icon_headset), ('DOOR', icon_door), ('BUILDING', icon_building),
         ('PARKING', icon_parking), ('POWER', icon_power), ('THERMO', icon_thermo), ('PLUS', icon_plus),
         ('MINUS', icon_minus), ('CHEVRON', icon_chevron), ('CHECK', icon_check), ('UP', icon_up),
         ('DOWN', icon_down), ('SUN', icon_sun)]


def pack4(image):
    out = bytearray()
    for y in range(image.height):
        row = [image.getpixel((x, y)) * 15 // 255 for x in range(image.width)]
        if len(row) % 2:
            row.append(0)
        out.extend((row[i] << 4) | row[i+1] for i in range(0, len(row), 2))
    return out


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--photo', type=Path, help='photo to use as the background instead of the drawn one')
    parser.add_argument('--center-y', type=float, default=0.5, help='vertical crop position 0 (top) .. 1 (bottom)')
    args = parser.parse_args()
    preview = ROOT/'tools'/'preview'
    preview.mkdir(exist_ok=True)
    bg = photo_background(args.photo, args.center_y) if args.photo else background()
    bg.save(preview/'background.png')
    lines = ['#pragma once', '// Generated by tools/make_assets.py (original artwork). Do not edit.', '#include <stdint.h>',
             f'static const uint16_t BG_IMAGE[{W*H}] = {{']
    pixels = to_rgb565(bg)
    for i in range(0, len(pixels), 64):
        lines.append(','.join(map(str, pixels[i:i+64])) + ',')
    lines.append('};')

    sheet = Image.new('L', (len(ICONS) * 64, 64 * len(ICON_SIZES)))
    bitmap, table = bytearray(), []
    for index, (name, draw) in enumerate(ICONS):
        offsets = []
        for row, size in enumerate(ICON_SIZES):
            pen = Pen(size)
            draw(pen)
            image = pen.done(size)
            sheet.paste(image, (index * 64, row * 64))
            offsets.append(len(bitmap))
            bitmap.extend(pack4(image))
        table.append(offsets)
    sheet.save(preview/'icons.png')
    lines.append('enum IconId { ' + ', '.join(f'ICON_{n}' for n, _ in ICONS) + ', ICON_COUNT };')
    lines.append('static const uint8_t ICON_SIZES[] = {' + ','.join(map(str, ICON_SIZES)) + '};')
    lines.append('static const uint8_t ICON_BITMAP[] = {')
    for i in range(0, len(bitmap), 32):
        lines.append(','.join(map(str, bitmap[i:i+32])) + ',')
    lines.append('};')
    lines.append(f'static const uint32_t ICON_OFFSET[ICON_COUNT][{len(ICON_SIZES)}] = {{')
    lines += ['{' + ','.join(map(str, o)) + '},' for o in table]
    lines.append('};')
    (ROOT/'HomePanel'/'assets_data.h').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'background {W}x{H} RGB565 {W*H*2/1024:.0f} KiB, {len(ICONS)} icons x {len(ICON_SIZES)} sizes '
          f'{len(bitmap)/1024:.1f} KiB; previews in {preview}')


if __name__ == '__main__':
    main()
