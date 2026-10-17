import math, cairosvg

STROKE = 40
GLYPHS = {  # drawn in a box about 0..150 wide, x-height top y=55, baseline y=160, ascender y=-20
    "s": ("M 135 58 H 58 C 28 58 28 107 58 107 H 102 C 132 107 132 157 102 157 H 22", 150),
    "h": ("M 20 -20 V 160 M 20 90 Q 20 58 52 58 H 108 Q 140 58 140 90 V 160", 160),
    "w": ("M 15 58 V 120 Q 15 158 47 158 Q 79 158 79 120 V 58 M 79 120 Q 79 158 111 158 Q 143 158 143 120 V 58", 158),
    "a": ("M 130 58 V 160 M 130 72 H 62 Q 28 72 28 108 Q 28 144 62 144 H 130", 150),
    "n": ("M 20 160 V 58 M 20 90 Q 20 58 52 58 H 108 Q 140 58 140 90 V 160", 160),
}
GLYPHS.update({
    "P": ("M 20 160 V 12 H 82 Q 128 12 128 56 Q 128 100 82 100 H 20", 128),
    "S": ("M 125 22 H 58 C 24 22 24 82 58 82 H 100 C 134 82 134 148 100 148 H 22", 130),
    "5": ("M 122 12 H 36 V 84 H 82 Q 128 84 128 122 Q 128 160 82 160 H 22", 130),
})
GAP = 34
SLANT = -10


def tag(text, cx, cy, width):
    x, parts = 0, []
    for ch in text:
        d, w = GLYPHS[ch]
        parts.append((d, x, w))
        x += w + 48
    total = x - 48
    k = width / total
    ox = cx - total * k / 2
    return "".join(f'<g transform="translate({ox + px * k} {cy - 86 * k}) scale({k})">{glyph_svg(d, 0, 0)}</g>' for d, px, _ in parts)


def word(gap=None):
    gap = GAP if gap is None else gap
    x, parts = 0, []
    for ch in "shwan":
        d, w = GLYPHS[ch]
        parts.append((ch, d, x, w))
        x += w + gap
    return parts, x - gap


def glyph_svg(d, tx, ty, slant=None):
    slant = SLANT if slant is None else slant
    return f'<path d="{d}" transform="translate({tx} {ty}) skewX({slant})" fill="none" stroke="#000" stroke-width="{STROKE}" stroke-linecap="round" stroke-linejoin="round"/>'


def flat():
    parts, total = word()
    s = 790 / total
    ox = 512 - total * s / 2
    g = "".join(f'<g transform="translate({ox + x * s} {430 - 70 * s}) scale({s})">{glyph_svg(d, 0, 0)}</g>' for _, d, x, _ in parts)
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024"><rect width="1024" height="1024" fill="#fff"/><circle cx="512" cy="512" r="470" fill="none" stroke="#000" stroke-width="22"/>{g}{tag("PS5", 512, 665, 270)}</svg>'


def ring():
    R, s = 330, 0.74
    parts, total = word(95)
    span = total * s / R
    start = -math.pi / 2 - span / 2
    g = []
    for ch, d, x, w in parts:
        ang = start + (x + w / 2) * s / R
        cx, cy = 512 + R * math.cos(ang), 512 + R * math.sin(ang)
        g.append(f'<g transform="translate({cx} {cy}) rotate({math.degrees(ang) + 90}) scale({s}) translate({-w / 2} {-70})">{glyph_svg(d, 0, 0, 0)}</g>')
    ang = start + (total + 110) * s / R
    g.append(f'<circle cx="{512 + R * math.cos(ang)}" cy="{512 + R * math.sin(ang)}" r="19" fill="#000"/>')
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024"><rect width="1024" height="1024" fill="#fff"/><circle cx="512" cy="512" r="470" fill="none" stroke="#000" stroke-width="22"/>{"".join(g)}{tag("PS5", 512, 610, 300)}</svg>'


for name, fn in (("shwan-wordmark", flat), ("shwan-ring", ring)):
    open(f"{name}.svg", "w").write(fn())
    cairosvg.svg2png(url=f"{name}.svg", write_to=f"{name}.png", output_width=1024, output_height=1024)
