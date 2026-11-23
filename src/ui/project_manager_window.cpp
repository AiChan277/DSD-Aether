#include "ui/project_manager_window.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"
#include "ui/widgets/feedback.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace aether::ui {
namespace {

bool icase_contains_pm(std::string_view hay, std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (hay.size() < needle.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(hay[i + j])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

template <size_t N> void copy_to_buf(std::array<char, N>& dst, std::string_view src) noexcept {
    dst.fill('\0');
    const size_t n = std::min(src.size(), N - 1u);
    for (size_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
}

[[nodiscard]] const ProjectThumbnailBinding* find_thumbnail(std::span<const ProjectThumbnailBinding> thumbs,
                                                            const std::string& apr_path) noexcept {
    for (const auto& tb : thumbs) {
        if (tb.apr_path == apr_path && tb.texture_id != 0ULL) {
            return &tb;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string format_short_utc(std::string_view utc) {
    if (utc.size() >= 16u) {
        std::string out(utc.substr(0, 10));
        out.push_back(' ');
        out.append(utc.substr(11, 5));
        out.append(" UTC");
        return out;
    }
    return utc.empty() ? std::string("--") : std::string(utc);
}

} // namespace

ProjectManagerWindow::ProjectManagerWindow() {
    filtered_indices_.reserve(64);
}

void ProjectManagerWindow::set_default_projects_dir(std::filesystem::path dir) {
    default_projects_dir_ = std::move(dir);
    if (np_folder_buf_[0] == '\0' && !default_projects_dir_.empty()) {
        copy_to_buf(np_folder_buf_, default_projects_dir_.string());
    }
}

void ProjectManagerWindow::open_new_project_modal(const std::filesystem::path& default_folder) {
    new_project_modal_open_ = true;
    copy_to_buf(np_name_buf_, "Untitled VTuber Studio");
    if (!default_folder.empty()) {
        copy_to_buf(np_folder_buf_, default_folder.string());
    } else if (!default_projects_dir_.empty()) {
        copy_to_buf(np_folder_buf_, default_projects_dir_.string());
    }
    np_avatar_buf_[0] = '\0';
    np_custom_alfx_buf_[0] = '\0';
    np_alfx_preset_idx_ = 0;
    np_aspect_mode_ = 1;
}

void ProjectManagerWindow::set_new_project_folder(const std::filesystem::path& dir) noexcept {
    copy_to_buf(np_folder_buf_, dir.string());
}

void ProjectManagerWindow::set_new_project_avatar(const std::filesystem::path& path) noexcept {
    copy_to_buf(np_avatar_buf_, path.string());
}

void ProjectManagerWindow::set_new_project_alfx(const std::filesystem::path& path) noexcept {
    copy_to_buf(np_custom_alfx_buf_, path.string());
    if (!path.empty()) {
        np_alfx_preset_idx_ = -1;
    }
}

void ProjectManagerWindow::open_template_modal(int template_idx) {
    open_new_project_modal(default_projects_dir_);
    auto find_sample_asset = [&](std::initializer_list<const char*> candidates) -> std::filesystem::path {
        std::error_code ec;
        for (const char* rel : candidates) {
            const std::filesystem::path p(rel);
            if (std::filesystem::is_regular_file(p, ec)) {
                return std::filesystem::weakly_canonical(p, ec);
            }
        }
        return {};
    };
    if (template_idx == 0) {
        copy_to_buf(np_name_buf_, "Blank VRM Stage");
        const auto vrm_p = find_sample_asset({"assets/samples/reference_avatar.vrm", "models/sample_vrm1.vrm"});
        if (!vrm_p.empty()) {
            copy_to_buf(np_avatar_buf_, vrm_p.string());
        }
        np_alfx_preset_idx_ = 0;
    } else if (template_idx == 1) {
        copy_to_buf(np_name_buf_, "Blank PMX Stage");
        const auto pmx_p = find_sample_asset({"models/sample_pmx.pmx", "assets/samples/sample_pmx.pmx"});
        if (!pmx_p.empty()) {
            copy_to_buf(np_avatar_buf_, pmx_p.string());
        }
        np_alfx_preset_idx_ = 0;
    } else if (template_idx == 2) {
        copy_to_buf(np_name_buf_, "3-Point Studio Broadcast");
        np_alfx_preset_idx_ = 0;
    } else {
        copy_to_buf(np_name_buf_, "Shinkai Window Stage");
        np_alfx_preset_idx_ = 1;
    }
}

void ProjectManagerWindow::open_alfx_import_modal(std::filesystem::path src_path, core::AlfxDocument doc) {
    alfx_src_path_ = std::move(src_path);
    alfx_doc_ = std::move(doc);
    alfx_mode_idx_ = 0;
    alfx_modal_open_ = true;
}

bool ProjectManagerWindow::draw_alfx_import_modal(const UiFontSet& fonts, float dpi_scale, core::AlfxDocument& out_doc,
                                                  core::AlfxImportMode& out_mode) {
    if (!alfx_modal_open_) {
        return false;
    }
    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp == nullptr) {
        return false;
    }
    ImGui::GetBackgroundDrawList(vp)->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                    ImGui::ColorConvertFloat4ToU32(t.scrim_modal));

    const float mw = std::min(560.0f * scale, vp->Size.x - 32.0f * scale);
    const float mh = std::min(360.0f * scale, vp->Size.y - 32.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, t.bg_overlay);
    ImGui::PushStyleColor(ImGuiCol_Border, t.border_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.radius_modal_px);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * scale, 14.0f * scale));

    bool applied = false;
    bool keep_open = alfx_modal_open_;
    if (ImGui::Begin(tr("Import Lighting & FX (.alfx)"), &keep_open,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        if (KeymapRegistry::is_raw_key_pressed(ImGuiKey_Escape)) {
            keep_open = false;
        }
        if (fonts.title != nullptr) {
            ImGui::PushFont(fonts.title);
        }
        ImGui::TextColored(t.text_primary, "%s",
                           alfx_doc_.meta.name.empty() ? tr("Custom .alfx Look") : alfx_doc_.meta.name.c_str());
        if (fonts.title != nullptr) {
            ImGui::PopFont();
        }
        ImGui::TextColored(t.text_secondary, "Author: %s  |  Lights: %u  |  Schema: v%s",
                           alfx_doc_.meta.author.empty() ? "Studio" : alfx_doc_.meta.author.c_str(),
                           alfx_doc_.rig.studio_light_count, alfx_doc_.schema_version.c_str());
        if (!alfx_src_path_.empty()) {
            ImGui::TextColored(t.text_tertiary, "%s", alfx_src_path_.filename().string().c_str());
        }
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(t.text_primary, "%s", tr("Select Import Application Mode:"));
        ImGui::Spacing();

        ImGui::RadioButton(tr("Apply Full Look (Default) — Lights, Environment, Circadian, Shading & Satsuei FX"),
                           &alfx_mode_idx_, static_cast<int>(core::AlfxImportMode::ReplaceAll));
        ImGui::RadioButton(tr("Lights & Optical Modifiers Only — Keep current Shading & Satsuei FX"), &alfx_mode_idx_,
                           static_cast<int>(core::AlfxImportMode::LightsOnly));
        ImGui::RadioButton(tr("Effects & Grading Only — Keep current 16-Light Studio Rig placement"), &alfx_mode_idx_,
                           static_cast<int>(core::AlfxImportMode::ColorPostFxOnly));
        ImGui::RadioButton(tr("Append / Merge Lights — Add incoming lights to empty slots (<= 16 max)"),
                           &alfx_mode_idx_, static_cast<int>(core::AlfxImportMode::AdditiveAppendLights));

        ImGui::Spacing();
        ImGui::Separator();
        const float bw = 120.0f * scale;
        if (ImGui::Button(tr("Apply .alfx"), ImVec2(bw, 0.0f))) {
            out_doc = alfx_doc_;
            out_mode = static_cast<core::AlfxImportMode>(std::clamp(alfx_mode_idx_, 0, 3));
            applied = true;
            keep_open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"), ImVec2(bw, 0.0f))) {
            keep_open = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    alfx_modal_open_ = keep_open;
    return applied;
}

void ProjectManagerWindow::open_relink_avatar_modal(std::string expected_rel, std::string expected_abs,
                                                    std::string expected_sha256) {
    relink_rel_ = std::move(expected_rel);
    relink_abs_ = std::move(expected_abs);
    relink_sha_ = std::move(expected_sha256);
    relink_modal_open_ = true;
}

RelinkAvatarChoice ProjectManagerWindow::draw_relink_avatar_modal(const UiFontSet& fonts, float dpi_scale) {
    if (!relink_modal_open_) {
        return RelinkAvatarChoice::None;
    }
    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp == nullptr) {
        return RelinkAvatarChoice::None;
    }
    ImGui::GetBackgroundDrawList(vp)->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                    ImGui::ColorConvertFloat4ToU32(t.scrim_modal));

    const float mw = std::min(560.0f * scale, vp->Size.x - 32.0f * scale);
    const float mh = std::min(270.0f * scale, vp->Size.y - 32.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, t.bg_overlay);
    ImGui::PushStyleColor(ImGuiCol_Border, t.status_warn);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.radius_modal_px);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * scale, 14.0f * scale));

