#pragma once

#include "core/expected.hpp"
#include "rhi/rhi_device.hpp"

#include <array>
#include <cstdint>

namespace aether::rhi {

enum class GpuPassId : uint8_t {
    SkinMorph = 0,
    Prepass = 1,
    RayTrace = 2,
    CsmFallback = 3,
    Denoise = 4,
    DiffuseBounce = 5,
    Live2dNpr = 6,
    OutlineHull = 7,
    CreaseLines = 8,
    PostBloom = 9,
    ToneMap = 10,
    OutputEncode = 11,
    ViewportCompose = 12,
    FrameTotal = 13,
    Count = 14,
};

struct GpuPassTimingSnapshot {
    std::array<float, static_cast<size_t>(GpuPassId::Count)> pass_ms{};
    float denoiser_ms{0.0f};
    float denoiser_p95_120f_ms{0.0f};
    float gpu_total_ms{0.0f};
    int64_t gpu_cpu_clock_offset_ns{0};
    bool valid{false};
};

/**
 * Per-frame GPU timestamp query ring + 120-frame denoiser p95 window (`§10.1`, `ADR-0009`).
 * Zero heap allocation after `init()`.
 */
class GpuTimingRing {
  public:
    static constexpr uint32_t kFrameSlots = 3;
    static constexpr uint32_t kPassCount = static_cast<uint32_t>(GpuPassId::Count);
    static constexpr uint32_t kQueriesPerSlot = kPassCount * 2u;
    static constexpr uint32_t kRollingWindowFrames = 120;

    GpuTimingRing() = default;
    ~GpuTimingRing();

    GpuTimingRing(const GpuTimingRing&) = delete;
    GpuTimingRing& operator=(const GpuTimingRing&) = delete;

    [[nodiscard]] Expected<void> init(const RhiDevice& device);
    void destroy() noexcept;

    void cmd_reset_frame(VkCommandBuffer cmd, uint32_t frame_slot) noexcept;
    void cmd_reset_scope(VkCommandBuffer cmd, uint32_t frame_slot, GpuPassId pass) noexcept;
    void begin_scope(VkCommandBuffer cmd, uint32_t frame_slot, GpuPassId pass) const noexcept;
    void end_scope(VkCommandBuffer cmd, uint32_t frame_slot, GpuPassId pass) noexcept;

    /** Reads back completed timestamps for `frame_slot` without blocking when unavailable. */
    [[nodiscard]] const GpuPassTimingSnapshot& resolve_frame(uint32_t frame_slot) noexcept;
    [[nodiscard]] const GpuPassTimingSnapshot& latest_snapshot() const noexcept { return latest_; }

  private:
    const RhiDevice* device_{nullptr};
    std::array<VkQueryPool, kFrameSlots> query_pools_{};
    std::array<uint32_t, kFrameSlots> recorded_mask_{};
    std::array<float, kRollingWindowFrames> denoiser_history_ms_{};
    uint32_t denoiser_history_count_{0};
    uint32_t denoiser_history_cursor_{0};
    GpuPassTimingSnapshot latest_{};
};

} // namespace aether::rhi
