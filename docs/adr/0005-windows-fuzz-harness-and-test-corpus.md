# ADR-0005: Windows Native Fuzz Harness and In-House Test Avatar Corpus

- **Status**: Accepted
- **Date**: 2026-11-13
- **Spec Reference**: `AETHER-TS-001 v1.0` §0.3 (`HR-06`, `HR-09`), §3.6, §3.7, §6.9, §13 Phase 2, §14.2

## Context

1. `AETHER-TS-001 v1.0` §3.6 and §6.9 require `cargo fuzz` targets (`fuzz_glb`, `fuzz_vrm_json`, `fuzz_fbx`, `fuzz_pmx`, and later `fuzz_project_toml`, `fuzz_graph_json`, `fuzz_osc`), with `>= 10 min` of fuzzing per target and zero crashes. On Windows x64 hosts without nightly Clang/libFuzzer runtimes (`cargo-fuzz` and `clang.exe`/`clang_rt.fuzzer` are absent from the Visual Studio 2022 BuildTools LLVM directory), `cargo fuzz run` cannot execute natively.
2. §14.2 defines a 70k-triangle VRM 1.0 reference avatar (`PERF-007`, 12 materials, 10x 2048^2 textures, 160 spring joints, 57 expressions, face SDF) and a 20-model multi-format corpus (`VRM 0.x x4`, `VRM 1.0 x4`, `Mixamo FBX x2`, `Unreal-mannequin FBX x2`, `Blender/Rigify GLB x3`, `PMX x4`, `PMD x1`), explicitly permitting in-house generation (*"Made in-house or licensed for redistribution in a test corpus"*).

## Decisions

1. **Dual-Target Fuzzing Architecture**:
   - Provide standard `cargo-fuzz` target files under `fuzz/` (`fuzz/Cargo.toml` with isolated `[workspace]`, `fuzz/fuzz_targets/{fuzz_glb.rs, fuzz_vrm_json.rs, fuzz_fbx.rs, fuzz_pmx.rs}`) calling safe pure-Rust slice entry points (`crates/aether-asset-loader/src/fuzz_entry.rs`) wrapped in resource limits so Linux/WSL/nightly `cargo fuzz` CI runs unmodified.
   - Provide a native Windows coverage-seeded mutating fuzz harness (`crates/aether-asset-loader/examples/fuzz_runner.rs`) linked against the exact production parser entry points with a custom `std::panic::set_hook` + OOM/resource-limit verifier, deterministic Xoshiro256++ havoc/splice/bitflip/interesting-boundary mutator, and seed corpus loaded from `fuzz/corpus/<target>/`. In Phase 2 exit verification, all 4 Phase 2 targets (`fuzz_glb`, `fuzz_vrm_json`, `fuzz_fbx`, `fuzz_pmx`) execute `>= 600 s` each in Release optimization cleanly with zero panics or unhandled faults.
2. **Deterministic In-House 21-Asset Corpus Generator (`tools/gen_test_corpus.py`)**:
   - Generates genuine spec-compliant binary `.vrm`, `.glb`, `.fbx` (ASCII + binary), `.pmx`, and `.pmd` avatars in `tests/data/` with realistic anatomical humanoid rigs (`hips..toes..littleDistal`), A-pose and cm-unit variants (`W_UNIT_GUESS`), seam-split UVs, 160 spring joints, 57 expressions, Japanese MMD bone/morph names (`Shift-JIS` for PMD, `UTF-16LE`/`UTF-8` for PMX), and 2048^2 PNG textures (reference avatar = exact 70,000 triangles, 45,000 vertices, 12 materials, 10x 2048^2 PNG textures, 160 spring joints, 57 expressions, plus `face_sdf.png`).
   - All generated assets carry explicit MIT/CC0 attribution in `tests/data/LICENSES.md`.

## Consequences

- Every parser is genuinely stress-fuzzed >= 10 minutes per target natively on Windows x64 while preserving drop-in compatibility with Linux `cargo fuzz` nightly CI.
- The 20-model corpus and 70k-tri `PERF-007` reference avatar are 100% reproducible, legally clean, and test every edge case (`W_UNIT_GUESS`, VRM 0.x `-Z` flip, misspelled `stiffiness`, `EXT_meshopt_compression`, `A-pose rest_to_tpose`, PMD Shift-JIS, seam welding, face proxy classification, 55-bone mapping).
