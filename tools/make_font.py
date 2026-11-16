"""Rasterize the UI character set into anti-aliased 4-bit bitmap fonts.

Source font: Noto Sans KR (SIL Open Font License 1.1), installed with Windows.
Only the glyphs used by the firmware's text literals are generated; the font file
itself is not redistributed. The fonts go into assets/assets.bin (see assets_pack.py).
Run after changing any UI text:  py -3 tools/make_font.py
"""
import math
import re
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'C:\Windows\Fonts\NotoSansKR-VF.ttf'
ASCII = ''.join(chr(c) for c in range(32, 127))
DIGITS = '0123456789.-°B%: '
# name, pixel size, weight, character set ('ui' = ASCII + every literal in the firmware)
# The order is the font id used by the firmware (assets.cpp).
FONTS = [
    ('CAPTION', 19, 400, 'ui'),
    ('BODY', 23, 500, 'ui'),
    ('TITLE', 30, 700, 'ui'),
    ('DISPLAY', 60, 600, DIGITS),
]


def ui_chars():
    literals = ''
    # .cpp literals plus headers (local_config.h holds the weather place name)
    for path in sorted((ROOT/'HomePanel').glob('*.cpp')) + sorted((ROOT/'HomePanel').glob('*.h')):
        literals += ''.join(re.findall(r'"([^"\n]*)"', path.read_text(encoding='utf-8')))
    return sorted(set(ASCII + literals + '°·'))


def load(size, weight):
    font = ImageFont.truetype(SOURCE, size)
    font.set_variation_by_axes([weight])
    return font


def build(name, size, weight, charset):
    """Returns (height, glyphs [(code, advance, offset)], 4-bit bitmap rows padded to bytes)."""
    font = load(size, weight)
    chars = ui_chars() if charset == 'ui' else sorted(set(charset))
    # One tight line box per font: union of the ink of every glyph.
    top = min(font.getbbox(ch)[1] for ch in chars if ch.strip())
    bottom = max(font.getbbox(ch)[3] for ch in chars if ch.strip())
    height = bottom - top
    glyphs, bitmap = [], bytearray()
    for ch in chars:
        advance = max(1, math.ceil(font.getlength(ch)))
        image = Image.new('L', (advance, height))
        ImageDraw.Draw(image).text((0, -top), ch, font=font, fill=255)
        offset = len(bitmap)
        for y in range(height):
            row = [image.getpixel((x, y)) * 15 // 255 for x in range(advance)]
            if len(row) % 2:
                row.append(0)
            bitmap.extend((row[i] << 4) | row[i+1] for i in range(0, len(row), 2))
        glyphs.append((ord(ch), advance, offset))
    return height, glyphs, bytes(bitmap)


def build_all():
    fonts = []
    for spec in FONTS:
        height, glyphs, bitmap = build(*spec)
        print(f'{spec[0]:8} {len(glyphs):4} glyphs  height {height:3}px  {len(bitmap)/1024:6.1f} KiB')
        fonts.append((height, glyphs, bitmap))
    return fonts


def main():
    import assets_pack
    assets_pack.pack()


if __name__ == '__main__':
    main()