    RelinkAvatarChoice choice = RelinkAvatarChoice::None;
    bool keep_open = relink_modal_open_;
    if (ImGui::Begin(tr("Missing Avatar Asset — Relink Required"), &keep_open,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        if (KeymapRegistry::is_raw_key_pressed(ImGuiKey_Escape)) {
            choice = RelinkAvatarChoice::Cancel;
            keep_open = false;
        }
        if (fonts.title != nullptr) {
            ImGui::PushFont(fonts.title);
        }
        ImGui::TextColored(t.status_warn, "%s", tr("Avatar file referenced by this .apr project could not be found:"));
        if (fonts.title != nullptr) {
            ImGui::PopFont();
        }
        ImGui::Spacing();
        if (!relink_rel_.empty()) {
            ImGui::TextColored(t.text_secondary, "Relative: %s", relink_rel_.c_str());
        }
        if (!relink_abs_.empty()) {
            ImGui::TextColored(t.text_secondary, "Absolute: %s", relink_abs_.c_str());
        }
        if (!relink_sha_.empty()) {
            const std::string short_sha = relink_sha_.size() > 16u ? relink_sha_.substr(0, 16) : relink_sha_;
            ImGui::TextColored(t.text_tertiary, "Expected SHA-256: %s...", short_sha.c_str());
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button(tr("Locate Avatar File..."), ImVec2(170.0f * scale, 0.0f))) {
            choice = RelinkAvatarChoice::BrowseFile;
            keep_open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Continue Without Avatar"), ImVec2(190.0f * scale, 0.0f))) {
            choice = RelinkAvatarChoice::ContinueWithoutAvatar;
            keep_open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"), ImVec2(100.0f * scale, 0.0f))) {
            choice = RelinkAvatarChoice::Cancel;
            keep_open = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    relink_modal_open_ = keep_open;
    if (!keep_open && choice == RelinkAvatarChoice::None) {
        choice = RelinkAvatarChoice::Cancel;
    }
    return choice;
}

void ProjectManagerWindow::open_unsaved_changes_modal(std::string project_name, uint32_t dirty_mask) {
    unsaved_project_name_ = project_name.empty() ? std::string("Untitled Project") : std::move(project_name);
    unsaved_dirty_mask_ = dirty_mask;
    unsaved_modal_open_ = true;
}

UnsavedPromptChoice ProjectManagerWindow::draw_unsaved_changes_modal(const UiFontSet& fonts, float dpi_scale) {
    if (!unsaved_modal_open_) {
        return UnsavedPromptChoice::None;
    }
    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp == nullptr) {
        return UnsavedPromptChoice::None;
    }
    ImGui::GetBackgroundDrawList(vp)->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                    ImGui::ColorConvertFloat4ToU32(t.scrim_modal));

    const float mw = std::min(520.0f * scale, vp->Size.x - 32.0f * scale);
    const float mh = std::min(220.0f * scale, vp->Size.y - 32.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, t.bg_overlay);
    ImGui::PushStyleColor(ImGuiCol_Border, t.border_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.radius_modal_px);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * scale, 14.0f * scale));

    UnsavedPromptChoice choice = UnsavedPromptChoice::None;
    bool keep_open = unsaved_modal_open_;
    if (ImGui::Begin(tr("Unsaved Project Changes"), &keep_open,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        if (KeymapRegistry::is_raw_key_pressed(ImGuiKey_Escape)) {
            choice = UnsavedPromptChoice::Cancel;
            keep_open = false;
        }
        if (fonts.title != nullptr) {
            ImGui::PushFont(fonts.title);
        }
        ImGui::TextColored(t.text_primary, "Save changes to \"%s\" before leaving?", unsaved_project_name_.c_str());
        if (fonts.title != nullptr) {
            ImGui::PopFont();
        }
        ImGui::Spacing();
        std::array<char, 192> dom_buf{};
        size_t pos = 0u;
        auto append_dom = [&](const char* lbl) {
            const int n = std::snprintf(dom_buf.data() + pos, dom_buf.size() - pos, "%s%s", pos > 0u ? ", " : "", lbl);
            if (n > 0) {
                pos = std::min(dom_buf.size() - 1u, pos + static_cast<size_t>(n));
            }
        };
        if (unsaved_dirty_mask_ & dirty_bit(DirtyDomain::ProjectMeta))
            append_dom("Metadata");
        if (unsaved_dirty_mask_ & dirty_bit(DirtyDomain::AvatarState))
            append_dom("Avatar");
        if (unsaved_dirty_mask_ & dirty_bit(DirtyDomain::LightingRig))
            append_dom("Lighting");
        if (unsaved_dirty_mask_ & dirty_bit(DirtyDomain::EffectsFx))
            append_dom("Effects");
        if (unsaved_dirty_mask_ & dirty_bit(DirtyDomain::CameraState))
            append_dom("Camera");
        if (unsaved_dirty_mask_ & dirty_bit(DirtyDomain::NodeGraph))
            append_dom("Node Graph");
        if (unsaved_dirty_mask_ & dirty_bit(DirtyDomain::TrackingCfg))
            append_dom("Tracking");
        if (unsaved_dirty_mask_ & dirty_bit(DirtyDomain::OutputsCfg))
            append_dom("Outputs");
        ImGui::TextColored(t.status_warn, "Modified domains: %s", pos > 0u ? dom_buf.data() : "Project State");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, t.accent);
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_on_accent);
        if (ImGui::Button(tr("Save (.apr) & Proceed"), ImVec2(170.0f * scale, 0.0f))) {
            choice = UnsavedPromptChoice::SaveAndContinue;
            keep_open = false;
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button(tr("Discard Changes"), ImVec2(150.0f * scale, 0.0f))) {
            choice = UnsavedPromptChoice::DiscardAndContinue;
            keep_open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"), ImVec2(100.0f * scale, 0.0f))) {
            choice = UnsavedPromptChoice::Cancel;
            keep_open = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    unsaved_modal_open_ = keep_open;
    if (!keep_open && choice == UnsavedPromptChoice::None) {
        choice = UnsavedPromptChoice::Cancel;
    }
    return choice;
}

