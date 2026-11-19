#include "renderer/dx12_bridge.hpp"

#include "core/clock.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#if defined(_WIN32)
#include <vulkan/vulkan_win32.h>
#endif

namespace aether::renderer {

namespace {

struct RetiredDx12BridgeObjects {
    const rhi::RhiDevice* device{nullptr};
    VkImage imported_image{VK_NULL_HANDLE};
    VkDeviceMemory imported_memory{VK_NULL_HANDLE};
    rhi::RhiImage owned_image{};
    rhi::RhiBuffer staging{};
};

void finalize_retired_dx12_bridge_objects(void* handle, uint64_t /*user_tag*/, uint64_t /*byte_size*/) noexcept {
    auto* r = static_cast<RetiredDx12BridgeObjects*>(handle);
    if (r == nullptr) {
        return;
    }
    if (r->device != nullptr && r->device->device() != VK_NULL_HANDLE) {
        if (r->imported_image != VK_NULL_HANDLE) {
            vkDestroyImage(r->device->device(), r->imported_image, nullptr);
        }
        if (r->imported_memory != VK_NULL_HANDLE) {
            vkFreeMemory(r->device->device(), r->imported_memory, nullptr);
        }
        r->device->destroy_image(r->owned_image);
        r->device->destroy_buffer(r->staging);
    }
    delete r;
}

uint32_t find_memory_type(VkPhysicalDevice pd, uint32_t type_filter, VkMemoryPropertyFlags props) noexcept {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) != 0u && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) != 0u) {
            return i;
        }
    }
    return UINT32_MAX;
}

} // namespace

Dx12Bridge::~Dx12Bridge() noexcept {
    release();
}

void Dx12Bridge::attach(rhi::RhiDevice& device, GarbageCollector& gc) noexcept {
    vk_device_ = &device;
    gc_ = &gc;
}

bool Dx12Bridge::wait_last_read() noexcept {
    if (vk_device_ == nullptr || vk_device_->device() == VK_NULL_HANDLE || last_read_timeline_ == 0U) {
        return true;
    }
    VkSemaphore sem = vk_device_->frame_timeline();
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &sem;
    wi.pValues = &last_read_timeline_;
    constexpr uint64_t kTimeoutNs = 250ULL * 1000ULL * 1000ULL;
    return vkWaitSemaphores(vk_device_->device(), &wi, kTimeoutNs) == VK_SUCCESS;
}

void Dx12Bridge::retire_vulkan_objects() noexcept {
    if (image_ == VK_NULL_HANDLE && memory_ == VK_NULL_HANDLE && staging_.handle == VK_NULL_HANDLE) {
        return;
    }
    auto* r = new (std::nothrow) RetiredDx12BridgeObjects{};
    if (r != nullptr) {
        r->device = vk_device_;
        if (foreign_) {
            r->imported_image = image_;
            r->imported_memory = memory_;
        } else {
            r->owned_image.handle = image_;
            r->owned_image.allocation = owned_allocation_;
        }
        r->staging = staging_;
    }
    const uint64_t bytes = static_cast<uint64_t>(width_) * height_ * 4ULL;
    if (r == nullptr || gc_ == nullptr ||
        !gc_->retire(r, bytes, last_read_timeline_, &finalize_retired_dx12_bridge_objects,
                     /*user_tag=*/0x44583132ULL /* 'DX12' */)) {
        (void)wait_last_read();
        finalize_retired_dx12_bridge_objects(r, 0, bytes);
    }
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    owned_allocation_ = nullptr;
    staging_ = {};
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    has_frame_ = false;
}

