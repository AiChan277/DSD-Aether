#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_4.h>
#include <windows.h>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "output/spout_output.hpp"

#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace aether::output {

namespace {

constexpr uint32_t kMaxSpoutNames = 64u;
constexpr uint32_t kSpoutNameEntryBytes = 256u;
constexpr DWORD kSpoutNamesMapBytes = kMaxSpoutNames * kSpoutNameEntryBytes;
constexpr DWORD kSpoutActiveMapBytes = 256u;

template <typename T> void safe_release(void*& ptr) noexcept {
    if (ptr != nullptr) {
        static_cast<T*>(ptr)->Release();
        ptr = nullptr;
    }
}

void close_handle_safe(void*& h) noexcept {
    if (h != nullptr) {
        CloseHandle(static_cast<HANDLE>(h));
        h = nullptr;
    }
}

void unmap_and_close(void*& view, void*& map) noexcept {
    if (view != nullptr) {
        UnmapViewOfFile(view);
        view = nullptr;
    }
    if (map != nullptr) {
        CloseHandle(static_cast<HANDLE>(map));
        map = nullptr;
    }
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

bool SpoutOutput::detect_display_adapter_mismatch(const std::array<uint8_t, 8>& luid, bool luid_valid) {
    if (!luid_valid) {
        return false;
    }
    LUID vk_luid{};
    std::memcpy(&vk_luid, luid.data(), sizeof(vk_luid));

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) || factory == nullptr) {
        return false;
    }

    bool mismatch = false;
    bool found_primary_output = false;
    for (UINT a = 0;; ++a) {
        IDXGIAdapter1* adp = nullptr;
        if (factory->EnumAdapters1(a, &adp) == DXGI_ERROR_NOT_FOUND || adp == nullptr) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adp->GetDesc1(&desc))) {
            IDXGIOutput* out = nullptr;
            for (UINT o = 0; adp->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
                if (out == nullptr) {
                    continue;
                }
                DXGI_OUTPUT_DESC odesc{};
                if (SUCCEEDED(out->GetDesc(&odesc)) && odesc.AttachedToDesktop != FALSE) {
                    const RECT& r = odesc.DesktopCoordinates;
                    const bool contains_origin = (r.left <= 0 && r.right > 0 && r.top <= 0 && r.bottom > 0);
                    if (contains_origin || !found_primary_output) {
                        mismatch = (desc.AdapterLuid.LowPart != vk_luid.LowPart) ||
                                   (desc.AdapterLuid.HighPart != vk_luid.HighPart);
                        found_primary_output = contains_origin;
                    }
                }
                out->Release();
                out = nullptr;
                if (found_primary_output) {
                    break;
                }
            }
        }
        adp->Release();
        if (found_primary_output) {
            break;
        }
    }
    factory->Release();
    return mismatch;
}

