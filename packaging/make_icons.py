#!/usr/bin/env python3
"""Render the Lumora logo (application/common/icons/logo.svg) into the installer icons.

  python3 packaging/make_icons.py

Writes packaging/lumora-hub.png (256 px, AppImage and .desktop icon) and packaging/lumora-hub.ico
(16-256 px, Windows executable and installer). The logo is three flat shapes, drawn here at 4x
and downsampled so the edges are smooth. Needs Pillow and NumPy. Re-run after changing the logo.
"""

from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
VIEWBOX = 24.0
OUTER = [(12, 1.8), (20.8, 6.9), (20.8, 17.1), (12, 22.2), (3.2, 17.1), (3.2, 6.9)]
INNER = [(12, 6.2), (16.9, 9.05), (16.9, 14.95), (12, 17.8), (7.1, 14.95), (7.1, 9.05)]
GRADIENT = ((0x7C, 0xC4, 0xFF), (0x6F, 0x5B, 0xFF))  # top-left -> bottom-right of the outer shape
INNER_COLOR = (0x0F, 0x11, 0x16, round(0.85 * 255))
DOT = (12, 12, 2.2)


def render(size: int) -> Image.Image:
    scale = 4
    px = size * scale
    k = px / VIEWBOX

    def pts(poly):
        return [(x * k, y * k) for x, y in poly]

    # Outer hexagon: diagonal gradient across its bounding box (objectBoundingBox 0,0 -> 1,1).
    xs, ys = [p[0] for p in OUTER], [p[1] for p in OUTER]
    u = (np.arange(px) / k - min(xs)) / (max(xs) - min(xs))
    v = (np.arange(px) / k - min(ys)) / (max(ys) - min(ys))
    t = np.clip((u[None, :] + v[:, None]) / 2, 0, 1)[..., None]
    rgb = (1 - t) * np.array(GRADIENT[0]) + t * np.array(GRADIENT[1])
    mask = Image.new("L", (px, px))
    ImageDraw.Draw(mask).polygon(pts(OUTER), fill=255)
    image = Image.fromarray(np.dstack([rgb, np.array(mask)]).astype(np.uint8), "RGBA")

    overlay = Image.new("RGBA", (px, px))
    ImageDraw.Draw(overlay).polygon(pts(INNER), fill=INNER_COLOR)
    image = Image.alpha_composite(image, overlay)

    cx, cy, r = DOT
    ImageDraw.Draw(image).ellipse([(cx - r) * k, (cy - r) * k, (cx + r) * k, (cy + r) * k], fill=(255, 255, 255, 255))
    return image.resize((size, size), Image.LANCZOS)


def main() -> None:
    big = render(256)
    big.save(HERE / "lumora-hub.png", optimize=True)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    big.save(HERE / "lumora-hub.ico", sizes=[(s, s) for s in sizes],
             append_images=[render(s) for s in sizes[:-1]])
    print("wrote packaging/lumora-hub.png and packaging/lumora-hub.ico")


if __name__ == "__main__":
    main()
