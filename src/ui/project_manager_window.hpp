#pragma once

#include "aether/ui_state_contract.hpp"
#include "core/alfx_serializer.hpp"
#include "core/session_manager.hpp"
#include "ui/dev_shell.hpp"
#include "ui/params/project_settings.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace aether::ui {

struct ProjectThumbnailBinding {
    std::string apr_path{};
    uint64_t texture_id{0ULL};
    uint32_t width{0u};
    uint32_t height{0u};
};

enum class ProjectManagerSort : uint8_t {
    LastOpened = 0,
    NameAsc = 1,
    RenderTier = 2,
};

enum class ProjectManagerFilter : uint8_t {
    All = 0,
    Pinned = 1,
    VrmOnly = 2,
    PmxOnly = 3,
};

enum class ProjectManagerSidebarView : uint8_t {
    AllRecents = 0,
    RecentOnly = 1,
    PinnedOnly = 2,
    LibraryDir = 3,
    PresetVault = 4,
};

enum class UnsavedPromptChoice : uint8_t {
    None = 0,
    SaveAndContinue = 1,
    DiscardAndContinue = 2,
    Cancel = 3,
};

enum class RelinkAvatarChoice : uint8_t {
    None = 0,
    BrowseFile = 1,
    ContinueWithoutAvatar = 2,
    Cancel = 3,
};

struct NewProjectCreateRequest {
    std::string project_name{};
    std::filesystem::path folder_dir{};
    std::filesystem::path avatar_path{};
    int32_t alfx_preset_index{0};
    std::filesystem::path custom_alfx_path{};
    int32_t aspect_mode{1}; // 1=16:9 Broadcast (default), 2=9:16 Portrait, 3=1:1 Square, 0=Free
};

struct ProjectManagerActions {
    bool request_open_project{false};
    std::string target_apr_path{};

    bool request_create_project{false};
    NewProjectCreateRequest create_spec{};

    bool request_browse_new_folder{false};
    bool request_browse_new_avatar{false};
    bool request_browse_new_alfx{false};
    bool request_open_file_dialog{false};
    bool request_import_aether_dialog{false};
    bool request_add_library_folder{false};
    bool request_refresh_cards{false};

    bool request_recover_autosave{false};
    std::string recover_autosave_path{};
    std::string recover_original_path{};
    bool request_discard_autosave{false};

    bool request_toggle_pin{false};
    bool pin_target_value{false};
    std::string pin_target_path{};

    bool request_rename_project{false};
    std::string rename_target_path{};
    std::string rename_new_name{};

    bool request_duplicate_project{false};
    std::string duplicate_target_path{};

    bool request_export_alfx_from_card{false};
    std::string export_alfx_source_apr{};

    bool request_show_in_explorer{false};
    std::string explorer_target_path{};

    bool request_delete_project{false};
    std::string delete_target_path{};

    bool request_startup_mode_change{false};
    core::StartupMode target_startup_mode{core::StartupMode::ShowProjectManager};

    bool request_open_preferences{false};
    bool request_apply_preferences{false};
    uint32_t pref_autosave_minutes{3u};
    std::string pref_render_backend{"vulkan"};
    uint8_t pref_lighting_tier{1u};
};

/**
 * DaVinci Resolve-style visual `.apr` Project Manager window (`Rev 3.1 §5`).
 * Zero per-frame heap churn when idle; uses exclusively `tokens()` colors and
 * `KeymapRegistry::is_raw_key_pressed()` for modal Escape handling.
 */
class ProjectManagerWindow {
  public:
    ProjectManagerWindow();

    void set_default_projects_dir(std::filesystem::path dir);

    void draw(const std::vector<core::ScannedProjectCard>& cards, std::span<const ProjectThumbnailBinding> thumbnails,
              const core::SessionState& session, const core::CrashRecoveryInfo& crash_recovery, const UiFontSet& fonts,
              float dpi_scale, bool scan_in_progress, ProjectManagerActions& out_actions);

