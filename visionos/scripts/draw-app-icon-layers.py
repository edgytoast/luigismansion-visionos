#!/usr/bin/env python3
# Draws Luigi's Mansion VR's app icon layers (original art, needs Pillow), which make-app-icon.py
# packages:
#   visionos/scripts/draw-app-icon-layers.py <dir>
#   visionos/scripts/make-app-icon.py visionos/App/Assets.xcassets \
#       --back <dir>/Back.png --middle <dir>/Middle.png --front <dir>/Front.png
# A moonlit night sky (Back), a crooked haunted house on its hill with warm windows (Middle), and a
# flashlight's beam catching a little sheet ghost (Front), in flat colours with dark outlines like
# the other Trevorbilt ports' icons. Drawn from scratch: no logos, box art, characters, screenshots
# or anything from the game.
import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

S = 1024
BIG = 2  # supersampling for the outlined shapes
INK = (18, 14, 38, 255)
WARM = (250, 176, 72, 255)
ORANGE = (232, 112, 52, 255)  # Trevorbilt's orange, in the lit windows


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def back():
    img = Image.new("RGBA", (S, S))
    px = img.load()
    top, mid, low = (14, 10, 40, 255), (46, 30, 96, 255), (34, 88, 104, 255)
    for y in range(S):
        t = y / (S - 1)
        c = lerp(top, mid, t / 0.6) if t < 0.6 else lerp(mid, low, (t - 0.6) / 0.4)
        for x in range(S):
            px[x, y] = c
    stars = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(stars)
    rng = random.Random(31)
    for _ in range(90):
        x, y = rng.randrange(S), rng.randrange(int(S * 0.62))
        r = rng.choice([1, 1, 2, 2, 3])
        a = rng.randrange(120, 255)
        sd.ellipse([x - r, y - r, x + r, y + r], fill=(255, 250, 230, a))
    img.alpha_composite(stars)
    # The moon, with a soft halo.
    halo = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    hd = ImageDraw.Draw(halo)
    cx, cy, r = 300, 290, 150
    hd.ellipse([cx - r * 1.9, cy - r * 1.9, cx + r * 1.9, cy + r * 1.9], fill=(200, 210, 255, 70))
    halo = halo.filter(ImageFilter.GaussianBlur(60))
    img.alpha_composite(halo)
    d = ImageDraw.Draw(img)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(246, 240, 214, 255))
    for ox, oy, rr in [(-40, -30, 26), (45, 35, 18), (-10, 60, 14), (60, -50, 12)]:
        d.ellipse([cx + ox - rr, cy + oy - rr, cx + ox + rr, cy + oy + rr], fill=(226, 218, 190, 255))
    return img


def poly(d, pts, fill, width):
    d.polygon([(x * BIG, y * BIG) for x, y in pts], fill=fill)
    d.line([(x * BIG, y * BIG) for x, y in pts + [pts[0]]], fill=INK, width=width * BIG, joint="curve")


