#pragma once

#include "core/expected.hpp"
#include "rhi/rhi_device.hpp"

#include <array>
#include <cstdint>

namespace aether::rhi {

struct RebarAllocation {
    void* mapped{nullptr};
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceSize offset{0};
    VkDeviceAddress device_address{0};
    VkDeviceSize size{0};
};

/**
 * Persistent ReBAR (`DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`) 8 MiB ring buffer (`§10.1`).
 * Zero heap allocation after `init()`, wait-free bump allocation across up to 3 in-flight frames.
 */
class RebarRingBuffer {
  public:
    static constexpr VkDeviceSize kDefaultCapacityBytes = 8ull * 1024ull * 1024ull;
    static constexpr uint32_t kMaxFramesInFlight = 3;

    RebarRingBuffer() = default;
    ~RebarRingBuffer();

    RebarRingBuffer(const RebarRingBuffer&) = delete;
    RebarRingBuffer& operator=(const RebarRingBuffer&) = delete;

    [[nodiscard]] Expected<void> init(const RhiDevice& device, VkDeviceSize capacity_bytes = kDefaultCapacityBytes);
    void destroy() noexcept;

    /** Advances frame slot and reclaims regions from `completed_frame_index` (`HR-01` zero alloc). */
    void begin_frame(uint64_t frame_index, uint64_t completed_frame_index) noexcept;

    /** Sub-allocates `size` bytes aligned to `alignment` (`<= capacity_bytes / 2` recommended). */
    [[nodiscard]] Expected<RebarAllocation> allocate(VkDeviceSize size, VkDeviceSize alignment = 16) noexcept;

    [[nodiscard]] bool is_rebar_heap() const noexcept { return rebar_heap_; }
    [[nodiscard]] VkDeviceSize capacity() const noexcept { return buffer_.size; }
    [[nodiscard]] VkDeviceSize used_in_current_frame() const noexcept { return frame_bytes_; }
    [[nodiscard]] VkBuffer buffer() const noexcept { return buffer_.handle; }

  private:
    struct FrameBookmark {
        uint64_t frame_index{0};
        VkDeviceSize start_offset{0};
        VkDeviceSize end_offset{0};
        bool active{false};
    };

    const RhiDevice* device_{nullptr};
    RhiBuffer buffer_{};
    uint8_t* mapped_base_{nullptr};
    VkDeviceAddress base_address_{0};
    VkDeviceSize write_cursor_{0};
    VkDeviceSize tail_limit_{0};
    VkDeviceSize frame_bytes_{0};
    uint64_t current_frame_index_{0};
    bool rebar_heap_{false};
    bool initialized_{false};
    std::array<FrameBookmark, kMaxFramesInFlight> bookmarks_{};
};

} // namespace aether::rhi
