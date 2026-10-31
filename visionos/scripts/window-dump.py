#!/usr/bin/env python3
# Turns a window-frame dump (<ENV>_TEST_WINDOW_DUMP=<frame>: window-<image>-<W>x<H>.raw in the app's
# Documents) into PNGs, and decomposes the screen effects as the window's WindowHud kernel does:
# scene = base * (1 - a) + e per pixel in linear light, with the least cover a. Writes the added
# light, the cover, and the layer over a checkerboard; prints how much of the picture changed.
# Composite it over a render-mirror-dump.py picture of the same game frame and diff against
# final.png to check the whole window offline (TPVR's Lost Woods: 3.9/255 mean).
#
#   visionos/scripts/window-dump.py <folder with the .raw files>   (numpy, Pillow)
import glob
import re
import sys

import numpy as np
from PIL import Image

d = sys.argv[1]
frames = {}
for f in glob.glob(f"{d}/window-*.raw"):
    name, w, h = re.search(r"window-(\w+)-(\d+)x(\d+)\.raw", f).groups()
    w, h = int(w), int(h)
    if name == "distance":
        dist = np.fromfile(f, dtype=np.float16).reshape(h, w, 4)[:, :, 0].astype(np.float64)
        finite = dist[np.isfinite(dist)]
        hi = np.percentile(finite, 99) if finite.size else 1.0
        Image.fromarray((np.clip(dist / max(hi, 1e-6), 0, 1) * 255).astype(np.uint8)).save(f"{d}/distance.png")
        print(f"distance: {finite.min():.3f} .. {finite.max():.3f} (game units)")
        continue
    a = np.fromfile(f, dtype=np.uint8).reshape(h, w, 4)[:, :, [2, 1, 0, 3]]
    frames[name] = a
    Image.fromarray(a[:, :, :3]).save(f"{d}/{name}.png")


def lin(x):
    x = x / 255.0
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def srgb(x):
    x = np.clip(x, 0, 1)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * x ** (1 / 2.4) - 0.055)


if "base" in frames and "scene" in frames:
    after = lin(frames["scene"][:, :, :3].astype(float))
    before = lin(frames["base"][:, :, :3].astype(float))
    same = np.all(frames["scene"][:, :, :3] == frames["base"][:, :, :3], axis=2)
    slack = 0.004 + 0.02 * after
    kept = (after + slack) / np.maximum(before, 1e-4)
    a = np.clip(1 - kept.min(axis=2), 0, 1)
    black = np.all(after < 1e-4, axis=2)
    e = np.maximum(after - before * (1 - a)[:, :, None], 0)
    a[same] = 0
    e[same] = 0
    a[black & ~same] = 1
    e[black & ~same] = 0
    print(f"effects: changed {100 * (~same).mean():.1f}% of pixels, cover mean {a.mean():.3f} max {a.max():.2f}, "
          f"added light mean {e.mean():.4f} max {e.max():.3f}, black (on the glass) {100 * (black & ~same).mean():.1f}%")
    Image.fromarray((srgb(e) * 255).astype(np.uint8)).save(f"{d}/effects-light.png")
    Image.fromarray((a * 255).astype(np.uint8)).save(f"{d}/effects-cover.png")
    h, w = a.shape
    yy, xx = np.mgrid[0:h, 0:w]
    checker = np.where(((xx // 16 + yy // 16) % 2)[:, :, None] == 0, 0.2, 0.05) * np.ones(3)
    over = e + checker * (1 - a)[:, :, None]
    Image.fromarray((srgb(over) * 255).astype(np.uint8)).save(f"{d}/effects-over-checker.png")
if "final" in frames and "scene" in frames:
    hud = np.any(np.abs(frames["final"][:, :, :3].astype(int) - frames["scene"][:, :, :3].astype(int)) > 1, axis=2)
    print(f"HUD: {100 * hud.mean():.1f}% of pixels differ between final and scene")
