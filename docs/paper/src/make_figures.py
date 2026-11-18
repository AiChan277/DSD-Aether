"""Builds the paper's figures from the headless snapshots written by the C++ test suite.

Usage: python docs/paper/src/make_figures.py <snapshot_dir> <out_dir>
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def load_on_white(path: Path) -> Image.Image:
    img = Image.open(path).convert("RGBA")
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    bg.alpha_composite(img)
    return bg.convert("RGB")


def alpha_bbox(paths, margin=8):
    """Union bounding box of non-transparent pixels across all images (keeps panels aligned)."""
    box = None
    for p in paths:
        b = Image.open(p).convert("RGBA").getchannel("A").point(lambda a: 255 if a > 32 else 0).getbbox()
        if b is None:
            continue
        box = b if box is None else (min(box[0], b[0]), min(box[1], b[1]), max(box[2], b[2]), max(box[3], b[3]))
    w, h = Image.open(paths[0]).size
    return (max(0, box[0] - margin), max(0, box[1] - margin), min(w, box[2] + margin), min(h, box[3] + margin))


def font(size):
    for name in ("arial.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def panel_row(paths, labels, out_path: Path, scale=3):
    box = alpha_bbox(paths)
    tiles = [load_on_white(p).crop(box) for p in paths]
    tiles = [t.resize((t.width * scale, t.height * scale), Image.LANCZOS) for t in tiles]
    label_h = 54
    gap = 16
    w = sum(t.width for t in tiles) + gap * (len(tiles) - 1)
    h = tiles[0].height + label_h
    canvas = Image.new("RGB", (w, h), (255, 255, 255))
    draw = ImageDraw.Draw(canvas)
    f = font(34)
    x = 0
    for t, label in zip(tiles, labels):
        canvas.paste(t, (x, 0))
        tw = draw.textlength(label, font=f)
        draw.text((x + (t.width - tw) / 2, t.height + 8), label, fill=(0, 0, 0), font=f)
        x += t.width + gap
    canvas.save(out_path, optimize=True)


def main():
    snap = Path(sys.argv[1])
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)

    panel_row([snap / "vk_parity_upright.png", snap / "dx12_parity_upright.png"],
              ["(a) Vulkan 1.3", "(b) DirectX 12"], out / "fig_parity.png")
    panel_row([snap / "dx12_mode0_traditional.png", snap / "dx12_mode1_hybrid_rt.png",
               snap / "dx12_mode2_path_tracing.png"],
              ["(a) Traditional", "(b) Hybrid RT", "(c) Path Tracing"], out / "fig_modes.png")

    load_on_white(snap / "studio_live_100.png").save(out / "fig_studio.png", optimize=True)
    face = load_on_white(snap / "after_8590256991748008892_face_neutral.png")
    face.resize((face.width // 2, face.height // 2), Image.LANCZOS).save(out / "fig_face.png", optimize=True)


if __name__ == "__main__":
    main()
