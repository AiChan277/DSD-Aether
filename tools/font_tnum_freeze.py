"""Verify or bake frozen tabular numerals (tnum) in Inter UI font assets (§12.3.4)."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
from fontTools.ttLib import TTFont


def verify_tabular_numerals(font_path: Path) -> bool:
    font = TTFont(str(font_path))
    cmap = font.getBestCmap()
    if cmap is None:
        print(f"ERROR: {font_path} has no cmap table", file=sys.stderr)
        return False
    hmtx = font["hmtx"]
    widths: dict[str, int] = {}
    for digit in "0123456789":
        glyph_name = cmap.get(ord(digit))
        if glyph_name is None:
            print(f"ERROR: {font_path} missing glyph for '{digit}'", file=sys.stderr)
            return False
        widths[digit] = hmtx[glyph_name][0]

    unique_widths = set(widths.values())
    if len(unique_widths) != 1:
        print(
            f"ERROR: {font_path} numerals do not have uniform tabular advance widths: {widths}",
            file=sys.stderr,
        )
        return False

    print(
        f"OK: {font_path.name} digits 0-9 share uniform tabular advance width = {next(iter(unique_widths))}"
    )
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "fonts",
        nargs="*",
        default=[
            "assets/fonts/Inter-Regular-tnum.ttf",
            "assets/fonts/Inter-SemiBold-tnum.ttf",
        ],
        help="Paths to tnum UI font files to verify.",
    )
    args = parser.parse_args()
    ok = True
    for raw in args.fonts:
        p = Path(raw)
        if not p.is_file():
            print(f"ERROR: missing font file {p}", file=sys.stderr)
            ok = False
            continue
        ok = verify_tabular_numerals(p) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
