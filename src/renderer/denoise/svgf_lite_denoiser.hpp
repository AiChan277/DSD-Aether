#pragma once

#include "renderer/denoise/denoiser_backend.hpp"

namespace aether::renderer {

/**
 * Mandatory built-in Vulkan compute SVGF-lite denoiser (`§10.9`, `ADR-0009`):
 * temporal MV reprojection + 3x3 AABB history clamping + 2 edge-stopping à-trous iterations.
 * Guaranteed zero heap allocation in `denoise()` and zero external SDK dependencies.
 */
class SvgfLiteDenoiser final : public IDenoiserBackend {
  public:
    SvgfLiteDenoiser() = default;
    ~SvgfLiteDenoiser() override { destroy(); }

    [[nodiscard]] DenoiserKind kind() const noexcept override { return DenoiserKind::SvgfLite; }
    [[nodiscard]] const char* name() const noexcept override { return "SVGF-Lite Compute"; }

    [[nodiscard]] Expected<void> init(const rhi::RhiDevice& device, const rhi::BindlessHeap& bindless, uint32_t width,
                                      uint32_t height) override;
    [[nodiscard]] Expected<void> resize(uint32_t width, uint32_t height) override;
    [[nodiscard]] Expected<void> denoise(VkCommandBuffer cmd, const DenoiserInputView& inputs) noexcept override;
    void destroy() noexcept override;

  private:
    const rhi::RhiDevice* device_{nullptr};
    VkDescriptorSet bindless_set_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkShaderModule shader_module_{VK_NULL_HANDLE};
    uint32_t width_{0};
    uint32_t height_{0};
};

} // namespace aether::renderer
