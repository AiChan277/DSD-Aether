"""AETHER-2.5D Shader Compiler Pipeline (§3.5, P4-02).

Compiles:
1) Legacy Phase 2 HLSL preview shaders via Vulkan SDK `glslc` -> `preview_shaders_spv.hpp`.
2) Phase 4 Slang shaders declared in `shaders/manifest.toml` via `slangc`:
   - Target 1: `spirv_1_6` (plus `-capability spvRayQueryKHR` for RT shaders) + `-reflection-json`
   - Reflection checks: push-constants <= 128 B (`max_push_constants`), Set <= 1, Set 1 restricted to RT shaders
   - Target 2: `dxil` SM 6.6 compile-check using Windows SDK `dxc.exe` when available
   - Emits `phase4_shaders_spv.hpp` + disk `.spv` blobs for Debug hot-reload.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import shutil
import struct
import subprocess
import sys
import tomllib
from pathlib import Path

LEGACY_PREVIEW_SHADERS: list[tuple[str, str, str]] = [
    ("preview_mesh.vert.hlsl", "vertex", "kPreviewMeshVertSpv"),
    ("preview_mesh.frag.hlsl", "fragment", "kPreviewMeshFragSpv"),
    ("preview_bones.vert.hlsl", "vertex", "kPreviewBonesVertSpv"),
    ("preview_bones.frag.hlsl", "fragment", "kPreviewBonesFragSpv"),
]

STAGE_TO_DXIL_PROFILE = {
    "compute": "cs_6_6",
    "vertex": "vs_6_6",
    "fragment": "ps_6_6",
}


def find_slangc() -> str | None:
    env_override = os.environ.get("AETHER_SLANGC") or os.environ.get("SLANGC")
    if env_override and Path(env_override).exists():
        return env_override
    exe = shutil.which("slangc")
    if exe:
        return exe
    vk_sdk = os.environ.get("VULKAN_SDK")
    if vk_sdk:
        for sub in ("Bin/slangc.exe", "bin/slangc.exe"):
            cand = Path(vk_sdk) / sub
            if cand.exists():
                return str(cand)
    local_app = os.environ.get("LOCALAPPDATA", "")
    if local_app:
        matches = sorted(
            glob.glob(str(Path(local_app) / "Aether" / "toolchains" / "slang-*" / "bin" / "slangc.exe")),
            reverse=True,
        )
        if matches:
            return matches[0]
    return None


def find_dxc_dir() -> str | None:
    dxc = shutil.which("dxc")
    if dxc:
        return str(Path(dxc).parent)
    matches = sorted(
        glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\*\x64\dxc.exe"),
        reverse=True,
    )
    if matches:
        return str(Path(matches[0]).parent)
    return None


def read_spv_words(spv_path: Path) -> list[int]:
    raw = spv_path.read_bytes()
    if len(raw) % 4 != 0 or len(raw) < 20:
        raise ValueError(f"Invalid SPIR-V binary size: {spv_path} ({len(raw)} bytes)")
    words = list(struct.unpack(f"<{len(raw) // 4}I", raw))
    if words[0] != 0x07230203:
        raise ValueError(f"Invalid SPIR-V magic word in {spv_path}: {hex(words[0])}")
    return words


def strip_redundant_forward_pointers(spv_path: Path) -> list[int]:
    """Remove redundant OpTypeForwardPointer declarations when the pointee already precedes OpTypePointer.

    Slang emits OpTypeForwardPointer for non-recursive PhysicalStorageBuffer struct pointers even when
    OpTypePointer appears immediately after OpTypeStruct with zero forward uses; Intel's Windows Vulkan
    driver (igvk64.dll 101.6737) silently returns VK_SUCCESS with VK_NULL_HANDLE on such modules.
    """
    words = read_spv_words(spv_path)
    insts: list[list[int]] = []
    i = 5
    while i < len(words):
        wc = words[i] >> 16
        if wc == 0 or i + wc > len(words):
            return words
        insts.append(words[i : i + wc])
        i += wc

    fwd_ids = {ins[1] for ins in insts if (ins[0] & 0xFFFF) == 39 and len(ins) >= 3}
    if not fwd_ids:
        return words

    ptr_defs: dict[int, tuple[int, int]] = {}
    type_defs: dict[int, int] = {}
    for idx, ins in enumerate(insts):
        op = ins[0] & 0xFFFF
        if op == 32 and len(ins) == 4:
            ptr_defs[ins[1]] = (idx, ins[3])
        if (19 <= op <= 33 or op in (4472, 5341)) and len(ins) >= 2:
            type_defs[ins[1]] = idx

    removable: set[int] = set()
    for fid in fwd_ids:
        if fid not in ptr_defs:
            continue
        pidx, pointee = ptr_defs[fid]
        if pointee not in type_defs or type_defs[pointee] >= pidx:
            continue
        early_ref = False
        for idx in range(pidx):
            op = insts[idx][0] & 0xFFFF
            if op in (3, 4, 5, 6, 7, 8, 10, 11, 14, 15, 16, 17, 19, 20, 21, 22, 26, 39, 71, 72, 73, 74, 75, 4472, 5341):
                continue
            args = insts[idx][1:]
            if op in (23, 24, 25, 27, 29, 31):
                id_args = [args[1]] if len(args) > 1 else []
            elif op == 28:
                id_args = args[1:3]
            elif op in (30, 33):
                id_args = args[1:]
            elif op == 32:
                id_args = [args[2]] if len(args) > 2 else []
            elif op == 44:
                id_args = [args[0]] + args[2:]
            elif op == 59:
                id_args = [args[0]] + args[3:]
            elif op in (41, 42, 43, 45, 46):
                id_args = [args[0]]
            elif op == 12:
                id_args = args[3:]
            else:
                id_args = args
            if fid in id_args:
                early_ref = True
                break
        if not early_ref:
            removable.add(fid)

    if not removable:
        return words

    out = list(words[:5])
    for ins in insts:
        if (ins[0] & 0xFFFF) == 39 and len(ins) >= 2 and ins[1] in removable:
            continue
        out.extend(ins)
    spv_path.write_bytes(struct.pack(f"<{len(out)}I", *out))
    return out


def write_u32_array(lines: list[str], symbol: str, words: list[int], as_std_array: bool = False) -> None:
    if as_std_array:
        lines.append(f"inline constexpr std::array<uint32_t, {len(words)}> {symbol} = {{{{")
    else:
        lines.append(f"inline constexpr uint32_t {symbol}[] = {{")
    for idx in range(0, len(words), 8):
        chunk = ", ".join(f"0x{w:08x}u" for w in words[idx : idx + 8])
        lines.append(f"    {chunk},")
    if as_std_array:
        lines.append("}};")
    else:
        lines.append("};")
    lines.append("")


def build_legacy_preview(repo_root: Path, out_dir: Path, header_path: Path, config: str) -> None:
    slangc = find_slangc()
    if not slangc:
        raise RuntimeError("slangc executable not found (set SLANGC or install Slang toolchain)")
    shader_dir = repo_root / "shaders"
    out_dir.mkdir(parents=True, exist_ok=True)
    header_path.parent.mkdir(parents=True, exist_ok=True)
    is_debug = config.lower() in ("debug", "asan", "asan_debug")
    opt_flag = "-O0" if is_debug else "-O3"

    compiled: list[tuple[str, list[int]]] = []
    for filename, stage, symbol in LEGACY_PREVIEW_SHADERS:
        src = shader_dir / filename
        spv = out_dir / f"{filename}.spv"
        cmd = [
            slangc,
            str(src),
            "-target",
            "spirv",
            "-profile",
            "spirv_1_5",
            "-stage",
            stage,
            "-entry",
            "main",
            opt_flag,
        ]
        if is_debug:
            cmd.append("-g2")
        cmd.extend(["-o", str(spv)])
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"slangc failed for {src}:\n{proc.stderr}\n{proc.stdout}")
        compiled.append((symbol, read_spv_words(spv)))

    lines = [
        "#pragma once",
        "",
        "// Auto-generated by tools/shader_build.py. Do not edit manually.",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace aether::renderer::spv {",
        "",
    ]
    for symbol, words in compiled:
        write_u32_array(lines, symbol, words, as_std_array=True)
    lines.append("} // namespace aether::renderer::spv")
    lines.append("")
    header_path.write_text("\n".join(lines), encoding="utf-8")


def inspect_push_constant_size(param: dict) -> int:
    t = param.get("type", {})
    ev = t.get("elementVarLayout", {})
    b = ev.get("binding", {})
    if isinstance(b.get("size"), int):
        return int(b["size"])
    et = t.get("elementType", {})
    for sz in et.get("sizes", []):
        if sz.get("kind") == "uniform" and isinstance(sz.get("value"), int):
            return int(sz["value"])
    return 0


def validate_reflection(json_path: Path, shader_id: str, max_pc: int, allow_rt_set: bool) -> None:
    data = json.loads(json_path.read_text(encoding="utf-8"))
    for param in data.get("parameters", []):
        binding = param.get("binding", {})
        kind = binding.get("kind", "")
        if kind == "pushConstantBuffer":
            sz = inspect_push_constant_size(param)
            if sz > max_pc:
                raise RuntimeError(
                    f"Shader '{shader_id}' push constants size {sz} B exceeds max {max_pc} B"
                )
        elif kind == "descriptorTableSlot":
            space = int(binding.get("space", 0))
            if space > 1:
                raise RuntimeError(
                    f"Shader '{shader_id}' parameter '{param.get('name')}' uses unsupported set {space} > 1"
                )
            if space == 1 and not allow_rt_set:
                raise RuntimeError(
                    f"Non-RT shader '{shader_id}' binds Set 1 descriptor '{param.get('name')}'"
                )


def write_u8_array(lines: list[str], symbol: str, raw: bytes) -> None:
    lines.append(f"inline constexpr std::array<uint8_t, {len(raw)}> {symbol} = {{")
    for off in range(0, len(raw), 16):
        chunk = ", ".join(f"0x{b:02X}U" for b in raw[off : off + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")


def build_phase4_slang(
    repo_root: Path,
    manifest_path: Path,
    out_dir: Path,
    header_path: Path,
    dxil_header_path: Path | None,
    config: str,
    skip_dxil: bool,
) -> None:
    slangc = find_slangc()
    if not slangc:
        raise RuntimeError("slangc executable not found (set SLANGC or install Slang toolchain)")

    dxc_dir = find_dxc_dir()
    env = os.environ.copy()
    if dxc_dir:
        env["PATH"] = f"{dxc_dir}{os.pathsep}{env.get('PATH', '')}"

    manifest = tomllib.loads(manifest_path.read_text(encoding="utf-8"))
    entries = manifest.get("shader", [])
    if not entries:
        raise RuntimeError(f"No [[shader]] entries in {manifest_path}")

    shader_src_dir = repo_root / "shaders"
    phase4_out_dir = out_dir / "phase4"
    phase4_out_dir.mkdir(parents=True, exist_ok=True)
    header_path.parent.mkdir(parents=True, exist_ok=True)

    is_debug = config.lower() in ("debug", "asan", "asan_debug")
    spv_opt = "-O0" if is_debug else "-O3"

    compiled_entries: list[tuple[dict, str, list[int], bytes]] = []

    for entry in entries:
        sid = str(entry["id"])
        src_file = shader_src_dir / str(entry["file"])
        entry_fn = str(entry["entry"])
        stage = str(entry["stage"])
        ray_query = bool(entry.get("ray_query", False))
        max_pc = int(entry.get("max_push_constants", 128))
        if max_pc > 128:
            raise RuntimeError(f"Manifest entry '{sid}' max_push_constants {max_pc} > 128")

        spv_name = f"{sid}.spv"
        spv_path = phase4_out_dir / spv_name
        json_path = phase4_out_dir / f"{sid}.reflection.json"

        cmd_spv = [
            slangc,
            str(src_file),
            "-I",
            str(shader_src_dir),
            "-target",
            "spirv",
            "-profile",
            "spirv_1_6",
            "-D__TARGET_SPIRV__",
            "-entry",
            entry_fn,
            spv_opt,
            "-reflection-json",
            str(json_path),
            "-o",
            str(spv_path),
        ]
        if is_debug:
            cmd_spv.append("-g2")
        if ray_query:
            cmd_spv.extend(["-capability", "spvRayQueryKHR"])

        proc_spv = subprocess.run(cmd_spv, env=env, capture_output=True, text=True)
        if proc_spv.returncode != 0:
            raise RuntimeError(
                f"slangc SPIR-V failed for {sid} ({src_file}:{entry_fn}):\n{proc_spv.stderr}\n{proc_spv.stdout}"
            )
        if "warning[" in proc_spv.stderr.lower():
            raise RuntimeError(f"slangc SPIR-V warning on {sid}:\n{proc_spv.stderr}")

        validate_reflection(json_path, sid, max_pc, ray_query)
        words = strip_redundant_forward_pointers(spv_path)

        dxil_bytes = b""
        if not skip_dxil and dxc_dir:
            dxil_profile = STAGE_TO_DXIL_PROFILE[stage]
            dxil_path = phase4_out_dir / f"{sid}.dxil"
            cmd_dxil = [
                slangc,
                str(src_file),
                "-I",
                str(shader_src_dir),
                "-D__TARGET_DXIL__",
                "-target",
                "dxil",
                "-profile",
                dxil_profile,
                "-entry",
                entry_fn,
                spv_opt,
                "-o",
                str(dxil_path),
            ]
            proc_dxil = subprocess.run(cmd_dxil, env=env, capture_output=True, text=True)
            if proc_dxil.returncode != 0:
                raise RuntimeError(
                    f"slangc DXIL compile failed for {sid} ({src_file}:{entry_fn}):\n"
                    f"{proc_dxil.stderr}\n{proc_dxil.stdout}"
                )
            if dxil_path.exists():
                dxil_bytes = dxil_path.read_bytes()
            dxil_path.unlink(missing_ok=True)

        compiled_entries.append((entry, spv_name, words, dxil_bytes))

    enum_items = []
    blob_entries = []
    lines = [
        "#pragma once",
        "",
        "// Auto-generated by tools/shader_build.py from shaders/manifest.toml. Do not edit manually.",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <filesystem>",
        "#include <fstream>",
        "#include <vector>",
        "",
        "namespace aether::renderer::shaders {",
        "",
    ]

    for idx, (entry, spv_name, words, _dxil) in enumerate(compiled_entries):
        sid = str(entry["id"])
        enum_name = "".join(part.capitalize() for part in sid.split("_"))
        sym = f"kPhase4_{enum_name}_Spv"
        write_u32_array(lines, sym, words)
        enum_items.append(f"    {enum_name} = {idx},")
        rt_flag = "true" if entry.get("ray_query", False) else "false"
        blob_entries.append(
            f'    Phase4ShaderView{{"{sid}", "{spv_name}", {sym}, sizeof({sym}) / sizeof(uint32_t), {rt_flag}}},'
        )

    lines.append("enum class Phase4ShaderId : size_t {")
    lines.extend(enum_items)
    lines.append(f"    Count = {len(compiled_entries)},")
    lines.append("};")
    lines.append("")
    lines.append("struct Phase4ShaderView {")
    lines.append("    const char* id{nullptr};")
    lines.append("    const char* spv_filename{nullptr};")
    lines.append("    const uint32_t* words{nullptr};")
    lines.append("    size_t word_count{0};")
    lines.append("    bool uses_rt_set{false};")
    lines.append("};")
    lines.append("")
    dir_escaped = str(phase4_out_dir).replace("\\", "/")
    lines.append(f'inline constexpr const char* kPhase4DiskShaderDir = "{dir_escaped}";')
    lines.append("")
    lines.append(
        f"inline constexpr std::array<Phase4ShaderView, {len(compiled_entries)}> kPhase4Shaders = {{{{"
    )
    lines.extend(blob_entries)
    lines.append("}};")
    lines.append("")
    lines.append(
        "[[nodiscard]] inline std::vector<uint32_t> load_phase4_shader_words(Phase4ShaderId id, bool allow_disk_reload = true) {"
    )
    lines.append("    const auto& entry = kPhase4Shaders[static_cast<size_t>(id)];")
    lines.append("    if (allow_disk_reload) {")
    lines.append(
        "        const std::filesystem::path disk_path = std::filesystem::path(kPhase4DiskShaderDir) / entry.spv_filename;"
    )
    lines.append("        std::ifstream ifs(disk_path, std::ios::binary | std::ios::ate);")
    lines.append("        if (ifs.good()) {")
    lines.append("            const auto sz = static_cast<size_t>(ifs.tellg());")
    lines.append("            if (sz >= 20 && (sz % 4) == 0) {")
    lines.append("                ifs.seekg(0);")
    lines.append("                std::vector<uint32_t> out(sz / 4);")
    lines.append("                if (ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(sz)) && out[0] == 0x07230203u) {")
    lines.append("                    return out;")
    lines.append("                }")
    lines.append("            }")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return std::vector<uint32_t>(entry.words, entry.words + entry.word_count);")
    lines.append("}")
    lines.append("")
    lines.append("} // namespace aether::renderer::shaders")
    lines.append("")
    header_path.write_text("\n".join(lines), encoding="utf-8")

    if dxil_header_path is not None:
        dxil_header_path.parent.mkdir(parents=True, exist_ok=True)
        dlines = [
            "#pragma once",
            "",
            "// Auto-generated by tools/shader_build.py from shaders/manifest.toml. Do not edit manually.",
            "#include <array>",
            "#include <cstddef>",
            "#include <cstdint>",
            "#include <span>",
            '#include "generated/phase4_shaders_spv.hpp"',
            "",
            "namespace aether::renderer::shaders {",
            "",
            "struct Phase4DxilView {",
            "    const uint8_t* bytes{nullptr};",
            "    size_t byte_count{0};",
            "    bool uses_rt_set{false};",
            "};",
            "",
        ]
        dentries = []
        for entry, _spv_name, _words, dxil_bytes in compiled_entries:
            sid = str(entry["id"])
            enum_name = "".join(part.capitalize() for part in sid.split("_"))
            rt_flag = "true" if entry.get("ray_query", False) else "false"
            if len(dxil_bytes) > 0:
                sym = f"kPhase4_{enum_name}_Dxil"
                write_u8_array(dlines, sym, dxil_bytes)
                dentries.append(f"    Phase4DxilView{{{sym}.data(), {sym}.size(), {rt_flag}}},")
            else:
                dentries.append(f"    Phase4DxilView{{nullptr, 0u, {rt_flag}}},")
        dlines.append(
            f"inline constexpr std::array<Phase4DxilView, {len(compiled_entries)}> kPhase4Dxil = {{{{"
        )
        dlines.extend(dentries)
        dlines.append("}};")
        dlines.append("")
        dlines.append(
            "[[nodiscard]] inline std::span<const uint8_t> load_phase4_shader_dxil(Phase4ShaderId id) noexcept {"
        )
        dlines.append("    const size_t idx = static_cast<size_t>(id);")
        dlines.append("    if (idx >= kPhase4Dxil.size() || kPhase4Dxil[idx].bytes == nullptr) {")
        dlines.append("        return {};")
        dlines.append("    }")
        dlines.append("    return {kPhase4Dxil[idx].bytes, kPhase4Dxil[idx].byte_count};")
        dlines.append("}")
        dlines.append("")
        dlines.append("} // namespace aether::renderer::shaders")
        dlines.append("")
        dxil_content = "\n".join(dlines)
        if not dxil_header_path.exists() or dxil_header_path.read_text(encoding="utf-8") != dxil_content:
            dxil_header_path.write_text(dxil_content, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--header", required=True)
    parser.add_argument("--phase4-header", default="")
    parser.add_argument("--phase4-dxil-header", default="")
    parser.add_argument("--manifest", default="")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--skip-dxil", action="store_true")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    out_dir = Path(args.out_dir).resolve()
    header_path = Path(args.header).resolve()

    build_legacy_preview(repo_root, out_dir, header_path, args.config)

    manifest_path = Path(args.manifest) if args.manifest else (repo_root / "shaders" / "manifest.toml")
    phase4_header = (
        Path(args.phase4_header).resolve()
        if args.phase4_header
        else (header_path.parent / "phase4_shaders_spv.hpp")
    )
    dxil_header = (
        Path(args.phase4_dxil_header).resolve()
        if args.phase4_dxil_header
        else (phase4_header.parent / "phase4_shaders_dxil.hpp")
    )
    if manifest_path.exists():
        build_phase4_slang(
            repo_root,
            manifest_path.resolve(),
            out_dir,
            phase4_header,
            dxil_header,
            args.config,
            args.skip_dxil,
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
