#!/usr/bin/env python3
"""Generate assets/icon0.png: the neutral LiveArea icon shipped in the VPK.

Plain geometry only (a filled circle and the letters "TH07" drawn from a
5x7 block font defined below); no font file and no artwork from the game.
On first start the game replaces it with the icon of the user's own
th07.exe / 東方妖々夢.exe (vita/Startup.cpp).  Output: 128x128 indexed PNG.
Requires Pillow.  Usage: python3 tools/make_icon.py assets/icon0.png
"""
import sys
from PIL import Image, ImageDraw

GLYPHS = {
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
}


def main(path):
    size, scale = 128, 4
    image = Image.new("RGB", (size, size), (24, 24, 32))
    draw = ImageDraw.Draw(image)
    draw.ellipse((8, 8, size - 9, size - 9), fill=(196, 84, 132))
    text = "TH07"
    width = len(text) * 5 * scale + (len(text) - 1) * scale
    x0, y0 = (size - width) // 2, (size - 7 * scale) // 2
    for i, ch in enumerate(text):
        for row, bits in enumerate(GLYPHS[ch]):
            for col, bit in enumerate(bits):
                if bit == "#":
                    x = x0 + i * 6 * scale + col * scale
                    y = y0 + row * scale
                    draw.rectangle((x, y, x + scale - 1, y + scale - 1), fill=(250, 246, 236))
    image.quantize(colors=16, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(
        path, format="PNG", optimize=True)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "assets/icon0.png")
