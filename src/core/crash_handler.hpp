#pragma once

#include <string_view>

namespace aether {

/**
 * Out-of-line minidump crash handler (`§4.8`): pre-spawns a dedicated low-priority
 * watcher thread sleeping on an auto-reset Win32 event and installs
 * `SetUnhandledExceptionFilter` so crash dumps require zero heap allocation at fault time.
 */
class CrashHandler {
  public:
    static bool install(std::string_view crash_dir_utf8) noexcept;
    static void shutdown() noexcept;
    [[nodiscard]] static bool is_installed() noexcept;
};

} // namespace aether
