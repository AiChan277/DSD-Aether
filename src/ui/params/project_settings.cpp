#include "ui/params/project_settings.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace aether::ui {
namespace {

inline constexpr const std::array<KeyBindingSpec, KeymapRegistry::kActionCount>& kKeySpecs = core::kCanonicalActions;

std::string trim_copy(std::string_view sv) {
    size_t start = 0;
    while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start])) != 0) {
        ++start;
    }
    size_t end = sv.size();
    while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1])) != 0) {
        --end;
    }
    return std::string(sv.substr(start, end - start));
}

std::string unquote_toml_str(std::string_view raw) {
    std::string t = trim_copy(raw);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        t = t.substr(1, t.size() - 2);
    }
    std::string out;
    out.reserve(t.size());
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\\' && i + 1 < t.size()) {
            const char nxt = t[++i];
            if (nxt == 'n') {
                out.push_back('\n');
            } else {
                out.push_back(nxt);
            }
        } else {
            out.push_back(t[i]);
        }
    }
    return out;
}

std::string quote_toml_str(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() + 4);
    out.push_back('"');
    for (const char ch : raw) {
        if (ch == '\\' || ch == '"') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

ImGuiKey token_to_imgui_key(std::string_view tok) noexcept {
    if (tok == "TAB") {
        return ImGuiKey_Tab;
    }
    if (tok == "SPACE") {
        return ImGuiKey_Space;
    }
    if (tok == "ENTER" || tok == "RETURN") {
        return ImGuiKey_Enter;
    }
    if (tok == "DEL" || tok == "DELETE") {
        return ImGuiKey_Delete;
    }
    if (tok == "ESC" || tok == "ESCAPE") {
        return ImGuiKey_Escape;
    }
    if (tok == ",") {
        return ImGuiKey_Comma;
    }
    if (tok == "/") {
        return ImGuiKey_Slash;
    }
    if (tok.size() == 1 && tok[0] >= '0' && tok[0] <= '9') {
        return static_cast<ImGuiKey>(ImGuiKey_0 + (tok[0] - '0'));
    }
    if (tok.size() == 1 && tok[0] >= 'A' && tok[0] <= 'Z') {
        return static_cast<ImGuiKey>(ImGuiKey_A + (tok[0] - 'A'));
    }
    if (tok.size() == 2 && tok[0] == 'F' && tok[1] >= '1' && tok[1] <= '9') {
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (tok[1] - '1'));
    }
    if (tok == "F10") {
        return ImGuiKey_F10;
    }
    if (tok == "F11") {
        return ImGuiKey_F11;
    }
    if (tok == "F12") {
        return ImGuiKey_F12;
    }
    return ImGuiKey_None;
}

} // namespace

KeymapRegistry::KeymapRegistry() {
    reset_defaults();
}

void KeymapRegistry::reset_defaults() {
    for (size_t i = 0; i < kActionCount; ++i) {
        chords_[i] = normalize_chord(kKeySpecs[i].default_chord);
        global_hotkeys_[i] = false;
    }
}

const std::string& KeymapRegistry::chord(ActionId id) const noexcept {
    const size_t idx = static_cast<size_t>(id);
    if (idx >= kActionCount) {
        static const std::string kEmpty{};
        return kEmpty;
    }
    return chords_[idx];
}

bool KeymapRegistry::global_hotkey_enabled(ActionId id) const noexcept {
    const size_t idx = static_cast<size_t>(id);
    return idx < kActionCount ? global_hotkeys_[idx] : false;
}

void KeymapRegistry::set_chord(ActionId id, const std::string& new_chord) {
    const size_t idx = static_cast<size_t>(id);
    if (idx < kActionCount) {
        chords_[idx] = normalize_chord(new_chord);
    }
}

void KeymapRegistry::set_global_hotkey_enabled(ActionId id, bool enabled) {
    const size_t idx = static_cast<size_t>(id);
    if (idx < kActionCount && kKeySpecs[idx].global_hotkey_capable) {
        global_hotkeys_[idx] = enabled;
    }
}