void ProjectManagerWindow::draw_new_project_modal(const UiFontSet& fonts, float scale,
                                                  ProjectManagerActions& out_actions) {
    if (!new_project_modal_open_) {
        return;
    }
    const PrecisionDarkTokens& t = tokens();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp == nullptr) {
        return;
    }
    ImGui::GetBackgroundDrawList(vp)->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                    ImGui::ColorConvertFloat4ToU32(t.scrim_modal));

    const float mw = std::min(600.0f * scale, vp->Size.x - 32.0f * scale);
    const float mh = std::min(430.0f * scale, vp->Size.y - 32.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, t.bg_overlay);
    ImGui::PushStyleColor(ImGuiCol_Border, t.border_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.radius_modal_px);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * scale, 14.0f * scale));

    bool keep_open = new_project_modal_open_;
    if (ImGui::Begin(tr("Create New Studio Project (.apr)"), &keep_open,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        if (KeymapRegistry::is_raw_key_pressed(ImGuiKey_Escape)) {
            keep_open = false;
        }
        if (fonts.title != nullptr) {
            ImGui::PushFont(fonts.title);
        }
        ImGui::TextColored(t.text_primary, "%s", tr("New AETHER-2.5D Project"));
        if (fonts.title != nullptr) {
            ImGui::PopFont();
        }
        ImGui::Separator();
        ImGui::Spacing();

        const float browse_w = 88.0f * scale;
        const float field_w = mw - 36.0f * scale - browse_w;

        ImGui::TextColored(t.text_secondary, "%s", tr("1. Project Name"));
        ImGui::SetNextItemWidth(mw - 32.0f * scale);
        ImGui::InputText("##NPName", np_name_buf_.data(), np_name_buf_.size());
        ImGui::Spacing();

        ImGui::TextColored(t.text_secondary, "%s", tr("2. Save Location (.apr archive directory)"));
        ImGui::SetNextItemWidth(field_w);
        ImGui::InputText("##NPFolder", np_folder_buf_.data(), np_folder_buf_.size());
        ImGui::SameLine();
        if (ImGui::Button(tr("Browse...##NPF"), ImVec2(browse_w, 0.0f))) {
            out_actions.request_browse_new_folder = true;
        }
        ImGui::Spacing();

        ImGui::TextColored(t.text_secondary, "%s", tr("3. Initial Avatar (optional: .vrm / .pmx / .fbx)"));
        ImGui::SetNextItemWidth(field_w);
        ImGui::InputTextWithHint("##NPAvatar", tr("Leave blank for empty stage"), np_avatar_buf_.data(),
                                 np_avatar_buf_.size());
        ImGui::SameLine();
        if (ImGui::Button(tr("Browse...##NPA"), ImVec2(browse_w, 0.0f))) {
            out_actions.request_browse_new_avatar = true;
        }
        ImGui::Spacing();

        ImGui::TextColored(t.text_secondary, "%s", tr("4. Starting Look Preset (.alfx)"));
        const auto catalog = core::AlfxSerializer::builtin_preset_catalog();
        const char* preset_preview =
            (np_alfx_preset_idx_ < 0 && np_custom_alfx_buf_[0] != '\0')
                ? np_custom_alfx_buf_.data()
                : catalog[static_cast<size_t>(std::clamp(np_alfx_preset_idx_, 0, 4))].display_name;
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::BeginCombo("##NPPreset", preset_preview)) {
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Selectable(catalog[static_cast<size_t>(i)].display_name, i == np_alfx_preset_idx_)) {
                    np_alfx_preset_idx_ = i;
                    np_custom_alfx_buf_[0] = '\0';
                }
            }
            if (np_custom_alfx_buf_[0] != '\0') {
                ImGui::Separator();
                ImGui::Selectable(np_custom_alfx_buf_.data(), np_alfx_preset_idx_ < 0);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Custom .alfx..."), ImVec2(browse_w, 0.0f))) {
            out_actions.request_browse_new_alfx = true;
        }
        ImGui::Spacing();

        ImGui::TextColored(t.text_secondary, "%s", tr("5. Default Broadcast Aspect Ratio"));
        ImGui::RadioButton("16:9 (1920x1080)", &np_aspect_mode_, 1);
        ImGui::SameLine();
        ImGui::RadioButton("9:16 (Portrait)", &np_aspect_mode_, 2);
        ImGui::SameLine();
        ImGui::RadioButton("1:1 (Square)", &np_aspect_mode_, 3);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, t.accent);
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_on_accent);
        if (ImGui::Button(tr("Create & Launch Studio"), ImVec2(190.0f * scale, 0.0f))) {
            out_actions.request_create_project = true;
            out_actions.create_spec.project_name =
                (np_name_buf_[0] != '\0') ? std::string(np_name_buf_.data()) : std::string("Untitled VTuber Studio");
            out_actions.create_spec.folder_dir = std::filesystem::path(np_folder_buf_.data());
            out_actions.create_spec.avatar_path =
                (np_avatar_buf_[0] != '\0') ? std::filesystem::path(np_avatar_buf_.data()) : std::filesystem::path{};
            out_actions.create_spec.alfx_preset_index = np_alfx_preset_idx_;
            out_actions.create_spec.custom_alfx_path = (np_alfx_preset_idx_ < 0 && np_custom_alfx_buf_[0] != '\0')
                                                           ? std::filesystem::path(np_custom_alfx_buf_.data())
                                                           : std::filesystem::path{};
            out_actions.create_spec.aspect_mode = np_aspect_mode_;
            keep_open = false;
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"), ImVec2(100.0f * scale, 0.0f))) {
            keep_open = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    new_project_modal_open_ = keep_open;
}