Expected<void> SpoutOutput::init(rhi::RhiDevice& device, uint32_t width, uint32_t height, const char* sender_name,
                                 SpoutSharingMode mode) {
    shutdown();

    if (device.device() == VK_NULL_HANDLE || width == 0u || height == 0u) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "SpoutOutput::init invalid arguments")};
    }

    device_ = &device;
    width_ = width;
    height_ = height;
    requested_mode_ = mode;
    cpu_fallback_ = (mode == SpoutSharingMode::CpuCompatibility);
    cmd_cursor_ = 0u;
    cmd_tl_.fill(0u);
    uint64_t cur_tl = 0u;
    if (device.output_timeline() != VK_NULL_HANDLE) {
        (void)vkGetSemaphoreCounterValue(device.device(), device.output_timeline(), &cur_tl);
    }
    output_tl_counter_ = cur_tl;
    init_tl_baseline_ = cur_tl;
    frames_published_ = 0u;
    skipped_mutex_ = 0u;
    last_publish_ms_ = 0.0;

    const char* effective_name = (sender_name != nullptr && sender_name[0] != '\0') ? sender_name : "Aether2.5D";
    std::snprintf(sender_name_.data(), sender_name_.size(), "%s", effective_name);

    adapter_mismatch_ = detect_display_adapter_mismatch(device.gpu_info().luid, device.gpu_info().luid_valid);

    // Match D3D11 device strictly to Vulkan GPU LUID (`§11.2.1`)
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) || factory == nullptr) {
        shutdown();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "CreateDXGIFactory1 failed for SpoutOutput")};
    }

    IDXGIAdapter1* adapter = nullptr;
    if (device.gpu_info().luid_valid) {
        LUID vk_luid{};
        std::memcpy(&vk_luid, device.gpu_info().luid.data(), sizeof(vk_luid));
        (void)factory->EnumAdapterByLuid(vk_luid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
    }
    if (adapter == nullptr) {
        (void)factory->EnumAdapters1(0, &adapter);
    }
    factory->Release();
    if (adapter == nullptr) {
        shutdown();
        return Unexpected<Error>{Error::make(ErrorCode::Unsupported, "No DXGI adapter available for SpoutOutput")};
    }

    ID3D11Device* d3d_dev = nullptr;
    ID3D11DeviceContext* d3d_ctx = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL chosen_level = D3D_FEATURE_LEVEL_11_0;
    const HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         levels, 2u, D3D11_SDK_VERSION, &d3d_dev, &chosen_level, &d3d_ctx);
    adapter->Release();
    if (FAILED(hr) || d3d_dev == nullptr || d3d_ctx == nullptr) {
        if (d3d_ctx != nullptr) {
            d3d_ctx->Release();
        }
        if (d3d_dev != nullptr) {
            d3d_dev->Release();
        }
        shutdown();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "D3D11CreateDevice failed for SpoutOutput")};
    }
    d3d_device_ = d3d_dev;
    d3d_context_ = d3d_ctx;

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = device.transfer_family();
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(device.device(), &cpci, nullptr, &cmd_pool_) != VK_SUCCESS) {
        shutdown();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "SpoutOutput vkCreateCommandPool failed")};
    }

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmd_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = static_cast<uint32_t>(cmds_.size());
    if (vkAllocateCommandBuffers(device.device(), &cbai, cmds_.data()) != VK_SUCCESS) {
        shutdown();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "SpoutOutput vkAllocateCommandBuffers failed")};
    }

    if (auto res = allocate_shared_targets(width_, height_); !res) {
        shutdown();
        return res;
    }
    if (auto res = register_spout_maps(); !res) {
        shutdown();
        return res;
    }

    initialized_ = true;
    AETHER_LOG_INFO("output", "SpoutOutput initialized: sender='%s' %ux%u mode=%s kmt=0x%08X mismatch=%d",
                    sender_name_.data(), width_, height_, cpu_fallback_ ? "CPU-Fallback" : "GPU-KMT", share_handle_u32_,
                    adapter_mismatch_ ? 1 : 0);
    return {};
}

Expected<void> SpoutOutput::resize(uint32_t width, uint32_t height) {
    if (!initialized_ || device_ == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "SpoutOutput::resize called before init")};
    }
    if (width == 0u || height == 0u) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "SpoutOutput::resize zero extent")};
    }
    if (width == width_ && height == height_) {
        return {};
    }

    if (output_tl_counter_ > init_tl_baseline_) {
        const VkSemaphore sem = device_->output_timeline();
        VkSemaphoreWaitInfo swi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        swi.semaphoreCount = 1;
        swi.pSemaphores = &sem;
        swi.pValues = &output_tl_counter_;
        (void)vkWaitSemaphores(device_->device(), &swi, 1'000'000'000ULL);
    }

    release_shared_targets();
    width_ = width;
    height_ = height;
    if (auto res = allocate_shared_targets(width_, height_); !res) {
        return res;
    }

    // Update sender SharedTextureInfo record with new dimensions & KMT handle
    if (sender_view_ != nullptr) {
        if (access_mutex_ != nullptr) {
            (void)WaitForSingleObject(static_cast<HANDLE>(access_mutex_), 5u);
        }
        auto* info = static_cast<SpoutSharedTextureInfo*>(sender_view_);
        info->share_handle = share_handle_u32_;
        info->width = width_;
        info->height = height_;
        info->format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (access_mutex_ != nullptr) {
            ReleaseMutex(static_cast<HANDLE>(access_mutex_));
        }
    }
    return {};
}

