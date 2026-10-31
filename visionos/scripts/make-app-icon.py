#!/usr/bin/env python3
"""Writes a visionOS app icon: an AppIcon.solidimagestack with three 1024x1024
layers (Back opaque; Middle and Front with alpha, for the parallax visionOS
gives icons) and every Contents.json the asset catalog needs.

    make-app-icon.py <Assets.xcassets> [--back B.png --middle M.png --front F.png]
                     [--colors "#16142e,#be5834"] [--glyph "TP"] [--preview preview.png]

With layer PNGs it packages them. Without, it draws a placeholder: a vertical
gradient from the two colours, a soft disc, and the glyph (Pillow), or plain
solid layers when Pillow is missing. For the real icon, draw game-themed art
procedurally (TPVR's twilight sky, sun, hills and sword in
tpvr-visionos/visionos/scripts/make-app-icon.py is the model) and pass the
layers in: never use the game's logo or box art.

Wire it up in project.yml: `ASSETCATALOG_COMPILER_APPICON_NAME: AppIcon` and
`- path: Assets.xcassets` under the target's sources.
"""
import argparse
import json
import os
import shutil
import struct
import zlib

S = 1024
INFO = {"info": {"author": "xcode", "version": 1}}


def write_json(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")


def plain_png(path, rgba):
    """A solid 1024x1024 RGBA PNG without Pillow."""
    row = b"\x00" + bytes(rgba) * S
    raw = row * S

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def hex_rgb(value):
    value = value.strip().lstrip("#")
    return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))


def draw_placeholder(paths, colors, glyph, preview):
    top, bottom = colors
    try:
        from PIL import Image, ImageDraw, ImageFilter, ImageFont
    except ImportError:
        plain_png(paths["Back"], (*top, 255))
        plain_png(paths["Middle"], (0, 0, 0, 0))
        plain_png(paths["Front"], (0, 0, 0, 0))
        print("Pillow not installed: wrote solid placeholder layers (pip install pillow for a drawn one)")
        return
    back = Image.new("RGB", (S, S))
    px = back.load()
    for y in range(S):
        t = y / (S - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(S):
            px[x, y] = c
    middle = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    r = S * 0.30
    gd.ellipse([S / 2 - r, S / 2 - r, S / 2 + r, S / 2 + r], fill=(255, 255, 255, 70))
    middle = Image.alpha_composite(middle, glow.filter(ImageFilter.GaussianBlur(24)))
    front = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    if glyph:
        fd = ImageDraw.Draw(front)
        try:
            font = ImageFont.load_default(size=int(S * 0.34))
        except TypeError:
            font = ImageFont.load_default()
        box = fd.textbbox((0, 0), glyph, font=font)
        w, h = box[2] - box[0], box[3] - box[1]
        fd.text(((S - w) / 2 - box[0], (S - h) / 2 - box[1]), glyph, font=font, fill=(255, 255, 255, 255))
    back.save(paths["Back"])
    middle.save(paths["Middle"])
    front.save(paths["Front"])
    if preview:
        comp = Image.alpha_composite(Image.alpha_composite(back.convert("RGBA"), middle), front)
        mask = Image.new("L", (S, S), 0)
        ImageDraw.Draw(mask).ellipse([0, 0, S, S], fill=255)
        bg = Image.new("RGBA", (S, S), (40, 40, 40, 255))
        bg.paste(comp, (0, 0), mask)
        bg.resize((512, 512)).save(preview)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("catalog", help="path to Assets.xcassets (created if missing)")
    parser.add_argument("--back")
    parser.add_argument("--middle")
    parser.add_argument("--front")
    parser.add_argument("--colors", default="#1b2140,#5a3a7a")
    parser.add_argument("--glyph", default="")
    parser.add_argument("--preview")
    args = parser.parse_args()

    stack = os.path.join(args.catalog, "AppIcon.solidimagestack")
    write_json(os.path.join(args.catalog, "Contents.json"), INFO)
    write_json(os.path.join(stack, "Contents.json"),
               {**INFO, "layers": [{"filename": f"{n}.solidimagestacklayer"} for n in ("Front", "Middle", "Back")]})
    paths = {}
    for name in ("Back", "Middle", "Front"):
        layer = os.path.join(stack, f"{name}.solidimagestacklayer")
        write_json(os.path.join(layer, "Contents.json"), INFO)
        imageset = os.path.join(layer, "Content.imageset")
        write_json(os.path.join(imageset, "Contents.json"),
                   {"images": [{"filename": f"{name}.png", "idiom": "vision", "scale": "2x"}], **INFO})
        paths[name] = os.path.join(imageset, f"{name}.png")

    given = {"Back": args.back, "Middle": args.middle, "Front": args.front}
    if all(given.values()):
        for name, source in given.items():
            shutil.copyfile(source, paths[name])
    elif any(given.values()):
        parser.error("pass all three of --back, --middle and --front, or none")
    else:
        colors = [hex_rgb(c) for c in args.colors.split(",")[:2]]
        if len(colors) == 1:
            colors.append(colors[0])
        draw_placeholder(paths, colors, args.glyph, args.preview)
    print(f"App icon: {stack}")


if __name__ == "__main__":
    main()
