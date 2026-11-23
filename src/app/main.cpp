#include "app/application.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <Windows.h>
#include <shellapi.h>
// clang-format on
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] bool has_ext_icase(std::string_view path, std::string_view ext) noexcept {
    if (path.size() < ext.size()) {
        return false;
    }
    const std::string_view tail = path.substr(path.size() - ext.size());
    for (size_t i = 0; i < ext.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(tail[i])) != std::tolower(static_cast<unsigned char>(ext[i]))) {
            return false;
        }
    }
    return true;
}

void assign_positional_path(std::string_view p, aether::app::AppLaunchOptions& opts) {
    if (has_ext_icase(p, ".apr") || has_ext_icase(p, ".aether")) {
        opts.project_path = std::string(p);
    } else if (has_ext_icase(p, ".alfx")) {
        opts.alfx_path = std::string(p);
    } else {
        opts.avatar_path = std::string(p);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
#if defined(_WIN32)
        (void)::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        (void)::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
        std::vector<std::string> args_u8;
#if defined(_WIN32)
        int wargc = 0;
        if (LPWSTR* wargv = ::CommandLineToArgvW(::GetCommandLineW(), &wargc)) {
            args_u8.reserve(static_cast<size_t>(wargc));
            for (int i = 0; i < wargc; ++i) {
                const int n = ::WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
                std::string s((n > 1) ? static_cast<size_t>(n - 1) : 0U, '\0');
                if (n > 1) {
                    (void)::WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
                }
                args_u8.push_back(std::move(s));
            }
            ::LocalFree(static_cast<HLOCAL>(wargv));
        }
#endif
        if (args_u8.empty()) {
            args_u8.reserve(static_cast<size_t>(argc));
            for (int i = 0; i < argc; ++i) {
                args_u8.emplace_back(argv[i] != nullptr ? argv[i] : "");
            }
        }

        aether::app::AppLaunchOptions opts{};
        for (size_t i = 1; i < args_u8.size(); ++i) {
            const std::string& arg = args_u8[i];
            if (arg == "--smoke-frames" && (i + 1) < args_u8.size()) {
                opts.smoke_frames = static_cast<uint32_t>(std::strtoul(args_u8[++i].c_str(), nullptr, 10));
            } else if (arg == "--require-validation") {
                opts.require_validation = true;
            } else if (arg == "--avatar" && (i + 1) < args_u8.size()) {
                opts.avatar_path = args_u8[++i];
            } else if (arg == "--project" && (i + 1) < args_u8.size()) {
                opts.project_path = args_u8[++i];
            } else if (arg == "--alfx" && (i + 1) < args_u8.size()) {
                opts.alfx_path = args_u8[++i];
            } else if (arg == "--render-backend" && (i + 1) < args_u8.size()) {
                opts.render_backend = args_u8[++i];
            } else if (arg == "--smoke-reload-avatar-at" && (i + 1) < args_u8.size()) {
                opts.smoke_reload_avatar_at = static_cast<uint32_t>(std::strtoul(args_u8[++i].c_str(), nullptr, 10));
            } else if (!arg.empty() && arg[0] != '-') {
                assign_positional_path(arg, opts);
            }
        }
#if defined(_WIN32)
        if (opts.smoke_frames > 0U && ::GetStdHandle(STD_OUTPUT_HANDLE) == nullptr) {
            if (::AttachConsole(ATTACH_PARENT_PROCESS) != FALSE) {
                FILE* fp = nullptr;
                (void)freopen_s(&fp, "CONOUT$", "w", stdout);
                (void)freopen_s(&fp, "CONOUT$", "w", stderr);
            }
        }
#endif
        return aether::app::run_application(opts);
    } catch (...) {
        return 1;
    }
}
