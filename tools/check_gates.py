"""Repository static-analysis gates for AETHER-2.5D hard rules (`§0.3`, `§3.7`, `§12.3`)."""

from __future__ import annotations

import json
import re
import sys
import tomllib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

HOT_DIRS_CPP = [
    REPO_ROOT / "src" / "core",
    REPO_ROOT / "src" / "rhi",
    REPO_ROOT / "src" / "app",
    REPO_ROOT / "src" / "tracking",
    REPO_ROOT / "src" / "animation",
    REPO_ROOT / "src" / "renderer",
    REPO_ROOT / "src" / "output",
]

CPP_EXTS = {".h", ".hpp", ".cpp"}

BLOCKING_LOCK_RE = re.compile(
    r"\b(std::mutex|std::recursive_mutex|std::shared_mutex|std::timed_mutex|"
    r"std::condition_variable|std::condition_variable_any|"
    r"std::lock_guard|std::unique_lock|std::scoped_lock|"
    r"SRWLOCK|CRITICAL_SECTION|InitializeCriticalSection|EnterCriticalSection)\b"
)

RUST_LOCK_RE = re.compile(r"\b(std::sync::Mutex|std::sync::RwLock|parking_lot::)\b")

SLEEP_RE = re.compile(
    r"\b(Sleep|SleepEx|std::this_thread::sleep_for|std::this_thread::sleep_until)\s*\("
)

ABORT_RE = re.compile(r"\b(abort|std::abort|std::terminate|ExitProcess)\s*\(")

UI_LITERAL_COLOR_PATTERNS = [
    re.compile(r"#[0-9A-Fa-f]{6}\b"),
    re.compile(r"#[0-9A-Fa-f]{8}\b"),
    re.compile(r"\bIM_COL32\s*\("),
    re.compile(r"\bhex_rgba\s*\("),
    re.compile(r"\bhex_col32\s*\("),
    re.compile(r"\bImVec4\s*\(\s*[0-9]"),
]


def strip_cpp_comments(text: str) -> str:
    text = re.sub(r"//.*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return text


def iter_cpp_files(dirs: list[Path]) -> list[Path]:
    out: list[Path] = []
    for d in dirs:
        if not d.exists():
            continue
        for p in sorted(d.rglob("*")):
            if p.suffix in CPP_EXTS and p.is_file():
                out.append(p)
    return out


def check_regex_gate(files: list[Path], pattern: re.Pattern[str], label: str) -> list[str]:
    failures: list[str] = []
    for p in files:
        cleaned = strip_cpp_comments(p.read_text(encoding="utf-8", errors="replace"))
        for lineno, line in enumerate(cleaned.splitlines(), start=1):
            m = pattern.search(line)
            if m:
                rel = p.relative_to(REPO_ROOT)
                failures.append(f"[{label}] {rel}:{lineno}: matched '{m.group(0)}'")
    return failures


def check_ui_literal_colors() -> list[str]:
    failures: list[str] = []
    ui_dir = REPO_ROOT / "src" / "ui"
    for p in sorted(ui_dir.rglob("*")):
        if p.suffix not in CPP_EXTS or not p.is_file():
            continue
        if p.name == "tokens.hpp":
            continue
        cleaned = strip_cpp_comments(p.read_text(encoding="utf-8", errors="replace"))
        for lineno, line in enumerate(cleaned.splitlines(), start=1):
            for pat in UI_LITERAL_COLOR_PATTERNS:
                m = pat.search(line)
                if m:
                    rel = p.relative_to(REPO_ROOT)
                    failures.append(
                        f"[REQ-UI-001] {rel}:{lineno}: literal color '{m.group(0)}' forbidden outside tokens.hpp"
                    )
    return failures


def check_rust_locks() -> list[str]:
    failures: list[str] = []
    crates_dir = REPO_ROOT / "crates"
    for p in sorted(crates_dir.rglob("*.rs")):
        if not p.is_file():
            continue
        for lineno, line in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if line.lstrip().startswith("//"):
                continue
            m = RUST_LOCK_RE.search(line)
            if m:
                rel = p.relative_to(REPO_ROOT)
                failures.append(f"[HR-02 Rust] {rel}:{lineno}: matched '{m.group(0)}'")
    return failures


def check_dependency_allowlist() -> list[str]:
    failures: list[str] = []
    allow_doc = tomllib.loads((REPO_ROOT / "docs" / "dependencies_allowlist.toml").read_text("utf-8"))
    allowed_vcpkg = set(allow_doc["vcpkg"]["allowed"])
    allowed_cargo = set(allow_doc["cargo"]["allowed"])

    vcpkg_manifest = json.loads((REPO_ROOT / "vcpkg.json").read_text("utf-8"))
    for dep in vcpkg_manifest.get("dependencies", []):
        name = dep if isinstance(dep, str) else dep["name"]
        if name not in allowed_vcpkg:
            failures.append(f"[HR-10] vcpkg.json dependency '{name}' missing from allowlist/ADR")

    cargo_toml = tomllib.loads((REPO_ROOT / "Cargo.toml").read_text("utf-8"))
    for name in cargo_toml.get("workspace", {}).get("dependencies", {}):
        if name not in allowed_cargo:
            failures.append(f"[HR-10] Cargo.toml workspace dep '{name}' missing from allowlist/ADR")

    return failures


def main() -> int:
    all_src_cpp = iter_cpp_files([REPO_ROOT / "src"])
    hot_cpp = iter_cpp_files(HOT_DIRS_CPP)

    failures: list[str] = []
    failures.extend(check_regex_gate(hot_cpp, BLOCKING_LOCK_RE, "HR-02 Blocking Lock"))
    failures.extend(check_regex_gate(all_src_cpp, SLEEP_RE, "§4.6 Sleep Forbidden"))
    failures.extend(check_regex_gate(hot_cpp, ABORT_RE, "Priority-1 Crash-Free"))
    failures.extend(check_ui_literal_colors())
    failures.extend(check_rust_locks())
    failures.extend(check_dependency_allowlist())

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1

    print(
        f"Gates passed cleanly: checked {len(all_src_cpp)} C++ files, Rust crates, "
        f"UI token isolation, and dependency allowlist."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