std::string KeymapRegistry::normalize_chord(std::string_view raw) {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    std::string main_key{};

    size_t pos = 0;
    while (pos <= raw.size()) {
        const size_t plus = raw.find('+', pos);
        const std::string part =
            trim_copy(plus == std::string_view::npos ? raw.substr(pos) : raw.substr(pos, plus - pos));
        std::string upper = part;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (upper == "CTRL" || upper == "CONTROL") {
            ctrl = true;
        } else if (upper == "SHIFT") {
            shift = true;
        } else if (upper == "ALT") {
            alt = true;
        } else if (!part.empty()) {
            if (part.size() == 1 && (part[0] == ',' || part[0] == '/')) {
                main_key = part;
            } else {
                main_key = upper;
                if (main_key == "RETURN") {
                    main_key = "ENTER";
                } else if (main_key == "DELETE") {
                    main_key = "DEL";
                }
            }
        }
        if (plus == std::string_view::npos) {
            break;
        }
        pos = plus + 1;
    }
    if (main_key.empty()) {
        return {};
    }
    std::string out;
    if (ctrl) {
        out += "Ctrl+";
    }
    if (alt) {
        out += "Alt+";
    }
    if (shift) {
        out += "Shift+";
    }
    if (main_key == "ENTER") {
        out += "Enter";
    } else if (main_key == "TAB") {
        out += "Tab";
    } else if (main_key == "SPACE") {
        out += "Space";
    } else if (main_key == "DEL") {
        out += "Del";
    } else {
        out += main_key;
    }
    return out;
}

std::vector<KeymapConflict> KeymapRegistry::detect_conflicts() const {
    std::vector<KeymapConflict> conflicts;
    for (size_t i = 0; i < kActionCount; ++i) {
        if (chords_[i].empty()) {
            continue;
        }
        for (size_t j = i + 1; j < kActionCount; ++j) {
            if (chords_[i] != chords_[j]) {
                continue;
            }
            const KeyScope sa = kKeySpecs[i].scope;
            const KeyScope sb = kKeySpecs[j].scope;
            if (sa == sb || sa == KeyScope::Global || sb == KeyScope::Global) {
                conflicts.push_back(KeymapConflict{kKeySpecs[i].id, kKeySpecs[j].id, chords_[i]});
            }
        }
    }
    return conflicts;
}

bool KeymapRegistry::is_pressed(ActionId id) const noexcept {
    if (ImGui::GetCurrentContext() == nullptr) {
        return false;
    }
    const std::string& ch = chord(id);
    if (ch.empty()) {
        return false;
    }
    bool need_ctrl = false;
    bool need_alt = false;
    bool need_shift = false;
    std::string_view key_part{};
    size_t pos = 0;
    while (pos <= ch.size()) {
        const size_t plus = ch.find('+', pos);
        const std::string_view part =
            plus == std::string::npos ? std::string_view(ch).substr(pos) : std::string_view(ch).substr(pos, plus - pos);
        if (part == "Ctrl") {
            need_ctrl = true;
        } else if (part == "Alt") {
            need_alt = true;
        } else if (part == "Shift") {
            need_shift = true;
        } else {
            key_part = part;
        }
        if (plus == std::string::npos) {
            break;
        }
        pos = plus + 1;
    }
    if (key_part.empty() || key_part.size() > 24U) {
        return false;
    }
    std::array<char, 24> upper_buf{};
    for (size_t i = 0; i < key_part.size(); ++i) {
        upper_buf[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(key_part[i])));
    }
    const ImGuiKey key = token_to_imgui_key(std::string_view(upper_buf.data(), key_part.size()));
    if (key == ImGuiKey_None) {
        return false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl != need_ctrl || io.KeyAlt != need_alt || io.KeyShift != need_shift) {
        return false;
    }
    return ImGui::IsKeyPressed(key, false);
}

