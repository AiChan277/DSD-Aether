#pragma once

#include "renderer/denoise/denoiser_backend.hpp"
#include "renderer/denoise/svgf_lite_denoiser.hpp"
#include "renderer/denoise/vendor_denoisers.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace aether::renderer {

enum class DenoiserFallbackReason : uint8_t {
    None = 0,
    SdkUnavailable = 1,
    RuntimeError = 2,
    BudgetExceeded = 3,
    ManualOverride = 4,
};

/**
 * Owns both the active vendor denoiser and the pre-allocated built-in `SvgfLiteDenoiser` fallback (`§10.9`,
 * `ADR-0009`). Guarantees zero heap allocations (`HR-01`) during live execution and switches automatically to
 * `SvgfLiteDenoiser` if `denoise()` fails or if 120-frame denoiser `p95 > 0.8 ms`.
 */
class DenoiserManager {
  public:
    static constexpr float kDenoiserBudgetP95Ms = 0.8f;
    static constexpr uint32_t kWindowFrames = 120;

    DenoiserManager() = default;
    ~DenoiserManager() { destroy(); }

    DenoiserManager(const DenoiserManager&) = delete;
    DenoiserManager& operator=(const DenoiserManager&) = delete;

    [[nodiscard]] static DenoiserKind select_default_for_vendor(uint32_t vendor_id, bool is_software_adapter = false,
                                                                bool optix_available = false,
                                                                bool oidn_available = false) noexcept;

    [[nodiscard]] Expected<void> init(const rhi::RhiDevice& device, const rhi::BindlessHeap& bindless, uint32_t width,
                                      uint32_t height, DenoiserKind requested = DenoiserKind::Auto);
    [[nodiscard]] Expected<void> resize(uint32_t width, uint32_t height);
    void destroy() noexcept;

    /** Runs active backend; falls back to pre-allocated `SvgfLiteDenoiser` with zero allocs on failure. */
    [[nodiscard]] Expected<void> execute(VkCommandBuffer cmd, const DenoiserInputView& inputs) noexcept;

    /** Feeds latest resolved GPU denoiser duration (ms); triggers fallback when 120f p95 > 0.8 ms. */
    void observe_gpu_time_ms(float denoiser_ms) noexcept;

    /** Switches between already-initialized backends (`Auto` / `SvgfLite` / active vendor) without allocating. */
    [[nodiscard]] Expected<void> select_backend(DenoiserKind kind) noexcept;

    void inject_failure_next_execute() noexcept { inject_failure_ = true; }
    void reset_budget_monitor() noexcept;
    /** Invalidates temporal history state on avatar swap, resize, or camera cut. */
    void invalidate_temporal_history() noexcept;

    [[nodiscard]] DenoiserKind active_kind() const noexcept;
    [[nodiscard]] const char* active_name() const noexcept;
    [[nodiscard]] DenoiserFallbackReason fallback_reason() const noexcept { return fallback_reason_; }
    [[nodiscard]] bool fallback_triggered() const noexcept { return fallback_reason_ != DenoiserFallbackReason::None; }
    [[nodiscard]] float rolling_p95_ms() const noexcept { return rolling_p95_ms_; }

  private:
    void trigger_fallback(DenoiserFallbackReason reason) noexcept;

    SvgfLiteDenoiser svgf_fallback_{};
    NvidiaSigmaDenoiser sigma_backend_{};
    FidelityFxComputeDenoiser ffx_backend_{};
    OptionalSdkDenoiser optix_backend_{DenoiserKind::Optix};
    OptionalSdkDenoiser oidn_backend_{DenoiserKind::Oidn};

    IDenoiserBackend* active_{nullptr};
    IDenoiserBackend* preferred_{nullptr};
    DenoiserKind requested_kind_{DenoiserKind::Auto};
    DenoiserFallbackReason fallback_reason_{DenoiserFallbackReason::None};
    bool initialized_{false};
    bool inject_failure_{false};

    std::array<float, kWindowFrames> sample_ring_ms_{};
    std::array<float, kWindowFrames> sort_scratch_ms_{};
    uint32_t sample_count_{0};
    uint32_t sample_cursor_{0};
    float rolling_p95_ms_{0.0f};
};

} // namespace aether::renderer
