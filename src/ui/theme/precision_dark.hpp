#pragma once

#include "core/paths.hpp"
#include "ui/theme/tokens.hpp"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace aether::ui {

/**
 * Custom segregated-bin arena allocator for Dear ImGui (`§12.15`, `HR-01`).
 * Pre-allocates one aligned block at startup and recycles freed size classes via O(1)
 * free-lists so steady-state frames trigger zero OS heap calls (`malloc`/`new`).
 */
class ImGuiArenaAllocator {
  public:
    static constexpr size_t kArenaCapacityBytes = 16ULL * 1024ULL * 1024ULL;
    static constexpr size_t kMinBinShift = 5;  // 32 B
    static constexpr size_t kMaxBinShift = 24; // 16 MiB
    static constexpr size_t kNumBins = (kMaxBinShift - kMinBinShift) + 1U;

    bool init(size_t capacity_bytes = kArenaCapacityBytes) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] void* alloc(size_t size) noexcept;
    void free(void* ptr) noexcept;

    void mark_warmup_complete() noexcept;

    [[nodiscard]] size_t used_bytes() const noexcept { return cursor_; }
    [[nodiscard]] size_t live_bytes() const noexcept { return live_bytes_; }
    [[nodiscard]] size_t peak_live_bytes() const noexcept { return peak_live_bytes_; }
    [[nodiscard]] size_t capacity_bytes() const noexcept { return capacity_; }
    [[nodiscard]] size_t steady_bump_growth_bytes() const noexcept { return steady_bump_growth_; }
    [[nodiscard]] uint64_t os_fallback_allocs() const noexcept { return os_fallback_allocs_; }
    [[nodiscard]] uint64_t total_allocs() const noexcept { return total_allocs_; }

  private:
    struct BlockHeader {
        uint64_t magic{0};
        uint32_t bin_idx{0};
        uint32_t req_size{0};
        BlockHeader* next{nullptr};
    };

    uint8_t* base_{nullptr};
    size_t capacity_{0};
    size_t cursor_{0};
    size_t live_bytes_{0};
    size_t peak_live_bytes_{0};
    bool warmup_done_{false};
    size_t steady_bump_growth_{0};
    uint64_t os_fallback_allocs_{0};
    uint64_t total_allocs_{0};
    std::array<BlockHeader*, kNumBins> free_bins_{};
};

struct UiFontSet {
    ImFont* body{nullptr};
    ImFont* body_strong{nullptr};
    ImFont* caption{nullptr};
    ImFont* title{nullptr};
    ImFont* display{nullptr};
    ImFont* mono{nullptr};
};

/** Installs custom ImGui allocator callbacks before `ImGui::CreateContext()`. */
bool install_imgui_arena(ImGuiArenaAllocator& arena) noexcept;

/** Restores standard CRT `malloc`/`free` callbacks after `ImGui::DestroyContext()`. */
void restore_default_imgui_allocator() noexcept;

/** Applies Precision Dark visual style tokens cleanly onto `style` (`high_contrast` & `dpi_scale` safe to reapply). */
void apply_precision_dark_style(ImGuiStyle& style, float dpi_scale = 1.0f, bool high_contrast = false) noexcept;

/** Loads `Inter-*-tnum.ttf` + `JetBrainsMono-Regular.ttf` + merged `AetherIcons-Regular.ttf` + `NotoSansJP-Subset.ttf`.
 */
UiFontSet load_precision_dark_fonts(const RuntimePaths& paths, float dpi_scale = 1.0f);

/** Incremented by every `load_precision_dark_fonts()` call; text-metric caches key on it. */
[[nodiscard]] uint32_t ui_font_generation() noexcept;

/** Draws subtle multi-layer elevation shadow for popovers/modals/palette (`§12.3.4`). */
void draw_elevation_shadow(ImDrawList* draw_list, ImVec2 p_min, ImVec2 p_max, float rounding) noexcept;

/** Returns hairline width `max(1, round(dpi_scale))` physical pixels (`§12.14`). */
[[nodiscard]] float hairline_px(float dpi_scale) noexcept;

} // namespace aether::ui
