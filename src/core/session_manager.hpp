#pragma once

#include "aether/ui_state_contract.hpp"
#include "core/apr_serializer.hpp"
#include "core/expected.hpp"
#include "core/paths.hpp"
#include "core/spsc_ring.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace aether::core {

enum class StartupMode : uint8_t {
    ShowProjectManager = 0, // DaVinci Resolve-style visual browser on boot (< 400 ms)
    ResumeLastProject = 1,  // Optional skip straight to `last_opened_project_path`
};

struct WindowGeometry {
    int32_t x{100};
    int32_t y{100};
    int32_t width{1280};
    int32_t height{720};
    bool maximized{false};
    bool valid{false};
};

struct RecentProjectEntry {
    std::string path{};
    std::string project_uuid{};
    std::string project_name{};
    bool pinned{false};
    std::string last_opened_utc{};
    std::string avatar_display_name{};
    std::string avatar_format{};
    std::string active_alfx_name{};
    uint32_t active_light_count{0u};
    std::string render_tier{"HybridRT"};
    bool has_thumbnail{false};
};

/**
 * Persistent cross-project user preferences & session history stored exclusively in
 * `%APPDATA%\AETHER-2.5D-Studio\session_state.json` (or `./userdata/session_state.json`
 * when `portable.flag` is present next to the executable).
 * Hardware preferences (`preferred_gpu_luid`, `render_backend`, `lighting_tier`) live ONLY here,
 * never inside per-project `.apr` archives (`Rev 3.1 §1.4 & §5.1`).
 */
struct SessionState {
    static constexpr size_t kMaxRecentProjects = 30u;

    std::vector<RecentProjectEntry> recent_projects{};
    std::string last_opened_project_path{};
    std::vector<std::string> project_library_dirs{};
    StartupMode startup_mode{StartupMode::ShowProjectManager};
    uint32_t autosave_interval_minutes{3u};
    WindowGeometry window_geometry{};
    std::string imgui_dock_layout_path{};

    // Global hardware & workstation preferences (never stored inside .apr)
    std::string preferred_gpu_luid{};
    std::string preferred_gpu_name{};
    std::string render_backend{"vulkan"};
    uint8_t lighting_tier{1u}; // 0 = Traditional, 1 = HybridRT, 2 = PathTracing_25D
    std::string preferred_camera_link{};
    std::string preferred_mic_id{};
    std::string ws_auth_token{"aether-local-dev-token"};
    bool telemetry_show_substats{true};
    uint32_t settings_revision{2u};
    std::map<std::string, double> app_params{};

    bool wizard_completed{false};
    uint64_t hw_inventory_hash{0u};
    bool migrated_from_legacy_toml{false};
};

struct CrashRecoveryInfo {
    bool detected{false};
    uint32_t orphan_pid{0u};
    std::string lock_started_utc{};
    std::string project_uuid{};
    std::string project_name{};
    std::filesystem::path autosave_apr_path{};
    std::filesystem::path original_apr_path{};
    std::string autosave_modified_utc{};
    std::string original_modified_utc{};
};

struct ScannedProjectCard {
    std::filesystem::path apr_path{};
    AprManifest manifest{};
    std::vector<uint8_t> thumbnail_png{};
    bool pinned{false};
    bool missing_file{false};
};

enum class AprWorkerOp : uint8_t {
    SaveApr = 0,
    WriteAutosave = 1,
    ScanLibraries = 2,
};

struct AprWorkerResult {
    AprWorkerOp op{AprWorkerOp::SaveApr};
    bool success{false};
    std::filesystem::path target_path{};
    AprManifest manifest{};
    uint64_t snapshot_dirty_generation{0u};
    std::string error_message{};
    std::vector<ScannedProjectCard> scanned_cards{};
};

/** Lock-free SPSC background worker executing atomic `.apr` saves, 3-minute autosaves, and card scans (`HR-02`). */
class AprIoWorker {
  public:
    AprIoWorker() = default;
    ~AprIoWorker();
    AprIoWorker(const AprIoWorker&) = delete;
    AprIoWorker& operator=(const AprIoWorker&) = delete;

    void start();
    void stop() noexcept;

    /** Enqueues an atomic `.apr` save from an immutable main-thread snapshot (< 0.5 ms UI side). */
    [[nodiscard]] bool enqueue_save(std::filesystem::path dst_apr_path, const StudioStateStore& state_snapshot,
                                    const renderer::LightingRig& rig_snapshot, std::string_view active_alfx_name,
                                    std::vector<uint8_t> thumbnail_png, AprWorkerOp op = AprWorkerOp::SaveApr);