void ProjectManagerWindow::rebuild_filtered_list(const std::vector<core::ScannedProjectCard>& cards,
                                                 const core::SessionState& session) {
    filtered_indices_.clear();
    const std::string_view q(search_buf_.data());
    std::filesystem::path lib_filter_dir{};
    if (sidebar_view_ == ProjectManagerSidebarView::LibraryDir && selected_library_idx_ >= 0 &&
        static_cast<size_t>(selected_library_idx_) < session.project_library_dirs.size()) {
        lib_filter_dir =
            std::filesystem::path(session.project_library_dirs[static_cast<size_t>(selected_library_idx_)]);
    }

    for (size_t i = 0; i < cards.size(); ++i) {
        const auto& c = cards[i];
        if (sidebar_view_ == ProjectManagerSidebarView::PinnedOnly && !c.pinned) {
            continue;
        }
        if (sidebar_view_ == ProjectManagerSidebarView::RecentOnly) {
            bool in_recents = false;
            for (const auto& rec : session.recent_projects) {
                if (std::filesystem::path(rec.path) == c.apr_path) {
                    in_recents = true;
                    break;
                }
            }
            if (!in_recents) {
                continue;
            }
        }
        if (!lib_filter_dir.empty()) {
            std::error_code ec;
            const auto rel = std::filesystem::relative(c.apr_path, lib_filter_dir, ec);
            if (ec || rel.empty() || rel.string().rfind("..", 0) == 0) {
                continue;
            }
        }
        if (filter_ == ProjectManagerFilter::Pinned && !c.pinned) {
            continue;
        }
        if (filter_ == ProjectManagerFilter::VrmOnly && !icase_contains_pm(c.manifest.summary.avatar_format, "VRM")) {
            continue;
        }
        if (filter_ == ProjectManagerFilter::PmxOnly && !icase_contains_pm(c.manifest.summary.avatar_format, "PMX")) {
            continue;
        }
        if (!q.empty()) {
            const bool match = icase_contains_pm(c.manifest.project_name, q) ||
                               icase_contains_pm(c.manifest.summary.avatar_display_name, q) ||
                               icase_contains_pm(c.manifest.summary.active_alfx_name, q);
            if (!match) {
                continue;
            }
        }
        filtered_indices_.push_back(i);
    }

    std::sort(filtered_indices_.begin(), filtered_indices_.end(), [&](size_t ia, size_t ib) {
        const auto& a = cards[ia];
        const auto& b = cards[ib];
        if (a.pinned != b.pinned) {
            return a.pinned && !b.pinned;
        }
        if (sort_mode_ == ProjectManagerSort::NameAsc) {
            return a.manifest.project_name < b.manifest.project_name;
        }
        if (sort_mode_ == ProjectManagerSort::RenderTier) {
            return a.manifest.summary.render_tier < b.manifest.summary.render_tier;
        }
        return a.manifest.modified_utc > b.manifest.modified_utc;
    });
}

