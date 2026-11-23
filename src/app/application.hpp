#pragma once

#include <cstdint>
#include <string>

namespace aether::app {

struct AppLaunchOptions {
    uint32_t smoke_frames{0}; // 0 = interactive GUI until closed; >0 = run N rendered frames and exit cleanly
    bool require_validation{false};
    std::string avatar_path{};          // Optional UTF-8 model path (.vrm/.glb/.fbx/.pmx/.pmd) preloaded at startup
    std::string project_path{};         // Optional UTF-8 .apr or legacy .aether project path opened at startup
    std::string alfx_path{};            // Optional UTF-8 .alfx lighting/FX preset opening the 4-mode import modal
    std::string render_backend{};       // "vulkan" | "dx11"; empty = persisted `render.backend` setting
    uint32_t smoke_reload_avatar_at{0}; // >0: reload `avatar_path` at this frame (exercises GC avatar retirement)
};

/** Runs AETHER-2.5D Studio Phase 1 shell; returns process exit code (0 = clean). */
int run_application(const AppLaunchOptions& opts);

} // namespace aether::app
