#include "ui/panels/viewport/camera_presets.hpp"

#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace aether::ui {

const char* builtin_framing_preset_name(int32_t index) noexcept {
    switch (index) {
    case 0:
        return "1: Bust / Streamer (Default)";
    case 1:
        return "2: Face Close-Up";
    case 2:
        return "3: Waist-Up / Half Body";
    case 3:
        return "4: Full Body";
    case 4:
        return "5: Stream Corner (Right)";
    case 5:
        return "6: Stream Corner (Left)";
    default:
        return "1: Bust / Streamer (Default)";
    }
}

const char* builtin_framing_preset_shortcut(int32_t index) noexcept {
    switch (index) {
    case 0:
        return "Ctrl+1";
    case 1:
        return "Ctrl+2";
    case 2:
        return "Ctrl+3";
    case 3:
        return "Ctrl+4";
    case 4:
        return "Ctrl+5";
    case 5:
        return "Ctrl+6";
    default:
        return "";
    }
}

CameraFramingPreset compute_builtin_framing_preset(int32_t index, float hips_y, float chest_y, float head_y,
                                                   const std::array<float, 3>& avatar_pos, float avatar_scale,
                                                   float viewport_aspect) {
    const int32_t idx = std::clamp(index, 0, static_cast<int32_t>(kBuiltinFramingPresetCount) - 1);
    const float sc = std::clamp(avatar_scale, 0.25f, 4.0f);
    const float base_hips = std::clamp(hips_y, 0.25f, 2.5f);
    const float base_chest = std::max(base_hips + 0.10f, chest_y);
    const float base_head = std::max(base_chest + 0.12f, head_y);

    const float hy = avatar_pos[1] + base_hips * sc;
    const float cy = avatar_pos[1] + base_chest * sc;
    const float heady = avatar_pos[1] + base_head * sc;
    const float foot_y = avatar_pos[1];
    const float chest_head_h = std::max(0.14f, heady - cy);
    const float crown_y = heady + std::max(0.14f * sc, chest_head_h * 0.72f);

    CameraFramingPreset p{};
    p.name = builtin_framing_preset_name(idx);
    p.builtin = true;
    p.builtin_index = idx;
    p.fovy_rad = 0.2805f; // ~85mm portrait telephoto lens
    const float half_tan = std::tan(p.fovy_rad * 0.5f);
    const float aspect = std::clamp(viewport_aspect, 0.5f, 3.0f);

    p.target_x = avatar_pos[0];
    p.target_z = avatar_pos[2];
    p.has_avatar_xform = false;

    switch (idx) {
    case 0: { // 1: Bust / Streamer (Default)
        const float vh = std::max(0.45f * sc, chest_head_h * 2.45f);
        p.distance = std::clamp((vh * 0.5f) / half_tan, 0.70f, 14.0f);
        p.target_y = cy + chest_head_h * 0.62f;
        p.yaw_rad = 0.08f;
        p.pitch_rad = 0.04f;
        break;
    }
    case 1: { // 2: Face Close-Up (horizontal eye-level framing without upward tilt)
        const float vh = std::max(0.28f * sc, chest_head_h * 1.48f);
        p.distance = std::clamp((vh * 0.5f) / half_tan, 0.50f, 10.0f);
        p.target_y = heady + chest_head_h * 0.14f;
        p.yaw_rad = 0.05f;
        p.pitch_rad = 0.0f;
        break;
    }
    case 2: { // 3: Waist-Up / Half Body
        const float vh = std::max(0.65f * sc, (crown_y - hy + 0.10f * sc) * 1.15f);
        p.distance = std::clamp((vh * 0.5f) / half_tan, 0.90f, 16.0f);
        p.target_y = 0.5f * (hy + crown_y);
        p.yaw_rad = 0.10f;
        p.pitch_rad = 0.04f;
        break;
    }
    case 3: { // 4: Full Body
        const float total_h = std::max(0.95f * sc, crown_y - foot_y);
        const float vh = total_h * 1.14f;
        p.distance = std::clamp((vh * 0.5f) / half_tan, 1.35f, 22.0f);
        p.target_y = foot_y + total_h * 0.50f;
        p.yaw_rad = 0.14f;
        p.pitch_rad = 0.05f;
        break;
    }
    case 4: { // 5: Stream Corner (Right) - avatar sits in bottom-right third, left 2/3 clear
        const float vh = std::max(0.60f * sc, (crown_y - cy + chest_head_h * 0.55f) * 1.25f);
        p.distance = std::clamp((vh * 0.5f) / half_tan, 0.90f, 15.0f);
        const float half_h = p.distance * half_tan;
        const float half_w = half_h * aspect;
        p.target_x = avatar_pos[0] - 0.56f * half_w;
        p.target_y = crown_y - 0.80f * half_h;
        p.yaw_rad = -0.14f;
        p.pitch_rad = 0.04f;
        break;
    }
    case 5: { // 6: Stream Corner (Left) - avatar sits in bottom-left third, right 2/3 clear
        const float vh = std::max(0.60f * sc, (crown_y - cy + chest_head_h * 0.55f) * 1.25f);
        p.distance = std::clamp((vh * 0.5f) / half_tan, 0.90f, 15.0f);
        const float half_h = p.distance * half_tan;
        const float half_w = half_h * aspect;
        p.target_x = avatar_pos[0] + 0.56f * half_w;
        p.target_y = crown_y - 0.80f * half_h;
        p.yaw_rad = 0.14f;
        p.pitch_rad = 0.04f;
        break;
    }
    default:
        break;
    }
    return p;
}

