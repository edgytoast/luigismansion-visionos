#!/usr/bin/env python3
"""Copies main.dol out of the player's own Luigi's Mansion disc image (.iso/.gcm), for the
build-time recompiler. Prints the disc's game ID (GLME01 USA, GLMP01 Europe).

    extract-dol.py <disc.iso> <out/main.dol>

Plain GameCube images only; convert .rvz/.ciso to .iso with Dolphin first. The DOL is the
player's data and stays in .scratch (never committed). Written for luigismansion-visionos.
"""
import struct
import sys


def main():
    disc_path, out_path = sys.argv[1], sys.argv[2]
    with open(disc_path, "rb") as disc:
        header = disc.read(0x440)
        if len(header) < 0x440 or header[0x1C:0x20] != b"\xc2\x33\x9f\x3d":
            sys.exit(f"{disc_path}: not a plain GameCube disc image (convert .rvz/.ciso to .iso with Dolphin)")
        game_id = header[0:6].decode("ascii", "replace")
        if game_id not in ("GLME01", "GLMP01"):
            sys.exit(f"{disc_path}: game {game_id}, not Luigi's Mansion (GLME01 or GLMP01)")
        dol_offset = struct.unpack(">I", header[0x420:0x424])[0]
        disc.seek(dol_offset)
        dol_header = disc.read(0x100)
        offsets = struct.unpack(">18I", dol_header[0x00:0x48])
        sizes = struct.unpack(">18I", dol_header[0x90:0xD8])
        size = max([0x100] + [o + s for o, s in zip(offsets, sizes) if s])
        disc.seek(dol_offset)
        dol = disc.read(size)
    with open(out_path, "wb") as out:
        out.write(dol)
    print(game_id)


if __name__ == "__main__":
    main()