void SpoutOutput::shutdown() noexcept {
    if (device_ != nullptr && output_tl_counter_ > init_tl_baseline_ && device_->output_timeline() != VK_NULL_HANDLE) {
        const VkSemaphore sem = device_->output_timeline();
        VkSemaphoreWaitInfo swi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        swi.semaphoreCount = 1;
        swi.pSemaphores = &sem;
        swi.pValues = &output_tl_counter_;
        (void)vkWaitSemaphores(device_->device(), &swi, 1'000'000'000ULL);
    }
    init_tl_baseline_ = output_tl_counter_;
    cmd_tl_.fill(0u);

    unregister_spout_maps();
    release_shared_targets();

    if (device_ != nullptr && device_->device() != VK_NULL_HANDLE && cmd_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_->device(), cmd_pool_, nullptr);
    }
    cmd_pool_ = VK_NULL_HANDLE;
    cmds_.fill(VK_NULL_HANDLE);

    safe_release<ID3D11DeviceContext>(d3d_context_);
    safe_release<ID3D11Device>(d3d_device_);

    initialized_ = false;
    cpu_fallback_ = false;
    adapter_mismatch_ = false;
    share_handle_u32_ = 0u;
    kmt_handle_ = nullptr;
    width_ = 0u;
    height_ = 0u;
    device_ = nullptr;
}

Expected<void> SpoutOutput::allocate_shared_targets(uint32_t width, uint32_t height) {
    auto* d3d_dev = static_cast<ID3D11Device*>(d3d_device_);
    if (d3d_dev == nullptr || device_ == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "SpoutOutput D3D11 device unavailable")};
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(d3d_dev->CreateTexture2D(&desc, nullptr, &tex)) || tex == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "CreateTexture2D(MISC_SHARED) failed")};
    }
    d3d_shared_tex_ = tex;

    IDXGIResource* dxgi_res = nullptr;
    if (FAILED(tex->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&dxgi_res))) ||
        dxgi_res == nullptr) {
        release_shared_targets();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "QueryInterface(IDXGIResource) failed")};
    }
    HANDLE kmt = nullptr;
    const HRESULT hres = dxgi_res->GetSharedHandle(&kmt);
    dxgi_res->Release();
    if (FAILED(hres) || kmt == nullptr) {
        release_shared_targets();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "IDXGIResource::GetSharedHandle failed")};
    }
    kmt_handle_ = kmt;
    share_handle_u32_ = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kmt));

    bool imported_vk = false;
    if (requested_mode_ == SpoutSharingMode::GpuDefault && device_->gpu_info().external_memory_win32_active) {
        VkExternalMemoryImageCreateInfo ext_img_ci{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext_img_ci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &ext_img_ci;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(device_->device(), &ici, nullptr, &vk_shared_image_) == VK_SUCCESS) {
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device_->device(), vk_shared_image_, &req);

            uint32_t type_bits = req.memoryTypeBits;
            auto get_win32_props = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
                vkGetDeviceProcAddr(device_->device(), "vkGetMemoryWin32HandlePropertiesKHR"));
            if (get_win32_props != nullptr) {
                VkMemoryWin32HandlePropertiesKHR hprops{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
                if (get_win32_props(device_->device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, kmt,
                                    &hprops) == VK_SUCCESS &&
                    hprops.memoryTypeBits != 0u) {
                    type_bits &= hprops.memoryTypeBits;
                }
            }

            const uint32_t mem_idx =
                find_memory_type(device_->physical_device(), type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mem_idx != UINT32_MAX) {
                VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
                ded.image = vk_shared_image_;

                VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
                imp.pNext = &ded;
                imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
                imp.handle = kmt;

                VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                mai.pNext = &imp;
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = mem_idx;

                if (vkAllocateMemory(device_->device(), &mai, nullptr, &vk_shared_mem_) == VK_SUCCESS &&
                    vkBindImageMemory(device_->device(), vk_shared_image_, vk_shared_mem_, 0) == VK_SUCCESS) {
                    imported_vk = true;
                    vk_shared_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
                }
            }
        }

        if (!imported_vk) {
            AETHER_LOG_WARN("output", "SpoutOutput KMT VkImage import unavailable; falling back to CPU staging");
            if (vk_shared_image_ != VK_NULL_HANDLE) {
                vkDestroyImage(device_->device(), vk_shared_image_, nullptr);
                vk_shared_image_ = VK_NULL_HANDLE;
            }
            if (vk_shared_mem_ != VK_NULL_HANDLE) {
                vkFreeMemory(device_->device(), vk_shared_mem_, nullptr);
                vk_shared_mem_ = VK_NULL_HANDLE;
            }
            cpu_fallback_ = true;
        }
    } else {
        cpu_fallback_ = true;
    }

    if (cpu_fallback_) {
        D3D11_TEXTURE2D_DESC sdesc{};
        sdesc.Width = width;
        sdesc.Height = height;
        sdesc.MipLevels = 1;
        sdesc.ArraySize = 1;
        sdesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sdesc.SampleDesc.Count = 1;
        sdesc.Usage = D3D11_USAGE_STAGING;
        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Texture2D* stg = nullptr;
        if (FAILED(d3d_dev->CreateTexture2D(&sdesc, nullptr, &stg)) || stg == nullptr) {
            release_shared_targets();
            return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "D3D11 staging texture creation failed")};
        }
        d3d_staging_tex_ = stg;

        const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4ull;
        auto buf_res =
            device_->create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!buf_res) {
            release_shared_targets();
            return Unexpected<Error>{buf_res.error()};
        }
        readback_buf_ = buf_res.value();
    }

    return {};
}

