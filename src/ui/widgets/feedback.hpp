#pragma once

#include "ui/theme/precision_dark.hpp"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <functional>

namespace aether::ui {

enum class ToastSeverity : uint8_t {
    Info = 0,
    Success = 1,
    Warning = 2,
    Error = 3,
};

enum class ToastActionKind : uint8_t {
    None = 0,
    Undo = 1,
    Retry = 2,
    Details = 3,
    OpenWizard = 4,
};

struct ToastItem {
    bool active{false};
    ToastSeverity severity{ToastSeverity::Info};
    ToastActionKind action_kind{ToastActionKind::None};
    uint32_t id{0};
    double expires_at_s{0.0}; // 0.0 => sticky until dismissed (errors)
    std::array<char, 64> title{};
    std::array<char, 160> detail{};
    std::array<char, 32> action_label{};
};

/**
 * Fixed-capacity, allocation-free bottom-right Toast Stack (`§12.13`, `REQ-UI-020`).
 * Info/success: 5 s, warnings: 8 s, errors: sticky. At most 3 visible simultaneously.
 */
class ToastStack {
  public:
    static constexpr size_t kMaxToasts = 8;
    static constexpr size_t kMaxVisible = 3;

    void push(ToastSeverity sev, const char* title, const char* detail, double now_s,
              ToastActionKind action = ToastActionKind::None, const char* action_label = nullptr) noexcept;

    void dismiss(uint32_t toast_id) noexcept;
    void clear() noexcept;

    /** Renders up to 3 stacked toasts; returns invoked ToastActionKind (None if no action clicked). */
    [[nodiscard]] ToastActionKind draw(double now_s, const UiFontSet& fonts, float dpi_scale, bool reduce_motion);

    [[nodiscard]] size_t active_count() const noexcept;

  private:
    std::array<ToastItem, kMaxToasts> items_{};
    uint32_t next_id_{1};
};

/** Draws a 2 px top accent progress bar inside the active panel (`§12.13`). */
void draw_panel_progress_bar(const UiFontSet& fonts, float progress_01, const char* status_text, bool can_cancel,
                             bool* out_cancel_clicked = nullptr) noexcept;

/** Draws inline `what · consequence · action` error line (`§12.13`). */
bool draw_inline_error_banner(const UiFontSet& fonts, const char* what_happened, const char* consequence,
                              const char* action_label) noexcept;

/** Draws plain-text empty state centered in panel with one optional primary button (`§12.13`). */
bool draw_empty_state(const UiFontSet& fonts, const char* message, const char* action_label = nullptr) noexcept;

struct PluginConsentPrompt {
    bool open{false};
    std::array<char, 128> plugin_id{};
    std::array<char, 128> plugin_name{};
    std::array<char, 65> sha256_hex{};
    uint32_t capability_mask{0};
};

enum class ConsentDecision : uint8_t {
    Pending = 0,
    Deny = 1,
    TrustAndEnable = 2,
};

[[nodiscard]] ConsentDecision draw_plugin_consent_modal(PluginConsentPrompt& prompt, const UiFontSet& fonts) noexcept;

} // namespace aether::ui
