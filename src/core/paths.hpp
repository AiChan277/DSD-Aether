#pragma once

#include <string>
#include <string_view>

namespace aether {

struct RuntimePaths {
    std::string exe_dir;
    std::string app_data_dir;
    std::string cache_dir;
    std::string logs_dir;
    std::string crash_dir;
    std::string assets_dir;
    bool portable_mode{false};
};

/** Discovers OS runtime/user/assets paths cleanly (§3.8); safe to call before logging starts. */
[[nodiscard]] RuntimePaths discover_runtime_paths();

/** Joins a relative asset path (e.g. "fonts/Inter-Regular-tnum.ttf") under `paths.assets_dir`. */
[[nodiscard]] std::string resolve_asset_path(const RuntimePaths& paths, std::string_view relative_path);

/** Creates directory hierarchy recursively if absent (UTF-8 path). */
bool ensure_directory_exists(std::string_view utf8_path);

} // namespace aether
