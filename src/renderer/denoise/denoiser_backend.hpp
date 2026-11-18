#pragma once

#include "core/expected.hpp"
#include "rhi/bindless.hpp"
#include "rhi/rhi_device.hpp"

#include <cstdint>

namespace aether::renderer {

enum class DenoiserKind : uint8_t {
    Auto = 0,
    Optix = 1,
    NrdSigma = 2,
    FfxCompute = 3,
    Oidn = 4,
    SvgfLite = 5,
};

struct DenoiserInputView {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t frame_index{0};
    uint32_t raw_shadow_sampled_slot{0};
    uint32_t denoised_shadow_storage_slot{0};
    uint32_t history_shadow_sampled_slot{0};
    uint32_t raw_rim_sampled_slot{0};
    uint32_t denoised_rim_storage_slot{0};
    uint32_t history_rim_sampled_slot{0};
    uint32_t raw_bounce_sampled_slot{UINT32_MAX};
    uint32_t denoised_bounce_storage_slot{UINT32_MAX};
    uint32_t history_bounce_sampled_slot{UINT32_MAX};
    uint32_t bounce_width{0};
    uint32_t bounce_height{0};
    uint32_t gb_normal_sampled_slot{0};
    uint32_t gb_mv_sampled_slot{0};
    uint32_t depth_sampled_slot{0};
    uint32_t prev_depth_sampled_slot{0};
    uint32_t prev_normal_sampled_slot{0};
    uint64_t frame_globals_bda{0};
    float temporal_alpha{0.20f};
};

/**
 * Abstract denoiser interface for RT cone shadows and rim occlusion (`§10.9`, `ADR-0009`).
 * Implementations MUST pre-allocate all GPU pipelines/scratch during `init()`/`resize()` so `denoise()`
 * executes with zero heap allocations (`HR-01`).
 */
class IDenoiserBackend {
  public:
    virtual ~IDenoiserBackend() = default;

    [[nodiscard]] virtual DenoiserKind kind() const noexcept = 0;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual bool is_external_sdk() const noexcept { return false; }

    [[nodiscard]] virtual Expected<void> init(const rhi::RhiDevice& device, const rhi::BindlessHeap& bindless,
                                              uint32_t width, uint32_t height) = 0;
    [[nodiscard]] virtual Expected<void> resize(uint32_t width, uint32_t height) = 0;
    [[nodiscard]] virtual Expected<void> denoise(VkCommandBuffer cmd, const DenoiserInputView& inputs) noexcept = 0;
    virtual void destroy() noexcept = 0;
};

} // namespace aether::renderer
