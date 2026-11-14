#!/usr/bin/env python3
"""
Generates uniform-stroke 24-unit grid studio UI icon font (`AetherIcons-Regular.ttf`, ISC)
and a compact Japanese font subset (`NotoSansJP-Subset.ttf`, SIL OFL 1.1) for PMX/VRM
morph & bone labels (`AETHER-TS-001` §12.3.2, §12.3.5, ADR-0013).
"""
from __future__ import annotations

import math
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.subset import Subsetter, Options, save_font
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

REPO_ROOT = Path(__file__).resolve().parents[1]
FONTS_DIR = REPO_ROOT / "assets" / "fonts"

UPM = 1000
SCALE = UPM / 24.0
STROKE_HALF = 0.88 * SCALE  # ~1.76 px on 24-unit grid -> crisp 1 px+ on 14 px icons


def to_font_xy(x: float, y: float) -> tuple[int, int]:
    # 24x24 viewBox mapped to [0..1000] horizontally, [-120..880] vertically (y flipped)
    fx = int(round(x * SCALE))
    fy = int(round((21.0 - y) * SCALE))
    return fx, fy


def add_quad_cw(pen: TTGlyphPen, p0: tuple[float, float], p1: tuple[float, float], p2: tuple[float, float], p3: tuple[float, float]) -> None:
    pen.moveTo(to_font_xy(*p0))
    pen.lineTo(to_font_xy(*p1))
    pen.lineTo(to_font_xy(*p2))
    pen.lineTo(to_font_xy(*p3))
    pen.closePath()


def stroke_segment(pen: TTGlyphPen, ax: float, ay: float, bx: float, by: float, hw: float = 0.88) -> None:
    dx = bx - ax
    dy = by - ay
    length = math.hypot(dx, dy)
    if length < 1e-4:
        return
    ux, uy = dx / length, dy / length
    nx, ny = -uy, ux
    # Extend half-cap along segment to seal corners cleanly
    ex0, ey0 = ax - ux * hw * 0.45, ay - uy * hw * 0.45
    ex1, ey1 = bx + ux * hw * 0.45, by + uy * hw * 0.45
    # Clockwise in SVG Y-down maps correctly when Y flips in TTF? Verify signed area!
    pts = [
        (ex0 + nx * hw, ey0 + ny * hw),
        (ex1 + nx * hw, ey1 + ny * hw),
        (ex1 - nx * hw, ey1 - ny * hw),
        (ex0 - nx * hw, ey0 - ny * hw),
    ]
    fpts = [to_font_xy(px, py) for px, py in pts]
    area2 = sum((fpts[i][0] * fpts[(i + 1) % 4][1] - fpts[(i + 1) % 4][0] * fpts[i][1]) for i in range(4))
    # TrueType non-zero winding requires clockwise outer contour (negative shoelace area in Y-up!)
    if area2 > 0:
        fpts.reverse()
    pen.moveTo(fpts[0])
    pen.lineTo(fpts[1])
    pen.lineTo(fpts[2])
    pen.lineTo(fpts[3])
    pen.closePath()


def stroke_polyline(pen: TTGlyphPen, pts: list[tuple[float, float]], closed: bool = False) -> None:
    n = len(pts)
    limit = n if closed else n - 1
    for i in range(limit):
        a = pts[i]
        b = pts[(i + 1) % n]
        stroke_segment(pen, a[0], a[1], b[0], b[1])


def stroke_rect(pen: TTGlyphPen, x: float, y: float, w: float, h: float) -> None:
    stroke_polyline(pen, [(x, y), (x + w, y), (x + w, y + h), (x, y + h)], closed=True)


def stroke_circle(pen: TTGlyphPen, cx: float, cy: float, r: float, segs: int = 14) -> None:
    pts = [
        (cx + r * math.cos(2.0 * math.pi * i / segs), cy + r * math.sin(2.0 * math.pi * i / segs))
        for i in range(segs)
    ]
    stroke_polyline(pen, pts, closed=True)


def fill_circle(pen: TTGlyphPen, cx: float, cy: float, r: float, segs: int = 12) -> None:
    fpts = []
    for i in range(segs):
        a = 2.0 * math.pi * i / segs
        fpts.append(to_font_xy(cx + r * math.cos(a), cy + r * math.sin(a)))
    area2 = sum((fpts[i][0] * fpts[(i + 1) % segs][1] - fpts[(i + 1) % segs][0] * fpts[i][1]) for i in range(segs))
    if area2 > 0:
        fpts.reverse()
    pen.moveTo(fpts[0])
    for p in fpts[1:]:
        pen.lineTo(p)
    pen.closePath()


