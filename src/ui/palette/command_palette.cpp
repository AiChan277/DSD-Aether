#include "ui/palette/command_palette.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace aether::ui {
namespace {

template <size_t N> void copy_cstr(std::array<char, N>& dst, const char* src) noexcept {
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst.data(), dst.size(), "%s", src);
}

const char* category_name(PaletteCategory c) noexcept {
    switch (c) {
    case PaletteCategory::Actions:
        return tr("Actions");
    case PaletteCategory::PanelsAndSettings:
        return tr("Panels & Settings");
    case PaletteCategory::Nodes:
        return tr("Nodes");
    case PaletteCategory::Files:
        return tr("Files");
    }
    return tr("Actions");
}

struct NodeSpec {
    const char* type_id;
    const char* label;
};

constexpr std::array<NodeSpec, 20> kBuiltinNodePaletteSpecs = {{
    {"aether.source.npu_pose@1", "Add Node: NPU Pose In"},
    {"aether.source.dsp_audio@1", "Add Node: DSP Audio In"},
    {"aether.source.clock@1", "Add Node: Studio Clock"},
    {"aether.source.osc_in@1", "Add Node: OSC In"},
    {"aether.ik.visibility_first@1", "Add Node: Visibility-First IK"},
    {"aether.solver.look_at@1", "Add Node: LookAt Solver"},
    {"aether.solver.idle_breath@1", "Add Node: Idle Breath"},
    {"aether.expr.formant_lipsync@1", "Add Node: Formant Lip-Sync"},
    {"aether.expr.mixer@1", "Add Node: Expression Mixer"},
    {"aether.expr.auto_blink@1", "Add Node: Auto Blink"},
    {"aether.math.add@1", "Add Node: Add (Float)"},
    {"aether.math.multiply@1", "Add Node: Multiply (Float)"},
    {"aether.math.lerp@1", "Add Node: Lerp"},
    {"aether.math.clamp@1", "Add Node: Clamp"},
    {"aether.math.smooth@1", "Add Node: 1-Euro / Smooth"},
    {"aether.math.spring@1", "Add Node: Scalar Spring"},
    {"aether.output.pose@1", "Add Node: Output Pose"},
    {"aether.output.expressions@1", "Add Node: Output Expressions"},
    {"aether.output.camera_control@1", "Add Node: Camera Control"},
    {"aether.output.light_control@1", "Add Node: Light Control"},
}};

bool is_action_blocked_on_air(PaletteCommandTag tag) noexcept {
    switch (tag) {
    case PaletteCommandTag::ProjectNew:
    case PaletteCommandTag::ProjectOpen:
    case PaletteCommandTag::OpenRecentFile:
    case PaletteCommandTag::OpenWizard:
    case PaletteCommandTag::ForceDeviceLost:
        return true;
    default:
        return false;
    }
}

void draw_highlighted_label(ImDrawList* dl, ImVec2 pos, const char* text, const PaletteMatch& match,
                            const UiFontSet& fonts, const PrecisionDarkTokens& t) noexcept {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    const size_t len = std::strlen(text);
    size_t match_cursor = 0;
    float cur_x = pos.x;
    char ch_buf[2]{'\0', '\0'};

    ImFont* reg_font = fonts.body != nullptr ? fonts.body : ImGui::GetFont();
    ImFont* bold_font = fonts.body_strong != nullptr ? fonts.body_strong : reg_font;
    const float font_size = ImGui::GetFontSize();

    for (size_t i = 0; i < len; ++i) {
        bool is_hit = false;
        while (match_cursor < match.match_count && match.match_positions[match_cursor] < i) {
            ++match_cursor;
        }
        if (match_cursor < match.match_count && match.match_positions[match_cursor] == i) {
            is_hit = true;
            ++match_cursor;
        }
        ch_buf[0] = text[i];
        ImFont* f = is_hit ? bold_font : reg_font;
        const ImU32 col = is_hit ? t.col32_text_primary : t.col32_text_secondary;
        dl->AddText(f, font_size, ImVec2(cur_x, pos.y), col, ch_buf, ch_buf + 1);
        cur_x += f->CalcTextSizeA(font_size, 1000.0f, -1.0f, ch_buf, ch_buf + 1).x;
    }
}

} // namespace

