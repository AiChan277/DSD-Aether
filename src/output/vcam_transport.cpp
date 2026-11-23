#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "output/vcam_transport.hpp"

#include <algorithm>
#include <cstring>

namespace aether::output {

namespace {

long read_volatile_long(const volatile long* p) noexcept {
    const long val = InterlockedCompareExchange(const_cast<volatile long*>(p), 0L, 0L);
    MemoryBarrier();
    return val;
}

void write_volatile_long(volatile long* p, long v) noexcept {
    MemoryBarrier();
    InterlockedExchange(p, v);
}

void copy_scaled_bgra(const uint8_t* src, uint32_t src_w, uint32_t src_h, uint8_t* dst, uint32_t dst_w,
                      uint32_t dst_h) noexcept {
    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        const uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(dy) * src_h) / dst_h);
        const uint8_t* srow = src + static_cast<size_t>(sy) * src_w * 4u;
        uint8_t* drow = dst + static_cast<size_t>(dy) * dst_w * 4u;
        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(dx) * src_w) / dst_w);
            std::memcpy(drow + static_cast<size_t>(dx) * 4u, srow + static_cast<size_t>(sx) * 4u, 4u);
        }
    }
}

void copy_scaled_nv12(const uint8_t* src_y, const uint8_t* src_uv, uint32_t src_w, uint32_t src_h, uint8_t* dst_y,
                      uint8_t* dst_uv, uint32_t dst_w, uint32_t dst_h) noexcept {
    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        const uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(dy) * src_h) / dst_h);
        const uint8_t* srow = src_y + static_cast<size_t>(sy) * src_w;
        uint8_t* drow = dst_y + static_cast<size_t>(dy) * dst_w;
        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(dx) * src_w) / dst_w);
            drow[dx] = srow[sx];
        }
    }
    const uint32_t src_cw = std::max(src_w / 2u, 1u);
    const uint32_t src_ch = std::max(src_h / 2u, 1u);
    const uint32_t dst_cw = std::max(dst_w / 2u, 1u);
    const uint32_t dst_ch = std::max(dst_h / 2u, 1u);
    for (uint32_t dy = 0; dy < dst_ch; ++dy) {
        const uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(dy) * src_ch) / dst_ch);
        const uint8_t* srow = src_uv + static_cast<size_t>(sy) * src_cw * 2u;
        uint8_t* drow = dst_uv + static_cast<size_t>(dy) * dst_cw * 2u;
        for (uint32_t dx = 0; dx < dst_cw; ++dx) {
            const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(dx) * src_cw) / dst_cw);
            drow[static_cast<size_t>(dx) * 2u + 0u] = srow[static_cast<size_t>(sx) * 2u + 0u];
            drow[static_cast<size_t>(dx) * 2u + 1u] = srow[static_cast<size_t>(sx) * 2u + 1u];
        }
    }
}

} // namespace

Expected<void> VcamTransport::open_shm_and_prefault() {
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0u,
                                        static_cast<DWORD>(AETHER_VCAM_SECTION_BYTES), L"Local\\AetherVCam_v1");
    if (mapping == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "CreateFileMappingW(AetherVCam_v1) failed")};
    }
    const bool existed = (GetLastError() == ERROR_ALREADY_EXISTS);

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0u, 0u, AETHER_VCAM_SECTION_BYTES);
    if (view == nullptr) {
        CloseHandle(mapping);
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "MapViewOfFile(AetherVCam_v1) failed")};
    }

    shm_mapping_ = mapping;
    shm_base_ = static_cast<uint8_t*>(view);

    // Pre-fault SHM pages once during init to avoid lazy PTE page faults on first 3 frames
    for (size_t off = AETHER_VCAM_HEADER_BYTES; off < AETHER_VCAM_SECTION_BYTES; off += 4096u) {
        volatile uint8_t b = shm_base_[off];
        shm_base_[off] = b;
    }

    auto* hdr = header();
    const long prev_pin_flags = existed ? read_volatile_long(&hdr->active_pin_flags) : 0L;
    std::memset(hdr, 0, sizeof(AetherVCamHeader));
    hdr->magic = AETHER_VCAM_MAGIC;
    hdr->version = AETHER_VCAM_VERSION;
    hdr->slot_stride_bytes = AETHER_VCAM_SLOT_STRIDE_BYTES;
    hdr->nv12_offset_in_slot = AETHER_VCAM_NV12_OFFSET_IN_SLOT;
    write_volatile_long(&hdr->active_pin_flags, prev_pin_flags);
    write_volatile_long(&hdr->write_seq, 0L);
    return {};
}

