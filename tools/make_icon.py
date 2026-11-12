"""Write src/penumbra.ico: an amber ring, half filled (a brightness symbol).

Run: python tools/make_icon.py
"""
import math
import struct
from pathlib import Path

SIZES = (16, 20, 24, 32, 40, 48, 64)
AMBER = (255, 176, 0)
DARK = (48, 48, 48)
SAMPLES = 4


def pixel(size, x, y):
    """RGBA of one pixel, supersampled."""
    c = size / 2.0
    outer = size / 2.0 - 0.5
    ring = max(1.25, size / 10.0)
    inner = outer - ring - max(0.75, size / 16.0)
    acc = [0.0, 0.0, 0.0, 0.0]
    for sy in range(SAMPLES):
        for sx in range(SAMPLES):
            px = x + (sx + 0.5) / SAMPLES - c
            py = y + (sy + 0.5) / SAMPLES - c
            d = math.hypot(px, py)
            color = None
            if outer - ring <= d <= outer:
                color = AMBER
            elif d <= inner:
                color = AMBER if px < 0 else DARK
            if color:
                for i in range(3):
                    acc[i] += color[i]
                acc[3] += 1
    n = SAMPLES * SAMPLES
    if not acc[3]:
        return (0, 0, 0, 0)
    return (round(acc[0] / acc[3]), round(acc[1] / acc[3]), round(acc[2] / acc[3]),
            round(255 * acc[3] / n))


def bmp_entry(size):
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = []
    for y in reversed(range(size)):  # bottom-up
        row = bytearray()
        for x in range(size):
            r, g, b, a = pixel(size, x, y)
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    mask_row = ((size + 31) // 32) * 4
    return header + b"".join(rows) + bytes(mask_row * size)


def main():
    images = [bmp_entry(s) for s in SIZES]
    out = struct.pack("<HHH", 0, 1, len(SIZES))
    offset = 6 + 16 * len(SIZES)
    for size, data in zip(SIZES, images):
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out += b"".join(images)
    target = Path(__file__).resolve().parent.parent / "src" / "penumbra.ico"
    target.write_bytes(out)
    print(f"wrote {target} ({len(out)} bytes)")


if __name__ == "__main__":
    main()