CommandPalette::CommandPalette() {
    entries_.reserve(128);
    filtered_.reserve(128);
}

void CommandPalette::open() noexcept {
    is_open_ = true;
    focus_input_next_frame_ = true;
    query_buf_[0] = '\0';
    last_query_[0] = '\x01'; // force refilter on first draw
    selected_row_ = 0;
    active_category_filter_ = 255;
}

int32_t CommandPalette::fuzzy_score(const char* query, const char* text, std::array<uint8_t, 48>& out_positions,
                                    uint8_t& out_count) noexcept {
    out_count = 0;
    if (query == nullptr || query[0] == '\0') {
        return 1;
    }
    if (text == nullptr || text[0] == '\0') {
        return -1;
    }

    size_t qi = 0;
    size_t ti = 0;
    int32_t score = 0;
    int32_t consecutive = 0;
    const size_t qlen = std::strlen(query);
    const size_t tlen = std::strlen(text);

    while (qi < qlen && ti < tlen) {
        const char qc = static_cast<char>(std::tolower(static_cast<unsigned char>(query[qi])));
        if (qc == ' ') {
            ++qi;
            continue;
        }
        const char tc = static_cast<char>(std::tolower(static_cast<unsigned char>(text[ti])));
        if (qc == tc) {
            int32_t bonus = 10;
            if (ti == 0) {
                bonus += 24;
            } else {
                const char prev = text[ti - 1];
                if (prev == ' ' || prev == ':' || prev == '-' || prev == '_' || prev == '/') {
                    bonus += 18;
                } else if (std::isupper(static_cast<unsigned char>(text[ti])) != 0) {
                    bonus += 12;
                }
            }
            if (consecutive > 0) {
                bonus += consecutive * 14;
            }
            score += bonus;
            ++consecutive;
            if (out_count < out_positions.size() && ti <= 255U) {
                out_positions[out_count++] = static_cast<uint8_t>(ti);
            }
            ++qi;
        } else {
            consecutive = 0;
            score -= 1;
        }
        ++ti;
    }

    if (qi < qlen) {
        while (qi < qlen && query[qi] == ' ') {
            ++qi;
        }
        if (qi < qlen) {
            return -1;
        }
    }
    return score;
}

