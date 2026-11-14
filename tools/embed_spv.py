"""Converts compiled Vulkan SPIR-V binaries into a single deterministic C++20 header."""

from __future__ import annotations

import struct
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 4 or len(sys.argv[2:]) % 2 != 0:
        print("Usage: embed_spv.py <out.hpp> <symbol1> <file1.spv> [<symbol2> <file2.spv> ...]", file=sys.stderr)
        return 2

    out_path = Path(sys.argv[1])
    pairs = [(sys.argv[i], Path(sys.argv[i + 1])) for i in range(2, len(sys.argv), 2)]

    lines = [
        "#pragma once",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace aether::renderer::spv {",
        "",
    ]

    for sym, spv_file in pairs:
        raw = spv_file.read_bytes()
        if len(raw) % 4 != 0 or len(raw) < 20:
            print(f"Invalid SPIR-V binary: {spv_file} ({len(raw)} bytes)", file=sys.stderr)
            return 1
        words = struct.unpack(f"<{len(raw) // 4}I", raw)
        lines.append(f"inline constexpr std::array<uint32_t, {len(words)}> {sym} = {{")
        for off in range(0, len(words), 8):
            chunk = ", ".join(f"0x{w:08X}U" for w in words[off : off + 8])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")

    lines.append("} // namespace aether::renderer::spv")
    lines.append("")

    content = "\n".join(lines)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    if not out_path.exists() or out_path.read_text(encoding="utf-8") != content:
        out_path.write_text(content, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