bool KeymapRegistry::load_toml(const std::filesystem::path& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return false;
    }
    std::string section;
    std::string line;
    while (std::getline(ifs, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line.erase(hash);
        }
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            section = trim_copy(std::string_view(trimmed).substr(1, trimmed.size() - 2));
            continue;
        }
        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string k = unquote_toml_str(std::string_view(trimmed).substr(0, eq));
        const std::string v = trim_copy(std::string_view(trimmed).substr(eq + 1));
        for (size_t i = 0; i < kActionCount; ++i) {
            if (k == kKeySpecs[i].name) {
                if (section == "bindings" || section.empty()) {
                    chords_[i] = normalize_chord(unquote_toml_str(v));
                } else if (section == "global_hotkeys" && kKeySpecs[i].global_hotkey_capable) {
                    global_hotkeys_[i] = (v == "true");
                }
                break;
            }
        }
    }
    return true;
}

bool KeymapRegistry::save_toml(const std::filesystem::path& path) const {
    std::ostringstream oss;
    oss << "# AETHER-2.5D Studio Keymap (§12.11)\n";
    oss << "[bindings]\n";
    for (size_t i = 0; i < kActionCount; ++i) {
        oss << quote_toml_str(kKeySpecs[i].name) << " = " << quote_toml_str(chords_[i]) << "\n";
    }
    oss << "\n[global_hotkeys]\n";
    for (size_t i = 0; i < kActionCount; ++i) {
        if (kKeySpecs[i].global_hotkey_capable) {
            oss << quote_toml_str(kKeySpecs[i].name) << " = " << (global_hotkeys_[i] ? "true" : "false") << "\n";
        }
    }
    return atomic_write_text_file(path, oss.str());
}

const KeyBindingSpec& KeymapRegistry::spec(ActionId id) noexcept {
    const size_t idx = static_cast<size_t>(id);
    return kKeySpecs[idx < kActionCount ? idx : 0];
}

const std::array<KeyBindingSpec, KeymapRegistry::kActionCount>& KeymapRegistry::all_specs() noexcept {
    return kKeySpecs;
}

const char* KeymapRegistry::scope_name(KeyScope scope) noexcept {
    switch (scope) {
    case KeyScope::Global:
        return "Global";
    case KeyScope::Viewport:
        return "Viewport";
    case KeyScope::NodeGraph:
        return "Node Graph";
    case KeyScope::Outliner:
        return "Outliner";
    case KeyScope::Lighting:
        return "Lighting";
    }
    return "Global";
}

bool KeymapRegistry::is_raw_key_pressed(ImGuiKey key, bool repeat) noexcept {
    return ImGui::GetCurrentContext() != nullptr && ImGui::IsKeyPressed(key, repeat);
}

void AppSettings::record_recent_project(std::string_view path) {
    if (path.empty()) {
        return;
    }
    std::string p(path);
    recent_projects.erase(std::remove(recent_projects.begin(), recent_projects.end(), p), recent_projects.end());
    recent_projects.insert(recent_projects.begin(), std::move(p));
    if (recent_projects.size() > 10) {
        recent_projects.resize(10);
    }
}

std::filesystem::path default_config_dir() {
#if defined(_WIN32)
    wchar_t buf[1024]{};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        return std::filesystem::path(buf) / L"AETHER-2.5D-Studio";
    }
#endif
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / "AETHER-2.5D-Studio";
}

bool atomic_write_text_file(const std::filesystem::path& target, std::string_view utf8_content) {
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    const std::filesystem::path tmp = target.wstring() + L".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return false;
        }
        ofs.write(utf8_content.data(), static_cast<std::streamsize>(utf8_content.size()));
        ofs.flush();
        if (!ofs.good()) {
            return false;
        }
    }
#if defined(_WIN32)
    if (MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
#else
    std::filesystem::rename(tmp, target, ec);
    return !ec;
#endif
}