Expected<void> Dx12Bridge::create_vulkan_image(bool try_import) {
    foreign_ = false;
#if defined(_WIN32)
    void* nt_handle = fg_.shared_rgba16f_nt_handle();
    const bool same_luid = (d3d12_ != nullptr) && vk_device_->gpu_info().luid_valid &&
                           d3d12_->gpu_info().luid_valid &&
                           std::memcmp(vk_device_->gpu_info().luid.data(), d3d12_->gpu_info().luid.data(), 8u) == 0;
    if (try_import && same_luid && nt_handle != nullptr && vk_device_->gpu_info().external_memory_win32_active) {
        VkExternalMemoryImageCreateInfo ext_ci{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext_ci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &ext_ci;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ici.extent = {width_, height_, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage img = VK_NULL_HANDLE;
        if (vkCreateImage(vk_device_->device(), &ici, nullptr, &img) == VK_SUCCESS) {
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(vk_device_->device(), img, &req);
            uint32_t type_bits = req.memoryTypeBits;
            auto get_props = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
                vkGetDeviceProcAddr(vk_device_->device(), "vkGetMemoryWin32HandlePropertiesKHR"));
            if (get_props != nullptr) {
                VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
                if (get_props(vk_device_->device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, nt_handle,
                              &hp) == VK_SUCCESS &&
                    hp.memoryTypeBits != 0u) {
                    type_bits &= hp.memoryTypeBits;
                }
            }
            const uint32_t mem_idx =
                find_memory_type(vk_device_->physical_device(), type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkDeviceMemory mem = VK_NULL_HANDLE;
            if (mem_idx != UINT32_MAX) {
                VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
                ded.image = img;
                VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
                imp.pNext = &ded;
                imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
                imp.handle = nt_handle;
                VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                mai.pNext = &imp;
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = mem_idx;
                if (vkAllocateMemory(vk_device_->device(), &mai, nullptr, &mem) == VK_SUCCESS &&
                    vkBindImageMemory(vk_device_->device(), img, mem, 0) == VK_SUCCESS) {
                    image_ = img;
                    memory_ = mem;
                    foreign_ = true;
                    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
                    return {};
                }
            }
            if (mem != VK_NULL_HANDLE) {
                vkFreeMemory(vk_device_->device(), mem, nullptr);
            }
            vkDestroyImage(vk_device_->device(), img, nullptr);
        }
        AETHER_LOG_WARN("renderer", "Dx12Bridge: D3D12 NT import unavailable, using sRGB-accurate staging copy");
    }
#else
    (void)try_import;
#endif

    // sRGB-accurate CPU readback fallback: VK_FORMAT_R8G8B8A8_SRGB auto-linearizes when blitted to out_rgba16f_.
    auto img_res = vk_device_->create_image_2d(width_, height_, VK_FORMAT_R8G8B8A8_SRGB,
                                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                   VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!img_res) {
        return Unexpected<Error>{img_res.error()};
    }
    rhi::RhiImage owned = img_res.value();
    if (owned.view != VK_NULL_HANDLE) {
        vk_device_->destroy_image_view(owned.view);
        owned.view = VK_NULL_HANDLE;
    }
    auto buf_res = vk_device_->create_buffer(
        static_cast<VkDeviceSize>(width_) * height_ * 4ULL, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!buf_res) {
        vk_device_->destroy_image(owned);
        return Unexpected<Error>{buf_res.error()};
    }
    image_ = owned.handle;
    owned_allocation_ = owned.allocation;
    staging_ = buf_res.value();
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    return {};
}

Expected<void> Dx12Bridge::configure(uint32_t width, uint32_t height, uint32_t max_vertices,
                                     const rhi::SelectedGpuInfo* preferred_gpu) {
    if (vk_device_ == nullptr || gc_ == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "Dx12Bridge::configure before attach")};
    }
    if (width == 0U || height == 0U) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "Dx12Bridge::configure zero size")};
    }
    max_vertices = std::max(max_vertices, 4U);

    const rhi::SelectedGpuInfo& gpu = (preferred_gpu != nullptr) ? *preferred_gpu : vk_device_->gpu_info();
    const bool want_igpu = gpu.is_intel_igpu || gpu.is_uma;
    const bool same_luid = (target_luid_valid_ == gpu.luid_valid) &&
                           (!gpu.luid_valid || std::memcmp(target_luid_.data(), gpu.luid.data(), 8u) == 0);
    const bool same_device_cfg =
        (d3d12_ != nullptr && d3d12_->is_valid()) && same_luid && (target_prefer_igpu_ == want_igpu);

    if (ready() && same_device_cfg && width == width_ && height == height_) {
        if (max_vertices > max_vertices_) {
            if (auto rc = fg_.ensure_upload_capacity(max_vertices); !rc) {
                return rc;
            }
            max_vertices_ = max_vertices;
        }
        return {};
    }

    if (foreign_) {
        (void)wait_last_read();
    }
    retire_vulkan_objects();

    if (!same_device_cfg) {
        fg_.destroy();
        if (d3d12_ != nullptr) {
            d3d12_->destroy();
            delete d3d12_;
            d3d12_ = nullptr;
        }
        rhi::D3D12DeviceConfig dcfg{};
#if defined(AETHER_ENABLE_VULKAN_VALIDATION) && AETHER_ENABLE_VULKAN_VALIDATION
        dcfg.enable_debug_layer = false; // Keep live viewport responsive; standalone tests enable debug layer
#endif
        dcfg.prefer_integrated = want_igpu;
        dcfg.allow_warp_fallback = true;
        if (gpu.luid_valid) {
            dcfg.preferred_luid = gpu.luid;
            dcfg.use_preferred_luid = true;
        }
        auto d_res = rhi::D3D12Device::create(dcfg);
        if (!d_res) {
            return Unexpected<Error>{d_res.error()};
        }
        d3d12_ = d_res.value();
        target_luid_ = gpu.luid;
        target_luid_valid_ = gpu.luid_valid;
        target_prefer_igpu_ = want_igpu;

        if (auto rc = fg_.init(*d3d12_, width, height, max_vertices, true); !rc) {
            d3d12_->destroy();
            delete d3d12_;
            d3d12_ = nullptr;
            return rc;
        }
        max_vertices_ = max_vertices;
        if (bound_avatar_ != nullptr) {
            if (auto brc = fg_.bind_avatar(bound_avatar_); !brc) {
                bound_avatar_ = nullptr;
                return brc;
            }
        }
    } else {
        if (width != width_ || height != height_) {
            if (auto rc = fg_.resize(width, height); !rc) {
                return rc;
            }
        }
        if (max_vertices > max_vertices_) {
            if (auto rc = fg_.ensure_upload_capacity(max_vertices); !rc) {
                return rc;
            }
            max_vertices_ = max_vertices;
        }
        if (bound_avatar_ != nullptr && fg_.bound_avatar() != bound_avatar_) {
            if (auto brc = fg_.bind_avatar(bound_avatar_); !brc) {
                bound_avatar_ = nullptr;
                return brc;
            }
        }
    }

    width_ = width;
    height_ = height;
    if (auto rc = create_vulkan_image(!force_cpu_copy_); !rc) {
        return rc;
    }

    AETHER_LOG_INFO("renderer", "Dx12Bridge ready on '%s' %ux%u (UMA=%d DXR1.1=%d mode=%s)",
                    d3d12_->gpu_info().device_name.c_str(), width_, height_, d3d12_->is_uma() ? 1 : 0,
                    d3d12_->dxr_supported() ? 1 : 0, foreign_ ? "NT-Shared" : "sRGB-Staging");
    return {};
}

