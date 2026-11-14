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

/**
 * Single authoritative source of Precision Dark colors, typography, spacing,
 * and corner radii (`AETHER-TS-001` §12.3, §12.14, `REQ-UI-001`).
 */
struct PrecisionDarkTokens {
    // Surfaces (§12.3.1)
    ImVec4 bg_app = hex_rgba(0x111318FF);
    ImVec4 bg_panel = hex_rgba(0x181B22FF);
    ImVec4 bg_raised = hex_rgba(0x1F232CFF);
    ImVec4 bg_overlay = hex_rgba(0x252A34FF);
    ImVec4 bg_inset = hex_rgba(0x0F1115FF);
    ImVec4 bg_hover = hex_rgba(0x232834FF);
    ImVec4 bg_selected = hex_rgba(0x1B2A45FF);
    ImVec4 bg_canvas = hex_rgba(0x13151AFF);

    // Borders
    ImVec4 border_default = hex_rgba(0x262B36FF);
    ImVec4 border_subtle = hex_rgba(0x1F232CFF);
    ImVec4 border_strong = hex_rgba(0x333A48FF);

    // Text
    ImVec4 text_primary = hex_rgba(0xE3E6EBFF);
    ImVec4 text_secondary = hex_rgba(0xA0A7B4FF);
    ImVec4 text_tertiary = hex_rgba(0x7D8494FF);
    ImVec4 text_disabled = hex_rgba(0x4B515DFF);
    ImVec4 text_on_accent = hex_rgba(0xFFFFFFFF);

    // Accent
    ImVec4 accent = hex_rgba(0x3B82F6FF);
    ImVec4 accent_hover = hex_rgba(0x5B96F7FF);
    ImVec4 accent_pressed = hex_rgba(0x2D6FDBFF);
    ImVec4 accent_button = hex_rgba(0x2563EBFF);

    // Semantics
    ImVec4 status_ok = hex_rgba(0x10B981FF);
    ImVec4 status_warn = hex_rgba(0xF5A524FF);
    ImVec4 status_error = hex_rgba(0xF04438FF);
    ImVec4 status_live = hex_rgba(0xE5484DFF);

    // Axis
    ImVec4 axis_x = hex_rgba(0xE5534BFF);
    ImVec4 axis_y = hex_rgba(0x57AB5AFF);
    ImVec4 axis_z = hex_rgba(0x4A8FE7FF);

    // Viz series
    ImVec4 viz_t1 = hex_rgba(0x3987E5FF);
    ImVec4 viz_t2 = hex_rgba(0xD95926FF);
    ImVec4 viz_t3 = hex_rgba(0x199E70FF);
    ImVec4 viz_t4 = hex_rgba(0xC98500FF);

    // Viewport checker + scrim + scrollbar + elevation shadow
    ImVec4 checker_a = hex_rgba(0x15171CFF);
    ImVec4 checker_b = hex_rgba(0x1A1D23FF);
    ImVec4 scrim_modal = hex_rgba(0x08090C99);
    ImVec4 shadow_soft = hex_rgba(0x05060A50);
    ImVec4 transparent = hex_rgba(0x00000000);
    ImVec4 docking_preview = hex_rgba(0x3B82F659);
    ImVec4 scrollbar_thumb = hex_rgba(0x2E3440FF);
    ImVec4 scrollbar_hover = hex_rgba(0x3A4150FF);

