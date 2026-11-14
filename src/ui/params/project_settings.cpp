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

constexpr std::array<KeyBindingSpec, KeymapRegistry::kActionCount> kKeySpecs = {{
    {ActionId::OpenPalette, "global.palette", "Command Palette", KeyScope::Global, "Ctrl+K", false},
    {ActionId::ModeLiveStudio, "global.mode_live", "Mode: Live Studio", KeyScope::Global, "Ctrl+1", false},
    {ActionId::ModeRigging, "global.mode_rigging", "Mode: Rigging", KeyScope::Global, "Ctrl+2", false},
    {ActionId::ModeNodes, "global.mode_nodes", "Mode: Nodes", KeyScope::Global, "Ctrl+3", false},
    {ActionId::ProjectNew, "file.new", "New Project", KeyScope::Global, "Ctrl+N", false},
    {ActionId::ProjectOpen, "file.open", "Open Project...", KeyScope::Global, "Ctrl+O", false},
    {ActionId::ProjectSave, "file.save", "Save Project", KeyScope::Global, "Ctrl+S", false},
    {ActionId::ProjectSaveAs, "file.save_as", "Save Project As...", KeyScope::Global, "Ctrl+Shift+S", false},
    {ActionId::ImportAvatar, "file.import_avatar", "Import Avatar...", KeyScope::Global, "Ctrl+I", false},
    {ActionId::Undo, "edit.undo", "Undo", KeyScope::Global, "Ctrl+Z", false},
    {ActionId::Redo, "edit.redo", "Redo", KeyScope::Global, "Ctrl+Y", false},
    {ActionId::Preferences, "app.preferences", "Preferences...", KeyScope::Global, "Ctrl+,", false},
    {ActionId::ShortcutOverlay, "app.shortcuts", "Keyboard Shortcuts", KeyScope::Global, "Ctrl+/", false},
    {ActionId::ToggleOnAir, "live.on_air", "Toggle ON AIR", KeyScope::Global, "Ctrl+Shift+L", false},
    {ActionId::PauseTracking, "tracking.pause", "Pause Tracking", KeyScope::Global, "Ctrl+P", false},
    {ActionId::CalibrateNeutral, "tracking.calibrate", "Calibrate Neutral", KeyScope::Global, "Ctrl+Shift+K", false},
    {ActionId::ToggleRayTracing, "render.toggle_rt", "Toggle Ray Tracing", KeyScope::Global, "Ctrl+Shift+R", false},
    {ActionId::MaximizeHoveredPanel, "workspace.maximize", "Maximize Hovered Panel", KeyScope::Global, "Shift+Space",
     false},
    {ActionId::Expression1, "expr.slot_1", "Trigger Expression 1 (Joy)", KeyScope::Global, "F1", true},
    {ActionId::Expression2, "expr.slot_2", "Trigger Expression 2 (Surprised)", KeyScope::Global, "F2", true},
    {ActionId::Expression3, "expr.slot_3", "Trigger Expression 3 (Sorrow)", KeyScope::Global, "F3", true},
    {ActionId::Expression4, "expr.slot_4", "Trigger Expression 4 (Angry)", KeyScope::Global, "F4", true},
    {ActionId::Expression5, "expr.slot_5", "Trigger Expression 5 (Fun)", KeyScope::Global, "F5", true},
    {ActionId::Expression6, "expr.slot_6", "Trigger Expression 6 (Wink L)", KeyScope::Global, "F6", true},
    {ActionId::Expression7, "expr.slot_7", "Trigger Expression 7 (Wink R)", KeyScope::Global, "F7", true},
    {ActionId::Expression8, "expr.slot_8", "Trigger Expression 8 (Relaxed)", KeyScope::Global, "F8", true},

    {ActionId::ViewportHeatmap, "viewport.heatmap", "Toggle Confidence Heatmap", KeyScope::Viewport, "Alt+C", false},
    {ActionId::ViewportOrbitOrb, "viewport.orbit_orb", "Show Light Orbit Orb", KeyScope::Viewport, "Alt+L", false},
    {ActionId::ViewportFrameHead, "viewport.frame_head", "Frame Head", KeyScope::Viewport, "F", false},
    {ActionId::ViewportFrameBody, "viewport.frame_body", "Frame Full Body", KeyScope::Viewport, "Shift+F", false},
    {ActionId::ViewportCam1, "viewport.cam_1", "Recall Camera 1", KeyScope::Viewport, "1", false},
    {ActionId::ViewportCam2, "viewport.cam_2", "Recall Camera 2", KeyScope::Viewport, "2", false},
    {ActionId::ViewportCam3, "viewport.cam_3", "Recall Camera 3", KeyScope::Viewport, "3", false},
    {ActionId::ViewportCam4, "viewport.cam_4", "Recall Camera 4", KeyScope::Viewport, "4", false},
    {ActionId::ViewportCam5, "viewport.cam_5", "Recall Camera 5", KeyScope::Viewport, "5", false},
    {ActionId::ViewportStoreCam1, "viewport.store_cam_1", "Store Camera 1", KeyScope::Viewport, "Ctrl+Alt+1", false},
    {ActionId::ViewportStoreCam2, "viewport.store_cam_2", "Store Camera 2", KeyScope::Viewport, "Ctrl+Alt+2", false},
    {ActionId::ViewportStoreCam3, "viewport.store_cam_3", "Store Camera 3", KeyScope::Viewport, "Ctrl+Alt+3", false},
    {ActionId::ViewportStoreCam4, "viewport.store_cam_4", "Store Camera 4", KeyScope::Viewport, "Ctrl+Alt+4", false},
    {ActionId::ViewportStoreCam5, "viewport.store_cam_5", "Store Camera 5", KeyScope::Viewport, "Ctrl+Alt+5", false},
    {ActionId::GizmoSelect, "viewport.gizmo_select", "Select Tool", KeyScope::Viewport, "Q", false},
    {ActionId::GizmoMove, "viewport.gizmo_move", "Move Tool", KeyScope::Viewport, "W", false},
    {ActionId::GizmoRotate, "viewport.gizmo_rotate", "Rotate Tool", KeyScope::Viewport, "E", false},
    {ActionId::GizmoScale, "viewport.gizmo_scale", "Scale Tool", KeyScope::Viewport, "R", false},
    {ActionId::GizmoLocalWorld, "viewport.gizmo_space", "Toggle Local/World", KeyScope::Viewport, "X", false},
    {ActionId::ViewportGrid, "viewport.grid", "Toggle Grid", KeyScope::Viewport, "G", false},

    {ActionId::GraphAddNode, "graph.add_node", "Add Node Menu", KeyScope::NodeGraph, "Tab", false},
    {ActionId::GraphDuplicate, "graph.duplicate", "Duplicate Selection", KeyScope::NodeGraph, "Ctrl+D", false},
    {ActionId::GraphDelete, "graph.delete", "Delete Selection", KeyScope::NodeGraph, "Del", false},
    {ActionId::GraphFrame, "graph.frame", "Frame Graph", KeyScope::NodeGraph, "F", false},
    {ActionId::GraphComment, "graph.comment", "Wrap in Comment Box", KeyScope::NodeGraph, "Ctrl+G", false},
    {ActionId::GraphBypass, "graph.bypass", "Toggle Bypass Selected", KeyScope::NodeGraph, "Ctrl+B", false},
    {ActionId::GraphRecompile, "graph.recompile", "Recompile Graph Now", KeyScope::NodeGraph, "Ctrl+Enter", false},

    {ActionId::OutlinerRename, "outliner.rename", "Rename Item", KeyScope::Outliner, "Shift+F2", false},
    {ActionId::OutlinerFilter, "outliner.filter", "Filter Outliner", KeyScope::Outliner, "Ctrl+F", false},
    {ActionId::OutlinerHide, "outliner.hide", "Hide Selected", KeyScope::Outliner, "H", false},
    {ActionId::OutlinerUnhideAll, "outliner.unhide_all", "Unhide All", KeyScope::Outliner, "Alt+H", false},
    {ActionId::OutlinerDelete, "outliner.delete", "Delete Selected", KeyScope::Outliner, "Del", false},
}};

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
    }
    return "Global";
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
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        return std::filesystem::path(buf) / L"Aether2.5D";
    }
#endif
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / "Aether2.5D";
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

} // namespace aether::ui
