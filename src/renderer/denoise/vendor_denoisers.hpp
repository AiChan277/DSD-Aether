#pragma once

#include "renderer/denoise/denoiser_backend.hpp"

#include <memory>

namespace aether::renderer {

/**
 * NVIDIA NRD SIGMA-inspired compute denoiser (`§10.9`, `ADR-0009`).
 * Runs purely in Vulkan compute so RTX GPUs get fast penumbra-preserving denoising with zero external SDK lock-in.
 */
class NvidiaSigmaDenoiser final : public IDenoiserBackend {
  public:
    NvidiaSigmaDenoiser() = default;
    ~NvidiaSigmaDenoiser() override { destroy(); }

    [[nodiscard]] DenoiserKind kind() const noexcept override { return DenoiserKind::NrdSigma; }
    [[nodiscard]] const char* name() const noexcept override { return "NVIDIA SIGMA Compute"; }

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

/**
 * AMD/Intel FidelityFX-style spatio-temporal compute denoiser (`§10.9`, `ADR-0009`).
 */
class FidelityFxComputeDenoiser final : public IDenoiserBackend {
  public:
    FidelityFxComputeDenoiser() = default;
    ~FidelityFxComputeDenoiser() override { destroy(); }

    [[nodiscard]] DenoiserKind kind() const noexcept override { return DenoiserKind::FfxCompute; }
    [[nodiscard]] const char* name() const noexcept override { return "FidelityFX Compute"; }

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

/**
 * Runtime-probed optional OptiX / OIDN 2.x backends (`ADR-0009`).
 * Probe functions check for native SDK DLLs/CUDA interop on the host and cleanly return `Unsupported` when absent.
 */
class OptionalSdkDenoiser final : public IDenoiserBackend {
  public:
    explicit OptionalSdkDenoiser(DenoiserKind sdk_kind) noexcept : kind_(sdk_kind) {}
    ~OptionalSdkDenoiser() override { destroy(); }

    [[nodiscard]] DenoiserKind kind() const noexcept override { return kind_; }
    [[nodiscard]] const char* name() const noexcept override {
        return kind_ == DenoiserKind::Optix ? "OptiX (Optional SDK)" : "OIDN 2.x (Optional SDK)";
    }
    [[nodiscard]] bool is_external_sdk() const noexcept override { return true; }

    [[nodiscard]] static bool probe_runtime_available(DenoiserKind sdk_kind, const rhi::RhiDevice& device) noexcept;

    [[nodiscard]] Expected<void> init(const rhi::RhiDevice& device, const rhi::BindlessHeap& bindless, uint32_t width,
                                      uint32_t height) override;
    [[nodiscard]] Expected<void> resize(uint32_t width, uint32_t height) override;
    [[nodiscard]] Expected<void> denoise(VkCommandBuffer cmd, const DenoiserInputView& inputs) noexcept override;
    void destroy() noexcept override;

  private:
    DenoiserKind kind_{DenoiserKind::Oidn};
    bool runtime_loaded_{false};
};

} // namespace aether::renderer
