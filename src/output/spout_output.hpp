#pragma once

#include "core/expected.hpp"
#include "output/output_config.hpp"
#include "rhi/rhi_device.hpp"

#include <array>
#include <cstdint>

namespace aether::output {

// Binary-compatible Spout2 `SharedTextureInfo` record (280 B, §11.2 / ADR-0011).
#pragma pack(push, 4)
struct SpoutSharedTextureInfo {
    uint32_t share_handle{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t format{0};
    uint32_t usage{0};
    char description[256]{};
    uint32_t partner_id{0};
};
#pragma pack(pop)
static_assert(sizeof(SpoutSharedTextureInfo) == 280);

/**
 * First-party LUID-matched DXGI/D3D11 + Vulkan external-memory (`D3D11_TEXTURE_KMT`) Spout2 sender (`§11.2`).
 * - Matches Vulkan adapter LUID via `IDXGIFactory4::EnumAdapterByLuid`.
 * - Publishes active sender in `SpoutSenderNames` + `<Sender>` `SharedTextureInfo` + `[Sender]_SpoutAccessMutex`
 *   with strict `1 ms` acquire timeout (skips frame on contention, never stalls T4).
 * - Supports `GpuDefault` (`<= 0.2 ms` copy + `VK_QUEUE_FAMILY_EXTERNAL` barrier) and `CpuCompatibility` fallback.
 */
class SpoutOutput {
  public:
    SpoutOutput() = default;
    ~SpoutOutput() { shutdown(); }

    SpoutOutput(const SpoutOutput&) = delete;
    SpoutOutput& operator=(const SpoutOutput&) = delete;

    [[nodiscard]] Expected<void> init(rhi::RhiDevice& device, uint32_t width, uint32_t height,
                                      const char* sender_name = "Aether2.5D",
                                      SpoutSharingMode mode = SpoutSharingMode::GpuDefault);
    [[nodiscard]] Expected<void> resize(uint32_t width, uint32_t height);
    void shutdown() noexcept;

    /**
     * Publishes `src_bgra_image` (in `VK_IMAGE_LAYOUT_GENERAL`, produced prior to `wait_timeline_value`)
     * to the Spout shared DXGI texture. Waits `1 ms` max on `[Sender]_SpoutAccessMutex` (`§11.2.3`).
     * Zero heap allocations (`HR-01`).
     */
    [[nodiscard]] Expected<void> publish_frame(VkImage src_bgra_image, uint32_t width, uint32_t height,
                                               uint64_t wait_timeline_value) noexcept;

    [[nodiscard]] bool is_active() const noexcept { return initialized_; }
    [[nodiscard]] bool using_cpu_fallback() const noexcept { return cpu_fallback_; }
    [[nodiscard]] bool adapter_mismatch_detected() const noexcept { return adapter_mismatch_; }
    [[nodiscard]] uint64_t frames_published() const noexcept { return frames_published_; }
    [[nodiscard]] uint64_t skipped_frames() const noexcept { return skipped_mutex_; }
    [[nodiscard]] double last_publish_ms() const noexcept { return last_publish_ms_; }
    [[nodiscard]] const char* sender_name() const noexcept { return sender_name_.data(); }

    /** Test/UI helper exposing created DXGI KMT share handle. */
    [[nodiscard]] uint32_t raw_kmt_share_handle() const noexcept { return share_handle_u32_; }

    /** Helper checking whether primary display output resides on a different DXGI adapter than `luid`. */
    [[nodiscard]] static bool detect_display_adapter_mismatch(const std::array<uint8_t, 8>& luid, bool luid_valid);

  private:
    Expected<void> allocate_shared_targets(uint32_t width, uint32_t height);
    void release_shared_targets() noexcept;
    Expected<void> register_spout_maps();
    void unregister_spout_maps() noexcept;

    rhi::RhiDevice* device_{nullptr};
    uint32_t width_{0};
    uint32_t height_{0};
    SpoutSharingMode requested_mode_{SpoutSharingMode::GpuDefault};
    bool cpu_fallback_{false};
    bool adapter_mismatch_{false};
    bool initialized_{false};

    std::array<char, 64> sender_name_{};
    uint32_t share_handle_u32_{0};

    // Opaque COM/Win32 pointers stored without leaking D3D11/DXGI headers into callers
    void* d3d_device_{nullptr};
    void* d3d_context_{nullptr};
    void* d3d_shared_tex_{nullptr};
    void* d3d_staging_tex_{nullptr};
    void* kmt_handle_{nullptr};
    void* access_mutex_{nullptr};
    void* names_mutex_{nullptr};
    void* names_map_{nullptr};
    void* names_view_{nullptr};
    void* sender_map_{nullptr};
    void* sender_view_{nullptr};
    void* active_map_{nullptr};
    void* active_view_{nullptr};
    void* count_semaphore_{nullptr};

    VkImage vk_shared_image_{VK_NULL_HANDLE};
    VkDeviceMemory vk_shared_mem_{VK_NULL_HANDLE};
    VkImageLayout vk_shared_layout_{VK_IMAGE_LAYOUT_UNDEFINED};
    rhi::RhiBuffer readback_buf_{};

    VkCommandPool cmd_pool_{VK_NULL_HANDLE};
    std::array<VkCommandBuffer, 3> cmds_{};
    std::array<uint64_t, 3> cmd_tl_{};
    uint32_t cmd_cursor_{0};
    uint64_t output_tl_counter_{0};
    uint64_t init_tl_baseline_{0};
    uint64_t frames_published_{0};
    uint64_t skipped_mutex_{0};
    double last_publish_ms_{0.0};
};

} // namespace aether::output