    /** Enqueues a non-blocking library + recent-project manifest & 480x270 thumbnail scan. */
    [[nodiscard]] bool enqueue_scan(std::vector<std::filesystem::path> library_dirs,
                                    std::vector<RecentProjectEntry> recent_entries);

    /** Drains completed worker notifications cleanly on the main thread. */
    [[nodiscard]] bool try_pop_result(AprWorkerResult& out_result);

  private:
    struct WorkerJob {
        AprWorkerOp op{AprWorkerOp::SaveApr};
        std::filesystem::path dst_path{};
        StudioStateStore state_snapshot{};
        renderer::LightingRig rig_snapshot{};
        std::string active_alfx_name{};
        std::vector<uint8_t> thumbnail_png{};
        std::vector<std::filesystem::path> scan_dirs{};
        std::vector<RecentProjectEntry> recent_entries{};
    };

    void worker_main(const std::stop_token& stoken);
    void wake_worker() noexcept;

    SpscRing<WorkerJob*, 32> req_queue_{};
    SpscRing<AprWorkerResult*, 32> res_queue_{};
    std::atomic<bool> started_{false};
    void* wake_event_{nullptr};
    std::jthread thread_{};
};

class SessionManager {
  public:
    /** Initializes paths cleanly; loads `session_state.json` or runs 1-time migration from `settings.toml`. */
    [[nodiscard]] Expected<void> init(const RuntimePaths& paths);

    [[nodiscard]] const std::filesystem::path& session_dir() const noexcept { return session_dir_; }
    [[nodiscard]] const std::filesystem::path& session_state_path() const noexcept { return session_state_path_; }
    [[nodiscard]] const std::filesystem::path& session_lock_path() const noexcept { return session_lock_path_; }
    [[nodiscard]] const std::filesystem::path& autosave_dir() const noexcept { return autosave_dir_; }

    [[nodiscard]] SessionState& state() noexcept { return state_; }
    [[nodiscard]] const SessionState& state() const noexcept { return state_; }

    /** Saves `session_state.json` atomically (`CreateFileW` -> `FlushFileBuffers` -> `.bak` -> `MoveFileExW`). */
    [[nodiscard]] Expected<void> save_session_state() const;

    /** Updates/promotes a `.apr` entry in `recent_projects` (max 30, preserves pin status). */
    void record_recent_project(const std::filesystem::path& apr_path, const AprManifest& manifest);
    bool set_project_pinned(const std::filesystem::path& apr_path, bool pinned);
    void remove_recent_project(const std::filesystem::path& apr_path);
    void add_library_dir(const std::filesystem::path& dir);

    /** Returns deterministic path `autosave/<project_uuid>.apr.autosave`. */
    [[nodiscard]] std::filesystem::path autosave_path_for(std::string_view project_uuid) const;

    /** Acquires `session.lock` containing current PID & project metadata; writes atomically. */
    [[nodiscard]] Expected<void> acquire_session_lock(std::string_view project_uuid,
                                                      const std::filesystem::path& project_path) const;

    /** Removes `session.lock` on clean shutdown or Project Manager return. */
    void release_session_lock() const noexcept;

    /** Deletes `autosave/<project_uuid>.apr.autosave` once changes are saved or discarded cleanly. */
    void remove_autosave_for(std::string_view project_uuid) const;

    /**
     * Inspects `session.lock` on boot: if the lock belongs to a dead PID and a newer `.apr.autosave`
     * exists, populates `CrashRecoveryInfo` so the Project Manager displays the Recover / Discard banner.
     */
    [[nodiscard]] CrashRecoveryInfo inspect_crash_recovery(bool force_treat_pid_dead_for_test = false) const;

    /** Synchronous scan helper (reused by `AprIoWorker` for background scanning and tests). */
    [[nodiscard]] static std::vector<ScannedProjectCard>
    scan_project_cards(std::span<const std::filesystem::path> library_dirs,
                       std::span<const RecentProjectEntry> recent_entries);

    /** Checks whether OS PID `pid` is still running (`OpenProcess` + `STILL_ACTIVE` on Win32). */
    [[nodiscard]] static bool is_process_alive(uint32_t pid) noexcept;
    [[nodiscard]] static uint32_t current_process_id() noexcept;

  private:
    void perform_legacy_toml_migration(const std::filesystem::path& legacy_dir);

    std::filesystem::path session_dir_{};
    std::filesystem::path session_state_path_{};
    std::filesystem::path session_lock_path_{};
    std::filesystem::path autosave_dir_{};
    SessionState state_{};
};

} // namespace aether::core
