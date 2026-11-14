# ADR-0010: Self-Contained C++20 NVIDIA FLIP Metric & Dual-GPU Verification Scope (`§14.3`, `HR-09`, `HR-10`)

- **Status**: Accepted
- **Date**: 2026-11-13
- **Spec Reference**: `AETHER-TS-001 v1.0` §3.7, §13 (Phase 4), §14.3, `HR-09`, `HR-10`

## Context

The Phase 4 exit gate requires golden-image verification across 6 canonical scenes (`front_key`, `three_quarter_left`, `rim_backlight`, `closeup_zflatten`, `hair_shadow_sweep`, `transparent_glow_checkerboard`) with RT ON and RT OFF across vendors using NVIDIA FLIP (`mean <= 0.05`). Neither NVIDIA's `flip-evaluator` C++ library nor an AMD RX 7800 XT physical GPU (`RM-B`) is present in vcpkg or the local test host (which contains **NVIDIA GeForce RTX 5060 Laptop GPU** and **Intel(R) Graphics**, both supporting Vulkan 1.4 and `VK_KHR_ray_query`).

## Decision

1. **In-House Test-Only Reference FLIP Evaluator (`tests/cpp/flip_metric.hpp`)**:
   - Implement the Andersson et al. (HPG 2020, "FLIP: A Difference Evaluator for Alternating Images") LDR FLIP mathematical algorithm directly in C++20 inside `tests/cpp/` (no external dependency added to `vcpkg.json` or production libraries).
   - Pipeline: sRGB decode -> linear CIE XYZ (D65) -> opponent `YCxCz` space -> separated 2D Gaussian CSF spatial frequency filtering tuned to standard viewing conditions (0.7 m distance, 0.7 mm pixel pitch -> ~67 px/deg) -> HyAB / opponent color discrepancy + Scharr partial-derivative edge/point feature discrepancy -> spatial map `[0, 1]` + mean / p95 / max summary.
   - Verify the FLIP implementation itself with unit assertions (`identical == 0.0`, `black_vs_white > 0.90`, `monotonicity`, `known ΔE sensitivity`).

2. **Hardware Cross-Vendor & RT Parity Coverage (`HR-09`)**:
   - Generate deterministic canonical scene golden PNGs (`tests/golden/references/`) rendered by the production Vulkan 1.3 14-pass renderer and visually inspected before locking references.
   - Execute `golden_render_test` comparing:
     - **Regression**: active GPU renders for all 6 canonical scenes (RT ON and RT OFF) vs locked references (`FLIP mean <= 0.05`).
     - **Cross-GPU physical vendor parity**: enumerate physical GPUs in the test and run both NVIDIA RTX 5060 (`0x10DE`) and Intel(R) Graphics (`0x8086`) on-device across the 6 scenes, verifying real cross-vendor perceptual agreement (`FLIP mean <= 0.05`).
     - Explicitly report in `docs/phases/phase-4-report.md` that NVIDIA (`RM-A`-class RTX 5060) and Intel (`0x8086`) were hardware-verified on this workstation while physical AMD (`0x1002`, RM-B) hardware verification remains pending self-hosted RM-B nightly runner (`§3.7`).

## Consequences

- Zero new third-party dependencies (`HR-10`).
- Offline Catch2 CI executes real Vulkan 1.3 multi-adapter rendering and perceptual FLIP verification cleanly across all presets.
- Exact observed test scope per vendor is reported honestly without overclaiming physical AMD GPU execution (`HR-09`).
