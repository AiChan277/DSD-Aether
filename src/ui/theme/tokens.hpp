#pragma once

#include <imgui.h>

#include <array>
#include <cstdint>

namespace aether::ui {

/** Converts an `0xRRGGBBAA` hex literal into an ImVec4 color in linear UI space. */
[[nodiscard]] constexpr ImVec4 hex_rgba(uint32_t rgba) noexcept {
    const float r = static_cast<float>((rgba >> 24) & 0xFFU) / 255.0f;
    const float g = static_cast<float>((rgba >> 16) & 0xFFU) / 255.0f;
    const float b = static_cast<float>((rgba >> 8) & 0xFFU) / 255.0f;
    const float a = static_cast<float>(rgba & 0xFFU) / 255.0f;
    return ImVec4{r, g, b, a};
}

/** Converts an `0xRRGGBBAA` hex literal into packed ImGui ABGR `ImU32`. */
[[nodiscard]] constexpr ImU32 hex_col32(uint32_t rgba) noexcept {
    const uint32_t r = (rgba >> 24) & 0xFFU;
    const uint32_t g = (rgba >> 16) & 0xFFU;
    const uint32_t b = (rgba >> 8) & 0xFFU;
    const uint32_t a = rgba & 0xFFU;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

/** Replaces the alpha byte of a packed `ImU32` (0..255). */
[[nodiscard]] constexpr ImU32 col32_with_alpha(ImU32 col, uint32_t alpha) noexcept {
    return (col & 0x00FFFFFFU) | ((alpha & 0xFFU) << 24);
}

/** Two-stop horizontal gradient used by Blueprint node header banners. */
struct GradientPair {
    ImU32 left{0};
    ImU32 right{0};
};

/**
 * Single authoritative source of Studio colors, typography, spacing, and corner radii
 * (`AETHER-TS-001` §12.3, §12.14, `REQ-UI-001`). Palette follows the broadcast design system:
 * DaVinci Resolve 19-style surfaces and Unreal Engine 5 Blueprint node/pin semantics.
 * `tools/check_gates.py` forbids color literals anywhere else under `src/ui/`.
 */
struct PrecisionDarkTokens {
    // Surfaces
    ImVec4 bg_app = hex_rgba(0x0B0D10FF);      // BgCanvasDeep: app void, dock gaps, menu & telemetry bars
    ImVec4 bg_panel = hex_rgba(0x14171DFF);    // BgPanelSurface: dockable panel body
    ImVec4 bg_raised = hex_rgba(0x1C2029FF);   // BgHeaderSection: section headers, tab bars, buttons
    ImVec4 bg_overlay = hex_rgba(0x1F242EFF);  // Popovers, menus, tooltips
    ImVec4 bg_inset = hex_rgba(0x0F1217FF);    // BgControlWell: inputs, scrubbers, combo wells, search
    ImVec4 bg_hover = hex_rgba(0x232936FF);    // BgControlHover
    ImVec4 bg_selected = hex_rgba(0x1A2B4AFF); // Selected rows (accent-tinted)
    ImVec4 bg_canvas = hex_rgba(0x0B0D10FF);   // Node graph void + viewport backdrop
    ImVec4 bg_well = hex_rgba(0x0E1015FF);     // Recessed Outliner scene-tree well

    // Borders
    ImVec4 border_default = hex_rgba(0x262B36FF); // BorderHairline
    ImVec4 border_subtle = hex_rgba(0x1C2029FF);
    ImVec4 border_strong = hex_rgba(0x333A48FF);

    // Text
    ImVec4 text_primary = hex_rgba(0xE2E8F0FF);   // TextPrimary: values & headers
    ImVec4 text_secondary = hex_rgba(0x8B95A5FF); // TextMuted: property labels, inactive tabs
    ImVec4 text_tertiary = hex_rgba(0x7A8496FF);  // Units, placeholders, metadata
    ImVec4 text_disabled = hex_rgba(0x4B515DFF);
    ImVec4 text_on_accent = hex_rgba(0xFFFFFFFF);

    // Accent
    ImVec4 accent = hex_rgba(0x2563EBFF); // AccentPrimary: active mode, slider fill, focus border
    ImVec4 accent_hover = hex_rgba(0x3B82F6FF);
    ImVec4 accent_pressed = hex_rgba(0x1D4ED8FF);
    ImVec4 accent_button = hex_rgba(0x2563EBFF);

    // Semantics
    ImVec4 status_ok = hex_rgba(0x10B981FF);   // StatusHealthyGreen
    ImVec4 status_warn = hex_rgba(0xF59E0BFF); // StatusStandbyAmber
    ImVec4 status_error = hex_rgba(0xEF4444FF);
    ImVec4 status_live = hex_rgba(0xDC2626FF); // AccentBroadcastLive
    ImVec4 status_off = hex_rgba(0x4B5563FF);  // Output "Off" pill

    // Axis
    ImVec4 axis_x = hex_rgba(0xE5534BFF);
    ImVec4 axis_y = hex_rgba(0x57AB5AFF);
    ImVec4 axis_z = hex_rgba(0x4A8FE7FF);

    // 3D Viewport Transform Gizmo & SpringBone Rigging Overlays
    ImU32 col32_gizmo_x = hex_col32(0xEF4444FF);
    ImU32 col32_gizmo_y = hex_col32(0x22C55EFF);
    ImU32 col32_gizmo_z = hex_col32(0x3B82F6FF);
    ImU32 col32_gizmo_hover = hex_col32(0xFACC15FF);
    ImU32 col32_gizmo_center = hex_col32(0xE2E8F0FF);
    ImU32 col32_collider_rest = hex_col32(0x06B6D4FF);
    ImU32 col32_collider_contact = hex_col32(0xF97316FF);

    // Viz series
    ImVec4 viz_t1 = hex_rgba(0x3987E5FF);
    ImVec4 viz_t2 = hex_rgba(0xD95926FF);
    ImVec4 viz_t3 = hex_rgba(0x199E70FF);
    ImVec4 viz_t4 = hex_rgba(0xC98500FF);

    // Viewport checker + scrim + scrollbar + elevation shadow
    ImVec4 checker_a = hex_rgba(0x101318FF);
    ImVec4 checker_b = hex_rgba(0x151920FF);
    ImVec4 scrim_modal = hex_rgba(0x05060899);
    ImVec4 shadow_soft = hex_rgba(0x03040650);
    ImVec4 transparent = hex_rgba(0x00000000);
    ImVec4 docking_preview = hex_rgba(0x2563EB59);
    ImVec4 scrollbar_thumb = hex_rgba(0x2A303CFF);
    ImVec4 scrollbar_hover = hex_rgba(0x3A4150FF);

    // Packed ImU32 mirrors for DrawList calls without color literals in panels
    ImU32 col32_bg_app = hex_col32(0x0B0D10FF);
    ImU32 col32_bg_panel = hex_col32(0x14171DFF);
    ImU32 col32_bg_raised = hex_col32(0x1C2029FF);
    ImU32 col32_bg_overlay = hex_col32(0x1F242EFF);
    ImU32 col32_bg_inset = hex_col32(0x0F1217FF);
    ImU32 col32_bg_hover = hex_col32(0x232936FF);
    ImU32 col32_bg_selected = hex_col32(0x1A2B4AFF);
    ImU32 col32_bg_canvas = hex_col32(0x0B0D10FF);
    ImU32 col32_bg_well = hex_col32(0x0E1015FF);
    ImU32 col32_grid_minor = hex_col32(0x151920FF);
    ImU32 col32_grid_major = hex_col32(0x1D222CFF);
    ImU32 col32_border_default = hex_col32(0x262B36FF);
    ImU32 col32_border_subtle = hex_col32(0x1C2029FF);
    ImU32 col32_border_strong = hex_col32(0x333A48FF);
    ImU32 col32_text_primary = hex_col32(0xE2E8F0FF);
    ImU32 col32_text_secondary = hex_col32(0x8B95A5FF);
    ImU32 col32_text_tertiary = hex_col32(0x7A8496FF);
    ImU32 col32_text_disabled = hex_col32(0x4B515DFF);
    ImU32 col32_text_on_accent = hex_col32(0xFFFFFFFF);
    ImU32 col32_accent = hex_col32(0x2563EBFF);
    ImU32 col32_accent_hover = hex_col32(0x3B82F6FF);
    ImU32 col32_accent_pressed = hex_col32(0x1D4ED8FF);
    ImU32 col32_status_ok = hex_col32(0x10B981FF);
    ImU32 col32_status_warn = hex_col32(0xF59E0BFF);
    ImU32 col32_status_error = hex_col32(0xEF4444FF);
    ImU32 col32_status_live = hex_col32(0xDC2626FF);
    ImU32 col32_status_off = hex_col32(0x4B5563FF);
    ImU32 col32_axis_x = hex_col32(0xE5534BFF);
    ImU32 col32_axis_y = hex_col32(0x57AB5AFF);
    ImU32 col32_axis_z = hex_col32(0x4A8FE7FF);
    ImU32 col32_checker_a = hex_col32(0x101318FF);
    ImU32 col32_checker_b = hex_col32(0x151920FF);
    ImU32 col32_shadow_soft = hex_col32(0x03040624);
    ImU32 col32_viz_t1 = hex_col32(0x3987E5FF);
    ImU32 col32_viz_t2 = hex_col32(0xD95926FF);
    ImU32 col32_viz_t3 = hex_col32(0x199E70FF);
    ImU32 col32_viz_t4 = hex_col32(0xC98500FF);

    // Scrubbable drag-slider (DaVinci Resolve style) fill bar
    ImU32 col32_scrub_fill = hex_col32(0x2563EB59);
    ImU32 col32_scrub_fill_active = hex_col32(0x2563EB8C);

    // Pin watch floating readout badge & leader
    ImU32 col32_pin_watch_bg = hex_col32(0x161B26EB);
    ImU32 col32_pin_watch_border = hex_col32(0x38BDF8CC);

    // Translucent HUD / overlay pills (viewport toolbar, tracking HUD, orb readout)
    ImU32 col32_hud_bg = hex_col32(0x0B0D10D9);
    ImU32 col32_hud_border = hex_col32(0x2A3040FF);

    // UE5 Blueprint node anatomy (§3.1)
    ImU32 col32_node_body = hex_col32(0x13161CEB);
    ImU32 col32_node_border = hex_col32(0x2E3545FF);
    ImU32 col32_node_border_hover = hex_col32(0x4B5670FF);
    ImU32 col32_node_selected = hex_col32(0xF59E0BFF);
    ImU32 col32_node_header_sep = hex_col32(0x00000066);
    ImU32 col32_node_footer = hex_col32(0x0E1015CC);
    ImU32 col32_node_shadow = hex_col32(0x00000059);
    ImU32 col32_node_bypass_tint = hex_col32(0x14171DB3);
    GradientPair node_hdr_source{hex_col32(0x881B2BFF), hex_col32(0x3A111AFF)}; // Input / sensor sources
    GradientPair node_hdr_solver{hex_col32(0x1D4E89FF), hex_col32(0x112642FF)}; // Solvers / filters / IK
    GradientPair node_hdr_blend{hex_col32(0x0F6B58FF), hex_col32(0x0A332BFF)};  // Blend / math / procedural
    GradientPair node_hdr_output{hex_col32(0x5B2182FF), hex_col32(0x2A113DFF)}; // Output sinks
    GradientPair node_hdr_comment{hex_col32(0x232936FF), hex_col32(0x1A1E27FF)};
    ImU32 col32_comment_body = hex_col32(0x161A2266);
    ImU32 col32_comment_border = hex_col32(0x2E354599);

    // Blueprint wires (§3.3, §3.4)
    ImU32 col32_wire_shadow = hex_col32(0x00000040); // 25% outer glow/shadow spline
    ImU32 col32_wire_selected = hex_col32(0xFBBF24FF);
    ImU32 col32_wire_pulse = hex_col32(0xFFFFFFB3);
    ImU32 col32_wire_cut = hex_col32(0xEF4444FF);

    // Pin data-type colors (§3.2) indexed by `PinDataKind`:
    // 0:Float 1:Bool 2:Vec3 3:Pose 4:Expression 5:Viseme 6:Event 7:AudioFrame
    std::array<ImU32, 8> col32_pin_types{
        hex_col32(0x38BDF8FF), // Float / Confidence: cyan
        hex_col32(0xFB7185FF), // Bool: rose
        hex_col32(0xFB923CFF), // Vec3 / direction: orange
        hex_col32(0xF472B6FF), // PoseStream: pink
        hex_col32(0xFACC15FF), // Blendshape52 expressions: gold
        hex_col32(0xEAB308FF), // Viseme5 lip-sync weights: amber
        hex_col32(0xE2E8F0FF), // Event / trigger exec: white
        hex_col32(0xA855F7FF), // AudioFrame DSP envelope & formants: purple
    };
    ImU32 col32_pin_hollow_fill = hex_col32(0x0B0D10FF);

    // 2.5D Light Orbit Gizmo dark vector sphere (§1 BUG-07)
    ImU32 col32_orb_fill = hex_col32(0x141820FF);
    ImU32 col32_orb_fill_edge = hex_col32(0x0C0F14FF);
    ImU32 col32_orb_ring = hex_col32(0x2E3646FF);
    ImU32 col32_orb_rim = hex_col32(0x8FA3C759);
    ImU32 col32_orb_specular = hex_col32(0xC8D4EA2E);
    ImU32 col32_orb_key = hex_col32(0xF59E0BFF);
    ImU32 col32_orb_fill_light = hex_col32(0x38BDF8FF);
    ImU32 col32_orb_rim_light = hex_col32(0x10B981FF);

    // Viewport aspect matte & broadcast safe-frame
    ImU32 col32_vp_aspect_matte = hex_col32(0x0B0D12FF);
    ImU32 col32_vp_safe_frame = hex_col32(0xFFFFFF40);

    // Live2D parameter monitor bar palette
    ImU32 col32_l2d_angle = hex_col32(0x52A8FFD2);
    ImU32 col32_l2d_eye = hex_col32(0x48E0E8D2);
    ImU32 col32_l2d_brow = hex_col32(0xFACE48D2);
    ImU32 col32_l2d_mouth = hex_col32(0xFF6EC4D2);
    ImU32 col32_l2d_vowel = hex_col32(0x78E898DC);

    // Viewport live webcam tracking & hand skeleton overlay
    ImU32 col32_hud_roi_calibrated = hex_col32(0x44E080EB);
    ImU32 col32_hud_roi_locked = hex_col32(0xB4E048E6);
    ImU32 col32_hud_roi_searching = hex_col32(0xEBA83CC8);
    ImU32 col32_hud_kp_jaw = hex_col32(0x48EB8AD2);
    ImU32 col32_hud_kp_nose = hex_col32(0x60E19CC3);
    ImU32 col32_hud_kp_brow = hex_col32(0xFFD84CEB);
    ImU32 col32_hud_kp_brow_curve = hex_col32(0xFFE05CF5);
    ImU32 col32_hud_kp_eye = hex_col32(0x50EBFFEB);
    ImU32 col32_hud_kp_iris = hex_col32(0xD2FEFFF5);
    ImU32 col32_hud_kp_gaze = hex_col32(0xFFFF82F0);
    ImU32 col32_hud_kp_lip_outer = hex_col32(0xFF66CCEB);
    ImU32 col32_hud_kp_lip_inner = hex_col32(0xFF9CE0DC);
    ImU32 col32_hud_kp_generic = hex_col32(0x5CECFFE1);
    ImU32 col32_hud_axis_x = hex_col32(0xFF5252F0);
    ImU32 col32_hud_axis_y = hex_col32(0x52EB70F0);
    ImU32 col32_hud_axis_z = hex_col32(0x52A8FFF0);
    ImU32 col32_hud_hand_left = hex_col32(0x38BDF8F0);
    ImU32 col32_hud_hand_right = hex_col32(0xF472B6F0);
    ImU32 col32_hud_hand_joint = hex_col32(0xFFFFFFEB);
    ImU32 col32_hud_hand_normal = hex_col32(0xFFEB6EF5);

    // Typography sizes (§2.2 asks for Inter 13.5 px / JetBrains Mono 12.5 px). The FreeType builder
    // rasterizes whole pixel sizes only (13.5 silently renders at 13), so the tokens state the sizes
    // actually rendered; `font_size / type_body_px` is then the exact UI scale used by layout code.
    float type_caption_px{11.0f};
    float type_body_px{13.0f};
    float type_title_px{14.0f};
    float type_display_px{20.0f};
    float type_mono_px{12.0f};

    // Spacing, sizing & radii (§2.2)
    float unit_px{4.0f};
    float panel_pad_px{10.0f};
    float frame_pad_x_px{8.0f};
    float frame_pad_y_px{5.0f};
    float item_spacing_x_px{8.0f};
    float item_spacing_y_px{6.0f};
    float control_h_compact_px{24.0f};
    float control_h_comfy_px{28.0f};
    float property_row_h_px{28.0f};
    float property_label_col_px{104.0f};
    float property_label_frac{0.38f};
    float menu_bar_h_px{34.0f};
    float status_bar_h_px{26.0f};
    float toast_w_px{320.0f};
    float palette_w_px{560.0f};
    float wizard_w_px{780.0f};
    float wizard_h_px{526.0f};
    float orbit_orb_px{124.0f};
    float left_dock_w_px{275.0f};
    float left_dock_min_w_px{240.0f};
    // 335 (spec: 325): the longest combo value ("Ray Query Shadow Mask") needs ~10 px more so no
    // Inspector label or combo value is clipped at 100-175% DPI (verified by studio_ui_test).
    float right_dock_w_px{335.0f};
    float right_dock_min_w_px{290.0f};
    float scene_well_h_px{190.0f};
    float viewport_toolbar_h_px{28.0f};
    float radius_panel_px{0.0f};
    float radius_control_px{3.0f};
    float radius_child_px{4.0f};
    float radius_popover_px{5.0f};
    float radius_modal_px{6.0f};
    float radius_node_px{7.0f};
    float scrollbar_px{8.0f};
    float splitter_px{2.0f};

    // UE5 Blueprint node geometry (world units, scaled by zoom * dpi)
    float node_header_h{26.0f};
    float node_row_h{24.0f};
    float node_footer_h{18.0f};
    float node_pin_inset{12.0f};
    float node_label_inset{24.0f};
    float node_inline_box_w{48.0f};
    float node_min_w{168.0f};
    float node_pin_r{5.0f};
    float node_pin_hit_r{11.0f};
    float node_snap_r{14.0f};
    float wire_hit_r{6.5f};
    float wire_thickness{2.6f};
    float wire_thickness_hot{3.8f};
    float wire_shadow_thickness{5.0f};
    float wire_detach_threshold{4.0f};
};

[[nodiscard]] inline PrecisionDarkTokens build_tokens(bool high_contrast) noexcept {
    PrecisionDarkTokens t{};
    if (high_contrast) {
        // §12.14 high-contrast accessibility overrides
        t.text_secondary = hex_rgba(0xC9CED6FF);
        t.col32_text_secondary = hex_col32(0xC9CED6FF);
        t.text_tertiary = hex_rgba(0xAEB6C4FF);
        t.col32_text_tertiary = hex_col32(0xAEB6C4FF);
        t.border_default = hex_rgba(0x4A5262FF);
        t.col32_border_default = hex_col32(0x4A5262FF);
        t.border_subtle = hex_rgba(0x363D4BFF);
        t.col32_border_subtle = hex_col32(0x363D4BFF);
        t.border_strong = hex_rgba(0x697489FF);
        t.col32_border_strong = hex_col32(0x697489FF);
        t.col32_node_border = hex_col32(0x4A5262FF);
        t.col32_orb_ring = hex_col32(0x4A5262FF);
    }
    return t;
}

inline PrecisionDarkTokens& mutable_active_tokens() noexcept {
    static PrecisionDarkTokens instance = build_tokens(false);
    return instance;
}

inline bool& high_contrast_flag_storage() noexcept {
    static bool enabled = false;
    return enabled;
}

inline void set_high_contrast_theme(bool enabled) noexcept {
    high_contrast_flag_storage() = enabled;
    mutable_active_tokens() = build_tokens(enabled);
}

[[nodiscard]] inline bool is_high_contrast_theme() noexcept {
    return high_contrast_flag_storage();
}

[[nodiscard]] inline const PrecisionDarkTokens& tokens() noexcept {
    return mutable_active_tokens();
}

} // namespace aether::ui
