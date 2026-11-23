#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aether {

struct RuntimePaths {
    std::string exe_dir;
    std::string app_data_dir;        // %APPDATA%\AETHER-2.5D-Studio or <exe_dir>/userdata (portable.flag)
    std::string legacy_app_data_dir; // %LOCALAPPDATA%\AetherStudio (source for one-time migration)
    std::string cache_dir;           // %LOCALAPPDATA%\AetherStudio\Cache or <exe_dir>/userdata/Cache
    std::string logs_dir;
    std::string crash_dir;
    std::string default_projects_dir; // %USERPROFILE%\Documents\AETHER-2.5D Projects or <exe_dir>/userdata/Projects
    std::string assets_dir;
    bool portable_mode{false};
};

/** Discovers OS runtime/user/assets paths cleanly (§3.8, Rev 3.1 §5.1); safe to call before logging starts. */
[[nodiscard]] RuntimePaths discover_runtime_paths(const std::filesystem::path* exe_dir_override = nullptr);

/** Joins `relative_path` against `paths.assets_dir` (`config/...`, `assets/...`). */
[[nodiscard]] std::string resolve_asset_path(const RuntimePaths& paths, std::string_view relative_path);

/** Ensures directory exists; returns true if directory exists or was created cleanly. */
bool ensure_directory_exists(std::string_view utf8_path);

} // namespace aether