def draw_icon(code: int, pen: TTGlyphPen) -> None:
    if code == 0xE001:  # Search
        stroke_circle(pen, 10.5, 10.5, 6.5)
        stroke_segment(pen, 15.2, 15.2, 20.5, 20.5)
    elif code == 0xE002:  # Plus
        stroke_segment(pen, 12, 4, 12, 20)
        stroke_segment(pen, 4, 12, 20, 12)
    elif code == 0xE003:  # Eye
        stroke_polyline(pen, [(2, 12), (7, 6.5), (12, 5.5), (17, 6.5), (22, 12), (17, 17.5), (12, 18.5), (7, 17.5)], True)
        stroke_circle(pen, 12, 12, 3.0)
    elif code == 0xE004:  # Eye-off
        stroke_polyline(pen, [(2, 12), (7, 6.5), (12, 5.5), (17, 6.5), (22, 12)], False)
        stroke_segment(pen, 3, 3, 21, 21)
    elif code == 0xE005:  # Lock
        stroke_rect(pen, 5, 11, 14, 10)
        stroke_polyline(pen, [(8, 11), (8, 7), (10, 4.5), (14, 4.5), (16, 7), (16, 11)], False)
    elif code == 0xE006:  # Unlock
        stroke_rect(pen, 5, 11, 14, 10)
        stroke_polyline(pen, [(8, 11), (8, 7), (10, 4.5), (14, 4.5), (16, 7)], False)
    elif code == 0xE007:  # Chevron right
        stroke_polyline(pen, [(9, 6), (15, 12), (9, 18)], False)
    elif code == 0xE008:  # Chevron down
        stroke_polyline(pen, [(6, 9), (12, 15), (18, 9)], False)
    elif code == 0xE009:  # Play
        stroke_polyline(pen, [(6, 4), (19, 12), (6, 20)], True)
    elif code == 0xE00A:  # Pause
        stroke_rect(pen, 6, 4, 4, 16)
        stroke_rect(pen, 14, 4, 4, 16)
    elif code == 0xE00B:  # Sliders
        stroke_segment(pen, 4, 7, 20, 7)
        stroke_segment(pen, 4, 12, 20, 12)
        stroke_segment(pen, 4, 17, 20, 17)
        fill_circle(pen, 9, 7, 2.2)
        fill_circle(pen, 15, 12, 2.2)
        fill_circle(pen, 10, 17, 2.2)
    elif code == 0xE00C:  # Camera
        stroke_polyline(pen, [(3, 8), (7, 8), (9, 5), (15, 5), (17, 8), (21, 8), (21, 19), (3, 19)], True)
        stroke_circle(pen, 12, 13.5, 3.5)
    elif code == 0xE00D:  # Sun (Key light)
        stroke_circle(pen, 12, 12, 4.5)
        for a in range(8):
            ang = a * math.pi / 4.0
            stroke_segment(pen, 12 + 7.2 * math.cos(ang), 12 + 7.2 * math.sin(ang), 12 + 9.5 * math.cos(ang), 12 + 9.5 * math.sin(ang))
    elif code == 0xE00E:  # CPU
        stroke_rect(pen, 6, 6, 12, 12)
        stroke_rect(pen, 9.5, 9.5, 5, 5)
        for p in (9, 15):
            stroke_segment(pen, p, 3, p, 6)
            stroke_segment(pen, p, 18, p, 21)
            stroke_segment(pen, 3, p, 6, p)
            stroke_segment(pen, 18, p, 21, p)
    elif code == 0xE00F:  # Folder
        stroke_polyline(pen, [(3, 5), (9, 5), (11, 8), (21, 8), (21, 19), (3, 19)], True)
    elif code == 0xE010:  # Save
        stroke_polyline(pen, [(4, 4), (17, 4), (20, 7), (20, 20), (4, 20)], True)
        stroke_rect(pen, 7, 4, 8, 5)
        stroke_rect(pen, 7, 13, 10, 7)
    elif code == 0xE011:  # Undo
        stroke_polyline(pen, [(9, 14), (4, 9), (9, 4)], False)
        stroke_polyline(pen, [(4, 9), (15, 9), (19, 12), (19, 16), (15, 19), (9, 19)], False)
    elif code == 0xE012:  # Redo
        stroke_polyline(pen, [(15, 14), (20, 9), (15, 4)], False)
        stroke_polyline(pen, [(20, 9), (9, 9), (5, 12), (5, 16), (9, 19), (15, 19)], False)
    elif code == 0xE013:  # Settings (gear hexagon)
        stroke_circle(pen, 12, 12, 3.5)
        stroke_circle(pen, 12, 12, 7.5, 6)
    elif code == 0xE014:  # Keyboard
        stroke_rect(pen, 3, 6, 18, 12)
        stroke_segment(pen, 7, 15, 17, 15)
        stroke_segment(pen, 6, 10, 8, 10)
        stroke_segment(pen, 11, 10, 13, 10)
        stroke_segment(pen, 16, 10, 18, 10)
    elif code == 0xE015:  # Check
        stroke_polyline(pen, [(4, 13), (9.5, 18), (20, 6)], False)
    elif code == 0xE016:  # Alert triangle
        stroke_polyline(pen, [(12, 3), (21, 20), (3, 20)], True)
        stroke_segment(pen, 12, 9, 12, 14)
        fill_circle(pen, 12, 17.2, 1.0)
    elif code == 0xE017:  # Error circle
        stroke_circle(pen, 12, 12, 9.0)
        stroke_segment(pen, 8.5, 8.5, 15.5, 15.5)
        stroke_segment(pen, 15.5, 8.5, 8.5, 15.5)
    elif code == 0xE018:  # Radio dot
        stroke_circle(pen, 12, 12, 8.5)
        fill_circle(pen, 12, 12, 4.0)
    elif code == 0xE019:  # Maximize
        stroke_polyline(pen, [(15, 4), (20, 4), (20, 9)], False)
        stroke_polyline(pen, [(9, 20), (4, 20), (4, 15)], False)
        stroke_segment(pen, 14, 10, 20, 4)
        stroke_segment(pen, 10, 14, 4, 20)
    elif code == 0xE01A:  # Layers
        stroke_polyline(pen, [(12, 3), (21, 8), (12, 13), (3, 8)], True)
        stroke_polyline(pen, [(3, 13), (12, 18), (21, 13)], False)
        stroke_polyline(pen, [(3, 17), (12, 22), (21, 17)], False)
    elif code == 0xE01B:  # Bone
        stroke_segment(pen, 6, 18, 18, 6)
        stroke_circle(pen, 5.5, 18.5, 2.2)
        stroke_circle(pen, 18.5, 5.5, 2.2)
    elif code == 0xE01C:  # Box (mesh)
        stroke_polyline(pen, [(12, 3), (20, 7.5), (20, 16.5), (12, 21), (4, 16.5), (4, 7.5)], True)
        stroke_polyline(pen, [(4, 7.5), (12, 12), (20, 7.5)], False)
        stroke_segment(pen, 12, 12, 12, 21)
    elif code == 0xE01D:  # Refresh
        stroke_polyline(pen, [(4, 12), (6, 6), (12, 4), (18, 6), (20, 11)], False)
        stroke_polyline(pen, [(20, 12), (18, 18), (12, 20), (6, 18), (4, 13)], False)
        stroke_polyline(pen, [(16, 4), (20, 4), (20, 8)], False)
    elif code == 0xE01E:  # Mic
        stroke_rect(pen, 9, 4, 6, 10)
        stroke_polyline(pen, [(6, 11), (6, 15), (9, 17), (15, 17), (18, 15), (18, 11)], False)
        stroke_segment(pen, 12, 17, 12, 21)
        stroke_segment(pen, 8, 21, 16, 21)
    elif code == 0xE01F:  # Nodes
        stroke_rect(pen, 3, 4, 6, 6)
        stroke_rect(pen, 15, 14, 6, 6)
        stroke_polyline(pen, [(9, 7), (12, 7), (12, 17), (15, 17)], False)
    elif code == 0xE020:  # Terminal
        stroke_polyline(pen, [(4, 17), (10, 12), (4, 7)], False)
        stroke_segment(pen, 12, 18, 20, 18)
    elif code == 0xE021:  # Close
        stroke_segment(pen, 5, 5, 19, 19)
        stroke_segment(pen, 19, 5, 5, 19)
    elif code == 0xE022:  # Info
        stroke_circle(pen, 12, 12, 9.0)
        fill_circle(pen, 12, 8, 1.0)
        stroke_segment(pen, 12, 11.5, 12, 16.5)
    elif code == 0xE023:  # Link
        stroke_polyline(pen, [(10, 7), (6, 7), (4, 9), (4, 15), (6, 17), (10, 17)], False)
        stroke_polyline(pen, [(14, 7), (18, 7), (20, 9), (20, 15), (18, 17), (14, 17)], False)
        stroke_segment(pen, 8, 12, 16, 12)
    elif code == 0xE024:  # Domain: NPU (neural matrix chip)
        stroke_rect(pen, 5, 5, 14, 14)
        fill_circle(pen, 9.5, 9.5, 1.4)
        fill_circle(pen, 14.5, 9.5, 1.4)
        fill_circle(pen, 9.5, 14.5, 1.4)
        fill_circle(pen, 14.5, 14.5, 1.4)
        stroke_segment(pen, 9.5, 9.5, 14.5, 14.5)
    elif code == 0xE025:  # Domain: Face SDF (split circle + curve)
        stroke_circle(pen, 12, 12, 8.5)
        stroke_polyline(pen, [(12, 3.5), (14, 8), (11, 12), (14, 16), (12, 20.5)], False)
    elif code == 0xE026:  # Domain: BLAS (BVH nested diamonds)
        stroke_polyline(pen, [(12, 3), (21, 12), (12, 21), (3, 12)], True)
        stroke_rect(pen, 9, 9, 6, 6)
    elif code == 0xE027:  # Domain: Spout (frame with outgoing stream arrow)
        stroke_rect(pen, 3, 5, 13, 14)
        stroke_segment(pen, 11, 12, 21, 12)
        stroke_polyline(pen, [(17, 8), (21, 12), (17, 16)], False)
    elif code == 0xE028:  # Domain: Live2D Flatness (parallel compressed planes)
        stroke_polyline(pen, [(4, 6), (16, 6), (20, 10), (8, 10)], True)
        stroke_polyline(pen, [(4, 14), (16, 14), (20, 18), (8, 18)], True)


