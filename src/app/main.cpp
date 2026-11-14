#include "app/application.hpp"

#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    try {
        aether::app::AppLaunchOptions opts{};
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--smoke-frames") == 0 && (i + 1) < argc) {
                opts.smoke_frames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            } else if (std::strcmp(argv[i], "--require-validation") == 0) {
                opts.require_validation = true;
            } else if (std::strcmp(argv[i], "--avatar") == 0 && (i + 1) < argc) {
                opts.avatar_path = argv[++i];
            }
        }
        return aether::app::run_application(opts);
    } catch (...) {
        return 1;
    }
}