Expected<void> Dx12Bridge::bind_avatar(const GpuAvatar* avatar) {
    if (!fg_.pipelines_ready()) {
        bound_avatar_ = avatar;
        return {};
    }
    if (auto rc = fg_.bind_avatar(avatar); !rc) {
        bound_avatar_ = nullptr;
        return rc;
    }
    bound_avatar_ = avatar;
    if (avatar != nullptr) {
        max_vertices_ = std::max(max_vertices_, std::max(avatar->requirements.vertex_count, 4U));
    }
    return {};
}

Expected<void> Dx12Bridge::apply_face_sdf(const SdfBakeOutput& baked) {
    if (!fg_.pipelines_ready()) {
        return {};
    }
    return fg_.apply_face_sdf(baked);
}

bool Dx12Bridge::render(const FrameRenderRequest& req, std::span<uint8_t> optional_bgra_out,
                        bool bottom_up_bgra) noexcept {
    if (!ready()) {
        return false;
    }
    if (req.avatar == nullptr || req.avatar != bound_avatar_ || fg_.bound_avatar() != req.avatar) {
        return false;
    }
    const uint64_t t0 = now_us();
    if (foreign_ && !wait_last_read()) {
        return false;
    }

    if (auto r = fg_.render_frame(req, true); !r) {
        return false;
    }

    if (!foreign_) {
        if (staging_.info.pMappedData == nullptr) {
            return false;
        }
        const size_t bytes = static_cast<size_t>(width_) * height_ * 4U;
        if (!fg_.readback_rgba8(std::span<uint8_t>(static_cast<uint8_t*>(staging_.info.pMappedData), bytes))) {
            return false;
        }
        if (vk_device_ != nullptr) {
            (void)vmaFlushAllocation(vk_device_->allocator(), staging_.allocation, 0, bytes);
        }
    }

    if (!optional_bgra_out.empty()) {
        const size_t bytes = static_cast<size_t>(width_) * height_ * 4U;
        if (optional_bgra_out.size() >= bytes) {
            (void)fg_.readback_bgra8(optional_bgra_out.subspan(0, bytes), bottom_up_bgra);
        }
    }

    has_frame_ = true;
    ++frames_;
    last_render_ms_ = static_cast<float>(static_cast<double>(now_us() - t0) * 0.001);
    return true;
}