def build_icon_font() -> Path:
    names = [".notdef"] + [f"icon_{c:04X}" for c in range(0xE001, 0xE029)]
    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({c: f"icon_{c:04X}" for c in range(0xE001, 0xE029)})

    glyphs = {}
    metrics = {}
    empty_pen = TTGlyphPen(None)
    glyphs[".notdef"] = empty_pen.glyph()
    metrics[".notdef"] = (UPM, 0)

    for c in range(0xE001, 0xE029):
        pen = TTGlyphPen(None)
        draw_icon(c, pen)
        gname = f"icon_{c:04X}"
        glyphs[gname] = pen.glyph()
        metrics[gname] = (UPM, 0)

    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=880, descent=-120)
    fb.setupNameTable({
        "familyName": "AetherIcons",
        "styleName": "Regular",
        "licenseDescription": "ISC License - see assets/fonts/AetherIcons-ISC.txt",
    })
    fb.setupOS2(sTypoAscender=880, sTypoDescender=-120, usWinAscent=880, usWinDescent=120)
    fb.setupPost()

    out_path = FONTS_DIR / "AetherIcons-Regular.ttf"
    fb.save(str(out_path))
    return out_path


def build_jp_subset() -> Path | None:
    src = Path(r"C:\Windows\Fonts\NotoSansJP-VF.ttf")
    if not src.exists():
        return None
    font = TTFont(str(src), fontNumber=0, recalcBBoxes=True)
    if "fvar" in font:
        font = instantiateVariableFont(font, {"wght": 400})
    ofl_text = ""
    for rec in font["name"].names:
        if rec.nameID in (0, 1, 13, 14):
            ofl_text += rec.toUnicode() + "\n"
    (FONTS_DIR / "NotoSansJP-OFL.txt").write_text(ofl_text.strip() + "\n", encoding="utf-8")

    # Hiragana + Katakana + CJK Symbols + Fullwidth ASCII/Kana + MMD/VRM bone & morph Kanji
    unicodes: list[int] = []
    unicodes.extend(range(0x3000, 0x30FF + 1))  # CJK symbols, Hiragana, Katakana
    unicodes.extend(range(0xFF01, 0xFF9F + 1))  # Fullwidth ASCII + halfwidth Katakana
    mmd_chars = (
        "全ての親センターグルーブ腰上半身下半身首頭目両目左目右目眉睫毛涙瞳"
        "口あいうえおん微笑怒悲驚呆閉半笑大腕肘手首手先指親指人差指中指薬指小指"
        "肩ひじひざ足足首つま先髪前髪後髪横髪横揺れ飾服装物理剛体重"
        "上下左右前後大小開閉強弱角度影輪郭光"
    )
    unicodes.extend(ord(ch) for ch in mmd_chars)

    opts = Options(layout_features=["*"], name_IDs=["*"])
    subsetter = Subsetter(options=opts)
    subsetter.populate(unicodes=sorted(set(unicodes)))
    subsetter.subset(font)

    out_path = FONTS_DIR / "NotoSansJP-Subset.ttf"
    save_font(font, str(out_path), opts)
    return out_path


if __name__ == "__main__":
    FONTS_DIR.mkdir(parents=True, exist_ok=True)
    icon_p = build_icon_font()
    jp_p = build_jp_subset()
    print(f"Generated {icon_p} ({icon_p.stat().st_size} bytes)")
    if jp_p is not None:
        print(f"Generated {jp_p} ({jp_p.stat().st_size} bytes)")