void CommandPalette::rebuild_entries(const KeymapRegistry& keymap, std::span<const std::string> recent_files) {
    // Preserve MRU counts across rebuilds cleanly
    struct MruKeep {
        std::array<char, 64> id{};
        uint32_t uses{0};
    };
    std::vector<MruKeep> prev_mru;
    prev_mru.reserve(entries_.size());
    for (const auto& e : entries_) {
        if (e.mru_uses > 0) {
            MruKeep m{};
            m.id = e.id;
            m.uses = e.mru_uses;
            prev_mru.push_back(m);
        }
    }

    entries_.clear();
    auto add = [&](PaletteCategory cat, PaletteCommandTag tag, const char* id, const char* label, const char* shortcut,
                   const char* payload = nullptr) {
        PaletteEntry e{};
        e.category = cat;
        e.tag = tag;
        copy_cstr(e.id, id);
        copy_cstr(e.label, label);
        copy_cstr(e.shortcut, shortcut);
        copy_cstr(e.payload, payload);
        for (const auto& m : prev_mru) {
            if (std::strcmp(m.id.data(), e.id.data()) == 0) {
                e.mru_uses = m.uses;
                break;
            }
        }
        entries_.push_back(e);
    };

    add(PaletteCategory::Actions, PaletteCommandTag::ToggleOnAir, "act.on_air", "Toggle ON AIR Lock",
        keymap.chord(ActionId::ToggleOnAir).c_str());
    add(PaletteCategory::Actions, PaletteCommandTag::PauseTracking, "act.pause_trk", "Pause / Resume Tracking",
        keymap.chord(ActionId::PauseTracking).c_str());
    add(PaletteCategory::Actions, PaletteCommandTag::CalibrateNeutral, "act.calibrate", "Calibrate Neutral Face Pose",
        keymap.chord(ActionId::CalibrateNeutral).c_str());
    add(PaletteCategory::Actions, PaletteCommandTag::ToggleRayTracing, "act.rt", "Toggle Ray-Traced Shadows & Bounce",
        keymap.chord(ActionId::ToggleRayTracing).c_str());
    add(PaletteCategory::Actions, PaletteCommandTag::ToggleHeatmap, "act.heatmap", "Toggle Joint Confidence Heatmap",
        keymap.chord(ActionId::ViewportHeatmap).c_str());
    add(PaletteCategory::Actions, PaletteCommandTag::ToggleOrbitOrb, "act.orb", "Toggle Light Orbit Orb",
        keymap.chord(ActionId::ViewportOrbitOrb).c_str());
    add(PaletteCategory::Actions, PaletteCommandTag::ToggleCheckerboard, "act.checker", "Toggle Alpha Checkerboard",
        "");
    add(PaletteCategory::Actions, PaletteCommandTag::Undo, "act.undo", "Undo Last Action",
        keymap.chord(ActionId::Undo).c_str());
    add(PaletteCategory::Actions, PaletteCommandTag::Redo, "act.redo", "Redo Action",
        keymap.chord(ActionId::Redo).c_str());
    add(PaletteCategory::Actions, PaletteCommandTag::ReloadPlugins, "act.reload_plugins",
        "Reload Node & Plugin Registry", "");
    add(PaletteCategory::Actions, PaletteCommandTag::ToggleWsControl, "act.ws_ctrl", "Toggle WebSocket Control Server",
        "");
    add(PaletteCategory::Actions, PaletteCommandTag::ForceDeviceLost, "act.dev_lost",
        "Diagnostics: Simulate GPU Device Lost", "");

    add(PaletteCategory::PanelsAndSettings, PaletteCommandTag::ModeLiveStudio, "mode.live", "Switch Mode: Live Studio",
        keymap.chord(ActionId::ModeLiveStudio).c_str());
    add(PaletteCategory::PanelsAndSettings, PaletteCommandTag::ModeRigging, "mode.rig", "Switch Mode: Rigging",
        keymap.chord(ActionId::ModeRigging).c_str());
    add(PaletteCategory::PanelsAndSettings, PaletteCommandTag::ModeNodes, "mode.nodes", "Switch Mode: Nodes",
        keymap.chord(ActionId::ModeNodes).c_str());
    add(PaletteCategory::PanelsAndSettings, PaletteCommandTag::ResetLayout, "panel.reset_layout",
        "Reset Workspace Dock Layout", "");
    add(PaletteCategory::PanelsAndSettings, PaletteCommandTag::OpenWizard, "panel.wizard",
        "Open Hardware Optimizer Wizard...", "");
    add(PaletteCategory::PanelsAndSettings, PaletteCommandTag::OpenPreferences, "panel.prefs",
        "Open Studio Preferences & Keymap...", keymap.chord(ActionId::Preferences).c_str());
    add(PaletteCategory::PanelsAndSettings, PaletteCommandTag::OpenShortcuts, "panel.keys", "Show Keyboard Shortcuts",
        keymap.chord(ActionId::ShortcutOverlay).c_str());

    for (const auto& n : kBuiltinNodePaletteSpecs) {
        add(PaletteCategory::Nodes, PaletteCommandTag::AddNodeByType, n.type_id, n.label, "", n.type_id);
    }

    add(PaletteCategory::Files, PaletteCommandTag::ProjectNew, "file.new", "File: New Studio Project",
        keymap.chord(ActionId::ProjectNew).c_str());
    add(PaletteCategory::Files, PaletteCommandTag::ProjectOpen, "file.open", "File: Open Project (.aether / .json)...",
        keymap.chord(ActionId::ProjectOpen).c_str());
    add(PaletteCategory::Files, PaletteCommandTag::ProjectSave, "file.save", "File: Save Project",
        keymap.chord(ActionId::ProjectSave).c_str());
    add(PaletteCategory::Files, PaletteCommandTag::ProjectSaveAs, "file.save_as", "File: Save Project As...",
        keymap.chord(ActionId::ProjectSaveAs).c_str());
    add(PaletteCategory::Files, PaletteCommandTag::ImportAvatar, "file.import_avatar",
        "File: Load Avatar (.vrm / .fbx / .pmx)...", "");

    for (size_t i = 0; i < recent_files.size() && i < 6; ++i) {
        std::array<char, 40> id{};
        std::array<char, 80> lbl{};
        std::snprintf(id.data(), id.size(), "recent.%zu", i);
        std::snprintf(lbl.data(), lbl.size(), "Recent: %s", recent_files[i].c_str());
        add(PaletteCategory::Files, PaletteCommandTag::OpenRecentFile, id.data(), lbl.data(), "",
            recent_files[i].c_str());
    }

    last_query_[0] = '\x01';
}

