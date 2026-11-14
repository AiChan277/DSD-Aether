#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "output/ndi_output.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace aether::output {

namespace {

template <typename Fn> Fn load_proc(HMODULE mod, const char* name) noexcept {
    static_assert(sizeof(FARPROC) == sizeof(Fn));
    const FARPROC proc = GetProcAddress(mod, name);
    Fn fn = nullptr;
    if (proc != nullptr) {
        std::memcpy(static_cast<void*>(&fn), static_cast<const void*>(&proc), sizeof(fn));
    }
    return fn;
}

bool build_ndi_dll_path(wchar_t* out_path, size_t cap) noexcept {
    wchar_t dir[512]{};
    const DWORD n = GetEnvironmentVariableW(L"NDI_RUNTIME_DIR_V6", dir, 512u);
    if (n == 0u || n >= 500u) {
        return false;
    }
    const bool trailing_slash = (dir[n - 1u] == L'\\' || dir[n - 1u] == L'/');
    const int wrote = std::swprintf(
        out_path, cap, trailing_slash ? L"%sProcessing.NDI.Lib.x64.dll" : L"%s\\Processing.NDI.Lib.x64.dll", dir);
    return wrote > 0;
}

inline uint8_t clamp_u8(int v) noexcept {
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

// BT.709 limited-range conversion (`Y [16..235]`, `Cb/Cr [16..240]`) from BGRA
inline void bgr_to_ycbcr709(uint8_t b, uint8_t g, uint8_t r, uint8_t& y, int& cb, int& cr) noexcept {
    const int ri = r;
    const int gi = g;
    const int bi = b;
    y = clamp_u8(((47 * ri + 157 * gi + 16 * bi + 128) >> 8) + 16);
    cb = ((-25 * ri - 86 * gi + 112 * bi + 128) >> 8) + 128;
    cr = ((112 * ri - 102 * gi - 10 * bi + 128) >> 8) + 128;
}

} // namespace

bool NdiOutput::is_runtime_dll_present() {
    wchar_t path[600]{};
    if (!build_ndi_dll_path(path, 600u)) {
        return false;
    }
    const DWORD attrs = GetFileAttributesW(path);
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0u);
}

Expected<void> NdiOutput::init(rhi::RhiDevice& device, uint32_t width, uint32_t height, NdiVideoFormat format,
                               const NdiDispatchTable* test_override) {
    shutdown();

    if (device.device() == VK_NULL_HANDLE || width == 0u || height == 0u) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "NdiOutput::init invalid args")};
    }

    device_ = &device;
    width_ = width;
    height_ = height;
    format_ = format;
    buf_idx_ = 0u;
    frames_sent_ = 0u;
    last_send_ms_ = 0.0;

    if (test_override != nullptr) {
        api_ = *test_override;
    } else {
        wchar_t dll_path[600]{};
        if (!build_ndi_dll_path(dll_path, 600u)) {
            shutdown();
            return Unexpected<Error>{Error::make(ErrorCode::IoError, "NDI_RUNTIME_DIR_V6 not set")};
        }
        HMODULE mod = LoadLibraryW(dll_path);
        if (mod == nullptr) {
            shutdown();
            return Unexpected<Error>{Error::make(ErrorCode::IoError, "Processing.NDI.Lib.x64.dll load failed")};
        }
        dll_module_ = mod;
        api_.initialize = load_proc<bool (*)()>(mod, "NDIlib_initialize");
        api_.destroy = load_proc<void (*)()>(mod, "NDIlib_destroy");
        api_.send_create = load_proc<void* (*)(const NdiSendCreateAbi*)>(mod, "NDIlib_send_create");
        api_.send_destroy = load_proc<void (*)(void*)>(mod, "NDIlib_send_destroy");
        api_.send_video_async_v2 =
            load_proc<void (*)(void*, const NdiVideoFrameV2Abi*)>(mod, "NDIlib_send_send_video_async_v2");
    }

    if (api_.initialize == nullptr || api_.destroy == nullptr || api_.send_create == nullptr ||
        api_.send_destroy == nullptr || api_.send_video_async_v2 == nullptr) {
        shutdown();
        return Unexpected<Error>{Error::make(ErrorCode::Unsupported, "NDI dispatch table incomplete")};
    }

    if (!api_.initialize()) {
        shutdown();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "NDIlib_initialize failed")};
    }
    initialized_ndi_lib_ = true;

    char host[64]{"Studio"};
    DWORD host_len = sizeof(host);
    if (GetComputerNameA(host, &host_len) == FALSE || host[0] == '\0') {
        std::snprintf(host, sizeof(host), "Studio");
    }
    std::snprintf(sender_name_.data(), sender_name_.size(), "Aether 2.5D (%s)", host);

    NdiSendCreateAbi create{};
    create.ndi_name = sender_name_.data();
    create.groups = nullptr;
    create.clock_video = false;
    create.clock_audio = false;
    sender_ = api_.send_create(&create);
    if (sender_ == nullptr) {
        shutdown();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "NDIlib_send_create failed")};
    }

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = device.transfer_family();
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(device.device(), &cpci, nullptr, &cmd_pool_) != VK_SUCCESS) {
        shutdown();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "NdiOutput vkCreateCommandPool failed")};
    }

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmd_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 2u;
    if (vkAllocateCommandBuffers(device.device(), &cbai, cmds_.data()) != VK_SUCCESS) {
        shutdown();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "NdiOutput vkAllocateCommandBuffers failed")};
    }

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device.device(), &fci, nullptr, &fence_) != VK_SUCCESS) {
        shutdown();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "NdiOutput vkCreateFence failed")};
    }

    if (auto res = allocate_buffers(width_, height_); !res) {
        shutdown();
        return res;
    }

    AETHER_LOG_INFO("output", "NdiOutput initialized: sender='%s' %ux%u fmt=%s", sender_name_.data(), width_, height_,
                    (format_ == NdiVideoFormat::UyvaStraight) ? "UYVA" : "BGRA");
    return {};
}

