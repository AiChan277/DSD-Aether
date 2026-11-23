#pragma once

#include "core/expected.hpp"
#include "core/garbage_collector.hpp"
#include "renderer/asset_upload/gpu_avatar.hpp"
#include "renderer/dx12_frame_graph.hpp"
#include "renderer/frame_graph.hpp"
#include "renderer/npr_renderer.hpp"
#include "rhi/d3d12_device.hpp"
#include "rhi/rhi_device.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace aether::renderer {

/** Live runtime telemetry for the DirectX 12 SM 6.6 + DXR 1.1 backend (`Task 1` / `Task 2`). */
struct Dx12BridgeStats {
    bool active{false};
    bool shared_texture{false}; // true: zero-copy NT shared handle; false: sRGB-accurate CPU readback
    bool uma_zero_copy{false};
    bool intel_igpu{false};
    bool dxr_tier_1_1{false};
    uint8_t effective_lighting_mode{0};
    uint32_t width{0};
    uint32_t height{0};
    uint64_t frames{0};
    float last_render_ms{0.0f};
    std::array<char, 128> adapter_name{};
};

/**
 * Hosts an active DirectX 12 (`D3D12Device` + `Dx12FrameGraph`) rendering pipeline and exposes
 * its output to the Vulkan swapchain compositor via `FrameRenderRequest::external_color_image`
 * alongside native DX12 `out_bgra8()` / CPU readback for Spout2 and Virtual Cam.
 */
class Dx12Bridge {
  public:
    Dx12Bridge() noexcept = default;
    ~Dx12Bridge() noexcept;

    Dx12Bridge(const Dx12Bridge&) = delete;
    Dx12Bridge& operator=(const Dx12Bridge&) = delete;

    void attach(rhi::RhiDevice& device, GarbageCollector& gc) noexcept;

    [[nodiscard]] Expected<void> configure(uint32_t width, uint32_t height, uint32_t max_vertices = 131072u,
                                           const rhi::SelectedGpuInfo* preferred_gpu = nullptr);

    [[nodiscard]] Expected<void> bind_avatar(const GpuAvatar* avatar);
    [[nodiscard]] Expected<void> apply_face_sdf(const SdfBakeOutput& baked);

    /**
     * Renders `req` on DX12 (`Dx12FrameGraph`), mirrors to the Vulkan composite target, and
     * optionally populates `optional_bgra_out` (`w * h * 4` bytes) for VCam / CPU Spout fallback.
     * Allocation-free per frame (`HR-01`).
     */
    [[nodiscard]] bool render(const FrameRenderRequest& req, std::span<uint8_t> optional_bgra_out = {},
                              bool bottom_up_bgra = false) noexcept;

    void fill_request(FrameRenderRequest& req) const noexcept;
    void on_submitted(uint64_t timeline_value) noexcept;
    void release() noexcept;

    void set_force_cpu_copy(bool force) noexcept { force_cpu_copy_ = force; }

    [[nodiscard]] bool ready() const noexcept {
        return image_ != VK_NULL_HANDLE && d3d12_ != nullptr && fg_.pipelines_ready();
    }
    [[nodiscard]] bool is_avatar_bound(const GpuAvatar* avatar) const noexcept {
        return bound_avatar_ == avatar && fg_.bound_avatar() == avatar;
    }
    [[nodiscard]] Dx12BridgeStats stats() const noexcept;
    [[nodiscard]] rhi::D3D12Device* d3d12_device() noexcept { return d3d12_; }
    [[nodiscard]] const rhi::D3D12Device* d3d12_device() const noexcept { return d3d12_; }
    [[nodiscard]] Dx12FrameGraph& frame_graph() noexcept { return fg_; }
    [[nodiscard]] const Dx12FrameGraph& frame_graph() const noexcept { return fg_; }

  private:
    [[nodiscard]] Expected<void> create_vulkan_image(bool try_import);
    void retire_vulkan_objects() noexcept;
    [[nodiscard]] bool wait_last_read() noexcept;

    rhi::RhiDevice* vk_device_{nullptr};
    GarbageCollector* gc_{nullptr};
    rhi::D3D12Device* d3d12_{nullptr};
    Dx12FrameGraph fg_{};

    VkImage image_{VK_NULL_HANDLE};
    VkDeviceMemory memory_{VK_NULL_HANDLE};
    VmaAllocation owned_allocation_{nullptr};
    rhi::RhiBuffer staging_{};
    bool foreign_{false};
    bool force_cpu_copy_{false};
    bool has_frame_{false};
    VkImageLayout layout_{VK_IMAGE_LAYOUT_UNDEFINED};
    uint64_t last_read_timeline_{0};

    uint32_t width_{0};
    uint32_t height_{0};
    uint32_t max_vertices_{0};
    uint64_t frames_{0};
    float last_render_ms_{0.0f};
    std::array<uint8_t, 8> target_luid_{};
    bool target_luid_valid_{false};
    bool target_prefer_igpu_{false};

    const GpuAvatar* bound_avatar_{nullptr};
};

} // namespace aether::renderer
