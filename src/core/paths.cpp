#include "core/paths.hpp"

#if defined(_WIN32)
// clang-format off
#include <Windows.h>
#include <KnownFolders.h>
#include <Objbase.h>
#include <ShlObj.h>
// clang-format on
#endif

#include <filesystem>
#include <system_error>

namespace aether {

namespace {

#if defined(_WIN32)
std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) {
        return {};
    }
    const int needed =
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::filesystem::path known_folder_or_fallback(const KNOWNFOLDERID& folder_id) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(folder_id, 0, nullptr, &raw)) && raw != nullptr) {
        std::filesystem::path p(raw);
        ::CoTaskMemFree(raw);
        return p;
    }
    if (raw != nullptr) {
        ::CoTaskMemFree(raw);
    }
    return std::filesystem::current_path();
}

std::filesystem::path known_local_app_data() {
    return known_folder_or_fallback(FOLDERID_LocalAppData);
}

std::filesystem::path known_roaming_app_data() {
    return known_folder_or_fallback(FOLDERID_RoamingAppData);
}

std::filesystem::path known_documents_dir() {
    return known_folder_or_fallback(FOLDERID_Documents);
}
#endif

std::filesystem::path executable_directory() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len > 0 && len < buf.size()) {
        buf.resize(len);
        return std::filesystem::path(buf).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::filesystem::path find_assets_root(const std::filesystem::path& exe_dir) {
    std::error_code ec;
    // Check working directory and ancestors of exe_dir (handles build/<preset>/src/app & build/<preset>/tests).
    std::filesystem::path probe = exe_dir;
    for (int depth = 0; depth < 6; ++depth) {
        const auto cand = probe / "assets";
        if (std::filesystem::exists(cand / "fonts" / "Inter-Regular-tnum.ttf", ec)) {
            return std::filesystem::weakly_canonical(cand, ec);
        }
        if (!probe.has_parent_path() || probe == probe.parent_path()) {
            break;
        }
        probe = probe.parent_path();
    }

    const auto cwd_cand = std::filesystem::current_path(ec) / "assets";
    if (std::filesystem::exists(cwd_cand / "fonts" / "Inter-Regular-tnum.ttf", ec)) {
        return std::filesystem::weakly_canonical(cwd_cand, ec);
    }
    return exe_dir / "assets";
}

} // namespace

RuntimePaths discover_runtime_paths(const std::filesystem::path* exe_dir_override) {
    RuntimePaths out{};
    std::error_code ec;
    const std::filesystem::path exe_dir =
        (exe_dir_override != nullptr && !exe_dir_override->empty()) ? *exe_dir_override : executable_directory();
    out.exe_dir = wide_to_utf8(exe_dir.native());

    const bool portable = std::filesystem::exists(exe_dir / "portable.flag", ec);
    out.portable_mode = portable;

    const std::filesystem::path local_legacy = known_local_app_data() / "AetherStudio";
    const std::filesystem::path base_data =
        portable ? (exe_dir / "userdata") : (known_roaming_app_data() / "AETHER-2.5D-Studio");
    const std::filesystem::path base_runtime_cache = portable ? base_data : local_legacy;
    const std::filesystem::path default_projects =
        portable ? (base_data / "Projects") : (known_documents_dir() / "AETHER-2.5D Projects");

    const std::filesystem::path cache = base_runtime_cache / "Cache";
    const std::filesystem::path logs = base_runtime_cache / "Logs";
    const std::filesystem::path crash = base_runtime_cache / "CrashDumps";
    const std::filesystem::path assets = find_assets_root(exe_dir);

    std::filesystem::create_directories(base_data, ec);
    std::filesystem::create_directories(cache, ec);
    std::filesystem::create_directories(logs, ec);
    std::filesystem::create_directories(crash, ec);

    out.app_data_dir = wide_to_utf8(base_data.native());
    out.legacy_app_data_dir = wide_to_utf8(local_legacy.native());
    out.cache_dir = wide_to_utf8(cache.native());
    out.logs_dir = wide_to_utf8(logs.native());
    out.crash_dir = wide_to_utf8(crash.native());
    out.default_projects_dir = wide_to_utf8(default_projects.native());
    out.assets_dir = wide_to_utf8(assets.native());
    return out;
}

std::string resolve_asset_path(const RuntimePaths& paths, std::string_view relative_path) {
    const std::filesystem::path base(utf8_to_wide(paths.assets_dir));
    const std::filesystem::path rel(utf8_to_wide(relative_path));
    return wide_to_utf8((base / rel).lexically_normal().native());
}

bool ensure_directory_exists(std::string_view utf8_path) {
    std::error_code ec;
    return std::filesystem::create_directories(std::filesystem::path(utf8_to_wide(utf8_path)), ec) ||
           std::filesystem::exists(std::filesystem::path(utf8_to_wide(utf8_path)), ec);
}

} // namespace aether