void CommandPalette::refilter() {
    filtered_.clear();
    for (uint16_t i = 0; i < static_cast<uint16_t>(entries_.size()); ++i) {
        const auto& e = entries_[i];
        if (active_category_filter_ != 255 && static_cast<uint8_t>(e.category) != active_category_filter_) {
            continue;
        }
        PaletteMatch m{};
        m.entry_idx = i;
        const int32_t s = fuzzy_score(query_buf_.data(), e.label.data(), m.match_positions, m.match_count);
        if (s < 0) {
            continue;
        }
        m.score = s + static_cast<int32_t>(std::min<uint32_t>(e.mru_uses, 10u) * 8u);
        filtered_.push_back(m);
    }

    std::stable_sort(filtered_.begin(), filtered_.end(), [&](const PaletteMatch& a, const PaletteMatch& b) {
        if (query_buf_[0] == '\0') {
            if (entries_[a.entry_idx].mru_uses != entries_[b.entry_idx].mru_uses) {
                return entries_[a.entry_idx].mru_uses > entries_[b.entry_idx].mru_uses;
            }
            if (entries_[a.entry_idx].category != entries_[b.entry_idx].category) {
                return static_cast<uint8_t>(entries_[a.entry_idx].category) <
                       static_cast<uint8_t>(entries_[b.entry_idx].category);
            }
            return a.entry_idx < b.entry_idx;
        }
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.entry_idx < b.entry_idx;
    });

    if (filtered_.empty()) {
        selected_row_ = 0;
    } else {
        selected_row_ = std::clamp(selected_row_, 0, static_cast<int32_t>(filtered_.size()) - 1);
    }
    last_query_ = query_buf_;
}

