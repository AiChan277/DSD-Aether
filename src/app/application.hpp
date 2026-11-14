#pragma once

#include <cstdint>
#include <string>

namespace aether::app {

struct AppLaunchOptions {
    uint32_t smoke_frames{0}; // 0 = interactive GUI until closed; >0 = run N rendered frames and exit cleanly
    bool require_validation{false};
    std::string avatar_path{}; // Optional UTF-8 model path (.vrm/.glb/.fbx/.pmx/.pmd) preloaded at startup
};

/** Runs AETHER-2.5D Studio Phase 1 shell; returns process exit code (0 = clean). */
int run_application(const AppLaunchOptions& opts);

} // namespace aether::app
