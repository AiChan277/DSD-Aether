# ADR-0013: Phase 6 Studio UI Plotting, Gizmos, Node Canvas, and Icon/CJK Fonts

- **Status**: Accepted
- **Date**: 2026-11-14
- **Spec Reference**: `AETHER-TS-001 v1.0` §0.3 (`HR-10`, `HR-11`), §3.2, §12.3.2, §12.3.5, §12.3.6, §12.6, §12.8

## Context

Phase 6 implements the full 5-zone Precision Dark Studio UI:
1. Telemetry Performance popover stacked thread frame-time charts (`ImPlot`, §12.8).
2. Interactive Viewport translation/rotation/scale gizmos for lights, props, and cameras (`ImGuizmo`, §12.6).
3. Interactive Warudo/Blueprint-style Node Graph canvas with custom pin shapes (circle, diamond Event, rounded-square Pose), 2 px top category strips, inline live mono values + `40x12 px` float sparklines, cached Bézier wire tessellation, double-click wire reroutes, `Alt+click` wire removal, comment frames, and `150x88 px` minimap (§12.3.6, §12.8, `REQ-UI-020`).
4. Uniform-stroke 24-unit grid iconography (`ISC`) plus Japanese bone/morph glyph support (`SIL OFL 1.1`, §12.3.2, §12.3.5).

Evaluating the pinned `vcpkg` baseline (`3af1d1e60af2b2abf55760538cd607829029b07a`) shows `implot` (`0.16`, MIT) and `imguizmo` (`2024-05-29`, MIT) are available directly in the baseline, whereas `imnodes` (nelarius) is absent (only the distinct `imgui-node-editor 0.9.3` port exists) and external network icon packages can introduce mismatched stroke weights or unvetted bloat.

## Decision

1. **Add `implot` (`0.16`, MIT) and `imguizmo` (`2024-05-29`, MIT) to `vcpkg.json` and `docs/dependencies_allowlist.toml`**:
   - Both compile statically into `aether_ui`, use ImGui's allocator callbacks (`ImGuiArenaAllocator` zero steady-state allocations), and carry MIT licenses compatible with commercial release.
   - `ImPlot` powers the `440x320` telemetry Performance popover stacked frame-time chart (`Chart | Table` toggle).
   - `ImGuizmo` powers `Q/W/E/R/X` light/prop/camera manipulation inside Zone 2 (`Viewport`).
2. **In-House Precision Dark Node Graph Canvas (`src/ui/panels/nodegraph/node_canvas.*`)**:
   - Rather than altering the pinned vcpkg baseline or patching third-party `imgui-node-editor` against Dear ImGui `1.91.9`, we implement `AetherNodeCanvas` directly on `ImDrawList` using the exact §12.3.6 `ImNodesStyle` constants and §12.8 anatomy tokens.
   - Uses fixed pre-sized arenas, cached cubic Bézier polyline tessellation per edge (re-tessellated only when endpoints move), and viewport frustum culling, guaranteeing zero per-frame allocations (`REQ-UI-020` `<= 0.4 ms` UI CPU p95 with 64 nodes) and exact compliance with diamond/rounded-square pins, inline sparklines, and wire reroutes.
3. **In-Repo Deterministic Icon & Japanese Font Assets (`assets/fonts/`)**:
   - `assets/fonts/AetherIcons-Regular.ttf` (`ISC` notice in `assets/fonts/AetherIcons-ISC.txt`): generated reproducibly by `tools/gen_ui_fonts.py` (`fontTools`) on the 24x24 grid with uniform stroke weight, covering all Studio UI icons (`search`, `eye`, `lock`, `camera`, `play`, `pause`, `sliders`, etc.) and the 5 domain glyphs (`NPU`, `SDF`, `BLAS`, `Spout`, `Live2D flatness`) in PUA `U+E001..U+E040`, free of emoji or mismatched strokes (§12.3.5, §12.2).
   - `assets/fonts/NotoSansJP-Subset.ttf` (`SIL OFL 1.1`, notice in `assets/fonts/NotoSansJP-OFL.txt`): subsetted by `tools/gen_ui_fonts.py` from `Noto Sans JP` covering Hiragana, Katakana, CJK symbols, fullwidth ASCII, and common MMD/VRM kanji bone/morph characters, merged dynamically into the Inter UI atlas with FreeType light hinting.

## Consequences

- No change to the pinned vcpkg baseline commit `3af1d1e60af2b2abf55760538cd607829029b07a`.
- Zero steady-state heap allocations inside `ImGuiArenaAllocator` across `ImPlot`, `ImGuizmo`, and `AetherNodeCanvas`.
- Full CJK glyph rendering for Japanese PMX/PMD/VRM avatars without multi-megabyte font atlas bloat.
