#include "core/session_manager.hpp"

#include "core/zip_archive.hpp"

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

namespace aether::core {
namespace {

[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    std::u8string u8;
    u8.reserve(utf8.size());
    for (const char ch : utf8) {
        u8.push_back(static_cast<char8_t>(ch));
    }
    return std::filesystem::path(u8);
}

[[nodiscard]] std::string utf8_from_path(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    std::string out;
    out.reserve(u8.size());
    for (const char8_t ch : u8) {
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

[[nodiscard]] std::string normalized_key(const std::filesystem::path& p) {
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::weakly_canonical(p, ec);
    std::string s = utf8_from_path(ec ? p.lexically_normal() : abs);
#if defined(_WIN32)
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return s;
}

[[nodiscard]] bool has_apr_extension(const std::filesystem::path& p) {
    const std::string ext = utf8_from_path(p.extension());
    if (ext.size() != 4u) {
        return false;
    }
    return ext[0] == '.' && static_cast<char>(std::tolower(static_cast<unsigned char>(ext[1]))) == 'a' &&
           static_cast<char>(std::tolower(static_cast<unsigned char>(ext[2]))) == 'p' &&
           static_cast<char>(std::tolower(static_cast<unsigned char>(ext[3]))) == 'r';
}

[[nodiscard]] std::string trim_copy(std::string_view sv) {
    size_t b = 0u;
    size_t e = sv.size();
    while (b < e && std::isspace(static_cast<unsigned char>(sv[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1u]))) {
        --e;
    }
    return std::string(sv.substr(b, e - b));
}

[[nodiscard]] std::string unquote_toml(std::string_view sv) {
    std::string s = trim_copy(sv);
    if (s.size() >= 2u && s.front() == '"' && s.back() == '"') {
        std::string out;
        out.reserve(s.size() - 2u);
        for (size_t i = 1u; i + 1u < s.size(); ++i) {
            if (s[i] == '\\' && (i + 2u) < s.size()) {
                const char nxt = s[++i];
                if (nxt == 'n') {
                    out.push_back('\n');
                } else if (nxt == 't') {
                    out.push_back('\t');
                } else {
                    out.push_back(nxt);
                }
            } else {
                out.push_back(s[i]);
            }
        }
        return out;
    }
    return s;
}

[[nodiscard]] nlohmann::json serialize_session_state(const SessionState& s) {
    nlohmann::json rec_arr = nlohmann::json::array();
    for (const auto& r : s.recent_projects) {
        rec_arr.push_back({
            {"path", r.path},
            {"project_uuid", r.project_uuid},
            {"project_name", r.project_name},
            {"pinned", r.pinned},
            {"last_opened_utc", r.last_opened_utc},
            {"avatar_display_name", r.avatar_display_name},
            {"avatar_format", r.avatar_format},
            {"active_alfx_name", r.active_alfx_name},
            {"active_light_count", r.active_light_count},
            {"render_tier", r.render_tier},
            {"has_thumbnail", r.has_thumbnail},
        });
    }

    return nlohmann::json{
        {"schema_version", "1.0.0"},
        {"startup_mode", s.startup_mode == StartupMode::ResumeLastProject ? "ResumeLastProject" : "ShowProjectManager"},
        {"autosave_interval_minutes", std::clamp(s.autosave_interval_minutes, 1u, 15u)},
        {"last_opened_project_path", s.last_opened_project_path},
        {"project_library_dirs", s.project_library_dirs},
        {"recent_projects", std::move(rec_arr)},
        {"window_geometry",
         {{"x", s.window_geometry.x},
          {"y", s.window_geometry.y},
          {"width", s.window_geometry.width},
          {"height", s.window_geometry.height},
          {"maximized", s.window_geometry.maximized},
          {"valid", s.window_geometry.valid}}},
        {"imgui_dock_layout_path", s.imgui_dock_layout_path},
        {"preferred_gpu_luid", s.preferred_gpu_luid},
        {"preferred_gpu_name", s.preferred_gpu_name},
        {"render_backend", s.render_backend},
        {"lighting_tier", static_cast<int>(s.lighting_tier)},
        {"preferred_camera_link", s.preferred_camera_link},
        {"preferred_mic_id", s.preferred_mic_id},
        {"ws_auth_token", s.ws_auth_token},
        {"telemetry_show_substats", s.telemetry_show_substats},
        {"settings_revision", s.settings_revision},
        {"app_params", s.app_params},
        {"wizard_completed", s.wizard_completed},
        {"hw_inventory_hash", s.hw_inventory_hash},
        {"migrated_from_legacy_toml", s.migrated_from_legacy_toml},
    };
}

[[nodiscard]] Expected<void> deserialize_session_state(std::span<const uint8_t> bytes, SessionState& s) {
    try {
        const nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end());
        if (!j.is_object()) {
            return fail(ErrorCode::ParseError, "session_state.json root is not an object");
        }
        const std::string mode_str = j.value("startup_mode", std::string("ShowProjectManager"));
        s.startup_mode =
            (mode_str == "ResumeLastProject") ? StartupMode::ResumeLastProject : StartupMode::ShowProjectManager;
        s.autosave_interval_minutes = std::clamp(j.value("autosave_interval_minutes", 3u), 1u, 15u);
        s.last_opened_project_path = j.value("last_opened_project_path", s.last_opened_project_path);

        if (const auto dirs_it = j.find("project_library_dirs"); dirs_it != j.end() && dirs_it->is_array()) {
            s.project_library_dirs.clear();
            for (const auto& d : *dirs_it) {
                if (d.is_string()) {
                    const std::string& p = d.get_ref<const std::string&>();
                    if (!p.empty()) {
                        s.project_library_dirs.push_back(p);
                    }
                }
            }
        }

        if (const auto rec_it = j.find("recent_projects"); rec_it != j.end() && rec_it->is_array()) {
            s.recent_projects.clear();
            for (const auto& rj : *rec_it) {
                if (!rj.is_object() || s.recent_projects.size() >= SessionState::kMaxRecentProjects) {
                    continue;
                }
                RecentProjectEntry e{};
                e.path = rj.value("path", std::string{});
                e.project_uuid = rj.value("project_uuid", std::string{});
                e.project_name = rj.value("project_name", std::string{});
                e.pinned = rj.value("pinned", false);
                e.last_opened_utc = rj.value("last_opened_utc", std::string{});
                e.avatar_display_name = rj.value("avatar_display_name", std::string{});
                e.avatar_format = rj.value("avatar_format", std::string{});
                e.active_alfx_name = rj.value("active_alfx_name", std::string{});
                e.active_light_count = rj.value("active_light_count", 0u);
                e.render_tier = rj.value("render_tier", std::string{"HybridRT"});
                e.has_thumbnail = rj.value("has_thumbnail", false);
                if (!e.path.empty()) {
                    s.recent_projects.push_back(std::move(e));
                }
            }
        }

        if (const auto wg_it = j.find("window_geometry"); wg_it != j.end() && wg_it->is_object()) {
            s.window_geometry.x = wg_it->value("x", s.window_geometry.x);
            s.window_geometry.y = wg_it->value("y", s.window_geometry.y);
            s.window_geometry.width = std::max(640, wg_it->value("width", s.window_geometry.width));
            s.window_geometry.height = std::max(480, wg_it->value("height", s.window_geometry.height));
            s.window_geometry.maximized = wg_it->value("maximized", s.window_geometry.maximized);
            s.window_geometry.valid = wg_it->value("valid", s.window_geometry.valid);
        }

        s.imgui_dock_layout_path = j.value("imgui_dock_layout_path", s.imgui_dock_layout_path);
        s.preferred_gpu_luid = j.value("preferred_gpu_luid", s.preferred_gpu_luid);
        s.preferred_gpu_name = j.value("preferred_gpu_name", s.preferred_gpu_name);
        s.render_backend = j.value("render_backend", s.render_backend);
        s.lighting_tier =
            static_cast<uint8_t>(std::clamp(j.value("lighting_tier", static_cast<int>(s.lighting_tier)), 0, 2));
        s.preferred_camera_link = j.value("preferred_camera_link", s.preferred_camera_link);
        s.preferred_mic_id = j.value("preferred_mic_id", s.preferred_mic_id);
        s.ws_auth_token = j.value("ws_auth_token", s.ws_auth_token);
        s.telemetry_show_substats = j.value("telemetry_show_substats", s.telemetry_show_substats);
        s.settings_revision = j.value("settings_revision", s.settings_revision);
        if (const auto ap_it = j.find("app_params"); ap_it != j.end() && ap_it->is_object()) {
            s.app_params.clear();
            for (auto it = ap_it->begin(); it != ap_it->end(); ++it) {
                if (it.value().is_number()) {
                    s.app_params[it.key()] = it.value().get<double>();
                } else if (it.value().is_boolean()) {
                    s.app_params[it.key()] = it.value().get<bool>() ? 1.0 : 0.0;
                }
            }
        }
        s.wizard_completed = j.value("wizard_completed", s.wizard_completed);
        s.hw_inventory_hash = j.value("hw_inventory_hash", s.hw_inventory_hash);
        s.migrated_from_legacy_toml = j.value("migrated_from_legacy_toml", s.migrated_from_legacy_toml);
        return {};
    } catch (const std::exception& e) {
        return fail(ErrorCode::ParseError, std::string("Failed parsing session_state.json: ") + e.what());
    }
}

[[nodiscard]] bool is_iso_utc_newer(std::string_view candidate, std::string_view baseline) noexcept {
    if (candidate.empty()) {
        return false;
    }
    if (baseline.empty()) {
        return true;
    }
    return candidate >= baseline;
}

} // namespace

AprIoWorker::~AprIoWorker() {
    stop();
}

void AprIoWorker::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
#if defined(_WIN32)
    wake_event_ = static_cast<void*>(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
#endif
    thread_ = std::jthread([this](const std::stop_token& st) { worker_main(st); });
}

void AprIoWorker::stop() noexcept {
    if (!started_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.request_stop();
        wake_worker();
        thread_.join();
    }
    WorkerJob* stale_req = nullptr;
    while (req_queue_.try_pop(stale_req)) {
        delete stale_req;
    }
    AprWorkerResult* stale_res = nullptr;
    while (res_queue_.try_pop(stale_res)) {
        delete stale_res;
    }
#if defined(_WIN32)
    if (wake_event_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(wake_event_));
        wake_event_ = nullptr;
    }
#endif
}

void AprIoWorker::wake_worker() noexcept {
#if defined(_WIN32)
    if (wake_event_ != nullptr) {
        ::SetEvent(static_cast<HANDLE>(wake_event_));
    }
#endif
}

bool AprIoWorker::enqueue_save(std::filesystem::path dst_apr_path, const StudioStateStore& state_snapshot,
                               const renderer::LightingRig& rig_snapshot, std::string_view active_alfx_name,
                               std::vector<uint8_t> thumbnail_png, AprWorkerOp op) {
    if (!started_.load(std::memory_order_acquire)) {
        return false;
    }
    auto* job = new WorkerJob{};
    job->op = op;
    job->dst_path = std::move(dst_apr_path);
    job->state_snapshot = state_snapshot;
    job->rig_snapshot = rig_snapshot;
    job->active_alfx_name = std::string(active_alfx_name);
    job->thumbnail_png = std::move(thumbnail_png);
    if (!req_queue_.try_push(job)) {
        delete job;
        return false;
    }
    wake_worker();
    return true;
}

bool AprIoWorker::enqueue_scan(std::vector<std::filesystem::path> library_dirs,
                               std::vector<RecentProjectEntry> recent_entries) {
    if (!started_.load(std::memory_order_acquire)) {
        return false;
    }
    auto* job = new WorkerJob{};
    job->op = AprWorkerOp::ScanLibraries;
    job->scan_dirs = std::move(library_dirs);
    job->recent_entries = std::move(recent_entries);
    if (!req_queue_.try_push(job)) {
        delete job;
        return false;
    }
    wake_worker();
    return true;
}

bool AprIoWorker::try_pop_result(AprWorkerResult& out_result) {
    AprWorkerResult* ptr = nullptr;
    if (!res_queue_.try_pop(ptr) || ptr == nullptr) {
        return false;
    }
    out_result = std::move(*ptr);
    delete ptr;
    return true;
}

void AprIoWorker::worker_main(const std::stop_token& stoken) {
    while (!stoken.stop_requested()) {
        WorkerJob* job = nullptr;
        while (req_queue_.try_pop(job)) {
            if (job == nullptr) {
                continue;
            }
            auto* res = new AprWorkerResult{};
            res->op = job->op;
            res->target_path = job->dst_path;
            res->snapshot_dirty_generation = job->state_snapshot.dirty_generation;

            if (job->op == AprWorkerOp::ScanLibraries) {
                res->scanned_cards = SessionManager::scan_project_cards(job->scan_dirs, job->recent_entries);
                res->success = true;
            } else {
                lighting::StudioLightingStore store(job->rig_snapshot);
                store.set_active_alfx_name(job->active_alfx_name);
                AlfxMetadata meta{};
                meta.name = job->active_alfx_name;
                AprSaveInput input{};
                input.state = &job->state_snapshot;
                input.lighting = &store;
                input.rust_host = nullptr; // Snapshot already contains serialized node_graph_json.
                input.lighting_meta = &meta;
                input.preencoded_thumbnail_png = job->thumbnail_png;
                auto saved = AprSerializer::save_file(job->dst_path, input);
                if (saved) {
                    res->success = true;
                    res->manifest = std::move(saved.value());
                } else {
                    res->success = false;
                    res->error_message = std::string(saved.error().what());
                }
            }

            delete job;
            job = nullptr;
            if (!res_queue_.try_push(res)) {
                delete res;
            }
        }
#if defined(_WIN32)
        if (!stoken.stop_requested() && wake_event_ != nullptr) {
            ::WaitForSingleObject(static_cast<HANDLE>(wake_event_), 150u);
        }
#else
        break;
#endif
    }
}

Expected<void> SessionManager::init(const RuntimePaths& paths) {
    state_ = SessionState{};
    session_dir_ =
        paths.app_data_dir.empty() ? std::filesystem::current_path() / "userdata" : path_from_utf8(paths.app_data_dir);
    std::error_code ec;
    std::filesystem::create_directories(session_dir_, ec);
    if (ec) {
        return fail(ErrorCode::IoError, "Failed creating session_dir: " + ec.message());
    }
    session_state_path_ = session_dir_ / "session_state.json";
    session_lock_path_ = session_dir_ / "session.lock";
    autosave_dir_ = session_dir_ / "autosave";
    std::filesystem::create_directories(autosave_dir_, ec);

    if (!paths.default_projects_dir.empty()) {
        state_.project_library_dirs.push_back(paths.default_projects_dir);
    } else {
        state_.project_library_dirs.push_back(utf8_from_path(session_dir_ / "Projects"));
    }

    if (std::filesystem::exists(session_state_path_, ec)) {
        auto bytes = read_file_bytes(session_state_path_);
        if (!bytes) {
            return make_unexpected(bytes.error());
        }
        auto parsed = deserialize_session_state(bytes.value(), state_);
        if (!parsed) {
            return parsed;
        }
        if (state_.project_library_dirs.empty() && !paths.default_projects_dir.empty()) {
            state_.project_library_dirs.push_back(paths.default_projects_dir);
        }
        return {};
    }

    // One-time migration from legacy settings.toml (%APPDATA% or %LOCALAPPDATA%\AetherStudio).
    if (std::filesystem::exists(session_dir_ / "settings.toml", ec)) {
        perform_legacy_toml_migration(session_dir_);
    } else if (!paths.legacy_app_data_dir.empty()) {
        const std::filesystem::path legacy_dir = path_from_utf8(paths.legacy_app_data_dir);
        if (std::filesystem::exists(legacy_dir / "settings.toml", ec)) {
            perform_legacy_toml_migration(legacy_dir);
        }
    }

    return save_session_state();
}

void SessionManager::perform_legacy_toml_migration(const std::filesystem::path& legacy_dir) {
    auto bytes = read_file_bytes(legacy_dir / "settings.toml");
    if (bytes) {
        const std::string_view text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
        std::string section;
        size_t pos = 0u;
        uint32_t legacy_rev = 1u;
        while (pos < text.size()) {
            size_t eol = text.find('\n', pos);
            if (eol == std::string_view::npos) {
                eol = text.size();
            }
            const std::string line = trim_copy(text.substr(pos, eol - pos));
            pos = eol + 1u;
            if (line.empty() || line[0] == '#') {
                continue;
            }
            if (line.front() == '[' && line.back() == ']') {
                section = trim_copy(std::string_view(line).substr(1u, line.size() - 2u));
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = unquote_toml(trim_copy(std::string_view(line).substr(0u, eq)));
            const std::string val = trim_copy(std::string_view(line).substr(eq + 1u));
            if (section.empty() || section == "app") {
                if (key == "wizard_completed") {
                    state_.wizard_completed = (val == "true");
                } else if (key == "hw_inventory_hash") {
                    state_.hw_inventory_hash = std::strtoull(val.c_str(), nullptr, 10);
                } else if (key == "preferred_gpu_luid") {
                    state_.preferred_gpu_luid = unquote_toml(val);
                } else if (key == "preferred_gpu_name") {
                    state_.preferred_gpu_name = unquote_toml(val);
                } else if (key == "preferred_camera_link") {
                    state_.preferred_camera_link = unquote_toml(val);
                } else if (key == "preferred_mic_id") {
                    state_.preferred_mic_id = unquote_toml(val);
                } else if (key == "ws_auth_token") {
                    state_.ws_auth_token = unquote_toml(val);
                } else if (key == "telemetry_show_substats") {
                    state_.telemetry_show_substats = (val == "true");
                } else if (key == "settings_revision") {
                    legacy_rev = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
                } else if (key == "active_project_path") {
                    const std::string p = unquote_toml(val);
                    if (has_apr_extension(path_from_utf8(p))) {
                        state_.last_opened_project_path = p;
                    }
                }
            } else if (section == "params") {
                const double parsed = (val == "true")    ? 1.0
                                      : (val == "false") ? 0.0
                                                         : std::strtod(val.c_str(), nullptr);
                state_.app_params[key] = parsed;
                if (key == "render.backend") {
                    const int bi = static_cast<int>(parsed);
                    state_.render_backend = (bi == 1) ? "dx11" : ((bi == 2) ? "dx12" : "vulkan");
                } else if (key == "render.lighting_mode") {
                    const int lm = std::clamp(static_cast<int>(parsed), 0, 2);
                    state_.lighting_tier = static_cast<uint8_t>(lm);
                }
            } else if (section == "recent") {
                const std::string rp = unquote_toml(val);
                if (!rp.empty() && has_apr_extension(path_from_utf8(rp)) &&
                    state_.recent_projects.size() < SessionState::kMaxRecentProjects) {
                    RecentProjectEntry rec{};
                    rec.path = rp;
                    rec.project_name = utf8_from_path(path_from_utf8(rp).stem());
                    state_.recent_projects.push_back(std::move(rec));
                }
            }
        }
        if (legacy_rev < 2u) {
            if (auto it = state_.app_params.find("tracking.ep_mode");
                it != state_.app_params.end() && static_cast<int>(it->second) == 4) {
                it->second = 0.0;
            }
        }
        state_.settings_revision = 2u;
        state_.migrated_from_legacy_toml = true;
    }

    // Copy legacy keymap.toml & layouts/ once if absent in %APPDATA%\AETHER-2.5D-Studio\.
    std::error_code ec;
    if (std::filesystem::exists(legacy_dir / "keymap.toml", ec) &&
        !std::filesystem::exists(session_dir_ / "keymap.toml", ec)) {
        (void)std::filesystem::copy_file(legacy_dir / "keymap.toml", session_dir_ / "keymap.toml", ec);
    }
    if (std::filesystem::is_directory(legacy_dir / "layouts", ec) &&
        !std::filesystem::exists(session_dir_ / "layouts", ec)) {
        std::filesystem::create_directories(session_dir_ / "layouts", ec);
        for (const auto& entry : std::filesystem::directory_iterator(legacy_dir / "layouts", ec)) {
            if (entry.is_regular_file(ec)) {
                (void)std::filesystem::copy_file(entry.path(), session_dir_ / "layouts" / entry.path().filename(), ec);
            }
        }
    }
}

Expected<void> SessionManager::save_session_state() const {
    if (session_state_path_.empty()) {
        return fail(ErrorCode::InvalidArg, "SessionManager not initialized");
    }
    try {
        const std::string json_str = serialize_session_state(state_).dump(2);
        const std::vector<uint8_t> bytes(json_str.begin(), json_str.end());
        return write_bytes_atomic(session_state_path_, bytes);
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("Failed serializing session_state.json: ") + e.what());
    }
}

void SessionManager::record_recent_project(const std::filesystem::path& apr_path, const AprManifest& manifest) {
    const std::string key = normalized_key(apr_path);
    bool was_pinned = false;
    auto it = std::find_if(state_.recent_projects.begin(), state_.recent_projects.end(),
                           [&](const RecentProjectEntry& e) { return normalized_key(path_from_utf8(e.path)) == key; });
    if (it != state_.recent_projects.end()) {
        was_pinned = it->pinned;
        state_.recent_projects.erase(it);
    }

    RecentProjectEntry entry{};
    entry.path = utf8_from_path(apr_path);
    entry.project_uuid = manifest.project_uuid;
    entry.project_name = manifest.project_name.empty() ? utf8_from_path(apr_path.stem()) : manifest.project_name;
    entry.pinned = was_pinned;
    entry.last_opened_utc =
        manifest.modified_utc.empty() ? AprSerializer::current_utc_timestamp() : manifest.modified_utc;
    entry.avatar_display_name = manifest.summary.avatar_display_name;
    entry.avatar_format = manifest.summary.avatar_format;
    entry.active_alfx_name = manifest.summary.active_alfx_name;
    entry.active_light_count = manifest.summary.active_light_count;
    entry.render_tier = manifest.summary.render_tier;
    entry.has_thumbnail = manifest.summary.has_thumbnail;

    state_.recent_projects.insert(state_.recent_projects.begin(), entry);
    while (state_.recent_projects.size() > SessionState::kMaxRecentProjects) {
        auto rit = std::find_if(state_.recent_projects.rbegin(), state_.recent_projects.rend(),
                                [](const RecentProjectEntry& e) { return !e.pinned; });
        if (rit != state_.recent_projects.rend()) {
            state_.recent_projects.erase(std::next(rit).base());
        } else {
            state_.recent_projects.pop_back();
        }
    }
    state_.last_opened_project_path = entry.path;
}

bool SessionManager::set_project_pinned(const std::filesystem::path& apr_path, bool pinned) {
    const std::string key = normalized_key(apr_path);
    for (auto& e : state_.recent_projects) {
        if (normalized_key(path_from_utf8(e.path)) == key) {
            e.pinned = pinned;
            return true;
        }
    }
    return false;
}

void SessionManager::remove_recent_project(const std::filesystem::path& apr_path) {
    const std::string key = normalized_key(apr_path);
    state_.recent_projects.erase(
        std::remove_if(state_.recent_projects.begin(), state_.recent_projects.end(),
                       [&](const RecentProjectEntry& e) { return normalized_key(path_from_utf8(e.path)) == key; }),
        state_.recent_projects.end());
    if (normalized_key(path_from_utf8(state_.last_opened_project_path)) == key) {
        state_.last_opened_project_path.clear();
    }
}

void SessionManager::add_library_dir(const std::filesystem::path& dir) {
    const std::string key = normalized_key(dir);
    for (const auto& existing : state_.project_library_dirs) {
        if (normalized_key(path_from_utf8(existing)) == key) {
            return;
        }
    }
    state_.project_library_dirs.push_back(utf8_from_path(dir));
}

std::filesystem::path SessionManager::autosave_path_for(std::string_view project_uuid) const {
    const std::string safe_id = project_uuid.empty() ? std::string("untitled") : std::string(project_uuid);
    return autosave_dir_ / (safe_id + ".apr.autosave");
}

Expected<void> SessionManager::acquire_session_lock(std::string_view project_uuid,
                                                    const std::filesystem::path& project_path) const {
    if (session_lock_path_.empty()) {
        return fail(ErrorCode::InvalidArg, "SessionManager not initialized");
    }
    try {
        const nlohmann::json j{
            {"pid", current_process_id()},
            {"started_utc", AprSerializer::current_utc_timestamp()},
            {"project_uuid", std::string(project_uuid)},
            {"project_path", utf8_from_path(project_path)},
        };
        const std::string str = j.dump(2);
        return write_bytes_atomic(session_lock_path_, std::vector<uint8_t>(str.begin(), str.end()));
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("Failed writing session.lock: ") + e.what());
    }
}

