#include "rhi/rebar_ring.hpp"

#include <algorithm>

namespace aether::rhi {
namespace {

[[nodiscard]] VkDeviceSize align_up_pow2(VkDeviceSize val, VkDeviceSize align) noexcept {
    const VkDeviceSize a = std::max<VkDeviceSize>(align, 16ULL);
    return (val + a - 1ULL) & ~(a - 1ULL);
}

} // namespace

RebarRingBuffer::~RebarRingBuffer() {
    destroy();
}

Expected<void> RebarRingBuffer::init(const RhiDevice& device, VkDeviceSize capacity_bytes) {
    destroy();
    if (device.device() == VK_NULL_HANDLE || capacity_bytes < 4096ULL) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Invalid device or ReBAR ring capacity"));
    }

    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const VmaAllocationCreateFlags vma_flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    auto buf_res = device.create_buffer(capacity_bytes, usage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, vma_flags);
    if (!buf_res) {
        return make_unexpected(buf_res.error());
    }
    if (buf_res.value().info.pMappedData == nullptr) {
        auto copy = buf_res.value();
        device.destroy_buffer(copy);
        return make_unexpected(Error::make(ErrorCode::VulkanError, "ReBAR ring buffer failed persistent map"));
    }

    VkMemoryPropertyFlags mem_props = 0;
    vmaGetMemoryTypeProperties(device.allocator(), buf_res.value().info.memoryType, &mem_props);
    rebar_heap_ = (mem_props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
                  (mem_props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;

    device_ = &device;
    buffer_ = buf_res.value();
    mapped_base_ = static_cast<uint8_t*>(buffer_.info.pMappedData);
    base_address_ = device.buffer_device_address(buffer_.handle);
    if (base_address_ == 0) {
        base_address_ = buffer_.device_address;
    }

    write_cursor_ = 0;
    tail_limit_ = capacity_bytes;
    frame_bytes_ = 0;
    current_frame_index_ = 0;
    for (auto& b : bookmarks_) {
        b = FrameBookmark{};
    }
    initialized_ = true;
    return {};
}

void RebarRingBuffer::destroy() noexcept {
    if (device_ != nullptr && buffer_.handle != VK_NULL_HANDLE) {
        device_->destroy_buffer(buffer_);
    }
    buffer_ = RhiBuffer{};
    mapped_base_ = nullptr;
    base_address_ = 0;
    write_cursor_ = 0;
    tail_limit_ = 0;
    frame_bytes_ = 0;
    for (auto& b : bookmarks_) {
        b = FrameBookmark{};
    }
    rebar_heap_ = false;
    initialized_ = false;
    device_ = nullptr;
}

void RebarRingBuffer::begin_frame(uint64_t frame_index, uint64_t completed_frame_index) noexcept {
    if (!initialized_) {
        return;
    }
    current_frame_index_ = frame_index;
    frame_bytes_ = 0;

    for (auto& b : bookmarks_) {
        if (b.active && b.frame_index <= completed_frame_index) {
            b.active = false;
        }
    }

    const size_t slot = static_cast<size_t>(frame_index % kMaxFramesInFlight);
    bookmarks_[slot].frame_index = frame_index;
    bookmarks_[slot].start_offset = write_cursor_;
    bookmarks_[slot].end_offset = write_cursor_;
    bookmarks_[slot].active = true;
}

Expected<RebarAllocation> RebarRingBuffer::allocate(VkDeviceSize size, VkDeviceSize alignment) noexcept {
    if (!initialized_ || size == 0) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "RebarRingBuffer uninitialized or empty alloc"));
    }
    const VkDeviceSize cap = buffer_.size;
    if (size > cap) {
        return make_unexpected(Error::make(ErrorCode::OutOfMemory, "ReBAR allocation exceeds ring capacity"));
    }

    // Determine earliest active frame offset that must not be overwritten.
    bool has_prior = false;
    uint64_t oldest_frame = UINT64_MAX;
    VkDeviceSize barrier_offset = cap;
    for (const auto& b : bookmarks_) {
        if (b.active && b.frame_index < current_frame_index_ && b.frame_index < oldest_frame) {
            oldest_frame = b.frame_index;
            barrier_offset = b.start_offset;
            has_prior = true;
        }
    }

    const VkDeviceSize aligned = align_up_pow2(write_cursor_, alignment);
    VkDeviceSize alloc_offset = 0;
    if (!has_prior) {
        if (aligned + size <= cap) {
            alloc_offset = aligned;
        } else {
            alloc_offset = 0;
        }
    } else if (write_cursor_ >= barrier_offset) {
        if (aligned + size <= cap) {
            alloc_offset = aligned;
        } else if (size <= barrier_offset) {
            alloc_offset = 0;
        } else {
            return make_unexpected(Error::make(ErrorCode::OutOfMemory, "ReBAR ring exhausted by in-flight frames"));
        }
    } else {
        if (aligned + size <= barrier_offset) {
            alloc_offset = aligned;
        } else {
            return make_unexpected(Error::make(ErrorCode::OutOfMemory, "ReBAR ring tail collided with active frame"));
        }
    }

    write_cursor_ = alloc_offset + size;
    frame_bytes_ += size;

    const size_t slot = static_cast<size_t>(current_frame_index_ % kMaxFramesInFlight);
    if (bookmarks_[slot].active) {
        bookmarks_[slot].end_offset = write_cursor_;
    }

    RebarAllocation out{};
    out.mapped = mapped_base_ + alloc_offset;
    out.buffer = buffer_.handle;
    out.offset = alloc_offset;
    out.device_address = base_address_ + alloc_offset;
    out.size = size;
    return out;
}

} // namespace aether::rhi