void SpoutOutput::release_shared_targets() noexcept {
    if (device_ != nullptr && device_->device() != VK_NULL_HANDLE) {
        if (vk_shared_image_ != VK_NULL_HANDLE) {
            vkDestroyImage(device_->device(), vk_shared_image_, nullptr);
            vk_shared_image_ = VK_NULL_HANDLE;
        }
        if (vk_shared_mem_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_->device(), vk_shared_mem_, nullptr);
            vk_shared_mem_ = VK_NULL_HANDLE;
        }
        device_->destroy_buffer(readback_buf_);
    }
    vk_shared_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    safe_release<ID3D11Texture2D>(d3d_staging_tex_);
    safe_release<ID3D11Texture2D>(d3d_shared_tex_);
    // Note: kmt_handle_ is owned by d3d_shared_tex_ kernel object; never CloseHandle(KMT).
    kmt_handle_ = nullptr;
    share_handle_u32_ = 0u;
}

Expected<void> SpoutOutput::register_spout_maps() {
    std::array<char, 96> mtx_name{};
    std::snprintf(mtx_name.data(), mtx_name.size(), "%s_SpoutAccessMutex", sender_name_.data());
    access_mutex_ = CreateMutexA(nullptr, FALSE, mtx_name.data());
    if (access_mutex_ == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "CreateMutexA(SpoutAccessMutex) failed")};
    }

    sender_map_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SpoutSharedTextureInfo),
                                     sender_name_.data());
    if (sender_map_ == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "CreateFileMappingA(SenderInfo) failed")};
    }
    sender_view_ =
        MapViewOfFile(static_cast<HANDLE>(sender_map_), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SpoutSharedTextureInfo));
    if (sender_view_ == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "MapViewOfFile(SenderInfo) failed")};
    }

    auto* info = static_cast<SpoutSharedTextureInfo*>(sender_view_);
    std::memset(info, 0, sizeof(*info));
    info->share_handle = share_handle_u32_;
    info->width = width_;
    info->height = height_;
    info->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    info->usage = 0u;
    std::snprintf(info->description, sizeof(info->description), "AETHER-2.5D");
    info->partner_id = 1u;

    names_mutex_ = CreateMutexA(nullptr, FALSE, "SpoutSenderNames_SpoutAccessMutex");
    names_map_ =
        CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kSpoutNamesMapBytes, "SpoutSenderNames");
    if (names_map_ != nullptr) {
        const bool names_existed = (GetLastError() == ERROR_ALREADY_EXISTS);
        names_view_ = MapViewOfFile(static_cast<HANDLE>(names_map_), FILE_MAP_ALL_ACCESS, 0, 0, kSpoutNamesMapBytes);
        if (names_view_ != nullptr) {
            if (names_mutex_ != nullptr) {
                (void)WaitForSingleObject(static_cast<HANDLE>(names_mutex_), 5u);
            }
            auto* entries = static_cast<char*>(names_view_);
            if (!names_existed) {
                std::memset(entries, 0, kSpoutNamesMapBytes);
            }
            bool present = false;
            uint32_t free_idx = kMaxSpoutNames;
            for (uint32_t i = 0; i < kMaxSpoutNames; ++i) {
                char* slot = entries + static_cast<size_t>(i) * kSpoutNameEntryBytes;
                if (slot[0] == '\0') {
                    if (free_idx == kMaxSpoutNames) {
                        free_idx = i;
                    }
                } else if (std::strncmp(slot, sender_name_.data(), kSpoutNameEntryBytes) == 0) {
                    present = true;
                    break;
                }
            }
            if (!present && free_idx < kMaxSpoutNames) {
                std::snprintf(entries + static_cast<size_t>(free_idx) * kSpoutNameEntryBytes, kSpoutNameEntryBytes,
                              "%s", sender_name_.data());
            }
            if (names_mutex_ != nullptr) {
                ReleaseMutex(static_cast<HANDLE>(names_mutex_));
            }
        }
    }

    active_map_ =
        CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kSpoutActiveMapBytes, "ActiveSenderName");
    if (active_map_ != nullptr) {
        active_view_ = MapViewOfFile(static_cast<HANDLE>(active_map_), FILE_MAP_ALL_ACCESS, 0, 0, kSpoutActiveMapBytes);
        if (active_view_ != nullptr) {
            std::memset(active_view_, 0, kSpoutActiveMapBytes);
            std::snprintf(static_cast<char*>(active_view_), kSpoutActiveMapBytes, "%s", sender_name_.data());
        }
    }

    std::array<char, 96> sem_name{};
    std::snprintf(sem_name.data(), sem_name.size(), "%s_Count_Semaphore", sender_name_.data());
    count_semaphore_ = CreateSemaphoreA(nullptr, 0, 0x7FFFFFFF, sem_name.data());

    return {};
}