void SessionManager::release_session_lock() const noexcept {
    if (session_lock_path_.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(session_lock_path_, ec);
    const std::filesystem::path bak(session_lock_path_.wstring() + L".bak");
    std::filesystem::remove(bak, ec);
}

void SessionManager::remove_autosave_for(std::string_view project_uuid) const {
    if (autosave_dir_.empty()) {
        return;
    }
    std::error_code ec;
    const std::filesystem::path p = autosave_path_for(project_uuid);
    std::filesystem::remove(p, ec);
    const std::filesystem::path bak(p.wstring() + L".bak");
    std::filesystem::remove(bak, ec);
}

uint32_t SessionManager::current_process_id() noexcept {
#if defined(_WIN32)
    return static_cast<uint32_t>(::GetCurrentProcessId());
#else
    return 1u;
#endif
}

bool SessionManager::is_process_alive(uint32_t pid) noexcept {
    if (pid == 0u) {
        return false;
    }
#if defined(_WIN32)
    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (proc == nullptr) {
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exit_code = 0u;
    const BOOL ok_exit = ::GetExitCodeProcess(proc, &exit_code);
    ::CloseHandle(proc);
    return ok_exit != FALSE && exit_code == STILL_ACTIVE;
#else
    return false;
#endif
}

CrashRecoveryInfo SessionManager::inspect_crash_recovery(bool force_treat_pid_dead_for_test) const {
    CrashRecoveryInfo info{};
    std::error_code ec;
    if (session_lock_path_.empty() || !std::filesystem::exists(session_lock_path_, ec)) {
        return info;
    }
    auto bytes = read_file_bytes(session_lock_path_);
    if (!bytes) {
        return info;
    }
    try {
        const nlohmann::json j = nlohmann::json::parse(bytes.value().begin(), bytes.value().end());
        if (!j.is_object()) {
            return info;
        }
        info.orphan_pid = j.value("pid", 0u);
        info.lock_started_utc = j.value("started_utc", std::string{});
        info.project_uuid = j.value("project_uuid", std::string{});
        const std::string proj_p = j.value("project_path", std::string{});
        if (!proj_p.empty()) {
            info.original_apr_path = path_from_utf8(proj_p);
        }

        if (!force_treat_pid_dead_for_test && is_process_alive(info.orphan_pid)) {
            return info; // Another active instance holds the lock cleanly.
        }

        const std::filesystem::path candidate = autosave_path_for(info.project_uuid);
        if (!std::filesystem::exists(candidate, ec)) {
            return info;
        }
        auto auto_man = AprSerializer::read_manifest_fast(candidate);
        if (!auto_man) {
            return info;
        }
        info.autosave_apr_path = candidate;
        info.autosave_modified_utc = auto_man.value().modified_utc;
        info.project_name = auto_man.value().project_name;

        bool newer_than_original = true;
        if (!info.original_apr_path.empty() && std::filesystem::exists(info.original_apr_path, ec)) {
            if (auto orig_man = AprSerializer::read_manifest_fast(info.original_apr_path); orig_man) {
                info.original_modified_utc = orig_man.value().modified_utc;
                if (!info.autosave_modified_utc.empty() && !info.original_modified_utc.empty()) {
                    newer_than_original = is_iso_utc_newer(info.autosave_modified_utc, info.original_modified_utc);
                } else {
                    const auto ta = std::filesystem::last_write_time(candidate, ec);
                    const auto tb = std::filesystem::last_write_time(info.original_apr_path, ec);
                    newer_than_original = (ta >= tb);
                }
            }
        }
        info.detected = newer_than_original;
    } catch (...) {
        return info;
    }
    return info;
}

std::vector<ScannedProjectCard> SessionManager::scan_project_cards(std::span<const std::filesystem::path> library_dirs,
                                                                   std::span<const RecentProjectEntry> recent_entries) {
    std::vector<ScannedProjectCard> out;
    std::set<std::string> seen_keys;
    std::error_code ec;

    auto try_add = [&](const std::filesystem::path& p, bool pinned_hint) {
        if (p.empty() || !std::filesystem::is_regular_file(p, ec)) {
            return;
        }
        const std::string k = normalized_key(p);
        if (!seen_keys.insert(k).second) {
            return;
        }
        ScannedProjectCard card{};
        card.apr_path = p;
        card.pinned = pinned_hint;
        auto man = AprSerializer::read_manifest_fast(p, &card.thumbnail_png);
        if (!man) {
            return;
        }
        card.manifest = std::move(man.value());
        out.push_back(std::move(card));
    };

    for (const auto& r : recent_entries) {
        try_add(path_from_utf8(r.path), r.pinned);
    }

    for (const auto& dir : library_dirs) {
        if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && has_apr_extension(entry.path())) {
                const std::string ek = normalized_key(entry.path());
                if (seen_keys.find(ek) == seen_keys.end()) {
                    try_add(entry.path(), false);
                }
            }
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const ScannedProjectCard& a, const ScannedProjectCard& b) {
        if (a.pinned != b.pinned) {
            return a.pinned > b.pinned;
        }
        return a.manifest.modified_utc > b.manifest.modified_utc;
    });
    return out;
}

} // namespace aether::core
