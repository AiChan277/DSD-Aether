#pragma once

#include <cstdint>
#include <vector>

#include "core/expected.hpp"
#include "core/garbage_collector.hpp"
#include "renderer/dx11_renderer.hpp"
#include "renderer/frame_graph.hpp"
#include "rhi/rhi_device.hpp"

namespace aether::renderer {

/** Which backend draws the avatar each frame (ADR-0016). The UI, compositing and outputs always run on Vulkan. */
enum class RenderBackend : uint8_t {
    Vulkan = 0,    // Full 14-pass NPR + ray tracing frame graph (default)
    DirectX11 = 1, // Direct3D 11 compatibility renderer, handed to Vulkan through Dx11Bridge
    DirectX12 = 2, // Production DirectX 12 SM 6.6 + DXR 1.1 backend via Dx12Bridge
};

[[nodiscard]] const char* render_backend_name(RenderBackend b) noexcept;

/** Live state of the DirectX 11 backend for the UI and the smoke report. */
struct Dx11BridgeStats {
    bool active{false};
    bool shared_texture{false}; // true: zero-copy D3D11 KMT import; false: CPU readback copy
    bool warp{false};           // true: software (WARP) D3D11 device
    uint32_t width{0};
    uint32_t height{0};
    uint64_t frames{0};
    float last_render_ms{0.0f};
};

/**
 * Renders the CPU-skinned avatar with `Dx11NprRenderer` and exposes the result to the Vulkan frame graph as
 * `FrameRenderRequest::external_color_image` (ADR-0016). Every replaced Vulkan object is retired to the
 * `GarbageCollector` against the frame timeline instead of stalling the GPU.
 *
 * Synchronisation: before D3D11 overwrites the shared image, `render()` waits for the timeline value of the last
 * Vulkan submit that read it; after drawing, it waits for the D3D11 event query so Vulkan reads finished pixels.
 */
class Dx11Bridge {
  public:
    Dx11Bridge() noexcept = default;
    ~Dx11Bridge() noexcept;

    Dx11Bridge(const Dx11Bridge&) = delete;
    Dx11Bridge& operator=(const Dx11Bridge&) = delete;

    /** Binds the Vulkan device and collector. No D3D11 resources exist until `configure()`. */
    void attach(rhi::RhiDevice& device, GarbageCollector& gc) noexcept;

    /**
     * (Re)creates the D3D11 device/targets and the Vulkan image for `width x height` and the avatar capacity.
     * No-op when nothing changed. Not for the hot path: it allocates.
     */
    [[nodiscard]] Expected<void> configure(uint32_t width, uint32_t height, uint32_t max_vertices,
                                           uint32_t max_indices);

    /** Rebuilds the per-material draw list for a newly bound avatar (allocates; call on avatar change only). */
    void bind_avatar(const GpuAvatar* avatar);

    /** Draws the current pose. Allocation-free. Returns false if the bridge is not ready or D3D11 failed. */
    [[nodiscard]] bool render(const GpuAvatar& avatar, const LightingRig& rig, const PreviewCamera& camera) noexcept;

    /** Fills the `external_*` fields of `req` (leaves them empty when there is no valid frame). */
    void fill_request(FrameRenderRequest& req) const noexcept;

    /** Records that the frame submitted with `timeline_value` read the image (fences the next overwrite). */
    void on_submitted(uint64_t timeline_value) noexcept;

    /** Retires every Vulkan object to the collector and releases D3D11. Safe to call repeatedly. */
    void release() noexcept;

    /** Forces the CPU readback path even when shared textures are available (tests, driver workarounds). */
    void set_force_cpu_copy(bool force) noexcept { force_cpu_copy_ = force; }

    [[nodiscard]] bool ready() const noexcept { return image_ != VK_NULL_HANDLE && dx_.is_initialized(); }
    [[nodiscard]] Dx11BridgeStats stats() const noexcept;
    [[nodiscard]] Dx11NprRenderer& renderer() noexcept { return dx_; }

  private:
    [[nodiscard]] Expected<void> create_vulkan_image(bool try_import);
    void retire_vulkan_objects() noexcept;
    [[nodiscard]] bool wait_last_read() noexcept;

    rhi::RhiDevice* device_{nullptr};
    GarbageCollector* gc_{nullptr};
    Dx11NprRenderer dx_{};

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
    uint32_t max_indices_{0};
    uint64_t frames_{0};
    float last_render_ms_{0.0f};

    const GpuAvatar* bound_avatar_{nullptr};
    std::vector<Dx11DrawRange> draws_;
};

} // namespace aether::renderer
