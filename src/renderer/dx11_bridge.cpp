#include "renderer/dx11_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <span>

#include "core/clock.hpp"
#include "core/log.hpp"

namespace aether::renderer {

namespace {

constexpr float kDegToRad = 0.017453292519943295f;

/** Vulkan objects owned by the bridge, destroyed by the collector once the GPU has finished with them. */
struct RetiredBridgeObjects {
    const rhi::RhiDevice* device{nullptr};
    VkImage imported_image{VK_NULL_HANDLE};
    VkDeviceMemory imported_memory{VK_NULL_HANDLE};
    rhi::RhiImage owned_image{};
    rhi::RhiBuffer staging{};
};

void finalize_retired_bridge_objects(void* handle, uint64_t /*user_tag*/, uint64_t /*byte_size*/) noexcept {
    auto* r = static_cast<RetiredBridgeObjects*>(handle);
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

std::array<float, 3> studio_light_toward(const StudioLightSpec& s) noexcept {
    const auto pose = resolve_studio_light_world_pose(s, {0.0f, 1.32f, 0.0f}, {0.0f, 1.45f, 2.0f});
    const float dx = pose.position_ws[0] - pose.target_ws[0];
    const float dy = pose.position_ws[1] - pose.target_ws[1];
    const float dz = pose.position_ws[2] - pose.target_ws[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float inv = len > 1e-6f ? 1.0f / len : 0.0f;
    return {dx * inv, dy * inv, dz * inv};
}

} // namespace

const char* render_backend_name(RenderBackend b) noexcept {
    switch (b) {
        case RenderBackend::DirectX11:
            return "DirectX 11";
        case RenderBackend::DirectX12:
            return "DirectX 12";
        case RenderBackend::Vulkan:
        default:
            return "Vulkan";
    }
}

Dx11Bridge::~Dx11Bridge() noexcept {
    release();
}

void Dx11Bridge::attach(rhi::RhiDevice& device, GarbageCollector& gc) noexcept {
    device_ = &device;
    gc_ = &gc;
}

bool Dx11Bridge::wait_last_read() noexcept {
    if (device_ == nullptr || last_read_timeline_ == 0U) {
        return true;
    }
    VkSemaphore sem = device_->frame_timeline();
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &sem;
    wi.pValues = &last_read_timeline_;
    constexpr uint64_t kTimeoutNs = 250ULL * 1000ULL * 1000ULL;
    return vkWaitSemaphores(device_->device(), &wi, kTimeoutNs) == VK_SUCCESS;
}

void Dx11Bridge::retire_vulkan_objects() noexcept {
    if (image_ == VK_NULL_HANDLE && memory_ == VK_NULL_HANDLE && staging_.handle == VK_NULL_HANDLE) {
        return;
    }
    auto* r = new (std::nothrow) RetiredBridgeObjects{};
    if (r != nullptr) {
        r->device = device_;
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
        !gc_->retire(r, bytes, last_read_timeline_, &finalize_retired_bridge_objects,
                     /*user_tag=*/0x44583131ULL /* 'DX11' */)) {
        // Collector unavailable or full: fall back to a fenced synchronous destroy.
        (void)wait_last_read();
        finalize_retired_bridge_objects(r, 0, bytes);
    }
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    owned_allocation_ = nullptr;
    staging_ = {};
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    has_frame_ = false;
}

Expected<void> Dx11Bridge::create_vulkan_image(bool try_import) {
    foreign_ = false;
    if (try_import && dx_.shared_dxgi_handle() != nullptr && device_->gpu_info().external_memory_win32_active) {
        VkExternalMemoryImageCreateInfo ext_ci{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext_ci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &ext_ci;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {width_, height_, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage img = VK_NULL_HANDLE;
        if (vkCreateImage(device_->device(), &ici, nullptr, &img) == VK_SUCCESS) {
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device_->device(), img, &req);
            uint32_t type_bits = req.memoryTypeBits;
            auto get_props = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
                vkGetDeviceProcAddr(device_->device(), "vkGetMemoryWin32HandlePropertiesKHR"));
            if (get_props != nullptr) {
                VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
                if (get_props(device_->device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT,
                              dx_.shared_dxgi_handle(), &hp) == VK_SUCCESS &&
                    hp.memoryTypeBits != 0u) {
                    type_bits &= hp.memoryTypeBits;
                }
            }
            const uint32_t mem_idx =
                find_memory_type(device_->physical_device(), type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkDeviceMemory mem = VK_NULL_HANDLE;
            if (mem_idx != UINT32_MAX) {
                VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
                ded.image = img;
                VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
                imp.pNext = &ded;
                imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
                imp.handle = dx_.shared_dxgi_handle();
                VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                mai.pNext = &imp;
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = mem_idx;
                if (vkAllocateMemory(device_->device(), &mai, nullptr, &mem) == VK_SUCCESS &&
                    vkBindImageMemory(device_->device(), img, mem, 0) == VK_SUCCESS) {
                    image_ = img;
                    memory_ = mem;
                    foreign_ = true;
                    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
                    return {};
                }
            }
            if (mem != VK_NULL_HANDLE) {
                vkFreeMemory(device_->device(), mem, nullptr);
            }
            vkDestroyImage(device_->device(), img, nullptr);
        }
        AETHER_LOG_WARN("renderer", "Dx11Bridge: D3D11 KMT import unavailable, using CPU readback copy");
    }

    // CPU readback path: Vulkan-owned image + host-visible staging buffer that D3D11 readback writes into.
    auto img_res = device_->create_image_2d(width_, height_, VK_FORMAT_R8G8B8A8_UNORM,
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!img_res) {
        return Unexpected<Error>{img_res.error()};
    }
    rhi::RhiImage owned = img_res.value();
    // The bridge only needs the raw image; drop the default view now so retirement owns a single object.
    if (owned.view != VK_NULL_HANDLE) {
        device_->destroy_image_view(owned.view);
        owned.view = VK_NULL_HANDLE;
    }
    auto buf_res = device_->create_buffer(
        static_cast<VkDeviceSize>(width_) * height_ * 4ULL, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!buf_res) {
        device_->destroy_image(owned);
        return Unexpected<Error>{buf_res.error()};
    }
    image_ = owned.handle;
    owned_allocation_ = owned.allocation;
    staging_ = buf_res.value();
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    return {};
}

Expected<void> Dx11Bridge::configure(uint32_t width, uint32_t height, uint32_t max_vertices, uint32_t max_indices) {
    if (device_ == nullptr || gc_ == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "Dx11Bridge::configure before attach")};
    }
    if (width == 0U || height == 0U) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "Dx11Bridge::configure zero size")};
    }
    max_vertices = std::max(max_vertices, 4U);
    max_indices = std::max(max_indices, 6U);
    if (ready() && width == width_ && height == height_ && max_vertices <= max_vertices_ &&
        max_indices <= max_indices_) {
        return {};
    }

    // D3D11 KMT handles are not reference counted: Vulkan must be done with the image before D3D11 frees it.
    if (foreign_) {
        (void)wait_last_read();
    }
    retire_vulkan_objects();
    dx_.shutdown();

    width_ = width;
    height_ = height;
    max_vertices_ = max_vertices;
    max_indices_ = max_indices;

    Dx11RendererConfig cfg{};
    cfg.width = width;
    cfg.height = height;
    cfg.max_vertices = max_vertices;
    cfg.max_indices = max_indices;
    cfg.allow_warp_fallback = true;
    cfg.enable_shared_texture = device_->gpu_info().external_memory_win32_active;
    cfg.linear_output = true;
    const auto& info = device_->gpu_info();
    if (info.luid_valid) {
        std::memcpy(&cfg.adapter_luid, info.luid.data(), sizeof(cfg.adapter_luid));
    }
    if (auto rc = dx_.init(cfg); !rc) {
        return rc;
    }
    if (auto rc = create_vulkan_image(!dx_.is_warp_device() && !force_cpu_copy_); !rc) {
        dx_.shutdown();
        return rc;
    }
    AETHER_LOG_INFO("renderer", "Dx11Bridge ready %ux%u (%s%s, %u verts / %u indices)", width, height,
                    foreign_ ? "shared texture" : "CPU copy", dx_.is_warp_device() ? ", WARP" : "", max_vertices,
                    max_indices);
    return {};
}

