# Studio Design Tokens (Broadcast Overhaul — DaVinci Resolve 19 surfaces + UE5 Blueprint graph)

Single source of truth for all colours, typography, spacing, sizing, and radii: `src/ui/theme/tokens.hpp`
(applied to ImGui by `src/ui/theme/precision_dark.cpp`). Panel code MUST NOT contain literal colours —
`tools/check_gates.py` fails the build on `IM_COL32(` / `hex_rgba(` / `hex_col32(` outside `tokens.hpp` (`REQ-UI-001`).

## Surface & Semantic Colours

| Token (`tokens.hpp`) | Spec name | sRGB | Usage |
|---|---|---|---|
| `bg_app`, `bg_canvas` | BgCanvasDeep | `#0B0D10` | App void, dock gaps, menu/telemetry bars, Node Graph void, viewport backdrop |
| `bg_panel` | BgPanelSurface | `#14171D` | Dockable panel body |
| `bg_raised` | BgHeaderSection | `#1C2029` | Collapsible section headers, tab bars, buttons |
| `bg_inset` | BgControlWell | `#0F1217` | Inputs, scrub sliders, combo wells, search |
| `bg_hover` | BgControlHover | `#232936` | Hovered buttons, combos, scrubbers, expression buttons |
| `bg_well` | — | `#0E1015` | Recessed Outliner `##SceneTreeWell` |
| `bg_selected` | — | `#1A2B4A` | Selected rows / latched expressions |
| `border_default` | BorderHairline | `#262B36` | 1 px structural borders, table rows |
| `accent` | AccentPrimary | `#2563EB` | Active mode pill, slider fill, focus border |
| `status_live` | AccentBroadcastLive | `#DC2626` | `● LIVE` tally |
| `status_ok` | StatusHealthyGreen | `#10B981` | Active outputs, NPU dot, in-budget timings |
| `status_warn` | StatusStandbyAmber | `#F59E0B` | Idle outputs, modified-param dot |
| `status_off` | — | `#4B5563` | `Off` output pill |
| `text_primary` / `text_secondary` | TextPrimary / TextMuted | `#E2E8F0` / `#8B95A5` | Values & headers / labels, inactive tabs |
| `text_tertiary` | — | `#7A8496` | Units, placeholders, metadata |

## Node Graph (UE5 Blueprint)

| Token | Value | Usage |
|---|---|---|
| `col32_node_body` / `col32_node_border` / `col32_node_border_hover` / `col32_node_selected` | `#13161CEB` / `#2E3545` / `#4B5670` / `#F59E0B` | Node card fill, 1.5 px border, hover, 2 px selected outline |
| `node_hdr_source` / `solver` / `blend` / `output` | `#881B2B→#3A111A` / `#1D4E89→#112642` / `#0F6B58→#0A332B` / `#5B2182→#2A113D` | 26 px category gradient headers |
| `col32_pin_types[]` | Float `#38BDF8`, Bool `#FB7185`, Vec3 `#FB923C`, Pose `#F472B6`, Blendshape `#FACC15`, Viseme `#A855F7`, Event `#E2E8F0` | Pin glyphs (hollow = unconnected) and wire colours |
| `col32_wire_selected` / `col32_wire_shadow` | `#FBBF24` / `#000000` @ 25% | Selected wire (+ halo) / wire shadow spline |
| geometry | header 26, row 24, footer 18, pin inset 12, label inset 24, inline box 48, pin r 5, hit r 11, snap r 14, wire hit 6.5 | World units × zoom × dpi |

## Viewport Overlays

`col32_hud_bg` `#0B0D10D9` (toolbar pill, tracking HUD, orb readout), Light Orbit Gizmo `col32_orb_fill` `#141820`,
`col32_orb_ring` `#2E3646`, handles Key `#F59E0B` / Fill `#38BDF8` / Rim `#10B981`.

## Typography

- UI: Inter 13 px × dpi (spec 13.5 px — the FreeType builder rasterizes whole pixel sizes, so the token states the
  size actually rendered), semibold for titles/node headers; caption 11 px; title 14 px.
- Numerics: JetBrains Mono 12 px × dpi (spec 12.5 px) for HUDs, telemetry, timings, key badges.
- Layout code derives its scale as `GetFontSize() / type_body_px`, which equals the effective dpi.

## Spacing, Sizing & Radius (× dpi)

- WindowPadding 10×10, FramePadding 8×5, ItemSpacing 8×6, CellPadding 4×3.
- FrameRounding 3, ChildRounding 4, PopupRounding 5, node radius 7.
- Docks: Outliner 275 px (spec), Inspector 335 px (spec 325 — the longest combo value needs ~10 px more to avoid clipping;
  `studio_ui_test` verifies no Inspector label/combo or Outliner label is clipped at 100/125/150/175% DPI).
- Inspector property table: label column ≥ 38% (grows to the longest label + 12 px reset-dot slot, max 48%),
  control column stretches; scrub sliders fill from 0 on bipolar ranges.
- Scene tree well 190 px; viewport toolbar pill 28 px at client origin + (12, 10).
