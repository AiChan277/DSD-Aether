#pragma once

#include "core/expected.hpp"
#include "rhi/rhi_device.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace aether::rhi {

enum class SamplerSlot : uint32_t {
    LinearClamp = 0,
    NearestClamp = 1,
    LinearRepeat = 2,
    NearestRepeat = 3,
    ShadowCompare = 4,
    Count = 5,
};

struct BindlessConfig {
    uint32_t max_sampled_images{4096};
    uint32_t max_samplers{32};
    uint32_t max_storage_images{256};
    uint32_t max_uint_sampled_images{128};
    bool enable_rt_set{false};
};

/**
 * Global Vulkan 1.3 bindless descriptor heap (`§10.1`, `ADR-0009`).
 * Set 0 holds `sampled_images` (b0), `samplers` (b1), `storage_images` (b2), `uint_sampled_images` (b3);
 * TLAS lives strictly in Set 1 so non-RT devices and validation layers never bind acceleration-structure
 * descriptors (`HR-08`).
 */
class BindlessHeap {
  public:
    BindlessHeap() = default;
    ~BindlessHeap();

    BindlessHeap(const BindlessHeap&) = delete;
    BindlessHeap& operator=(const BindlessHeap&) = delete;

    [[nodiscard]] Expected<void> init(const RhiDevice& device, const BindlessConfig& cfg = {});
    void destroy() noexcept;

    [[nodiscard]] Expected<uint32_t>
    register_sampled_image(VkImageView view, VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) noexcept;
    void update_sampled_image(uint32_t slot, VkImageView view,
                              VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) const noexcept;
    void release_sampled_image(uint32_t slot) noexcept;

    [[nodiscard]] Expected<uint32_t> register_storage_image(VkImageView view,
                                                            VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL) noexcept;
    void update_storage_image(uint32_t slot, VkImageView view,
                              VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL) const noexcept;
    void release_storage_image(uint32_t slot) noexcept;

    [[nodiscard]] Expected<uint32_t>
    register_uint_sampled_image(VkImageView view,
                                VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) noexcept;
    void update_uint_sampled_image(uint32_t slot, VkImageView view,
                                   VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) const noexcept;
    void release_uint_sampled_image(uint32_t slot) noexcept;

    [[nodiscard]] Expected<void> update_tlas(VkAccelerationStructureKHR tlas) noexcept;

    [[nodiscard]] VkDescriptorSetLayout global_layout() const noexcept { return global_layout_; }
    [[nodiscard]] VkDescriptorSet global_set() const noexcept { return global_set_; }
    [[nodiscard]] VkDescriptorSetLayout rt_layout() const noexcept { return rt_layout_; }
    [[nodiscard]] VkDescriptorSet rt_set() const noexcept { return rt_set_; }
    [[nodiscard]] bool rt_enabled() const noexcept { return rt_set_ != VK_NULL_HANDLE; }

    [[nodiscard]] uint32_t sampled_capacity() const noexcept { return sampled_capacity_; }
    [[nodiscard]] uint32_t storage_capacity() const noexcept { return storage_capacity_; }
    [[nodiscard]] uint32_t uint_sampled_capacity() const noexcept { return uint_sampled_capacity_; }
    [[nodiscard]] uint32_t sampler_capacity() const noexcept { return sampler_capacity_; }
    [[nodiscard]] uint32_t sampled_in_use() const noexcept { return sampled_in_use_; }
    [[nodiscard]] uint32_t storage_in_use() const noexcept { return storage_in_use_; }

  private:
    const RhiDevice* device_{nullptr};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSetLayout global_layout_{VK_NULL_HANDLE};
    VkDescriptorSet global_set_{VK_NULL_HANDLE};
    VkDescriptorSetLayout rt_layout_{VK_NULL_HANDLE};
    VkDescriptorSet rt_set_{VK_NULL_HANDLE};

    std::array<VkSampler, static_cast<size_t>(SamplerSlot::Count)> static_samplers_{};
    uint32_t sampled_capacity_{0};
    uint32_t sampler_capacity_{0};
    uint32_t storage_capacity_{0};
    uint32_t uint_sampled_capacity_{0};
    uint32_t sampled_in_use_{0};
    uint32_t storage_in_use_{0};
    uint32_t uint_sampled_in_use_{0};

    // Preallocated free-index stacks in `init()` so runtime registration is allocation-free (`HR-01`).
    std::vector<uint32_t> sampled_free_stack_{};
    uint32_t sampled_free_top_{0};
    std::vector<uint32_t> storage_free_stack_{};
    uint32_t storage_free_top_{0};
    std::vector<uint32_t> uint_sampled_free_stack_{};
    uint32_t uint_sampled_free_top_{0};
};

} // namespace aether::rhi