bool CameraPresetBank::load_from_file(std::string_view path) {
    custom_.clear();
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path p{path};
    if (!std::filesystem::exists(p, ec)) {
        return false;
    }
    try {
        std::ifstream ifs(p, std::ios::binary);
        if (!ifs.is_open()) {
            return false;
        }
        nlohmann::json doc = nlohmann::json::parse(ifs);
        if (!doc.contains("presets") || !doc["presets"].is_array()) {
            return false;
        }
        for (const auto& item : doc["presets"]) {
            if (!item.is_object()) {
                continue;
            }
            CameraFramingPreset cp{};
            cp.name = item.value("name", "Custom Preset");
            if (cp.name.empty()) {
                continue;
            }
            cp.builtin = false;
            cp.builtin_index = -1;
            cp.target_x = item.value("target_x", 0.0f);
            cp.target_y = item.value("target_y", 0.85f);
            cp.target_z = item.value("target_z", 0.0f);
            cp.yaw_rad = item.value("yaw_rad", 0.08f);
            cp.pitch_rad = item.value("pitch_rad", 0.04f);
            cp.distance = std::clamp(item.value("distance", 2.16f), 0.35f, 30.0f);
            cp.fovy_rad = std::clamp(item.value("fovy_rad", 0.2805f), 0.10f, 1.80f);
            if (item.contains("avatar_pos") && item["avatar_pos"].is_array() && item["avatar_pos"].size() == 3) {
                cp.avatar_pos[0] = item["avatar_pos"][0].get<float>();
                cp.avatar_pos[1] = item["avatar_pos"][1].get<float>();
                cp.avatar_pos[2] = item["avatar_pos"][2].get<float>();
            }
            cp.avatar_rot_y_deg = item.value("avatar_rot_y_deg", 0.0f);
            cp.avatar_scale = std::clamp(item.value("avatar_scale", 1.0f), 0.25f, 4.0f);
            cp.has_avatar_xform = item.value("has_avatar_xform", true);
            custom_.push_back(std::move(cp));
        }
        return true;
    } catch (const std::exception& e) {
        AETHER_LOG_WARN("ui", "Failed to load camera presets from %s: %s", std::string(path).c_str(), e.what());
        return false;
    }
}

bool CameraPresetBank::save_to_file(std::string_view path) const {
    if (path.empty()) {
        return false;
    }
    try {
        const std::filesystem::path p{path};
        if (p.has_parent_path()) {
            ensure_directory_exists(p.parent_path().string());
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& cp : custom_) {
            arr.push_back({
                {"name", cp.name},
                {"target_x", cp.target_x},
                {"target_y", cp.target_y},
                {"target_z", cp.target_z},
                {"yaw_rad", cp.yaw_rad},
                {"pitch_rad", cp.pitch_rad},
                {"distance", cp.distance},
                {"fovy_rad", cp.fovy_rad},
                {"avatar_pos", {cp.avatar_pos[0], cp.avatar_pos[1], cp.avatar_pos[2]}},
                {"avatar_rot_y_deg", cp.avatar_rot_y_deg},
                {"avatar_scale", cp.avatar_scale},
                {"has_avatar_xform", cp.has_avatar_xform},
            });
        }
        const nlohmann::json doc = {{"version", 1}, {"presets", std::move(arr)}};
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return false;
        }
        ofs << doc.dump(2) << '\n';
        return ofs.good();
    } catch (const std::exception& e) {
        AETHER_LOG_WARN("ui", "Failed to save camera presets to %s: %s", std::string(path).c_str(), e.what());
        return false;
    }
}

size_t CameraPresetBank::save_or_overwrite(CameraFramingPreset preset, bool persist) {
    preset.builtin = false;
    preset.builtin_index = -1;
    for (size_t i = 0; i < custom_.size(); ++i) {
        if (custom_[i].name == preset.name) {
            custom_[i] = std::move(preset);
            if (persist) {
                (void)save_default();
            }
            return i;
        }
    }
    custom_.push_back(std::move(preset));
    if (persist) {
        (void)save_default();
    }
    return custom_.size() - 1;
}

bool CameraPresetBank::overwrite_at(size_t index, const CameraFramingPreset& updated, bool persist) {
    if (index >= custom_.size()) {
        return false;
    }
    CameraFramingPreset copy = updated;
    copy.name = custom_[index].name;
    copy.builtin = false;
    copy.builtin_index = -1;
    custom_[index] = std::move(copy);
    if (persist) {
        (void)save_default();
    }
    return true;
}

bool CameraPresetBank::remove_at(size_t index, bool persist) {
    if (index >= custom_.size()) {
        return false;
    }
    custom_.erase(custom_.begin() + static_cast<std::ptrdiff_t>(index));
    if (persist) {
        (void)save_default();
    }
    return true;
}

std::string resolve_camera_presets_json_path(const RuntimePaths& paths) {
    std::error_code ec;
    if (!paths.app_data_dir.empty()) {
        return (std::filesystem::path(paths.app_data_dir) / "camera_presets.json").string();
    }
    if (!paths.exe_dir.empty()) {
        const std::filesystem::path exe_cfg = std::filesystem::path(paths.exe_dir) / "config" / "camera_presets.json";
        if (std::filesystem::exists(exe_cfg, ec)) {
            return exe_cfg.string();
        }
    }
    return (std::filesystem::path("config") / "camera_presets.json").string();
}

} // namespace aether::ui