void SpoutOutput::unregister_spout_maps() noexcept {
    if (names_view_ != nullptr) {
        if (names_mutex_ != nullptr) {
            (void)WaitForSingleObject(static_cast<HANDLE>(names_mutex_), 5u);
        }
        auto* entries = static_cast<char*>(names_view_);
        for (uint32_t i = 0; i < kMaxSpoutNames; ++i) {
            char* slot = entries + static_cast<size_t>(i) * kSpoutNameEntryBytes;
            if (std::strncmp(slot, sender_name_.data(), kSpoutNameEntryBytes) == 0) {
                std::memset(slot, 0, kSpoutNameEntryBytes);
            }
        }
        if (names_mutex_ != nullptr) {
            ReleaseMutex(static_cast<HANDLE>(names_mutex_));
        }
    }

    unmap_and_close(active_view_, active_map_);
    unmap_and_close(names_view_, names_map_);
    unmap_and_close(sender_view_, sender_map_);
    close_handle_safe(count_semaphore_);
    close_handle_safe(names_mutex_);
    close_handle_safe(access_mutex_);
}

Expected<void> SpoutOutput::publish_frame(VkImage src_bgra_image, uint32_t width, uint32_t height,
                                          uint64_t wait_timeline_value) noexcept {
    if (!initialized_ || device_ == nullptr || src_bgra_image == VK_NULL_HANDLE) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "SpoutOutput::publish_frame invalid state")};
    }
    if (width != width_ || height != height_) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "SpoutOutput::publish_frame extent mismatch")};
    }

    // Strict 1 ms SpoutAccessMutex timeout: skip cleanly instead of blocking T4 (`§11.2.3`)
    if (access_mutex_ != nullptr) {
        const DWORD wait_res = WaitForSingleObject(static_cast<HANDLE>(access_mutex_), 1u);
        if (wait_res == WAIT_TIMEOUT) {
            ++skipped_mutex_;
            return {};
        }
        if (wait_res != WAIT_OBJECT_0 && wait_res != WAIT_ABANDONED) {
            ++skipped_mutex_;
            return {};
        }
    }

    const uint64_t t0_us = now_us();
    const uint32_t cidx = cmd_cursor_;
    cmd_cursor_ = (cmd_cursor_ + 1u) % static_cast<uint32_t>(cmds_.size());
    const VkCommandBuffer cmd = cmds_[cidx];

    // Wait only if this command buffer slot is still executing on GPU
    if (cmd_tl_[cidx] > init_tl_baseline_) {
        const VkSemaphore out_sem = device_->output_timeline();
        VkSemaphoreWaitInfo swi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        swi.semaphoreCount = 1u;
        swi.pSemaphores = &out_sem;
        swi.pValues = &cmd_tl_[cidx];
        (void)vkWaitSemaphores(device_->device(), &swi, 1'000'000'000ULL);
    }

    (void)vkResetCommandBuffer(cmd, 0u);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        if (access_mutex_ != nullptr) {
            ReleaseMutex(static_cast<HANDLE>(access_mutex_));
        }
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "SpoutOutput vkBeginCommandBuffer failed")};
    }

    if (!cpu_fallback_ && vk_shared_image_ != VK_NULL_HANDLE) {
        VkImageMemoryBarrier2 pre{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        pre.image = vk_shared_image_;
        pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        if (vk_shared_layout_ == VK_IMAGE_LAYOUT_UNDEFINED) {
            pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            pre.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            pre.srcAccessMask = VK_ACCESS_2_NONE;
        } else {
            pre.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            pre.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            pre.dstQueueFamilyIndex = device_->transfer_family();
            pre.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            pre.srcAccessMask = VK_ACCESS_2_NONE;
        }
        pre.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        pre.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;

        VkDependencyInfo pre_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        pre_dep.imageMemoryBarrierCount = 1u;
        pre_dep.pImageMemoryBarriers = &pre;
        vkCmdPipelineBarrier2(cmd, &pre_dep);

        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
        region.extent = {width_, height_, 1u};
        vkCmdCopyImage(cmd, src_bgra_image, VK_IMAGE_LAYOUT_GENERAL, vk_shared_image_,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);

        VkImageMemoryBarrier2 post{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        post.image = vk_shared_image_;
        post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        post.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        post.srcQueueFamilyIndex = device_->transfer_family();
        post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        post.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        post.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        post.dstAccessMask = VK_ACCESS_2_NONE;

        VkDependencyInfo post_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        post_dep.imageMemoryBarrierCount = 1u;
        post_dep.pImageMemoryBarriers = &post;
        vkCmdPipelineBarrier2(cmd, &post_dep);

        vk_shared_layout_ = VK_IMAGE_LAYOUT_GENERAL;
    } else {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
        region.imageExtent = {width_, height_, 1u};
        vkCmdCopyImageToBuffer(cmd, src_bgra_image, VK_IMAGE_LAYOUT_GENERAL, readback_buf_.handle, 1u, &region);

        VkBufferMemoryBarrier2 bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        bmb.buffer = readback_buf_.handle;
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
    }

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        if (access_mutex_ != nullptr) {
            ReleaseMutex(static_cast<HANDLE>(access_mutex_));
        }
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "SpoutOutput vkEndCommandBuffer failed")};
    }

    VkSemaphoreSubmitInfo wait_infos[2]{};
    uint32_t wait_count = 0u;
    if (wait_timeline_value > 0u) {
        wait_infos[wait_count].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_infos[wait_count].semaphore = device_->frame_timeline();
        wait_infos[wait_count].value = wait_timeline_value;
        wait_infos[wait_count].stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        ++wait_count;
    }
    if (output_tl_counter_ > init_tl_baseline_) {
        wait_infos[wait_count].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_infos[wait_count].semaphore = device_->output_timeline();
        wait_infos[wait_count].value = output_tl_counter_;
        wait_infos[wait_count].stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        ++wait_count;
    }

    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;

    const uint64_t next_tl = ++output_tl_counter_;
    cmd_tl_[cidx] = next_tl;
    VkSemaphoreSubmitInfo sig_info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig_info.semaphore = device_->output_timeline();
    sig_info.value = next_tl;
    sig_info.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;

    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = wait_count;
    si.pWaitSemaphoreInfos = (wait_count > 0u) ? wait_infos : nullptr;
    si.commandBufferInfoCount = 1u;
    si.pCommandBufferInfos = &cmd_info;
    si.signalSemaphoreInfoCount = 1u;
    si.pSignalSemaphoreInfos = &sig_info;

    if (vkQueueSubmit2(device_->transfer_queue(), 1u, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        if (access_mutex_ != nullptr) {
            ReleaseMutex(static_cast<HANDLE>(access_mutex_));
        }
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "SpoutOutput vkQueueSubmit2 failed")};
    }

    if (cpu_fallback_ || vk_shared_image_ == VK_NULL_HANDLE) {
        const VkSemaphore out_sem = device_->output_timeline();
        VkSemaphoreWaitInfo swi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        swi.semaphoreCount = 1u;
        swi.pSemaphores = &out_sem;
        swi.pValues = &next_tl;
        (void)vkWaitSemaphores(device_->device(), &swi, 1'000'000'000ULL);
    }

    if (cpu_fallback_ && d3d_staging_tex_ != nullptr && d3d_shared_tex_ != nullptr &&
        readback_buf_.info.pMappedData != nullptr) {
        const VkDeviceSize byte_size = static_cast<VkDeviceSize>(width_) * height_ * 4ull;
        (void)vmaInvalidateAllocation(device_->allocator(), readback_buf_.allocation, 0u, byte_size);

        auto* ctx = static_cast<ID3D11DeviceContext*>(d3d_context_);
        auto* stg = static_cast<ID3D11Texture2D*>(d3d_staging_tex_);
        auto* dst = static_cast<ID3D11Texture2D*>(d3d_shared_tex_);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(stg, 0u, D3D11_MAP_WRITE, 0u, &mapped)) && mapped.pData != nullptr) {
            const uint32_t row_bytes = width_ * 4u;
            const auto* src_bytes = static_cast<const uint8_t*>(readback_buf_.info.pMappedData);
            auto* dst_bytes = static_cast<uint8_t*>(mapped.pData);
            if (mapped.RowPitch == row_bytes) {
                std::memcpy(dst_bytes, src_bytes, static_cast<size_t>(row_bytes) * height_);
            } else {
                for (uint32_t y = 0; y < height_; ++y) {
                    std::memcpy(dst_bytes + static_cast<size_t>(y) * mapped.RowPitch,
                                src_bytes + static_cast<size_t>(y) * row_bytes, row_bytes);
                }
            }
            ctx->Unmap(stg, 0u);
            ctx->CopyResource(dst, stg);
            ctx->Flush();
        }
    }

    if (count_semaphore_ != nullptr) {
        (void)ReleaseSemaphore(static_cast<HANDLE>(count_semaphore_), 1L, nullptr);
    }

    if (access_mutex_ != nullptr) {
        ReleaseMutex(static_cast<HANDLE>(access_mutex_));
    }

    last_publish_ms_ = static_cast<double>(now_us() - t0_us) / 1000.0;
    ++frames_published_;
    return {};
}

} // namespace aether::output