Expected<void> NdiOutput::resize(uint32_t width, uint32_t height) {
    if (sender_ == nullptr || device_ == nullptr || width == 0u || height == 0u) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "NdiOutput::resize invalid state")};
    }
    if (width == width_ && height == height_) {
        return {};
    }
    free_buffers();
    width_ = width;
    height_ = height;
    return allocate_buffers(width_, height_);
}

void NdiOutput::shutdown() noexcept {
    if (sender_ != nullptr && api_.send_destroy != nullptr) {
        api_.send_destroy(sender_);
        sender_ = nullptr;
    }
    if (initialized_ndi_lib_ && api_.destroy != nullptr) {
        api_.destroy();
        initialized_ndi_lib_ = false;
    }

    free_buffers();

    if (device_ != nullptr && device_->device() != VK_NULL_HANDLE) {
        if (fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_->device(), fence_, nullptr);
            fence_ = VK_NULL_HANDLE;
        }
        if (cmd_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_->device(), cmd_pool_, nullptr);
            cmd_pool_ = VK_NULL_HANDLE;
        }
    }
    cmds_.fill(VK_NULL_HANDLE);

    if (dll_module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(dll_module_));
        dll_module_ = nullptr;
    }

    api_ = NdiDispatchTable{};
    width_ = 0u;
    height_ = 0u;
    frames_sent_ = 0u;
    last_send_ms_ = 0.0;
    buf_idx_ = 0u;
    device_ = nullptr;
}

Expected<void> NdiOutput::allocate_buffers(uint32_t width, uint32_t height) {
    if (device_ == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "null device")};
    }

    const VkDeviceSize bgra_bytes = static_cast<VkDeviceSize>(width) * height * 4ull;
    uyva_bytes_ = static_cast<size_t>(width) * height * 3u;
    for (uint32_t i = 0; i < 2u; ++i) {
        auto b =
            device_->create_buffer(bgra_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!b) {
            free_buffers();
            return Unexpected<Error>{b.error()};
        }
        host_staging_[i] = b.value();
        if (format_ == NdiVideoFormat::UyvaStraight) {
            uyva_buffers_[i] = static_cast<uint8_t*>(std::calloc(uyva_bytes_, 1u));
            if (uyva_buffers_[i] == nullptr) {
                free_buffers();
                return Unexpected<Error>{Error::make(ErrorCode::OutOfMemory, "NdiOutput UYVA alloc failed")};
            }
        }
    }
    return {};
}

void NdiOutput::free_buffers() noexcept {
    if (device_ != nullptr && device_->device() != VK_NULL_HANDLE) {
        for (uint32_t i = 0; i < 2u; ++i) {
            device_->destroy_buffer(host_staging_[i]);
        }
    }
    for (uint32_t i = 0; i < 2u; ++i) {
        if (uyva_buffers_[i] != nullptr) {
            std::free(uyva_buffers_[i]);
            uyva_buffers_[i] = nullptr;
        }
    }
    uyva_bytes_ = 0u;
}

