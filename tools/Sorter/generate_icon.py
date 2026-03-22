#!/usr/bin/env python3
"""Generate Sorter app icon (.ico) — folder with sort arrows theme."""

import os
from PIL import Image, ImageDraw, ImageFont


def make_icon_image(size):
    """Create a single icon at the given size."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    s = size
    pad = max(1, s // 16)

    bg_color = (22, 27, 46, 255)
    accent = (90, 140, 255, 255)
    text_col = (230, 235, 250, 255)
    sort_green = (50, 200, 100, 255)

    # Rounded rect background
    r = max(2, s // 6)
    draw.rounded_rectangle([pad, pad, s - pad - 1, s - pad - 1],
                           radius=r, fill=bg_color)

    # Folder body (accent blue)
    fx1 = s * 0.10
    fy1 = s * 0.22
    fx2 = s * 0.90
    fy2 = s * 0.75
    lw = max(1, s // 24)

    # Folder tab (above the body)
    tab_top = fy1 - s * 0.08
    if tab_top < fy1:
        draw.rounded_rectangle([fx1, tab_top, fx1 + s * 0.30, fy1],
                               radius=max(1, s // 32), fill=accent)
    # Folder body
    draw.rounded_rectangle([fx1, fy1, fx2, fy2],
                           radius=max(1, s // 20), outline=accent, width=lw)

    # Sort arrows inside folder (down arrow = green)
    cx = s * 0.38
    aw = s * 0.08  # arrow half-width
    # Down arrow
    draw.line([(cx, fy1 + s * 0.08), (cx, fy2 - s * 0.10)],
              fill=sort_green, width=max(1, s // 20))
    # Arrow head
    draw.polygon([
        (cx, fy2 - s * 0.06),
        (cx - aw * 1.5, fy2 - s * 0.16),
        (cx + aw * 1.5, fy2 - s * 0.16),
    ], fill=sort_green)

    # Up arrow (accent blue)
    cx2 = s * 0.62
    draw.line([(cx2, fy2 - s * 0.08), (cx2, fy1 + s * 0.10)],
              fill=accent, width=max(1, s // 20))
    draw.polygon([
        (cx2, fy1 + s * 0.06),
        (cx2 - aw * 1.5, fy1 + s * 0.16),
        (cx2 + aw * 1.5, fy1 + s * 0.16),
    ], fill=accent)

    # "S" text bottom right
    if s >= 32:
        font_size = max(8, int(s * 0.20))
        try:
            font = ImageFont.truetype("arialbd.ttf", font_size)
        except (OSError, IOError):
            try:
                font = ImageFont.truetype("arial.ttf", font_size)
            except (OSError, IOError):
                font = ImageFont.load_default()
        draw.text((s * 0.72, s * 0.74), "S", fill=text_col, font=font)

    return img


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "res")
    os.makedirs(out_dir, exist_ok=True)

    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [make_icon_image(s) for s in sizes]
    ico_path = os.path.join(out_dir, "sorter.ico")
    images[0].save(ico_path, format="ICO",
                   sizes=[(s, s) for s in sizes],
                   append_images=images[1:])
    print(f"[OK] {ico_path}")


if __name__ == "__main__":
    main()