void ProjectManagerWindow::draw(const std::vector<core::ScannedProjectCard>& cards,
                                std::span<const ProjectThumbnailBinding> thumbnails, const core::SessionState& session,
                                const core::CrashRecoveryInfo& crash_recovery, const UiFontSet& fonts, float dpi_scale,
                                bool scan_in_progress, ProjectManagerActions& out_actions) {
    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp == nullptr) {
        return;
    }

    ImGui::SetNextWindowPos(vp->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(vp->Size, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, t.bg_app);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kRootFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##ProjectManagerRoot", nullptr, kRootFlags)) {
        const float side_w = std::clamp(230.0f * scale, 180.0f * scale, vp->Size.x * 0.28f);

        // Left Sidebar: Libraries, Templates, Preset Vault (.alfx)
        size_t pinned_count = 0u;
        for (const auto& c : cards) {
            if (c.pinned) {
                ++pinned_count;
            }
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.pm_sidebar_bg);
        if (ImGui::BeginChild("##PMSidebar", ImVec2(side_w, -1.0f), ImGuiChildFlags_Borders)) {
            ImGui::SetCursorPos(ImVec2(14.0f * scale, 14.0f * scale));
            if (fonts.title != nullptr) {
                ImGui::PushFont(fonts.title);
            }
            ImGui::TextColored(t.text_primary, "%s", tr("AETHER STUDIO"));
            if (fonts.title != nullptr) {
                ImGui::PopFont();
            }
            ImGui::SetCursorPosX(14.0f * scale);
            ImGui::TextColored(t.text_tertiary, "%s", tr("Project Manager (.apr)"));
            ImGui::Spacing();
            ImGui::Separator();

            ImGui::SetCursorPosX(12.0f * scale);
            ImGui::TextColored(t.text_tertiary, "%s", tr("LIBRARIES"));
            char local_lbl[64]{};
            std::snprintf(local_lbl, sizeof(local_lbl), "  Local Projects (%zu)", cards.size());
            if (ImGui::Selectable(local_lbl, sidebar_view_ == ProjectManagerSidebarView::AllRecents)) {
                sidebar_view_ = ProjectManagerSidebarView::AllRecents;
                selected_library_idx_ = -1;
                filter_ = ProjectManagerFilter::All;
            }
            char rec_lbl[64]{};
            std::snprintf(rec_lbl, sizeof(rec_lbl), "  Recent (%zu)", session.recent_projects.size());
            if (ImGui::Selectable(rec_lbl, sidebar_view_ == ProjectManagerSidebarView::RecentOnly)) {
                sidebar_view_ = ProjectManagerSidebarView::RecentOnly;
                selected_library_idx_ = -1;
            }
            char pin_lbl[64]{};
            std::snprintf(pin_lbl, sizeof(pin_lbl), "  Pinned (%zu)", pinned_count);
            if (ImGui::Selectable(pin_lbl, sidebar_view_ == ProjectManagerSidebarView::PinnedOnly)) {
                sidebar_view_ = ProjectManagerSidebarView::PinnedOnly;
                selected_library_idx_ = -1;
            }
            for (int i = 0; i < static_cast<int>(session.project_library_dirs.size()); ++i) {
                const std::filesystem::path lp(session.project_library_dirs[static_cast<size_t>(i)]);
                const std::string lbl = std::string("  ") + icons::kFolder + " " + lp.filename().string();
                const bool sel = (sidebar_view_ == ProjectManagerSidebarView::LibraryDir && selected_library_idx_ == i);
                if (ImGui::Selectable(lbl.c_str(), sel)) {
                    sidebar_view_ = ProjectManagerSidebarView::LibraryDir;
                    selected_library_idx_ = i;
                }
            }
            ImGui::SetCursorPosX(12.0f * scale);
            if (ImGui::SmallButton(tr("+ Add Folder..."))) {
                out_actions.request_add_library_folder = true;
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::SetCursorPosX(12.0f * scale);
            ImGui::TextColored(t.text_tertiary, "%s", tr("TEMPLATES"));
            static constexpr std::array<const char*, 4> kTplNames = {"  Blank VRM", "  Blank PMX", "  3-Point Studio",
                                                                     "  Shinkai Window"};
            for (int ti = 0; ti < 4; ++ti) {
                if (ImGui::Selectable(kTplNames[static_cast<size_t>(ti)], false)) {
                    open_template_modal(ti);
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::SetCursorPosX(12.0f * scale);
            ImGui::TextColored(t.text_tertiary, "%s", tr("PRESET VAULT (.alfx)"));
            if (ImGui::Selectable("  .alfx Library", sidebar_view_ == ProjectManagerSidebarView::PresetVault)) {
                sidebar_view_ = ProjectManagerSidebarView::PresetVault;
            }
            const auto catalog = core::AlfxSerializer::builtin_preset_catalog();
            ImDrawList* side_dl = ImGui::GetWindowDrawList();
            for (size_t pi = 0; pi < catalog.size(); ++pi) {
                const ImVec2 cur = ImGui::GetCursorScreenPos();
                const float r = 4.0f * scale;
                side_dl->AddCircleFilled(ImVec2(cur.x + 14.0f * scale, cur.y + ImGui::GetTextLineHeight() * 0.5f), r,
                                         t.col32_pm_vault_swatches[pi]);
                ImGui::SetCursorPosX(26.0f * scale);
                ImGui::TextColored(t.text_secondary, "%s", catalog[pi].display_name);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::SetCursorPosX(12.0f * scale);
            if (ImGui::Button(scan_in_progress ? tr("Scanning...") : tr("Refresh Library"),
                              ImVec2(side_w - 24.0f * scale, 0.0f))) {
                out_actions.request_refresh_cards = true;
            }
            ImGui::SetCursorPosX(12.0f * scale);
            if (ImGui::SmallButton(tr("Preferences..."))) {
                prefs_modal_open_ = true;
                pref_startup_edit_ = session.startup_mode;
                pref_autosave_edit_ = static_cast<int>(std::clamp(session.autosave_interval_minutes, 1u, 15u));
                pref_backend_edit_ =
                    (session.render_backend == "dx11") ? 1 : ((session.render_backend == "dx12") ? 2 : 0);
                pref_tier_edit_ = static_cast<int>(std::clamp<uint8_t>(session.lighting_tier, 0u, 2u));
                out_actions.request_open_preferences = true;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::SameLine(0.0f, 0.0f);

        // Main Workspace Area
        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.bg_panel);
        if (ImGui::BeginChild("##PMMain", ImVec2(-1.0f, -1.0f), ImGuiChildFlags_AlwaysUseWindowPadding)) {
            // Crash Recovery Banner (Rev 3.1 §5.1)
            if (crash_recovery.detected) {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, t.pm_recovery_bg);
                ImGui::PushStyleColor(ImGuiCol_Border, t.pm_recovery_border);
                if (ImGui::BeginChild("##CrashRecoveryBanner", ImVec2(-1.0f, 76.0f * scale), ImGuiChildFlags_Borders)) {
                    if (fonts.body_strong != nullptr) {
                        ImGui::PushFont(fonts.body_strong);
                    }
                    ImGui::TextColored(t.status_warn, "%s  Recover Unsaved Session: %s (PID %u exited unexpectedly)",
                                       icons::kAlertTriangle,
                                       crash_recovery.project_name.empty() ? "Studio Project"
                                                                           : crash_recovery.project_name.c_str(),
                                       crash_recovery.orphan_pid);
                    if (fonts.body_strong != nullptr) {
                        ImGui::PopFont();
                    }
                    ImGui::TextColored(t.text_secondary, "Auto-Save: %s   |   Last Saved .apr: %s",
                                       format_short_utc(crash_recovery.autosave_modified_utc).c_str(),
                                       format_short_utc(crash_recovery.original_modified_utc).c_str());
                    ImGui::PushStyleColor(ImGuiCol_Button, t.accent);
                    ImGui::PushStyleColor(ImGuiCol_Text, t.text_on_accent);
                    if (ImGui::SmallButton(tr("Recover Auto-Save"))) {
                        out_actions.request_recover_autosave = true;
                        out_actions.recover_autosave_path = crash_recovery.autosave_apr_path.string();
                        out_actions.recover_original_path = crash_recovery.original_apr_path.string();
                    }
                    ImGui::PopStyleColor(2);
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr("Discard Auto-Save"))) {
                        out_actions.request_discard_autosave = true;
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleColor(2);
                ImGui::Spacing();
            }

            // Top Toolbar: New Project CTA, Search, Format Filter, Sort, Grid/List
            ImGui::PushStyleColor(ImGuiCol_Button, t.accent);
            ImGui::PushStyleColor(ImGuiCol_Text, t.text_on_accent);
            if (ImGui::Button(tr("+ New Project"), ImVec2(126.0f * scale, 0.0f))) {
                open_new_project_modal(default_projects_dir_);
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(
                std::min(260.0f * scale, std::max(140.0f * scale, ImGui::GetContentRegionAvail().x * 0.30f)));
            ImGui::InputTextWithHint("##PMSearch", tr("Search projects, avatar, .alfx..."), search_buf_.data(),
                                     search_buf_.size());

            ImGui::SameLine();
            static constexpr const char* kFilterNames[4] = {"All", "Pinned", "VRM", "PMX"};
            for (int fi = 0; fi < 4; ++fi) {
                const bool active = (static_cast<int>(filter_) == fi);
                ImGui::PushStyleColor(ImGuiCol_Button, active ? t.bg_selected : t.bg_raised);
                if (ImGui::SmallButton(kFilterNames[fi])) {
                    filter_ = static_cast<ProjectManagerFilter>(fi);
                }
                ImGui::PopStyleColor();
                if (fi < 3) {
                    ImGui::SameLine(0.0f, 4.0f * scale);
                }
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(135.0f * scale);
            static constexpr const char* kSortNames[3] = {"Last Opened", "Name (A-Z)", "Render Tier"};
            int s_idx = static_cast<int>(sort_mode_);
            if (ImGui::Combo("##PMSort", &s_idx, kSortNames, 3)) {
                sort_mode_ = static_cast<ProjectManagerSort>(std::clamp(s_idx, 0, 2));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(grid_view_ ? tr("List") : tr("Grid"))) {
                grid_view_ = !grid_view_;
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const float footer_h = 42.0f * scale;
            const float body_h = std::max(140.0f * scale, ImGui::GetContentRegionAvail().y - footer_h - 6.0f * scale);
            if (sidebar_view_ == ProjectManagerSidebarView::PresetVault) {
                if (ImGui::BeginChild("##PMVaultScroll", ImVec2(-1.0f, body_h), ImGuiChildFlags_None)) {
                    const auto catalog = core::AlfxSerializer::builtin_preset_catalog();
                    if (fonts.title != nullptr) {
                        ImGui::PushFont(fonts.title);
                    }
                    ImGui::TextColored(t.text_primary, "%s", tr("Canonical Studio Look Vault (.alfx v1.0.1)"));
                    if (fonts.title != nullptr) {
                        ImGui::PopFont();
                    }
                    ImGui::TextColored(
                        t.text_secondary, "%s",
                        tr("Double-click or press Create to start a new Studio project with any built-in look."));
                    ImGui::Spacing();
                    for (int pi = 0; pi < 5; ++pi) {
                        const auto& info = catalog[static_cast<size_t>(pi)];
                        ImGui::PushID(pi);
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.pm_card_bg);
                        if (ImGui::BeginChild("##VaultCard", ImVec2(-1.0f, 64.0f * scale), ImGuiChildFlags_Borders)) {
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            const ImVec2 p0 = ImGui::GetCursorScreenPos();
                            dl->AddCircleFilled(ImVec2(p0.x + 12.0f * scale, p0.y + 18.0f * scale), 7.0f * scale,
                                                t.col32_pm_vault_swatches[static_cast<size_t>(pi)]);
                            ImGui::SetCursorPosX(32.0f * scale);
                            ImGui::TextColored(t.text_primary, "%s  (%s)", info.display_name, info.filename);
                            ImGui::SameLine();
                            if (ImGui::SmallButton(tr("Create Project with Look"))) {
                                open_new_project_modal(default_projects_dir_);
                                np_alfx_preset_idx_ = pi;
                                np_custom_alfx_buf_[0] = '\0';
                            }
                            ImGui::SetCursorPosX(32.0f * scale);
                            ImGui::TextColored(t.text_tertiary, "%s", info.meta.description.c_str());
                        }
                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
            } else {
                rebuild_filtered_list(cards, session);
                if (ImGui::BeginChild("##PMCardScroll", ImVec2(-1.0f, body_h), ImGuiChildFlags_None)) {
                    if (grid_view_) {
                        const float avail_w = std::max(280.0f, ImGui::GetContentRegionAvail().x);
                        const float min_card_w = 270.0f * scale;
                        const float gap = 14.0f * scale;
                        const int cols =
                            std::max(1, static_cast<int>(std::floor((avail_w + gap) / (min_card_w + gap))));
                        const float card_w = (avail_w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
                        const float thumb_w = card_w - 16.0f * scale;
                        const float thumb_h = std::round(thumb_w * 9.0f / 16.0f);
                        const float card_h = thumb_h + 74.0f * scale;

                        const int total_items = 1 + static_cast<int>(filtered_indices_.size());
                        if (ImGui::BeginTable("##PMGridTbl", cols, ImGuiTableFlags_None)) {
                            for (int slot = 0; slot < total_items; ++slot) {
                                ImGui::TableNextColumn();
                                ImGui::PushID(slot);
                                if (slot == 0) {
                                    // Card #0: + NEW PROJECT card
                                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                                    const ImVec2 p1(p0.x + card_w, p0.y + card_h);
                                    if (ImGui::InvisibleButton("##NewCardBtn", ImVec2(card_w, card_h))) {
                                        open_new_project_modal(default_projects_dir_);
                                    }
                                    const bool hov = ImGui::IsItemHovered();
                                    ImDrawList* dl = ImGui::GetWindowDrawList();
                                    dl->AddRectFilled(p0, p1, hov ? t.col32_pm_card_hover : t.col32_pm_card_bg,
                                                      t.radius_modal_px);
                                    dl->AddRect(p0, p1, hov ? t.col32_pm_card_border_sel : t.col32_pm_card_border,
                                                t.radius_modal_px, 0, hov ? 2.0f : 1.0f);
                                    const char* big_lbl = tr("+ NEW PROJECT");
                                    const ImVec2 tsz = ImGui::CalcTextSize(big_lbl);
                                    dl->AddText(ImVec2(p0.x + (card_w - tsz.x) * 0.5f, p0.y + card_h * 0.45f),
                                                hov ? t.col32_text_primary : t.col32_accent_hover, big_lbl);
                                    const char* sub_lbl = tr("Choose folder, avatar & .alfx look");
                                    const ImVec2 ssz = ImGui::CalcTextSize(sub_lbl);
                                    dl->AddText(ImVec2(p0.x + (card_w - ssz.x) * 0.5f,
                                                       p0.y + card_h * 0.45f + tsz.y + 6.0f * scale),
                                                t.col32_text_tertiary, sub_lbl);
                                } else {
                                    const size_t card_idx = filtered_indices_[static_cast<size_t>(slot - 1)];
                                    const auto& card = cards[card_idx];
                                    const std::string apr_str = card.apr_path.string();
                                    const bool sel = (selected_card_idx_ == static_cast<int32_t>(card_idx));

                                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                                    const ImVec2 p1(p0.x + card_w, p0.y + card_h);
                                    if (ImGui::InvisibleButton("##ProjCard", ImVec2(card_w, card_h))) {
                                        selected_card_idx_ = static_cast<int32_t>(card_idx);
                                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                            out_actions.request_open_project = true;
                                            out_actions.target_apr_path = apr_str;
                                        }
                                    }
                                    const bool hov = ImGui::IsItemHovered();
                                    ImDrawList* dl = ImGui::GetWindowDrawList();
                                    const ImU32 bg_col = sel ? t.col32_pm_card_selected
                                                             : (hov ? t.col32_pm_card_hover : t.col32_pm_card_bg);
                                    dl->AddRectFilled(p0, p1, bg_col, t.radius_modal_px);
                                    dl->AddRect(p0, p1,
                                                (sel || hov) ? t.col32_pm_card_border_sel : t.col32_pm_card_border,
                                                t.radius_modal_px, 0, sel ? 2.0f : 1.0f);

                                    const ImVec2 th0(p0.x + 8.0f * scale, p0.y + 8.0f * scale);
                                    const ImVec2 th1(th0.x + thumb_w, th0.y + thumb_h);
                                    const auto* tb = find_thumbnail(thumbnails, apr_str);
                                    if (tb != nullptr) {
                                        dl->AddImage(static_cast<ImTextureID>(tb->texture_id), th0, th1);
                                    } else {
                                        dl->AddRectFilled(th0, th1, t.col32_pm_thumb_bg, t.radius_child_px);
                                        const char* empty_txt = card.manifest.summary.avatar_display_name.empty()
                                                                    ? tr("No Preview Thumbnail")
                                                                    : card.manifest.summary.avatar_display_name.c_str();
                                        const ImVec2 esz = ImGui::CalcTextSize(empty_txt);
                                        dl->AddText(ImVec2(th0.x + std::max(4.0f, (thumb_w - esz.x) * 0.5f),
                                                           th0.y + (thumb_h - esz.y) * 0.5f),
                                                    t.col32_text_tertiary, empty_txt);
                                    }

                                    // Format badge & Pin badge over thumbnail corner
                                    const char* fmt = card.manifest.summary.avatar_format.empty()
                                                          ? "STAGE"
                                                          : card.manifest.summary.avatar_format.c_str();
                                    const ImU32 fmt_col =
                                        icase_contains_pm(fmt, "PMX") ? t.col32_pm_format_pmx : t.col32_pm_format_vrm;
                                    const ImVec2 fsz = ImGui::CalcTextSize(fmt);
                                    dl->AddRectFilled(
                                        ImVec2(th0.x + 6.0f * scale, th0.y + 6.0f * scale),
                                        ImVec2(th0.x + fsz.x + 16.0f * scale, th0.y + fsz.y + 10.0f * scale), fmt_col,
                                        t.radius_control_px);
                                    dl->AddText(ImVec2(th0.x + 11.0f * scale, th0.y + 8.0f * scale),
                                                t.col32_text_on_accent, fmt);

                                    if (card.pinned) {
                                        const char* pin_lbl = "PIN";
                                        const ImVec2 psz = ImGui::CalcTextSize(pin_lbl);
                                        dl->AddRectFilled(ImVec2(th1.x - psz.x - 16.0f * scale, th0.y + 6.0f * scale),
                                                          ImVec2(th1.x - 6.0f * scale, th0.y + psz.y + 10.0f * scale),
                                                          t.col32_pm_pin_badge, t.radius_control_px);
                                        dl->AddText(ImVec2(th1.x - psz.x - 11.0f * scale, th0.y + 8.0f * scale),
                                                    t.col32_bg_app, pin_lbl);
                                    }

                                    // Card Footer Metadata
                                    const float tx = p0.x + 10.0f * scale;
                                    float ty = th1.y + 6.0f * scale;
                                    dl->AddText(ImVec2(tx, ty), t.col32_text_primary,
                                                card.manifest.project_name.empty()
                                                    ? "Untitled Project"
                                                    : card.manifest.project_name.c_str());
                                    ty += ImGui::GetTextLineHeight() + 3.0f * scale;
                                    char sub1[128]{};
                                    std::snprintf(sub1, sizeof(sub1), "%s \xC2\xB7 %u lights \xC2\xB7 %s",
                                                  card.manifest.summary.active_alfx_name.empty()
                                                      ? "Default Rig"
                                                      : card.manifest.summary.active_alfx_name.c_str(),
                                                  card.manifest.summary.active_light_count,
                                                  card.manifest.summary.render_tier.c_str());
                                    dl->AddText(ImVec2(tx, ty), t.col32_text_secondary, sub1);
                                    ty += ImGui::GetTextLineHeight() + 2.0f * scale;
                                    const std::string dt_str = format_short_utc(card.manifest.modified_utc);
                                    dl->AddText(ImVec2(tx, ty), t.col32_text_tertiary, dt_str.c_str());

                                    if (ImGui::BeginPopupContextItem("##CardCtx")) {
                                        if (ImGui::MenuItem(tr("Open Project"))) {
                                            out_actions.request_open_project = true;
                                            out_actions.target_apr_path = apr_str;
                                        }
                                        if (ImGui::MenuItem(card.pinned ? tr("Unpin") : tr("Pin to Top"))) {
                                            out_actions.request_toggle_pin = true;
                                            out_actions.pin_target_path = apr_str;
                                            out_actions.pin_target_value = !card.pinned;
                                        }
                                        if (ImGui::MenuItem(tr("Rename..."))) {
                                            rename_modal_open_ = true;
                                            rename_path_ = apr_str;
                                            copy_to_buf(rename_buf_, card.manifest.project_name);
                                        }
                                        if (ImGui::MenuItem(tr("Duplicate (.apr)"))) {
                                            out_actions.request_duplicate_project = true;
                                            out_actions.duplicate_target_path = apr_str;
                                        }
                                        if (ImGui::MenuItem(tr("Export Look (.alfx)..."))) {
                                            out_actions.request_export_alfx_from_card = true;
                                            out_actions.export_alfx_source_apr = apr_str;
                                        }
                                        if (ImGui::MenuItem(tr("Show in Explorer"))) {
                                            out_actions.request_show_in_explorer = true;
                                            out_actions.explorer_target_path = apr_str;
                                        }
                                        ImGui::Separator();
                                        if (ImGui::MenuItem(tr("Delete Project..."))) {
                                            delete_modal_open_ = true;
                                            delete_path_ = apr_str;
                                            copy_to_buf(delete_name_, card.manifest.project_name);
                                        }
                                        ImGui::EndPopup();
                                    }
                                }
                                ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }
                    } else {
                        // List view
                        if (ImGui::BeginTable("##PMListTbl", 5,
                                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                  ImGuiTableFlags_Resizable)) {
                            ImGui::TableSetupColumn(tr("Project Name"));
                            ImGui::TableSetupColumn(tr("Avatar"));
                            ImGui::TableSetupColumn(tr("Look (.alfx)"));
                            ImGui::TableSetupColumn(tr("Tier"));
                            ImGui::TableSetupColumn(tr("Modified"));
                            ImGui::TableHeadersRow();
                            for (size_t idx : filtered_indices_) {
                                const auto& card = cards[idx];
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                const bool sel = (selected_card_idx_ == static_cast<int32_t>(idx));
                                if (ImGui::Selectable(card.manifest.project_name.c_str(), sel,
                                                      ImGuiSelectableFlags_SpanAllColumns)) {
                                    selected_card_idx_ = static_cast<int32_t>(idx);
                                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                        out_actions.request_open_project = true;
                                        out_actions.target_apr_path = card.apr_path.string();
                                    }
                                }
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextUnformatted(card.manifest.summary.avatar_display_name.c_str());
                                ImGui::TableSetColumnIndex(2);
                                ImGui::Text("%s (%u)", card.manifest.summary.active_alfx_name.c_str(),
                                            card.manifest.summary.active_light_count);
                                ImGui::TableSetColumnIndex(3);
                                ImGui::TextUnformatted(card.manifest.summary.render_tier.c_str());
                                ImGui::TableSetColumnIndex(4);
                                const std::string dt_str = format_short_utc(card.manifest.modified_utc);
                                ImGui::TextUnformatted(dt_str.c_str());
                            }
                            ImGui::EndTable();
                        }
                    }
                }
                ImGui::EndChild();
            }

            // Bottom Footer Bar (Rev 3.1 §6.2)
            ImGui::Separator();
            bool show_pm_startup = (session.startup_mode != core::StartupMode::ResumeLastProject);
            if (ImGui::Checkbox(tr("Show Project Manager on startup"), &show_pm_startup)) {
                out_actions.request_startup_mode_change = true;
                out_actions.target_startup_mode =
                    show_pm_startup ? core::StartupMode::ShowProjectManager : core::StartupMode::ResumeLastProject;
            }
            const bool has_selected_card =
                (selected_card_idx_ >= 0 && static_cast<size_t>(selected_card_idx_) < cards.size());
            const float btn_open_apr_w = 110.0f * scale;
            const float btn_import_w = 122.0f * scale;
            const float btn_open_cta_w = 132.0f * scale;
            const float right_cluster_w =
                btn_open_apr_w + btn_import_w + btn_open_cta_w + ImGui::GetStyle().ItemSpacing.x * 2.0f;
            const float target_x = std::max(ImGui::GetCursorPosX() + 12.0f * scale,
                                            ImGui::GetWindowWidth() - right_cluster_w - 16.0f * scale);
            ImGui::SameLine(target_x);
            if (ImGui::Button(tr("Open .apr..."), ImVec2(btn_open_apr_w, 0.0f))) {
                out_actions.request_open_file_dialog = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Import .aether"), ImVec2(btn_import_w, 0.0f))) {
                out_actions.request_import_aether_dialog = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!has_selected_card);
            ImGui::PushStyleColor(ImGuiCol_Button, t.accent);
            ImGui::PushStyleColor(ImGuiCol_Text, t.text_on_accent);
            if (ImGui::Button(tr("Open Project"), ImVec2(btn_open_cta_w, 0.0f)) && has_selected_card) {
                out_actions.request_open_project = true;
                out_actions.target_apr_path = cards[static_cast<size_t>(selected_card_idx_)].apr_path.string();
            }
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    draw_new_project_modal(fonts, scale, out_actions);

    if (rename_modal_open_) {
        ImGui::SetNextWindowSize(ImVec2(380.0f * scale, 130.0f * scale), ImGuiCond_Always);
        if (ImGui::Begin(tr("Rename Project"), &rename_modal_open_,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
            if (KeymapRegistry::is_raw_key_pressed(ImGuiKey_Escape)) {
                rename_modal_open_ = false;
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##PMRename", rename_buf_.data(), rename_buf_.size());
            ImGui::Spacing();
            if (ImGui::Button(tr("Apply Name"), ImVec2(110.0f * scale, 0.0f))) {
                out_actions.request_rename_project = true;
                out_actions.rename_target_path = rename_path_;
                out_actions.rename_new_name = rename_buf_.data();
                rename_modal_open_ = false;
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"), ImVec2(90.0f * scale, 0.0f))) {
                rename_modal_open_ = false;
            }
        }
        ImGui::End();
    }

    if (delete_modal_open_) {
        ImGui::SetNextWindowSize(ImVec2(420.0f * scale, 140.0f * scale), ImGuiCond_Always);
        if (ImGui::Begin(tr("Delete Project Archive (.apr)"), &delete_modal_open_,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
            if (KeymapRegistry::is_raw_key_pressed(ImGuiKey_Escape)) {
                delete_modal_open_ = false;
            }
            ImGui::TextWrapped("Delete \"%s\" permanently from disk and recents?", delete_name_.data());
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, t.status_error);
            if (ImGui::Button(tr("Delete Permanently"), ImVec2(150.0f * scale, 0.0f))) {
                out_actions.request_delete_project = true;
                out_actions.delete_target_path = delete_path_;
                delete_modal_open_ = false;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"), ImVec2(90.0f * scale, 0.0f))) {
                delete_modal_open_ = false;
            }
        }
        ImGui::End();
    }

    if (prefs_modal_open_) {
        ImGui::SetNextWindowSize(ImVec2(440.0f * scale, 240.0f * scale), ImGuiCond_Always);
        if (ImGui::Begin(tr("Application Preferences (session_state.json)"), &prefs_modal_open_,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
            if (KeymapRegistry::is_raw_key_pressed(ImGuiKey_Escape)) {
                prefs_modal_open_ = false;
            }
            bool show_pm = (pref_startup_edit_ != core::StartupMode::ResumeLastProject);
            if (ImGui::Checkbox(tr("Show Project Manager on startup"), &show_pm)) {
                pref_startup_edit_ =
                    show_pm ? core::StartupMode::ShowProjectManager : core::StartupMode::ResumeLastProject;
            }
            ImGui::SliderInt(tr("Auto-Save Interval (min)"), &pref_autosave_edit_, 1, 15);
            static constexpr const char* kBackends[3] = {"Vulkan 1.3", "DirectX 11", "DirectX 12"};
            ImGui::Combo(tr("Default Render Backend"), &pref_backend_edit_, kBackends, 3);
            static constexpr const char* kTiers[3] = {"Traditional", "Hybrid Ray Tracing", "2.5D Path Tracing"};
            ImGui::Combo(tr("Default Lighting Tier"), &pref_tier_edit_, kTiers, 3);
            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Button(tr("Save Preferences"), ImVec2(150.0f * scale, 0.0f))) {
                out_actions.request_apply_preferences = true;
                out_actions.target_startup_mode = pref_startup_edit_;
                out_actions.pref_autosave_minutes = static_cast<uint32_t>(std::clamp(pref_autosave_edit_, 1, 15));
                out_actions.pref_render_backend =
                    (pref_backend_edit_ == 1) ? "dx11" : ((pref_backend_edit_ == 2) ? "dx12" : "vulkan");
                out_actions.pref_lighting_tier = static_cast<uint8_t>(std::clamp(pref_tier_edit_, 0, 2));
                prefs_modal_open_ = false;
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"), ImVec2(90.0f * scale, 0.0f))) {
                prefs_modal_open_ = false;
            }
        }
        ImGui::End();
    }
}

} // namespace aether::ui