    // Packed ImU32 mirrors for DrawList calls cleanly without color literals in panels
    ImU32 col32_bg_app = hex_col32(0x111318FF);
    ImU32 col32_bg_panel = hex_col32(0x181B22FF);
    ImU32 col32_bg_raised = hex_col32(0x1F232CFF);
    ImU32 col32_bg_overlay = hex_col32(0x252A34FF);
    ImU32 col32_bg_inset = hex_col32(0x0F1115FF);
    ImU32 col32_bg_hover = hex_col32(0x232834FF);
    ImU32 col32_bg_selected = hex_col32(0x1B2A45FF);
    ImU32 col32_bg_canvas = hex_col32(0x13151AFF);
    ImU32 col32_grid_minor = hex_col32(0x1B1F27FF);
    ImU32 col32_grid_major = hex_col32(0x252A36FF);
    ImU32 col32_border_default = hex_col32(0x262B36FF);
    ImU32 col32_border_subtle = hex_col32(0x1F232CFF);
    ImU32 col32_border_strong = hex_col32(0x333A48FF);
    ImU32 col32_text_primary = hex_col32(0xE3E6EBFF);
    ImU32 col32_text_secondary = hex_col32(0xA0A7B4FF);
    ImU32 col32_text_tertiary = hex_col32(0x7D8494FF);
    ImU32 col32_text_disabled = hex_col32(0x4B515DFF);
    ImU32 col32_text_on_accent = hex_col32(0xFFFFFFFF);
    ImU32 col32_accent = hex_col32(0x3B82F6FF);
    ImU32 col32_accent_hover = hex_col32(0x5B96F7FF);
    ImU32 col32_accent_pressed = hex_col32(0x2D6FDBFF);
    ImU32 col32_status_ok = hex_col32(0x10B981FF);
    ImU32 col32_status_warn = hex_col32(0xF5A524FF);
    ImU32 col32_status_error = hex_col32(0xF04438FF);
    ImU32 col32_status_live = hex_col32(0xE5484DFF);
    ImU32 col32_axis_x = hex_col32(0xE5534BFF);
    ImU32 col32_axis_y = hex_col32(0x57AB5AFF);
    ImU32 col32_axis_z = hex_col32(0x4A8FE7FF);
    ImU32 col32_checker_a = hex_col32(0x15171CFF);
    ImU32 col32_checker_b = hex_col32(0x1A1D23FF);
    ImU32 col32_shadow_soft = hex_col32(0x05060A24);
    ImU32 col32_viz_t1 = hex_col32(0x3987E5FF);
    ImU32 col32_viz_t2 = hex_col32(0xD95926FF);
    ImU32 col32_viz_t3 = hex_col32(0x199E70FF);
    ImU32 col32_viz_t4 = hex_col32(0xC98500FF);

    // 8-slot Colorblind-safe (CVD) categorical node pin palette (§12.7)
    // 0:Float, 1:Bool, 2:Vec2/3, 3:Pose, 4:Expression, 5:Light, 6:Trigger, 7:Event/String
    std::array<ImU32, 8> col32_pin_types{
        hex_col32(0x56B4E9FF), // Sky Blue
        hex_col32(0xE69F00FF), // Orange
        hex_col32(0x009E73FF), // Bluish Green
        hex_col32(0xCC79A7FF), // Reddish Purple
        hex_col32(0xF0E442FF), // Yellow
        hex_col32(0x3B82F6FF), // Accent Blue
        hex_col32(0xD55E00FF), // Vermilion
        hex_col32(0xA0A7B4FF), // Neutral Slate
    };

    // Typography sizes (§12.3.2)
    float type_caption_px{11.0f};
    float type_body_px{12.0f};
    float type_title_px{13.0f};
    float type_display_px{20.0f};
    float type_mono_px{11.0f};

    // Spacing, sizing & radii (§12.3.3, §12.5)
    float unit_px{4.0f};
    float panel_pad_px{8.0f};
    float control_h_compact_px{22.0f};
    float control_h_comfy_px{26.0f};
    float property_row_h_px{26.0f};
    float property_label_col_px{104.0f};
    float menu_bar_h_px{32.0f};
    float status_bar_h_px{24.0f};
    float toast_w_px{320.0f};
    float palette_w_px{560.0f};
    float wizard_w_px{780.0f};
    float wizard_h_px{526.0f};
    float orbit_orb_px{112.0f};
    float radius_panel_px{0.0f};
    float radius_control_px{3.0f};
    float radius_popover_px{4.0f};
    float radius_modal_px{6.0f};
    float scrollbar_px{8.0f};
    float splitter_px{2.0f};
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
