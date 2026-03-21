#!/usr/bin/env python3
"""Generate StripHelper app icon (.ico + .rc resource)."""

import os
from PIL import Image, ImageDraw, ImageFont


def make_icon_image(size):
    """Create a single StripHelper icon at the given size."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    s = size
    pad = max(1, s // 16)

    # ── Colours ──────────────────────────────────────────────────────
    bg_color  = (28, 22, 46, 255)      # deep purple
    accent    = (180, 90, 255, 255)     # violet accent
    merge_grn = (50, 220, 120, 255)     # merge green
    text_col  = (240, 235, 250, 255)    # near-white text

    # Rounded rect background
    r = max(2, s // 6)
    draw.rounded_rectangle([pad, pad, s - pad - 1, s - pad - 1],
                           radius=r, fill=bg_color)

    # ── Two overlapping film-strip bars (merge concept) ──────────────
    lw = max(1, s // 20)
    bar_h = s * 0.14
    # Left bar
    bx1, by1 = s * 0.10, s * 0.18
    bx2, by2 = s * 0.55, by1 + bar_h
    draw.rounded_rectangle([bx1, by1, bx2, by2], radius=max(1, s // 32),
                           fill=accent)
    # Right bar (overlapping)
    bx3, by3 = s * 0.45, s * 0.28
    bx4, by4 = s * 0.90, by3 + bar_h
    draw.rounded_rectangle([bx3, by3, bx4, by4], radius=max(1, s // 32),
                           fill=merge_grn)

    # ── Arrow pointing down (merge into one) ─────────────────────────
    cx = s * 0.50
    ay1 = s * 0.48
    ay2 = s * 0.58
    draw.line([(cx, ay1), (cx, ay2)], fill=text_col, width=max(1, lw))
    aw = s * 0.08
    draw.polygon([(cx - aw, ay2 - aw * 0.5),
                  (cx + aw, ay2 - aw * 0.5),
                  (cx, ay2 + aw * 0.3)], fill=text_col)

    # ── "SH" text at bottom ──────────────────────────────────────────
    font_size = max(8, int(s * 0.28))
    try:
        font = ImageFont.truetype("arialbd.ttf", font_size)
    except (OSError, IOError):
        try:
            font = ImageFont.truetype("arial.ttf", font_size)
        except (OSError, IOError):
            font = ImageFont.load_default()

    text = "SH"
    bbox = draw.textbbox((0, 0), text, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    tx = s * 0.50 - tw / 2 - bbox[0]
    ty = s * 0.62
    draw.text((tx, ty), text, fill=text_col, font=font)

    return img


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "res")
    os.makedirs(out_dir, exist_ok=True)

    # Generate multi-size .ico
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [make_icon_image(s) for s in sizes]
    ico_path = os.path.join(out_dir, "striphelper.ico")
    images[0].save(ico_path, format="ICO",
                   sizes=[(s, s) for s in sizes],
                   append_images=images[1:])
    print(f"[OK] {ico_path}")

    print("\nDone! Icon generated in res/")


if __name__ == "__main__":
    main()