Expected<void> VcamTransport::init(rhi::RhiDevice& device) {
    shutdown();

    if (device.device() == VK_NULL_HANDLE) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "VcamTransport::init null device")};
    }

    device_ = &device;
    ext_host_mem_active_ = false;
    slot_cursor_ = 0u;
    frames_published_ = 0u;
    last_dma_ms_ = 0.0;

    if (auto res = open_shm_and_prefault(); !res) {
        shutdown();
        return res;
    }
    if (auto res = init_dma_resources(); !res) {
        shutdown();
        return res;
    }

    AETHER_LOG_INFO("output", "VcamTransport initialized: Local\\AetherVCam_v1 (%u MB, %u slots, ext_host_mem=%d)",
                    AETHER_VCAM_SECTION_BYTES / (1024u * 1024u), AETHER_VCAM_SLOTS, ext_host_mem_active_ ? 1 : 0);
    return {};
}

Expected<void> VcamTransport::init_host_only() {
    shutdown();
    ext_host_mem_active_ = false;
    slot_cursor_ = 0u;
    frames_published_ = 0u;
    last_dma_ms_ = 0.0;
    if (auto res = open_shm_and_prefault(); !res) {
        shutdown();
        return res;
    }
    AETHER_LOG_INFO("output", "VcamTransport initialized (Host/DX12 UMA): Local\\AetherVCam_v1");
    return {};
}

void VcamTransport::shutdown() noexcept {
    destroy_dma_resources();

    if (shm_base_ != nullptr) {
        UnmapViewOfFile(shm_base_);
        shm_base_ = nullptr;
    }
    if (shm_mapping_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(shm_mapping_));
        shm_mapping_ = nullptr;
    }

    ext_host_mem_active_ = false;
    frames_published_ = 0u;
    last_dma_ms_ = 0.0;
    slot_cursor_ = 0u;
    device_ = nullptr;
}

