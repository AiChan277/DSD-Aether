#include "app/application.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    try {
#if defined(_WIN32)
        (void)::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        (void)::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
        aether::app::AppLaunchOptions opts{};
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--smoke-frames") == 0 && (i + 1) < argc) {
                opts.smoke_frames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            } else if (std::strcmp(argv[i], "--require-validation") == 0) {
                opts.require_validation = true;
            } else if (std::strcmp(argv[i], "--avatar") == 0 && (i + 1) < argc) {
                opts.avatar_path = argv[++i];
            } else if (std::strcmp(argv[i], "--render-backend") == 0 && (i + 1) < argc) {
                opts.render_backend = argv[++i];
            } else if (std::strcmp(argv[i], "--smoke-reload-avatar-at") == 0 && (i + 1) < argc) {
                opts.smoke_reload_avatar_at = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
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
