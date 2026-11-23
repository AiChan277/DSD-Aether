#pragma once

#include "core/paths.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aether::ui {

struct CameraFramingPreset {
    std::string name;
    bool builtin{false};
    int32_t builtin_index{-1};
    float target_x{0.0f};
    float target_y{0.85f};
    float target_z{0.0f};
    float yaw_rad{0.08f};
    float pitch_rad{0.04f};
    float distance{2.16f};
    float fovy_rad{0.2805f};
    std::array<float, 3> avatar_pos{0.0f, 0.0f, 0.0f};
    float avatar_rot_y_deg{0.0f};
    float avatar_scale{1.0f};
    bool has_avatar_xform{false};
};

inline constexpr size_t kBuiltinFramingPresetCount = 6;

[[nodiscard]] const char* builtin_framing_preset_name(int32_t index) noexcept;
[[nodiscard]] const char* builtin_framing_preset_shortcut(int32_t index) noexcept;

/**
 * Computes a bone-relative camera framing preset from humanoid bone world-rest heights
 * (`hips_y`, `chest_y`, `head_y`) and active `AvatarRoot` offset/scale.
 */
[[nodiscard]] CameraFramingPreset
compute_builtin_framing_preset(int32_t index, float hips_y, float chest_y, float head_y,
                               const std::array<float, 3>& avatar_pos = {0.0f, 0.0f, 0.0f}, float avatar_scale = 1.0f,
                               float viewport_aspect = 16.0f / 9.0f);

/**
 * Manages user-saved custom camera + AvatarRoot presets stored in `config/camera_presets.json`.
 */
class CameraPresetBank {
  public:
    void set_storage_path(std::string path) { storage_path_ = std::move(path); }
    [[nodiscard]] const std::string& storage_path() const noexcept { return storage_path_; }

    bool load_from_file(std::string_view path);
    bool save_to_file(std::string_view path) const;
    bool load_default() { return !storage_path_.empty() && load_from_file(storage_path_); }
    bool save_default() const { return !storage_path_.empty() && save_to_file(storage_path_); }

    /** Adds or overwrites an existing preset matching `preset.name` (returns its index). */
    size_t save_or_overwrite(CameraFramingPreset preset, bool persist = true);
    bool overwrite_at(size_t index, const CameraFramingPreset& updated, bool persist = true);
    bool remove_at(size_t index, bool persist = true);

    [[nodiscard]] const std::vector<CameraFramingPreset>& custom_presets() const noexcept { return custom_; }
    [[nodiscard]] std::vector<CameraFramingPreset>& custom_presets_mut() noexcept { return custom_; }

  private:
    std::string storage_path_{"config/camera_presets.json"};
    std::vector<CameraFramingPreset> custom_;
};

[[nodiscard]] std::string resolve_camera_presets_json_path(const RuntimePaths& paths);

} // namespace aether::ui