PaletteInvokeResult CommandPalette::draw(const KeymapRegistry& /*keymap*/, bool on_air, const UiFontSet& fonts,
                                         float dpi_scale) {
    PaletteInvokeResult result{};
    if (!is_open_) {
        return result;
    }

    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);
    ImGuiViewport* vp = ImGui::GetMainViewport();

    if (std::strcmp(last_query_.data(), query_buf_.data()) != 0) {
        refilter();
    }

    // Dim backdrop scrim
    ImDrawList* bg_dl = ImGui::GetBackgroundDrawList(vp);
    bg_dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                         ImGui::ColorConvertFloat4ToU32(t.scrim_modal));

    const float pal_w = std::min(t.palette_w_px * scale, vp->Size.x - 32.0f * scale);
    const float pos_x = vp->Pos.x + (vp->Size.x - pal_w) * 0.5f;
    const float pos_y = vp->Pos.y + vp->Size.y * 0.18f;

    ImGui::SetNextWindowPos(ImVec2(pos_x, pos_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pal_w, 0.0f), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, t.bg_overlay);
    ImGui::PushStyleColor(ImGuiCol_Border, t.border_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.radius_modal_px);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, hairline_px(scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    constexpr ImGuiWindowFlags kPalFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoDocking;

    if (ImGui::Begin("##CommandPalettePopup", nullptr, kPalFlags)) {
        const ImVec2 win_min = ImGui::GetWindowPos();
        const ImVec2 win_max = ImVec2(win_min.x + ImGui::GetWindowWidth(), win_min.y + ImGui::GetWindowHeight());
        draw_elevation_shadow(ImGui::GetBackgroundDrawList(vp), win_min, win_max, t.radius_modal_px);

        // Input row (44 px tall §12.10)
        ImGui::SetCursorPos(ImVec2(12.0f * scale, 8.0f * scale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, t.bg_inset);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * scale, 10.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, t.radius_control_px);
        if (focus_input_next_frame_) {
            ImGui::SetKeyboardFocusHere();
            focus_input_next_frame_ = false;
        }
        ImGui::SetNextItemWidth(pal_w - 24.0f * scale);
        if (ImGui::InputTextWithHint("##PaletteQuery", tr("Type a command, panel, node, or file..."), query_buf_.data(),
                                     query_buf_.size())) {
            selected_row_ = 0;
            refilter();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // Filter category strip (Tab cycles scope cleanly)
        if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
            if (active_category_filter_ == 255) {
                active_category_filter_ = 0;
            } else if (active_category_filter_ < 3) {
                ++active_category_filter_;
            } else {
                active_category_filter_ = 255;
            }
            selected_row_ = 0;
            refilter();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            close();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !filtered_.empty()) {
            selected_row_ = (selected_row_ + 1) % static_cast<int32_t>(filtered_.size());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !filtered_.empty()) {
            const int32_t n = static_cast<int32_t>(filtered_.size());
            selected_row_ = (selected_row_ - 1 + n) % n;
        }

        // Category pills
        ImGui::SetCursorPosX(12.0f * scale);
        if (fonts.caption != nullptr) {
            ImGui::PushFont(fonts.caption);
        }
        auto draw_cat_pill = [&](uint8_t cat_id, const char* label) {
            const bool active = active_category_filter_ == cat_id;
            ImGui::PushStyleColor(ImGuiCol_Button, active ? t.bg_selected : t.bg_raised);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.bg_hover);
            if (ImGui::SmallButton(label)) {
                active_category_filter_ = cat_id;
                selected_row_ = 0;
                refilter();
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine(0.0f, 4.0f * scale);
        };
        draw_cat_pill(255, tr("All"));
        draw_cat_pill(0, tr("Actions"));
        draw_cat_pill(1, tr("Panels"));
        draw_cat_pill(2, tr("Nodes"));
        draw_cat_pill(3, tr("Files"));
        ImGui::NewLine();
        if (fonts.caption != nullptr) {
            ImGui::PopFont();
        }

        ImGui::Separator();

        bool enter_invoke = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
        const float list_h =
            std::min(340.0f * scale,
                     std::max(60.0f * scale, static_cast<float>(filtered_.size()) * 28.0f * scale + 12.0f * scale));
        if (ImGui::BeginChild("##PaletteResults", ImVec2(pal_w, list_h), ImGuiChildFlags_None)) {
            if (filtered_.empty()) {
                ImGui::SetCursorPos(ImVec2(14.0f * scale, 14.0f * scale));
                ImGui::TextColored(t.text_tertiary, "%s", tr("No matching command or node."));
            } else {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(filtered_.size()), 28.0f * scale);
                while (clipper.Step()) {
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                        const auto& match = filtered_[static_cast<size_t>(row)];
                        const auto& entry = entries_[match.entry_idx];
                        const bool blocked = on_air && is_action_blocked_on_air(entry.tag);
                        const bool is_selected = (row == selected_row_);

                        ImGui::PushID(row);
                        const ImVec2 row_min = ImGui::GetCursorScreenPos();
                        const float row_h = 28.0f * scale;
                        if (ImGui::Selectable("##PalRow", is_selected, ImGuiSelectableFlags_None,
                                              ImVec2(pal_w, row_h))) {
                            if (!blocked) {
                                selected_row_ = row;
                                enter_invoke = true;
                            }
                        }
                        const bool hovered = ImGui::IsItemHovered();
                        if (hovered) {
                            selected_row_ = row;
                        }

                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        if (is_selected) {
                            dl->AddRectFilled(row_min, ImVec2(row_min.x + pal_w, row_min.y + row_h),
                                              t.col32_bg_selected);
                            dl->AddRectFilled(row_min, ImVec2(row_min.x + 2.0f * scale, row_min.y + row_h),
                                              t.col32_accent);
                        }

                        // Category tag on left + highlighted match text + shortcut pill on right
                        const char* cat_str = category_name(entry.category);
                        if (fonts.caption != nullptr) {
                            ImGui::PushFont(fonts.caption);
                        }
                        dl->AddText(ImVec2(row_min.x + 12.0f * scale, row_min.y + 7.0f * scale),
                                    blocked ? t.col32_text_disabled : t.col32_text_tertiary, cat_str);
                        if (fonts.caption != nullptr) {
                            ImGui::PopFont();
                        }

                        const ImVec2 label_pos(row_min.x + 124.0f * scale, row_min.y + 6.0f * scale);
                        if (blocked) {
                            dl->AddText(label_pos, t.col32_text_disabled, entry.label.data());
                        } else {
                            draw_highlighted_label(dl, label_pos, entry.label.data(), match, fonts, t);
                        }

                        if (entry.shortcut[0] != '\0') {
                            const ImVec2 sc_sz = ImGui::CalcTextSize(entry.shortcut.data());
                            const float sc_x = row_min.x + pal_w - sc_sz.x - 14.0f * scale;
                            dl->AddText(ImVec2(sc_x, row_min.y + 6.0f * scale), t.col32_text_tertiary,
                                        entry.shortcut.data());
                        }
                        ImGui::PopID();
                    }
                }
            }
        }
        ImGui::EndChild();

        if (enter_invoke && !filtered_.empty() && selected_row_ >= 0 &&
            selected_row_ < static_cast<int32_t>(filtered_.size())) {
            auto& entry = entries_[filtered_[static_cast<size_t>(selected_row_)].entry_idx];
            if (!(on_air && is_action_blocked_on_air(entry.tag))) {
                ++entry.mru_uses;
                result.invoked = true;
                result.tag = entry.tag;
                result.payload = entry.payload;
                close();
            }
        }

        // Click outside to close cleanly
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
            close();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    return result;
}