Expected<void> VcamTransport::init_dma_resources() {
    if (device_ == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "null device")};
    }

    // Attempt zero-copy VK_EXT_external_memory_host import directly into the 3 SHM slots (`§11.3`)
    ext_host_mem_active_ = false;
    const auto& gpu_info = device_->gpu_info();
    const auto get_host_props = device_->ext().get_memory_host_pointer_properties;
    const uint32_t min_align = std::max(gpu_info.min_imported_host_pointer_alignment, 4096u);
    if (gpu_info.external_memory_host_active && get_host_props != nullptr && shm_base_ != nullptr &&
        (AETHER_VCAM_SLOT_STRIDE_BYTES % min_align) == 0u) {
        bool all_slots_ok = true;
        VkPhysicalDeviceMemoryProperties mem_props{};
        vkGetPhysicalDeviceMemoryProperties(device_->physical_device(), &mem_props);

        for (uint32_t s = 0; s < AETHER_VCAM_SLOTS; ++s) {
            void* slot_ptr =
                shm_base_ + AETHER_VCAM_HEADER_BYTES + static_cast<size_t>(s) * AETHER_VCAM_SLOT_STRIDE_BYTES;
            if ((reinterpret_cast<uintptr_t>(slot_ptr) % min_align) != 0u) {
                all_slots_ok = false;
                break;
            }

            VkExternalMemoryBufferCreateInfo ext_bci{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
            ext_bci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;

            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.pNext = &ext_bci;
            bci.size = AETHER_VCAM_SLOT_STRIDE_BYTES;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(device_->device(), &bci, nullptr, &ext_buffers_[s]) != VK_SUCCESS) {
                all_slots_ok = false;
                break;
            }

            VkMemoryRequirements mreq{};
            vkGetBufferMemoryRequirements(device_->device(), ext_buffers_[s], &mreq);

            VkMemoryHostPointerPropertiesEXT hprops{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
            if (get_host_props(device_->device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, slot_ptr,
                               &hprops) != VK_SUCCESS ||
                mreq.size > AETHER_VCAM_SLOT_STRIDE_BYTES ||
                (mreq.alignment > 0u && (reinterpret_cast<uintptr_t>(slot_ptr) % mreq.alignment) != 0u)) {
                all_slots_ok = false;
                break;
            }

            const uint32_t type_mask = mreq.memoryTypeBits & hprops.memoryTypeBits;
            uint32_t mem_idx = UINT32_MAX;
            for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
                if ((type_mask & (1u << i)) != 0u &&
                    (mem_props.memoryTypes[i].propertyFlags &
                     (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
                    mem_idx = i;
                    break;
                }
            }
            if (mem_idx == UINT32_MAX) {
                for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
                    if ((type_mask & (1u << i)) != 0u) {
                        mem_idx = i;
                        break;
                    }
                }
            }
            if (mem_idx == UINT32_MAX) {
                all_slots_ok = false;
                break;
            }

            VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
            imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            imp.pHostPointer = slot_ptr;

            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.pNext = &imp;
            mai.allocationSize = mreq.size;
            mai.memoryTypeIndex = mem_idx;

            if (vkAllocateMemory(device_->device(), &mai, nullptr, &ext_memories_[s]) != VK_SUCCESS ||
                vkBindBufferMemory(device_->device(), ext_buffers_[s], ext_memories_[s], 0u) != VK_SUCCESS) {
                all_slots_ok = false;
                break;
            }
        }

        if (all_slots_ok) {
            ext_host_mem_active_ = true;
        } else {
            for (uint32_t s = 0; s < AETHER_VCAM_SLOTS; ++s) {
                if (ext_buffers_[s] != VK_NULL_HANDLE) {
                    vkDestroyBuffer(device_->device(), ext_buffers_[s], nullptr);
                    ext_buffers_[s] = VK_NULL_HANDLE;
                }
                if (ext_memories_[s] != VK_NULL_HANDLE) {
                    vkFreeMemory(device_->device(), ext_memories_[s], nullptr);
                    ext_memories_[s] = VK_NULL_HANDLE;
                }
            }
        }
    }

    if (!ext_host_mem_active_) {
        for (uint32_t s = 0; s < AETHER_VCAM_SLOTS; ++s) {
            auto buf_res = device_->is_uma()
                               ? device_->create_uma_zero_copy_buffer(AETHER_VCAM_SLOT_STRIDE_BYTES,
                                                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT)
                               : device_->create_buffer(AETHER_VCAM_SLOT_STRIDE_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                        VMA_MEMORY_USAGE_GPU_TO_CPU,
                                                        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                            VMA_ALLOCATION_CREATE_MAPPED_BIT);
            if (!buf_res) {
                return Unexpected<Error>{buf_res.error()};
            }
            staging_buffers_[s] = buf_res.value();
        }
    }

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = device_->transfer_family();
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(device_->device(), &cpci, nullptr, &dma_pool_) != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "VcamTransport vkCreateCommandPool failed")};
    }

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = dma_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = AETHER_VCAM_SLOTS;
    if (vkAllocateCommandBuffers(device_->device(), &cbai, dma_cmds_.data()) != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "VcamTransport vkAllocateCommandBuffers failed")};
    }

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device_->device(), &fci, nullptr, &dma_fence_) != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "VcamTransport vkCreateFence failed")};
    }

    return {};
}

void VcamTransport::destroy_dma_resources() noexcept {
    if (device_ == nullptr || device_->device() == VK_NULL_HANDLE) {
        return;
    }

    device_->destroy_image(scratch_scale_bgra_);
    scratch_w_ = 0u;
    scratch_h_ = 0u;

    for (uint32_t s = 0; s < AETHER_VCAM_SLOTS; ++s) {
        if (ext_buffers_[s] != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_->device(), ext_buffers_[s], nullptr);
            ext_buffers_[s] = VK_NULL_HANDLE;
        }
        if (ext_memories_[s] != VK_NULL_HANDLE) {
            vkFreeMemory(device_->device(), ext_memories_[s], nullptr);
            ext_memories_[s] = VK_NULL_HANDLE;
        }
        device_->destroy_buffer(staging_buffers_[s]);
    }

    if (dma_fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_->device(), dma_fence_, nullptr);
        dma_fence_ = VK_NULL_HANDLE;
    }
    if (dma_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_->device(), dma_pool_, nullptr);
        dma_pool_ = VK_NULL_HANDLE;
    }
    dma_cmds_.fill(VK_NULL_HANDLE);
}