def middle():
    big = Image.new("RGBA", (S * BIG, S * BIG), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    hill = (40, 28, 70, 255)
    house = (58, 44, 98, 255)
    roof = (36, 26, 64, 255)
    # The hill.
    pts = [(0, 860)] + [(x, 790 - 70 * math.sin(math.pi * x / S) + 18 * math.sin(x / 70)) for x in range(0, S + 1, 32)] + [(S, 860), (S, S), (0, S)]
    poly(d, pts, hill, 0)
    # The house: a main block, a tall left tower, a leaning right wing, crooked roofs.
    poly(d, [(330, 800), (330, 520), (700, 520), (700, 800)], house, 10)
    poly(d, [(300, 530), (515, 380), (730, 530)], roof, 10)
    poly(d, [(230, 800), (230, 400), (350, 400), (350, 800)], house, 10)
    poly(d, [(210, 410), (292, 230), (372, 410)], roof, 10)
    poly(d, [(690, 800), (700, 600), (820, 590), (830, 800)], house, 10)
    poly(d, [(680, 610), (770, 500), (850, 600)], roof, 10)
    # A chimney and a weather vane.
    poly(d, [(600, 470), (600, 405), (640, 405), (640, 480)], house, 8)
    # (a weather vane: a pole and an arrow, leaning)
    d.line([(292 * BIG, 230 * BIG), (300 * BIG, 176 * BIG)], fill=INK, width=8 * BIG)
    poly(d, [(268, 186), (326, 180), (326, 168), (346, 184), (328, 198), (328, 188), (270, 196)], INK, 0)
    # The windows: warm and lit, one dark.
    def window(x, y, w, h, lit=True):
        fill = WARM if lit else (28, 20, 52, 255)
        poly(d, [(x, y + h), (x, y + h * 0.35), (x + w / 2, y), (x + w, y + h * 0.35), (x + w, y + h)], fill, 7)
        if lit:
            d.line([((x + w / 2) * BIG, (y + h * 0.2) * BIG), ((x + w / 2) * BIG, (y + h) * BIG)], fill=INK, width=5 * BIG)
    window(262, 470, 56, 90)
    window(262, 620, 56, 90, lit=False)
    window(380, 590, 70, 100)
    window(580, 590, 70, 100)
    window(735, 650, 60, 80)
    # The door, glowing orange round its edge.
    poly(d, [(470, 800), (470, 680), (515, 640), (560, 680), (560, 800)], ORANGE, 9)
    poly(d, [(486, 800), (486, 690), (515, 664), (544, 690), (544, 800)], (60, 30, 40, 255), 0)
    # A bare tree at the left.
    d.line([(120 * BIG, 840 * BIG), (140 * BIG, 640 * BIG)], fill=INK, width=18 * BIG)
    for (x0, y0, x1, y1) in [(136, 700, 80, 640), (138, 670, 190, 600), (140, 650, 120, 570), (100, 655, 70, 630)]:
        d.line([(x0 * BIG, y0 * BIG), (x1 * BIG, y1 * BIG)], fill=INK, width=9 * BIG)
    return big.resize((S, S), Image.LANCZOS)


def front():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # The flashlight beam, from the bottom right towards the ghost.
    beam = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    bd = ImageDraw.Draw(beam)
    bd.polygon([(1024, 1024), (930, 1024), (600, 470), (820, 360)], fill=(255, 238, 160, 150))
    beam = beam.filter(ImageFilter.GaussianBlur(26))
    img.alpha_composite(beam)
    big = Image.new("RGBA", (S * BIG, S * BIG), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    # A little sheet ghost: a rounded head, a wavy hem, two eyes and a surprised mouth.
    cx, top, w, bottom = 712, 300, 190, 560
    body = []
    for i in range(0, 181, 6):
        a = math.radians(180 + i)
        body.append((cx + w / 2 * math.cos(a), top + w / 2 + w / 2 * math.sin(a)))
    hem = []
    for i in range(0, 101):
        x = cx + w / 2 - w * i / 100
        y = bottom - 26 * abs(math.sin(math.pi * 3 * i / 100))
        hem.append((x, y))
    shape = [(cx + w / 2, top + w / 2)] + [(cx + w / 2 + 6, bottom - 40)] + hem + [(cx - w / 2 - 6, bottom - 40)] + body[:1]
    shape = body + [(cx + w / 2, top + w / 2), (cx + w / 2 + 8, bottom - 30)] + hem + [(cx - w / 2 - 8, bottom - 30)]
    poly(d, shape, (242, 246, 255, 240), 10)
    for ex in (cx - 42, cx + 24):
        d.ellipse([(ex - 16) * BIG, (top + 100) * BIG, (ex + 16) * BIG, (top + 142) * BIG], fill=INK)
    d.ellipse([(cx - 22) * BIG, (top + 164) * BIG, (cx + 10) * BIG, (top + 198) * BIG], fill=INK)
    # Little arms, raised in fright.
    d.line([((cx - w / 2 + 6) * BIG, (top + 176) * BIG), ((cx - w / 2 - 42) * BIG, (top + 124) * BIG)], fill=INK, width=12 * BIG)
    d.line([((cx + w / 2 - 6) * BIG, (top + 176) * BIG), ((cx + w / 2 + 42) * BIG, (top + 124) * BIG)], fill=INK, width=12 * BIG)
    ghost = big.resize((S, S), Image.LANCZOS)
    glow = ghost.split()[3].filter(ImageFilter.GaussianBlur(30))
    halo = Image.new("RGBA", (S, S), (190, 230, 255, 0))
    halo.putalpha(glow.point(lambda a: int(a * 0.45)))
    img.alpha_composite(halo)
    img.alpha_composite(ghost)
    return img


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    back().convert("RGB").save(os.path.join(out, "Back.png"))  # the back layer is opaque
    middle().save(os.path.join(out, "Middle.png"))
    front().save(os.path.join(out, "Front.png"))
    print(out)


if __name__ == "__main__":
    main()