void Dx11Bridge::bind_avatar(const GpuAvatar* avatar) {
    bound_avatar_ = avatar;
    draws_.clear();
    if (avatar == nullptr) {
        return;
    }
    auto colors_for = [&](uint32_t mat_idx, Dx11DrawRange& d) {
        if (mat_idx < avatar->materials.size()) {
            const AetherNprMaterial& m = avatar->materials[mat_idx];
            d.lit_color = {m.base_color[0], m.base_color[1], m.base_color[2], 1.0f};
            d.shadow_color = {m.shade_color_gi[0], m.shade_color_gi[1], m.shade_color_gi[2], 1.0f};
        }
    };
    const auto index_total = static_cast<uint32_t>(avatar->rest_indices.size());
    if (avatar->submeshes.empty()) {
        Dx11DrawRange d{};
        d.index_count = index_total;
        colors_for(0U, d);
        draws_.push_back(d);
        return;
    }
    draws_.reserve(avatar->submeshes.size());
    for (const AetherSubmeshDesc& sm : avatar->submeshes) {
        Dx11DrawRange d{};
        d.index_offset = sm.index_offset;
        d.index_count = sm.index_count;
        colors_for(sm.material_index, d);
        draws_.push_back(d);
    }
}

bool Dx11Bridge::render(const GpuAvatar& avatar, const LightingRig& rig, const PreviewCamera& camera) noexcept {
    if (!ready() || &avatar != bound_avatar_ || draws_.empty() || avatar.rest_indices.empty()) {
        return false;
    }
    const bool posed = (avatar.slot_applied_pose_seq[0] != 0ULL || avatar.slot_applied_pose_seq[1] != 0ULL) &&
                       avatar.skinned_vertices_scratch.size() == avatar.rest_vertices.size();
    const std::span<const AetherVertex25D> verts =
        posed ? std::span<const AetherVertex25D>(avatar.skinned_vertices_scratch)
              : std::span<const AetherVertex25D>(avatar.rest_vertices);
    if (verts.empty() || verts.size() > max_vertices_ || avatar.rest_indices.size() > max_indices_) {
        return false;
    }

    const uint64_t t0 = now_us();
    // Vulkan must have finished reading the previous frame before D3D11 overwrites the shared image.
    if (foreign_ && !wait_last_read()) {
        return false;
    }

    Dx11NprFrameParams params{};
    params.view_proj = compute_view_proj(camera, static_cast<float>(width_) / static_cast<float>(height_));
    const StudioLightSpec* dom_key = rig.dominant_key_light();
    const std::array<float, 3> toward =
        (dom_key != nullptr) ? studio_light_toward(*dom_key) : std::array<float, 3>{0.42f, 0.56f, 0.71f};
    // The HLSL shader lights along -light_dir, so pass the direction from the light toward the scene.
    params.light_dir_flatness = {-toward[0], -toward[1], -toward[2],
                                 std::clamp(rig.global_live2d_flatness, 0.0f, 0.85f)};
    const uint32_t n_studio = std::min(rig.studio_light_count, kMaxStudioLights);
    const StudioLightSpec* rim = nullptr;
    for (uint32_t i = 0; i < n_studio; ++i) {
        if (&rig.studio_lights[i] != dom_key && rig.studio_lights[i].enabled &&
            rig.studio_lights[i].passive_flag == PassiveFlagMode::None) {
            rim = &rig.studio_lights[i];
        }
    }
    params.rim_color_strength = (rim != nullptr)
                                    ? std::array<float, 4>{rim->tint_rgb[0], rim->tint_rgb[1], rim->tint_rgb[2],
                                                           std::clamp(rim->intensity_ev100 * 0.25f, 0.0f, 1.0f)}
                                    : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
    params.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};

    if (!dx_.render_npr_draws(verts, avatar.rest_indices, draws_, params)) {
        return false;
    }
    if (!dx_.wait_for_gpu(100)) {
        AETHER_LOG_WARN("renderer", "Dx11Bridge: D3D11 frame did not complete within 100 ms");
        return false;
    }
    if (!foreign_) {
        if (staging_.info.pMappedData == nullptr) {
            return false;
        }
        const size_t bytes = static_cast<size_t>(width_) * height_ * 4U;
        if (!dx_.readback_rgba8(std::span<uint8_t>(static_cast<uint8_t*>(staging_.info.pMappedData), bytes))) {
            return false;
        }
        if (device_ != nullptr) {
            (void)vmaFlushAllocation(device_->allocator(), staging_.allocation, 0, bytes);
        }
    }
    has_frame_ = true;
    ++frames_;
    last_render_ms_ = static_cast<float>(static_cast<double>(now_us() - t0) * 0.001);
    return true;
}

void Dx11Bridge::fill_request(FrameRenderRequest& req) const noexcept {
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

void Dx11Bridge::on_submitted(uint64_t timeline_value) noexcept {
    if (!has_frame_ || image_ == VK_NULL_HANDLE || timeline_value == 0U) {
        return;
    }
    last_read_timeline_ = timeline_value;
    layout_ = VK_IMAGE_LAYOUT_GENERAL;
}

void Dx11Bridge::release() noexcept {
    if (device_ != nullptr && foreign_) {
        (void)wait_last_read();
    }
    if (device_ != nullptr) {
        retire_vulkan_objects();
    }
    dx_.shutdown();
    bound_avatar_ = nullptr;
    draws_.clear();
    width_ = 0;
    height_ = 0;
    max_vertices_ = 0;
    max_indices_ = 0;
}

Dx11BridgeStats Dx11Bridge::stats() const noexcept {
    Dx11BridgeStats s{};
    s.active = ready();
    s.shared_texture = foreign_;
    s.warp = dx_.is_warp_device();
    s.width = width_;
    s.height = height_;
    s.frames = frames_;
    s.last_render_ms = last_render_ms_;
    return s;
}

} // namespace aether::renderer
