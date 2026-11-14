#pragma once

namespace aether::ui {

/**
 * UI string localization lookup wrapper (`§1.1`, `REQ-UI-020`).
 * Every visible UI string must flow through `tr(...)` so localization catalogs can
 * be plugged in without refactoring panel code.
 */
[[nodiscard]] inline const char* tr(const char* key) noexcept {
    return key != nullptr ? key : "";
}

} // namespace aether::ui