bool VcamTransport::is_rgb_pin_connected() const noexcept {
    if (shm_base_ == nullptr) {
        return false;
    }
    const long flags = read_volatile_long(&header()->active_pin_flags);
    return (static_cast<uint32_t>(flags) & AETHER_VCAM_PIN_RGB_CONNECTED) != 0u;
}

bool VcamTransport::is_nv12_pin_connected() const noexcept {
    if (shm_base_ == nullptr) {
        return false;
    }
    const long flags = read_volatile_long(&header()->active_pin_flags);
    return (static_cast<uint32_t>(flags) & AETHER_VCAM_PIN_NV12_CONNECTED) != 0u;
}

bool VcamTransport::is_filter_active() const noexcept {
    if (shm_base_ == nullptr) {
        return false;
    }
    const long flags = read_volatile_long(&header()->active_pin_flags);
    return (static_cast<uint32_t>(flags) & AETHER_VCAM_PIN_FILTER_ACTIVE) != 0u;
}

Expected<void> VcamTransport::publish_frame(VkImage src_bgra, VkImage src_nv12_y, VkImage src_nv12_uv, uint32_t width,
                                            uint32_t height, uint32_t fps, bool is_bottom_up, uint64_t frame_sequence,
                                            uint64_t present_qpc_us, uint64_t wait_timeline_value) noexcept {
    if (shm_base_ == nullptr || device_ == nullptr || width == 0u || height == 0u) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "VcamTransport::publish_frame invalid state")};
    }

    const uint64_t t0_us = now_us();
    const bool rgb_pin = is_rgb_pin_connected();
    const bool nv12_pin = is_nv12_pin_connected();
    const bool want_bgra = (src_bgra != VK_NULL_HANDLE) && (rgb_pin || !nv12_pin);
    const bool want_nv12 = (src_nv12_y != VK_NULL_HANDLE && src_nv12_uv != VK_NULL_HANDLE) && nv12_pin;

    if (!want_bgra && !want_nv12) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "VcamTransport::publish_frame no valid image")};
    }

    const uint32_t out_w = std::min(width, AETHER_VCAM_MAX_WIDTH);
    const uint32_t out_h = std::min(height, AETHER_VCAM_MAX_HEIGHT);
    const bool downscale = (out_w != width || out_h != height);
    const bool direct_ext = ext_host_mem_active_ && !downscale && (ext_buffers_[0] != VK_NULL_HANDLE);

    const VkDeviceSize src_bgra_bytes = static_cast<VkDeviceSize>(width) * height * 4ull;
    const VkDeviceSize src_y_bytes = static_cast<VkDeviceSize>(width) * height;
    const VkDeviceSize src_uv_bytes =
        static_cast<VkDeviceSize>(std::max(width / 2u, 1u)) * std::max(height / 2u, 1u) * 2ull;
    const VkDeviceSize needed_staging =
        (want_bgra ? src_bgra_bytes : 0ull) + (want_nv12 ? (src_y_bytes + src_uv_bytes) : 0ull);

    const uint32_t slot = slot_cursor_;
    slot_cursor_ = (slot_cursor_ + 1u) % AETHER_VCAM_SLOTS;

    // Grow staging buffers only on off-path fallback / >1080p resolution changes
    if (!direct_ext && staging_buffers_[slot].size < needed_staging) {
        for (uint32_t s = 0; s < AETHER_VCAM_SLOTS; ++s) {
            device_->destroy_buffer(staging_buffers_[s]);
            auto b =
                device_->create_buffer(needed_staging, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU,
                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
            if (!b) {
                return Unexpected<Error>{b.error()};
            }
            staging_buffers_[s] = b.value();
        }
    }

    const VkBuffer dst_vk_buf = direct_ext ? ext_buffers_[slot] : staging_buffers_[slot].handle;
    const VkCommandBuffer cmd = dma_cmds_[slot];
    (void)vkResetCommandBuffer(cmd, 0u);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "VcamTransport vkBeginCommandBuffer failed")};
    }

    VkDeviceSize cursor = 0u;
    const VkDeviceSize stg_bgra_off = 0u;
    if (want_bgra) {
        VkBufferImageCopy r_bgra{};
        r_bgra.bufferOffset = stg_bgra_off;
        r_bgra.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
        r_bgra.imageExtent = {width, height, 1u};
        vkCmdCopyImageToBuffer(cmd, src_bgra, VK_IMAGE_LAYOUT_GENERAL, dst_vk_buf, 1u, &r_bgra);
        cursor = src_bgra_bytes;
    }

    const VkDeviceSize stg_y_off = direct_ext ? AETHER_VCAM_NV12_OFFSET_IN_SLOT : cursor;
    const VkDeviceSize stg_uv_off = stg_y_off + src_y_bytes;
    if (want_nv12) {
        VkBufferImageCopy r_y{};
        r_y.bufferOffset = stg_y_off;
        r_y.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
        r_y.imageExtent = {width, height, 1u};
        vkCmdCopyImageToBuffer(cmd, src_nv12_y, VK_IMAGE_LAYOUT_GENERAL, dst_vk_buf, 1u, &r_y);

        VkBufferImageCopy r_uv{};
        r_uv.bufferOffset = stg_uv_off;
        r_uv.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
        r_uv.imageExtent = {std::max(width / 2u, 1u), std::max(height / 2u, 1u), 1u};
        vkCmdCopyImageToBuffer(cmd, src_nv12_uv, VK_IMAGE_LAYOUT_GENERAL, dst_vk_buf, 1u, &r_uv);
        cursor = stg_uv_off + src_uv_bytes;
    }

    VkBufferMemoryBarrier2 bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    bmb.buffer = dst_vk_buf;
    bmb.offset = 0u;
    bmb.size = VK_WHOLE_SIZE;
    bmb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    bmb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bmb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    bmb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1u;
    dep.pBufferMemoryBarriers = &bmb;
    vkCmdPipelineBarrier2(cmd, &dep);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "VcamTransport vkEndCommandBuffer failed")};
    }

    (void)vkResetFences(device_->device(), 1u, &dma_fence_);

    VkSemaphoreSubmitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait_info.semaphore = device_->frame_timeline();
    wait_info.value = wait_timeline_value;
    wait_info.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;

    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;

    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    if (wait_timeline_value > 0u) {
        si.waitSemaphoreInfoCount = 1u;
        si.pWaitSemaphoreInfos = &wait_info;
    }
    si.commandBufferInfoCount = 1u;
    si.pCommandBufferInfos = &cmd_info;

    if (vkQueueSubmit2(device_->transfer_queue(), 1u, &si, dma_fence_) != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "VcamTransport vkQueueSubmit2 failed")};
    }
    (void)vkWaitForFences(device_->device(), 1u, &dma_fence_, VK_TRUE, 1'000'000'000ULL);

    uint8_t* slot_ptr =
        shm_base_ + AETHER_VCAM_HEADER_BYTES + static_cast<size_t>(slot) * AETHER_VCAM_SLOT_STRIDE_BYTES;

    auto* hdr = header();
    const long cur_seq = read_volatile_long(&hdr->write_seq);
    write_volatile_long(&hdr->write_seq, cur_seq | 1L);

    const uint32_t out_bgra_bytes = out_w * out_h * 4u;
    if (!direct_ext) {
        (void)vmaInvalidateAllocation(device_->allocator(), staging_buffers_[slot].allocation, 0u, cursor);
        const auto* stg_ptr = static_cast<const uint8_t*>(staging_buffers_[slot].info.pMappedData);
        if (want_bgra && stg_ptr != nullptr) {
            if (!downscale) {
                std::memcpy(slot_ptr, stg_ptr + stg_bgra_off, out_bgra_bytes);
            } else {
                copy_scaled_bgra(stg_ptr + stg_bgra_off, width, height, slot_ptr, out_w, out_h);
            }
        }
        if (want_nv12 && stg_ptr != nullptr) {
            uint8_t* dst_y = slot_ptr + AETHER_VCAM_NV12_OFFSET_IN_SLOT;
            uint8_t* dst_uv = dst_y + static_cast<size_t>(out_w) * out_h;
            if (!downscale) {
                std::memcpy(dst_y, stg_ptr + stg_y_off, static_cast<size_t>(src_y_bytes + src_uv_bytes));
            } else {
                copy_scaled_nv12(stg_ptr + stg_y_off, stg_ptr + stg_uv_off, width, height, dst_y, dst_uv, out_w, out_h);
            }
        }
    }

    hdr->width = out_w;
    hdr->height = out_h;
    hdr->fps_milli = fps * 1000u;
    hdr->active_fourcc = (want_nv12 && !want_bgra) ? AETHER_VCAM_FOURCC_NV12 : AETHER_VCAM_FOURCC_ARGB32;
    hdr->bgra_byte_size = out_bgra_bytes;
    hdr->bottom_up = is_bottom_up ? 1u : 0u;
    hdr->latest_slot = slot;
    hdr->producer_qpc_us = present_qpc_us;
    hdr->slot_qpc_us[slot] = present_qpc_us;
    hdr->slot_frame_seq[slot] = frame_sequence;

    write_volatile_long(&hdr->write_seq, (cur_seq + 2L) & ~1L);

    last_dma_ms_ = static_cast<double>(now_us() - t0_us) / 1000.0;
    ++frames_published_;
    return {};
}