bool load_app_settings_toml(const std::filesystem::path& path, AppSettings& settings, ParamStore& params) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return false;
    }
    settings.recent_projects.clear();
    settings.settings_revision = 1; // files without the key predate revisioning
    std::string section;
    std::string line;
    while (std::getline(ifs, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line.erase(hash);
        }
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            section = trim_copy(std::string_view(trimmed).substr(1, trimmed.size() - 2));
            continue;
        }
        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = unquote_toml_str(std::string_view(trimmed).substr(0, eq));
        const std::string val = trim_copy(std::string_view(trimmed).substr(eq + 1));

        if (section == "app") {
            if (key == "wizard_completed") {
                settings.wizard_completed = (val == "true");
            } else if (key == "hw_inventory_hash") {
                settings.hw_inventory_hash = std::strtoull(val.c_str(), nullptr, 10);
            } else if (key == "preferred_camera_link") {
                settings.preferred_camera_link = unquote_toml_str(val);
            } else if (key == "preferred_mic_id") {
                settings.preferred_mic_id = unquote_toml_str(val);
            } else if (key == "preferred_gpu_luid") {
                settings.preferred_gpu_luid = unquote_toml_str(val);
            } else if (key == "preferred_gpu_name") {
                settings.preferred_gpu_name = unquote_toml_str(val);
            } else if (key == "active_project_path") {
                settings.active_project_path = unquote_toml_str(val);
            } else if (key == "ws_auth_token") {
                settings.ws_auth_token = unquote_toml_str(val);
            } else if (key == "telemetry_show_substats") {
                settings.telemetry_show_substats = (val == "true");
            } else if (key == "settings_revision") {
                settings.settings_revision = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
            }
        } else if (section == "params") {
            if (const ParamSpec* spec = find_param_by_id(key)) {
                if ((spec->flags & PARAM_FLAG_APP_SCOPE) != 0u) {
                    const double parsed = (val == "true")    ? 1.0
                                          : (val == "false") ? 0.0
                                                             : std::strtod(val.c_str(), nullptr);
                    (void)params.set(spec->pid, parsed);
                }
            }
        } else if (section == "recent") {
            settings.record_recent_project(unquote_toml_str(val));
        }
    }
    if (settings.settings_revision < 2U) {
        // Revision 2: older wizard builds persisted CPU (36 ms/frame) as the tracking EP. Restore the Auto cascade so
        // the router picks the NPU/GPU tier; a CPU choice made explicitly after this migration is kept.
        if (params.get_int(ParamId::TrackingEpMode) == 4) {
            (void)params.set(ParamId::TrackingEpMode, 0.0);
        }
    }
    settings.settings_revision = AppSettings::kCurrentRevision;
    return true;
}

bool save_app_settings_toml(const std::filesystem::path& path, const AppSettings& settings, const ParamStore& params) {
    std::ostringstream oss;
    oss << "# AETHER-2.5D Studio Application Settings (§12.12)\n";
    oss << "[app]\n";
    oss << "wizard_completed = " << (settings.wizard_completed ? "true" : "false") << "\n";
    oss << "hw_inventory_hash = " << settings.hw_inventory_hash << "\n";
    oss << "preferred_camera_link = " << quote_toml_str(settings.preferred_camera_link) << "\n";
    oss << "preferred_mic_id = " << quote_toml_str(settings.preferred_mic_id) << "\n";
    oss << "preferred_gpu_luid = " << quote_toml_str(settings.preferred_gpu_luid) << "\n";
    oss << "preferred_gpu_name = " << quote_toml_str(settings.preferred_gpu_name) << "\n";
    oss << "active_project_path = " << quote_toml_str(settings.active_project_path) << "\n";
    oss << "ws_auth_token = " << quote_toml_str(settings.ws_auth_token) << "\n";
    oss << "telemetry_show_substats = " << (settings.telemetry_show_substats ? "true" : "false") << "\n";
    oss << "settings_revision = " << AppSettings::kCurrentRevision << "\n\n";

    oss << "[params]\n";
    for (const ParamSpec& spec : all_param_specs()) {
        if ((spec.flags & (PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE)) != (PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE)) {
            continue;
        }
        const double v = params.get(spec.pid);
        oss << quote_toml_str(spec.id) << " = ";
        if (spec.type == ParamType::Bool) {
            oss << (v >= 0.5 ? "true" : "false") << "\n";
        } else if (spec.type == ParamType::Int || spec.type == ParamType::Enum) {
            oss << static_cast<long long>(std::llround(v)) << "\n";
        } else {
            oss << v << "\n";
        }
    }

    oss << "\n[recent]\n";
    for (size_t i = 0; i < settings.recent_projects.size(); ++i) {
        oss << "item_" << i << " = " << quote_toml_str(settings.recent_projects[i]) << "\n";
    }
    return atomic_write_text_file(path, oss.str());
}

