#pragma once

#include "core/expected.hpp"
#include "rhi/rhi_device.hpp"
#include "vcam/vcam_shm.h"

#include <array>
#include <cstdint>
#include <span>

namespace aether::output {

/**
 * Producer side of `Local\AetherVCam_v1` shared-memory transport (`§11.3`).
 * - Transfers GPU `OUT_BGRA8` (or bottom-up / `NV12`) via Vulkan transfer queue (`DMA <= 1.1 ms`)
 *   into 3 host-mapped shared-memory slots using `VK_EXT_external_memory_host` when aligned & supported,
 *   or pre-allocated host-visible / UMA zero-copy readback buffers + single memcpy fallback.
 * - Also supports direct host/DX12 UMA readback publication (`init_host_only` + `publish_host_bgra8`).
 * - Zero heap allocations in `publish_frame()` / `publish_host_bgra8()` (`HR-01`).
 */
class VcamTransport {
  public:
    VcamTransport() = default;
    ~VcamTransport() { shutdown(); }

    VcamTransport(const VcamTransport&) = delete;
    VcamTransport& operator=(const VcamTransport&) = delete;

    [[nodiscard]] Expected<void> init(rhi::RhiDevice& device);
    [[nodiscard]] Expected<void> init_host_only();
    void shutdown() noexcept;

    /** Returns true when an external DirectShow consumer has an active RGB pin connected (`§11.1`). */
    [[nodiscard]] bool is_rgb_pin_connected() const noexcept;
    [[nodiscard]] bool is_nv12_pin_connected() const noexcept;
    [[nodiscard]] bool is_filter_active() const noexcept;

    /** Copies GPU frame -> mapped slot `(slot % 3)` on transfer queue, waits, and advances `write_seq` seqlock. */
    [[nodiscard]] Expected<void> publish_frame(VkImage src_bgra, VkImage src_nv12_y, VkImage src_nv12_uv,
                                               uint32_t width, uint32_t height, uint32_t fps, bool is_bottom_up,
                                               uint64_t frame_sequence, uint64_t present_qpc_us,
                                               uint64_t wait_timeline_value) noexcept;
    /** Publishes CPU/UMA-mapped BGRA8 bytes directly into `Local\AetherVCam_v1` seqlock slots (`HR-01`). */
    [[nodiscard]] Expected<void> publish_host_bgra8(std::span<const uint8_t> bgra, uint32_t width, uint32_t height,
                                                    uint32_t fps, bool is_bottom_up, uint64_t frame_sequence,
                                                    uint64_t present_qpc_us) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return shm_base_ != nullptr; }
    [[nodiscard]] bool using_external_host_mem() const noexcept { return ext_host_mem_active_; }
    [[nodiscard]] uint64_t frames_published() const noexcept { return frames_published_; }
    [[nodiscard]] double last_dma_ms() const noexcept { return last_dma_ms_; }
    [[nodiscard]] AetherVCamHeader* header() noexcept { return reinterpret_cast<AetherVCamHeader*>(shm_base_); }
    [[nodiscard]] const AetherVCamHeader* header() const noexcept {
        return reinterpret_cast<const AetherVCamHeader*>(shm_base_);
    }

  private:
    Expected<void> open_shm_and_prefault();
    Expected<void> init_dma_resources();
    void destroy_dma_resources() noexcept;

    rhi::RhiDevice* device_{nullptr};
    void* shm_mapping_{nullptr};
    uint8_t* shm_base_{nullptr};
    bool ext_host_mem_active_{false};

    // External host-memory imported buffers (1 per slot) OR coherent readback staging fallback
    std::array<VkBuffer, AETHER_VCAM_SLOTS> ext_buffers_{};
    std::array<VkDeviceMemory, AETHER_VCAM_SLOTS> ext_memories_{};
    std::array<rhi::RhiBuffer, AETHER_VCAM_SLOTS> staging_buffers_{};
    rhi::RhiImage scratch_scale_bgra_{}; // Used only if output > 1920x1080 (e.g. 4K -> 1080p VCam)
    uint32_t scratch_w_{0};
    uint32_t scratch_h_{0};

    VkCommandPool dma_pool_{VK_NULL_HANDLE};
    std::array<VkCommandBuffer, AETHER_VCAM_SLOTS> dma_cmds_{};
    VkFence dma_fence_{VK_NULL_HANDLE};
    uint32_t slot_cursor_{0};
    uint64_t frames_published_{0};
    double last_dma_ms_{0.0};
};

} // namespace aether::output