void NdiOutput::pack_bgra_straight_to_uyva(const uint8_t* bgra, uint32_t width, uint32_t height,
                                           uint8_t* out_uyva) noexcept {
    if (bgra == nullptr || out_uyva == nullptr || width == 0u || height == 0u) {
        return;
    }

    uint8_t* uyvy_plane = out_uyva;
    uint8_t* alpha_plane = out_uyva + static_cast<size_t>(width) * height * 2u;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* srow = bgra + static_cast<size_t>(y) * width * 4u;
        uint8_t* urow = uyvy_plane + static_cast<size_t>(y) * width * 2u;
        uint8_t* arow = alpha_plane + static_cast<size_t>(y) * width;

        for (uint32_t x = 0; x < width; x += 2u) {
            const uint8_t* p0 = srow + static_cast<size_t>(x) * 4u;
            const uint8_t* p1 = (x + 1u < width) ? (srow + static_cast<size_t>(x + 1u) * 4u) : p0;

            uint8_t y0 = 16u;
            uint8_t y1 = 16u;
            int cb0 = 128;
            int cr0 = 128;
            int cb1 = 128;
            int cr1 = 128;
            bgr_to_ycbcr709(p0[0], p0[1], p0[2], y0, cb0, cr0);
            bgr_to_ycbcr709(p1[0], p1[1], p1[2], y1, cb1, cr1);

            urow[static_cast<size_t>(x) * 2u + 0u] = clamp_u8((cb0 + cb1) >> 1);
            urow[static_cast<size_t>(x) * 2u + 1u] = y0;
            if (x + 1u < width) {
                urow[static_cast<size_t>(x) * 2u + 2u] = clamp_u8((cr0 + cr1) >> 1);
                urow[static_cast<size_t>(x) * 2u + 3u] = y1;
            }

            arow[x] = p0[3];
            if (x + 1u < width) {
                arow[x + 1u] = p1[3];
            }
        }
    }
}

Expected<void> NdiOutput::send_frame(VkImage src_bgra_straight, uint32_t width, uint32_t height, uint32_t fps,
                                     uint64_t present_qpc_us, uint64_t wait_timeline_value) noexcept {
    if (sender_ == nullptr || device_ == nullptr || src_bgra_straight == VK_NULL_HANDLE) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "NdiOutput::send_frame invalid state")};
    }
    if (width != width_ || height != height_) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "NdiOutput::send_frame extent mismatch")};
    }

    const uint64_t t0_us = now_us();
    const uint32_t idx = buf_idx_;
    const VkCommandBuffer cmd = cmds_[idx];

    (void)vkResetCommandBuffer(cmd, 0u);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "NdiOutput vkBeginCommandBuffer failed")};
    }

    VkBufferImageCopy reg{};
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    reg.imageExtent = {width_, height_, 1u};
    vkCmdCopyImageToBuffer(cmd, src_bgra_straight, VK_IMAGE_LAYOUT_GENERAL, host_staging_[idx].handle, 1u, &reg);

    VkBufferMemoryBarrier2 bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    bmb.buffer = host_staging_[idx].handle;
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
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "NdiOutput vkEndCommandBuffer failed")};
    }

    (void)vkResetFences(device_->device(), 1u, &fence_);
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

    if (vkQueueSubmit2(device_->transfer_queue(), 1u, &si, fence_) != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "NdiOutput vkQueueSubmit2 failed")};
    }
    (void)vkWaitForFences(device_->device(), 1u, &fence_, VK_TRUE, 1'000'000'000ULL);

    const VkDeviceSize bgra_bytes = static_cast<VkDeviceSize>(width_) * height_ * 4ull;
    (void)vmaInvalidateAllocation(device_->allocator(), host_staging_[idx].allocation, 0u, bgra_bytes);

    auto* bgra_ptr = static_cast<uint8_t*>(host_staging_[idx].info.pMappedData);
    uint8_t* send_ptr = bgra_ptr;
    int32_t stride = static_cast<int32_t>(width_ * 4u);
    uint32_t fourcc = kNdiFourCcBgra;
    if (format_ == NdiVideoFormat::UyvaStraight && uyva_buffers_[idx] != nullptr) {
        pack_bgra_straight_to_uyva(bgra_ptr, width_, height_, uyva_buffers_[idx]);
        send_ptr = uyva_buffers_[idx];
        stride = static_cast<int32_t>(width_ * 2u);
        fourcc = kNdiFourCcUyva;
    }

    NdiVideoFrameV2Abi frame{};
    frame.xres = static_cast<int32_t>(width_);
    frame.yres = static_cast<int32_t>(height_);
    frame.fourcc = fourcc;
    frame.frame_rate_n = static_cast<int32_t>(std::max(fps, 1u)) * 1000;
    frame.frame_rate_d = 1000;
    frame.picture_aspect_ratio = static_cast<float>(width_) / static_cast<float>(height_);
    frame.frame_format_type = 1;                                  // progressive
    frame.timecode = static_cast<int64_t>(present_qpc_us) * 10LL; // 100 ns units
    frame.p_data = send_ptr;
    frame.line_stride_in_bytes = stride;
    frame.p_metadata = nullptr;
    frame.timestamp = static_cast<int64_t>(present_qpc_us) * 10LL;

    api_.send_video_async_v2(sender_, &frame);
    buf_idx_ ^= 1u;

    last_send_ms_ = static_cast<double>(now_us() - t0_us) / 1000.0;
    ++frames_sent_;
    return {};
}

} // namespace aether::output
