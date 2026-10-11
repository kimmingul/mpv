#!/usr/bin/env python3
"""Generates etc/nanum-icon.ico, the NanumPlayer application icon.

The artwork is the one in etc/nanum-icon.svg (rounded square, cyan -> blue
gradient, a bold "N", two subtitle lines), redrawn here with Pillow at 4x
supersampling for each icon size. The ICO uses plain 32-bit BMP entries, which
every resource compiler (windres, llvm-rc, rc.exe) accepts.

    python etc/nanum-icon.py            # writes etc/nanum-icon.ico (+ nanum-icon-256.png)
"""
import os
import struct

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)

TOP_LEFT = (0x19, 0xD3, 0xF2)       # cyan
BOTTOM_RIGHT = (0x2B, 0x63, 0xFF)   # blue


def artwork(size):
    """Returns the icon as an RGBA image of size x size pixels."""
    ss = 4
    n = size * ss
    k = n / 256.0

    # Diagonal gradient.
    grad = Image.new("RGB", (n, n))
    px = grad.load()
    for y in range(n):
        for x in range(n):
            t = (x + y) / (2.0 * (n - 1))
            px[x, y] = tuple(int(a + (b - a) * t) for a, b in zip(TOP_LEFT, BOTTOM_RIGHT))

    # Soft top highlight.
    hi = Image.new("RGBA", (n, n), (255, 255, 255, 0))
    hd = ImageDraw.Draw(hi)
    for y in range(int(110 * k)):
        a = int(46 * (1 - y / (110 * k)))
        hd.line([(0, y), (n, y)], fill=(255, 255, 255, a))
    grad = Image.alpha_composite(grad.convert("RGBA"), hi)

    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [12 * k, 12 * k, 244 * k, 244 * k], radius=58 * k, fill=255)

    glyph = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(glyph)
    white = (255, 255, 255, 255)
    small = size <= 24

    top, bottom = (58, 172) if not small else (56, 196)
    lx0, lx1, rx0, rx1 = 70, 102, 154, 186
    d.polygon([(lx0 * k, top * k), (lx1 * k, top * k), (lx1 * k, bottom * k), (lx0 * k, bottom * k)], fill=white)
    d.polygon([(rx0 * k, top * k), (rx1 * k, top * k), (rx1 * k, bottom * k), (rx0 * k, bottom * k)], fill=white)
    d.polygon([(lx0 * k, top * k), (lx1 * k + 6 * k, top * k), (rx1 * k, bottom * k), (rx0 * k - 6 * k, bottom * k)], fill=white)

    if not small:
        d.rounded_rectangle([66 * k, 190 * k, 190 * k, 202 * k], radius=6 * k, fill=(255, 255, 255, 235))
        d.rounded_rectangle([92 * k, 210 * k, 164 * k, 222 * k], radius=6 * k, fill=(255, 255, 255, 170))

    out = Image.alpha_composite(grad, glyph)
    out.putalpha(mask)
    return out.resize((size, size), Image.LANCZOS)


def bmp_entry(img):
    w, h = img.size
    pix = img.load()
    rows = []
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b, a = pix[x, y]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    xor = b"".join(rows)
    and_stride = ((w + 31) // 32) * 4
    and_mask = bytes(and_stride * h)
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0,
                         len(xor) + len(and_mask), 0, 0, 0, 0)
    return header + xor + and_mask


def write_ico(path, images):
    entries = [bmp_entry(i) for i in images]
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for img, data in zip(images, entries):
        w, h = img.size
        out += struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for data in entries:
        out += data
    with open(path, "wb") as f:
        f.write(out)


if __name__ == "__main__":
    images = [artwork(s) for s in SIZES]
    write_ico(os.path.join(HERE, "nanum-icon.ico"), images)
    images[-1].save(os.path.join(HERE, "nanum-icon-256.png"))
    print("wrote nanum-icon.ico", [i.size[0] for i in images])