void Dx12Bridge::fill_request(FrameRenderRequest& req) const noexcept {
    if (!has_frame_ || image_ == VK_NULL_HANDLE) {
        return;
    }
    req.external_color_image = image_;
    req.external_width = width_;
    req.external_height = height_;
    req.external_layout = layout_;
    req.external_is_foreign = foreign_;
    req.external_staging_buffer = foreign_ ? VK_NULL_HANDLE : staging_.handle;
}

void Dx12Bridge::on_submitted(uint64_t timeline_value) noexcept {
    if (!has_frame_ || image_ == VK_NULL_HANDLE || timeline_value == 0U) {
        return;
    }
    last_read_timeline_ = timeline_value;
    layout_ = VK_IMAGE_LAYOUT_GENERAL;
}

void Dx12Bridge::release() noexcept {
    if (vk_device_ != nullptr && foreign_) {
        (void)wait_last_read();
    }
    if (vk_device_ != nullptr) {
        retire_vulkan_objects();
    }
    fg_.destroy();
    if (d3d12_ != nullptr) {
        d3d12_->destroy();
        delete d3d12_;
        d3d12_ = nullptr;
    }
    bound_avatar_ = nullptr;
    foreign_ = false;
    has_frame_ = false;
    last_read_timeline_ = 0ULL;
    width_ = 0;
    height_ = 0;
    max_vertices_ = 0;
    target_luid_valid_ = false;
}

Dx12BridgeStats Dx12Bridge::stats() const noexcept {
    Dx12BridgeStats s{};
    s.active = ready();
    s.shared_texture = foreign_;
    s.uma_zero_copy = (d3d12_ != nullptr) && d3d12_->is_uma();
    s.intel_igpu = (d3d12_ != nullptr) && d3d12_->is_intel_igpu();
    s.dxr_tier_1_1 = (d3d12_ != nullptr) && d3d12_->dxr_supported();
    s.effective_lighting_mode = fg_.last_effective_lighting_mode();
    s.width = width_;
    s.height = height_;
    s.frames = frames_;
    s.last_render_ms = last_render_ms_;
    if (d3d12_ != nullptr) {
        std::snprintf(s.adapter_name.data(), s.adapter_name.size(), "%s", d3d12_->gpu_info().device_name.c_str());
    }
    return s;
}

} // namespace aether::renderer
