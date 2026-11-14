"""Generates and validates docs/PARAMS.md from the single-source C++ parameter table (`REQ-UI-010`)."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
REGISTRY_HPP = REPO_ROOT / "src" / "ui" / "params" / "param_registry.hpp"
OUT_MD = REPO_ROOT / "docs" / "PARAMS.md"

SPEC_RE = re.compile(
    r'ParamSpec\{\s*ParamId::(?P<pid>[A-Za-z0-9_]+),\s*"(?P<id>[^"]+)",\s*"(?P<group>[^"]+)",\s*'
    r'"(?P<label>[^"]+)",\s*"(?P<tooltip>[^"]+)",\s*"(?P<unit>[^"]*)",\s*'
    r'ParamType::(?P<type>[A-Za-z]+),\s*ParamOwner::(?P<owner>[A-Za-z0-9_]+),\s*'
    r'(?P<default>[^,]+),\s*(?P<min>[^,]+),\s*(?P<max>[^,]+),\s*(?P<step>[^,]+),\s*'
    r'(?P<prec>[0-9]+),\s*(?P<enums>nullptr|"[^"]*"),\s*(?P<flags>[^\}]+)\}'
)


def parse_flags(raw: str) -> list[str]:
    names = []
    for token in [x.strip() for x in raw.split("|")]:
        if token == "PARAM_FLAG_PERSIST":
            names.append("persist")
        elif token == "PARAM_FLAG_PROJECT_SCOPE":
            names.append("project")
        elif token == "PARAM_FLAG_APP_SCOPE":
            names.append("app")
        elif token == "PARAM_FLAG_OSC_EXPOSED":
            names.append("osc")
        elif token == "PARAM_FLAG_GRAPH_EXPOSABLE":
            names.append("graph")
        elif token == "PARAM_FLAG_REQUIRES_RESTART":
            names.append("restart")
        elif token == "PARAM_FLAG_UNDOABLE":
            names.append("undoable")
    return names


def generate_markdown() -> str:
    text = REGISTRY_HPP.read_text(encoding="utf-8")
    specs = [m.groupdict() for m in SPEC_RE.finditer(text)]
    if not specs:
        raise RuntimeError(f"Failed to parse any ParamSpec rows from {REGISTRY_HPP}")

    seen_ids: set[str] = set()
    lines = [
        "# AETHER-2.5D Parameter Registry (`REQ-UI-010`)",
        "",
        "Generated automatically from [`src/ui/params/param_registry.hpp`](../src/ui/params/param_registry.hpp) (`tools/gen_params_doc.py`).",
        "Do not edit manually; every UI Inspector row, `settings.toml` / `project.toml` key, `/aether/param/<id>` OSC endpoint, Command Palette entry, and documentation table is derived from this single source.",
        "",
        "| ID | Group | Label | Type | Owner | Default | Range | Unit | OSC Endpoint | Flags |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]

    for row in specs:
        pid = row["id"]
        if pid in seen_ids:
            raise RuntimeError(f"Duplicate ParamSpec id: {pid}")
        seen_ids.add(pid)
        osc = f"`/aether/param/{pid}`" if "PARAM_FLAG_OSC_EXPOSED" in row["flags"] else "—"
        flags_str = ", ".join(parse_flags(row["flags"])) or "runtime"
        unit = row["unit"] or "—"
        rng = f"[{row['min'].strip()}, {row['max'].strip()}]"
        lines.append(
            f"| `{pid}` | {row['group']} | {row['label']} | `{row['type'].lower()}` | "
            f"{row['owner']} | `{row['default'].strip()}` | `{rng}` | {unit} | {osc} | {flags_str} |"
        )

    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="Verify docs/PARAMS.md is up to date")
    args = parser.parse_args()

    rendered = generate_markdown()
    if args.check:
        if not OUT_MD.exists():
            print(f"FAIL: missing generated {OUT_MD}", file=sys.stderr)
            return 1
        current = OUT_MD.read_text(encoding="utf-8")
        if current != rendered:
            print(f"FAIL: {OUT_MD} is out of sync with {REGISTRY_HPP}; run python tools/gen_params_doc.py", file=sys.stderr)
            return 1
        print(f"docs/PARAMS.md parity OK ({len(rendered.splitlines())} lines)")
        return 0

    OUT_MD.write_text(rendered, encoding="utf-8")
    print(f"Wrote {OUT_MD}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