void load_app_settings_from_session(const core::SessionState& session, AppSettings& settings, ParamStore& params) {
    settings.wizard_completed = session.wizard_completed;
    settings.hw_inventory_hash = session.hw_inventory_hash;
    settings.preferred_camera_link = session.preferred_camera_link;
    settings.preferred_mic_id = session.preferred_mic_id;
    settings.preferred_gpu_luid = session.preferred_gpu_luid;
    settings.preferred_gpu_name = session.preferred_gpu_name;
    settings.active_project_path = session.last_opened_project_path;
    if (!session.ws_auth_token.empty()) {
        settings.ws_auth_token = session.ws_auth_token;
    }
    settings.telemetry_show_substats = session.telemetry_show_substats;
    settings.settings_revision = std::max(session.settings_revision, AppSettings::kCurrentRevision);
    settings.recent_projects.clear();
    for (const auto& rec : session.recent_projects) {
        if (!rec.path.empty()) {
            settings.recent_projects.push_back(rec.path);
        }
    }

    for (const auto& [key, val] : session.app_params) {
        if (const ParamSpec* spec = find_param_by_id(key)) {
            if ((spec->flags & PARAM_FLAG_APP_SCOPE) != 0u) {
                (void)params.set(spec->pid, val);
            }
        }
    }
    if (session.app_params.find("render.backend") == session.app_params.end()) {
        const int b = (session.render_backend == "dx11") ? 1 : ((session.render_backend == "dx12") ? 2 : 0);
        (void)params.set(ParamId::RenderBackend, static_cast<double>(b));
    }
    if (session.settings_revision < 2u && params.get_int(ParamId::TrackingEpMode) == 4) {
        (void)params.set(ParamId::TrackingEpMode, 0.0);
    }
}

void save_app_settings_to_session(const AppSettings& settings, const ParamStore& params, core::SessionState& session) {
    session.wizard_completed = settings.wizard_completed;
    session.hw_inventory_hash = settings.hw_inventory_hash;
    session.preferred_camera_link = settings.preferred_camera_link;
    session.preferred_mic_id = settings.preferred_mic_id;
    session.preferred_gpu_luid = settings.preferred_gpu_luid;
    session.preferred_gpu_name = settings.preferred_gpu_name;
    if (!settings.active_project_path.empty()) {
        session.last_opened_project_path = settings.active_project_path;
    }
    session.ws_auth_token = settings.ws_auth_token;
    session.telemetry_show_substats = settings.telemetry_show_substats;
    session.settings_revision = AppSettings::kCurrentRevision;

    for (const ParamSpec& spec : all_param_specs()) {
        if ((spec.flags & (PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE)) != (PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE)) {
            continue;
        }
        session.app_params[std::string(spec.id)] = params.get(spec.pid);
    }
    const int backend = params.get_int(ParamId::RenderBackend);
    session.render_backend = (backend == 1) ? "dx11" : ((backend == 2) ? "dx12" : "vulkan");
    session.lighting_tier = static_cast<uint8_t>(std::clamp(params.get_int(ParamId::RenderLightingMode), 0, 2));
}

} // namespace aether::ui