    void open_new_project_modal(const std::filesystem::path& default_folder);
    void set_new_project_folder(const std::filesystem::path& dir) noexcept;
    void set_new_project_avatar(const std::filesystem::path& path) noexcept;
    void set_new_project_alfx(const std::filesystem::path& path) noexcept;
    [[nodiscard]] bool is_new_project_modal_open() const noexcept { return new_project_modal_open_; }

    // Shared Studio & Project Manager modals (Rev 3.1 §4.3 & §5.3)
    void open_alfx_import_modal(std::filesystem::path src_path, core::AlfxDocument doc);
    [[nodiscard]] bool is_alfx_import_modal_open() const noexcept { return alfx_modal_open_; }
    [[nodiscard]] bool draw_alfx_import_modal(const UiFontSet& fonts, float dpi_scale, core::AlfxDocument& out_doc,
                                              core::AlfxImportMode& out_mode);

    void open_relink_avatar_modal(std::string expected_rel, std::string expected_abs, std::string expected_sha256);
    [[nodiscard]] bool is_relink_avatar_modal_open() const noexcept { return relink_modal_open_; }
    [[nodiscard]] RelinkAvatarChoice draw_relink_avatar_modal(const UiFontSet& fonts, float dpi_scale);

    void open_unsaved_changes_modal(std::string project_name, uint32_t dirty_mask);
    [[nodiscard]] bool is_unsaved_changes_modal_open() const noexcept { return unsaved_modal_open_; }
    [[nodiscard]] UnsavedPromptChoice draw_unsaved_changes_modal(const UiFontSet& fonts, float dpi_scale);

  private:
    void rebuild_filtered_list(const std::vector<core::ScannedProjectCard>& cards, const core::SessionState& session);
    void draw_new_project_modal(const UiFontSet& fonts, float scale, ProjectManagerActions& out_actions);

    std::filesystem::path default_projects_dir_{};

    std::array<char, 128> search_buf_{};
    ProjectManagerFilter filter_{ProjectManagerFilter::All};
    ProjectManagerSort sort_mode_{ProjectManagerSort::LastOpened};
    ProjectManagerSidebarView sidebar_view_{ProjectManagerSidebarView::AllRecents};
    int32_t selected_library_idx_{-1};
    int32_t selected_card_idx_{-1};
    bool grid_view_{true};

    std::vector<size_t> filtered_indices_{};

    // New Project 4-field modal state
    bool new_project_modal_open_{false};
    std::array<char, 96> np_name_buf_{"My VTuber Studio"};
    std::array<char, 512> np_folder_buf_{};
    std::array<char, 512> np_avatar_buf_{};
    std::array<char, 512> np_custom_alfx_buf_{};
    int32_t np_alfx_preset_idx_{0}; // 0..4 built-in, -1 custom file
    int32_t np_aspect_mode_{1};     // 1=16:9, 2=9:16, 3=1:1

    void open_template_modal(int template_idx);

    // Rename & Delete confirm popups
    bool rename_modal_open_{false};
    std::string rename_path_{};
    std::array<char, 96> rename_buf_{};

    bool delete_modal_open_{false};
    std::string delete_path_{};
    std::array<char, 96> delete_name_{};

    // 4-Mode .alfx Import modal
    bool alfx_modal_open_{false};
    std::filesystem::path alfx_src_path_{};
    core::AlfxDocument alfx_doc_{};
    int32_t alfx_mode_idx_{0};

    // Relink Missing Avatar modal
    bool relink_modal_open_{false};
    std::string relink_rel_{};
    std::string relink_abs_{};
    std::string relink_sha_{};

    // Unsaved Changes (3-option) modal
    bool unsaved_modal_open_{false};
    std::string unsaved_project_name_{};
    uint32_t unsaved_dirty_mask_{0u};

    // Global Workstation Preferences modal (session_state.json)
    bool prefs_modal_open_{false};
    core::StartupMode pref_startup_edit_{core::StartupMode::ShowProjectManager};
    int pref_autosave_edit_{3};
    int pref_backend_edit_{0};
    int pref_tier_edit_{1};
};

} // namespace aether::ui