void PreferencesModal::open(int initial_tab) noexcept {
    open_ = true;
    active_tab_ = initial_tab;
    editing_action_ = -1;
    token_buf_init_ = false;
}

void PreferencesModal::draw(bool on_air, ParamStore& params, UndoStack& undo, KeymapRegistry& keymap,
                            AppSettings& settings, const UiFontSet& fonts, float dpi_scale,
                            PropertyRowActions& row_actions, StudioActions& actions, bool& out_settings_dirty) {
    if (!open_) {
        return;
    }

    if (!token_buf_init_) {
        std::snprintf(ws_token_buf_.data(), ws_token_buf_.size(), "%s", settings.ws_auth_token.c_str());
        token_buf_init_ = true;
    }

    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);
    ImGuiViewport* vp = ImGui::GetMainViewport();

    ImDrawList* bg_dl = ImGui::GetBackgroundDrawList(vp);
    bg_dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                         ImGui::ColorConvertFloat4ToU32(t.scrim_modal));

    const float modal_w = std::min(720.0f * scale, vp->Size.x - 40.0f * scale);
    const float modal_h = std::min(540.0f * scale, vp->Size.y - 40.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(modal_w, modal_h), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, t.bg_overlay);
    ImGui::PushStyleColor(ImGuiCol_Border, t.border_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.radius_modal_px);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * scale, 12.0f * scale));

    bool keep_open = open_;
    if (ImGui::Begin(tr("Studio Preferences"), &keep_open,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        const ImVec2 wmin = ImGui::GetWindowPos();
        draw_elevation_shadow(bg_dl, wmin, ImVec2(wmin.x + modal_w, wmin.y + modal_h), t.radius_modal_px);

        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && editing_action_ < 0) {
            keep_open = false;
        }

        if (ImGui::BeginTabBar("##PrefsTabs")) {
            if (ImGui::BeginTabItem(tr("Engine & Tracking"), nullptr,
                                    active_tab_ == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                active_tab_ = 0;
                const bool changed_a =
                    draw_property_row(ParamId::TrackingEpMode, params, undo, fonts, row_actions, false, on_air);
                if (changed_a) {
                    const int idx = params.get_int(ParamId::TrackingEpMode);
                    actions.viewport.request_ep_override = tracking_ep_mode_to_router_override(idx);
                    out_settings_dirty = true;
                }
                out_settings_dirty |=
                    draw_property_row(ParamId::TrackingCameraFps, params, undo, fonts, row_actions, false, on_air);
                out_settings_dirty |= draw_property_row(ParamId::TrackingAntiFlicker, params, undo, fonts, row_actions);
                out_settings_dirty |=
                    draw_property_row(ParamId::TrackingExposureLock, params, undo, fonts, row_actions);
                out_settings_dirty |=
                    draw_property_row(ParamId::TrackingSyntheticCamera, params, undo, fonts, row_actions);
                ImGui::Separator();
                if (draw_property_row(ParamId::RenderDenoiser, params, undo, fonts, row_actions, false, on_air)) {
                    actions.viewport.denoiser_kind = static_cast<uint8_t>(params.get_int(ParamId::RenderDenoiser));
                    actions.viewport.request_denoiser_apply = true;
                    out_settings_dirty = true;
                }
                out_settings_dirty |=
                    draw_property_row(ParamId::OutputSpoutCpuFallback, params, undo, fonts, row_actions);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(tr("Keybindings"), nullptr, active_tab_ == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                active_tab_ = 1;
                const auto conflicts = keymap.detect_conflicts();
                if (!conflicts.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.bg_inset);
                    if (ImGui::BeginChild("##KeymapConflictBanner", ImVec2(-1.0f, 36.0f * scale),
                                          ImGuiChildFlags_Borders)) {
                        const auto& c = conflicts[0];
                        ImGui::TextColored(t.status_error, "%s", icons::kAlertTriangle);
                        ImGui::SameLine();
                        ImGui::Text(tr("Conflict (%zu): '%s' and '%s' share [%s]"), conflicts.size(),
                                    KeymapRegistry::spec(c.first).label, KeymapRegistry::spec(c.second).label,
                                    c.chord.c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton(tr("Reset All"))) {
                            keymap.reset_defaults();
                            out_settings_dirty = true;
                        }
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                } else if (ImGui::SmallButton(tr("Reset All Shortcuts"))) {
                    keymap.reset_defaults();
                    out_settings_dirty = true;
                }

                if (ImGui::BeginTable("##KeymapTable", 4,
                                      ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                                      ImVec2(-1.0f, modal_h - 155.0f * scale))) {
                    ImGui::TableSetupColumn(tr("Scope"), ImGuiTableColumnFlags_WidthFixed, 90.0f * scale);
                    ImGui::TableSetupColumn(tr("Action"), ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn(tr("Chord"), ImGuiTableColumnFlags_WidthFixed, 140.0f * scale);
                    ImGui::TableSetupColumn(tr("Global"), ImGuiTableColumnFlags_WidthFixed, 56.0f * scale);
                    ImGui::TableHeadersRow();

                    const auto& specs = KeymapRegistry::all_specs();
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(specs.size()));
                    while (clipper.Step()) {
                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                            const auto& sp = specs[static_cast<size_t>(row)];
                            const ActionId aid = sp.id;
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextColored(t.text_tertiary, "%s", KeymapRegistry::scope_name(sp.scope));

                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(tr(sp.label));

                            ImGui::TableSetColumnIndex(2);
                            ImGui::PushID(row);
                            if (editing_action_ == row) {
                                ImGui::SetNextItemWidth(132.0f * scale);
                                if (ImGui::InputText("##EditChord", chord_edit_buf_.data(), chord_edit_buf_.size(),
                                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                                    keymap.set_chord(aid, chord_edit_buf_.data());
                                    editing_action_ = -1;
                                    out_settings_dirty = true;
                                }
                            } else {
                                const std::string& ch = keymap.chord(aid);
                                if (ImGui::SmallButton(ch.empty() ? "<Unbound>" : ch.c_str())) {
                                    editing_action_ = row;
                                    std::snprintf(chord_edit_buf_.data(), chord_edit_buf_.size(), "%s", ch.c_str());
                                }
                            }

                            ImGui::TableSetColumnIndex(3);
                            if (sp.global_hotkey_capable) {
                                bool g_en = keymap.global_hotkey_enabled(aid);
                                if (ImGui::Checkbox("##G", &g_en)) {
                                    keymap.set_global_hotkey_enabled(aid, g_en);
                                    out_settings_dirty = true;
                                }
                            } else {
                                ImGui::TextColored(t.text_disabled, "--");
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(tr("Accessibility & Remote"), nullptr,
                                    active_tab_ == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                active_tab_ = 2;
                if (draw_property_row(ParamId::AppUiScalePct, params, undo, fonts, row_actions, false, on_air) ||
                    draw_property_row(ParamId::AppHighContrast, params, undo, fonts, row_actions) ||
                    draw_property_row(ParamId::AppReduceMotion, params, undo, fonts, row_actions)) {
                    actions.request_reapply_theme_or_dpi = true;
                    out_settings_dirty = true;
                }
                ImGui::Separator();
                out_settings_dirty |= draw_property_row(ParamId::OutputOscEnabled, params, undo, fonts, row_actions);
                if (draw_property_row(ParamId::OutputWsEnabled, params, undo, fonts, row_actions)) {
                    actions.request_toggle_ws = true;
                    actions.target_ws_enabled = params.get_bool(ParamId::OutputWsEnabled);
                    out_settings_dirty = true;
                }
                ImGui::TextColored(t.text_secondary, "%s", tr("WebSocket Bearer Token (ws://127.0.0.1:19190/v1)"));
                ImGui::SetNextItemWidth(340.0f * scale);
                if (ImGui::InputText("##WsToken", ws_token_buf_.data(), ws_token_buf_.size())) {
                    settings.ws_auth_token = ws_token_buf_.data();
                    out_settings_dirty = true;
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::SetCursorPosY(modal_h - 36.0f * scale);
        ImGui::Separator();
        if (ImGui::Button(tr("Save & Close"))) {
            out_settings_dirty = true;
            keep_open = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    open_ = keep_open;
}

void ShortcutOverlayModal::draw(const KeymapRegistry& keymap, const UiFontSet& fonts, float dpi_scale) {
    if (!open_) {
        return;
    }

    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);
    ImGuiViewport* vp = ImGui::GetMainViewport();

    ImDrawList* bg_dl = ImGui::GetBackgroundDrawList(vp);
    bg_dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                         ImGui::ColorConvertFloat4ToU32(t.scrim_modal));

    const float mw = std::min(760.0f * scale, vp->Size.x - 40.0f * scale);
    const float mh = std::min(480.0f * scale, vp->Size.y - 40.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, t.bg_overlay);
    ImGui::PushStyleColor(ImGuiCol_Border, t.border_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.radius_modal_px);

    bool keep_open = open_;
    if (ImGui::Begin(tr("Keyboard Shortcuts"), &keep_open,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            keep_open = false;
        }
        const auto& specs = KeymapRegistry::all_specs();
        if (ImGui::BeginTable("##ShortcutCheatSheet", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                              ImVec2(-1.0f, mh - 52.0f * scale))) {
            ImGui::TableSetupColumn(tr("Global & Project"));
            ImGui::TableSetupColumn(tr("Viewport & Camera"));
            ImGui::TableSetupColumn(tr("Node Graph & Outliner"));
            ImGui::TableNextRow();

            auto draw_scope_group = [&](int col_idx, KeyScope s1, KeyScope s2) {
                ImGui::TableSetColumnIndex(col_idx);
                for (const auto& sp : specs) {
                    if (sp.scope != s1 && sp.scope != s2) {
                        continue;
                    }
                    const std::string& ch = keymap.chord(sp.id);
                    if (fonts.mono != nullptr) {
                        ImGui::PushFont(fonts.mono);
                    }
                    ImGui::TextColored(t.accent, "%-11s", ch.c_str());
                    if (fonts.mono != nullptr) {
                        ImGui::PopFont();
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(t.text_secondary, "%s", tr(sp.label));
                }
            };
            draw_scope_group(0, KeyScope::Global, KeyScope::Global);
            draw_scope_group(1, KeyScope::Viewport, KeyScope::Viewport);
            draw_scope_group(2, KeyScope::NodeGraph, KeyScope::Outliner);
            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    open_ = keep_open;
}

} // namespace aether::ui
