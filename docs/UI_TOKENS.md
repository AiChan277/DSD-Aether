# Precision Dark Design Tokens (`AETHER-TS-001` §12.3)

Single source of truth for all colours, typography, spacing, sizing, and radii. Reflected directly in `src/ui/theme/tokens.hpp` and `src/ui/theme/precision_dark.cpp`. Panel code MUST NOT contain literal colours (`REQ-UI-001`).

## Colour Tokens (§12.3.1)

| Token | sRGB Hex | Usage |
|---|---|---|
| `bg.app` | `#111318` | Window and dockspace background, dock gaps, menu bar, telemetry bar |
| `bg.panel` | `#181B22` | Panel surface, active tab |
| `bg.raised` | `#1F232C` | Buttons, popovers, menus, node title, table header |
| `bg.overlay` | `#252A34` | Tooltips, drag previews |
| `bg.inset` | `#0F1115` | Input fields, sliders, segmented control track |
| `bg.hover` | `#232834` | Row hover |
| `bg.selected` | `#1B2A45` | Selected row (with 2 px accent left bar) |
| `bg.canvas` | `#13151A` | Node canvas background (16 px dot grid in `#262B36`) |
| `border.default` | `#262B36` | Hard 1 px borders: fields, popovers, panel separators |
| `border.subtle` | `#1F232C` | Row and section dividers |
| `border.strong` | `#333A48` | Button and popup borders, hovered fields |
| `text.primary` | `#E3E6EB` | Values, names, titles (contrast 13.8:1 on `bg.panel`) |
| `text.secondary` | `#A0A7B4` | Property labels, menu items, inactive values (contrast 7.1:1) |
| `text.tertiary` | `#7D8494` | Units, placeholders, metadata, inactive tabs (contrast 4.6:1) |
| `text.disabled` | `#4B515D` | Disabled controls only |
| `accent` | `#3B82F6` | Selection, focus ring, slider fill, active toggles, focused-tab overline |
| `accent.hover` | `#5B96F7` | Accent hover |
| `accent.pressed` | `#2D6FDB` | Accent pressed |
| `accent.button` | `#2563EB` | Primary button fill with `#FFFFFF` text (contrast 5.2:1) |
| `status.npu` / `status.ok` | `#10B981` | NPU active indicator, healthy state dots |
| `status.warn` | `#F5A524` | Fallback tier and near-budget |
| `status.error` | `#F04438` | Over budget, failures |
| `status.live` | `#E5484D` | ON AIR tally only |
| `axis.x` / `axis.y` / `axis.z` | `#E5534B` / `#57AB5A` / `#4A8FE7` | Vector field axis letters and gizmo handles |
| `viz.thread.T1..T4` | `#3987E5` / `#D95926` / `#199E70` / `#C98500` | Telemetry series (T1 Vision, T2 Sim, T3 Render, T4 Output) |
| `viz.confidence.*` | `#199E70` / `#C98500` / `#3987E5` | Tracked / interpolating / procedural |
| `checker.a` / `checker.b` | `#15171C` / `#1A1D23` | Viewport 8 px alpha checkerboard (§10.11) |
| `scrim.modal` | `#08090C` @ 60% | Modal scrim |
| `scrollbar.thumb` / `hover` | `#2E3440` / `#3A4150` | 8 px scrollbar thumb |

## Typography (§12.3.2)

- `type.caption`: Inter 11/14 (400) — units, badges, legend, metadata
- `type.body`: Inter 12/16 (400) — default UI base size
- `type.body-strong`: Inter 12/16 (500/600) — tabs, buttons, section headers, node titles
- `type.title`: Inter 13/18 (600) — inspector object title, dialog titles
- `type.display`: Inter 20/26 (600) — wizard step heading only
- `type.mono`: JetBrains Mono 11/14 (400/500) — telemetry, console, param IDs, OSC addresses, key caps
- Numeric fields use `Inter-Regular-tnum.ttf` / `Inter-SemiBold-tnum.ttf` (`tnum` + `cv11` frozen).

## Spacing, Sizing & Radius (§12.3.3)

- Base unit: 4 px (steps 2, 4, 6, 8, 12, 16, 24, 32)
- Control height: 22 px compact (default), 26 px comfortable
- Property row: 26 px; label column 104 px (min 88, max 140); 8 px right padding
- Tree row: 22 px; indent 14 px; twisty 14 px
- Panel padding: 8 px; section inset 22 px; tab bar 28 px; menu bar 32 px; telemetry bar 24 px fixed
- Radius: 0 panels, 3 controls, 4 popovers/nodes, 6 modals, 999 status pills
- Borders: 1 px everywhere; dock splitter 2 px (`bg.app`, hover turns `accent`); scrollbar 8 px (no arrows)