Expected<void> VcamTransport::publish_host_bgra8(std::span<const uint8_t> bgra, uint32_t width, uint32_t height,
                                                 uint32_t fps, bool is_bottom_up, uint64_t frame_sequence,
                                                 uint64_t present_qpc_us) noexcept {
    if (shm_base_ == nullptr || width == 0u || height == 0u || bgra.size() < static_cast<size_t>(width) * height * 4u) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "VcamTransport::publish_host_bgra8 invalid args")};
    }

    const uint64_t t0_us = now_us();
    const uint32_t out_w = std::min(width, AETHER_VCAM_MAX_WIDTH);
    const uint32_t out_h = std::min(height, AETHER_VCAM_MAX_HEIGHT);
    const bool downscale = (out_w != width || out_h != height);
    const uint32_t out_bgra_bytes = out_w * out_h * 4u;

    const uint32_t slot = slot_cursor_;
    slot_cursor_ = (slot_cursor_ + 1u) % AETHER_VCAM_SLOTS;
    uint8_t* slot_ptr =
        shm_base_ + AETHER_VCAM_HEADER_BYTES + static_cast<size_t>(slot) * AETHER_VCAM_SLOT_STRIDE_BYTES;

    auto* hdr = header();
    const long cur_seq = read_volatile_long(&hdr->write_seq);
    write_volatile_long(&hdr->write_seq, cur_seq | 1L);

    if (!downscale) {
        std::memcpy(slot_ptr, bgra.data(), out_bgra_bytes);
    } else {
        copy_scaled_bgra(bgra.data(), width, height, slot_ptr, out_w, out_h);
    }

    hdr->width = out_w;
    hdr->height = out_h;
    hdr->fps_milli = fps * 1000u;
    hdr->active_fourcc = AETHER_VCAM_FOURCC_ARGB32;
    hdr->bgra_byte_size = out_bgra_bytes;
    hdr->bottom_up = is_bottom_up ? 1u : 0u;
    hdr->latest_slot = slot;
    hdr->producer_qpc_us = present_qpc_us;
    hdr->slot_qpc_us[slot] = present_qpc_us;
    hdr->slot_frame_seq[slot] = frame_sequence;

    write_volatile_long(&hdr->write_seq, (cur_seq + 2L) & ~1L);

    last_dma_ms_ = static_cast<double>(now_us() - t0_us) / 1000.0;
    ++frames_published_;
    return {};
}

} // namespace aether::output
