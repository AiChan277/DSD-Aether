#pragma once

#include "core/expected.hpp"
#include "renderer/frame_graph.hpp"
#include "renderer/sdf_generator.hpp"
#include "rhi/rhi_swapchain.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace aether::renderer {

struct DeviceRecoveryReport {
    bool recovered{false};
    double recovery_ms{0.0};
    std::string breadcrumb_path{};
    std::string error_message{};
};

/**
 * Top-level Phase 4 Renderer Coordinator (`§10.1–§10.11`).
 * Supports BOTH:
 * - Headless dual-cmd / single-cmd offscreen execution (`Golden` / `RT` / `Effects` tests), and
 * - Live Studio dual command buffer submission (`cmd_critical` -> `frame_tl` -> `cmd_ui`) +
 *   ImGui descriptor exposure + `VK_ERROR_DEVICE_LOST` (`<= 3.0 s`) automatic rebuild.
 */
class NprRenderer {
  public:
    static constexpr uint32_t kFrameSlots = rhi::RhiSwapchain::kFramesInFlight;

    NprRenderer() = default;
    ~NprRenderer() { destroy(); }

    NprRenderer(const NprRenderer&) = delete;
    NprRenderer& operator=(const NprRenderer&) = delete;

    [[nodiscard]] Expected<void> init(rhi::RhiDevice& device, const RendererConfig& cfg = {});
    [[nodiscard]] Expected<void> resize(uint32_t viewport_w, uint32_t viewport_h, uint32_t output_w = UINT32_MAX,
                                        uint32_t output_h = UINT32_MAX);
    void destroy() noexcept;

    [[nodiscard]] Expected<void> bind_avatar(const GpuAvatar* avatar);

    /** Executes one headless frame (Passes 1–13), signaling `frame_timeline` between Passes 1–12 and Pass 13. */
    [[nodiscard]] Expected<void> render_headless_frame(const FrameRenderRequest& req);

    /** Live Studio split submission: records Passes 1–12 into a dedicated critical cmd and signals `frame_timeline`. */
    [[nodiscard]] Expected<void> submit_critical_and_signal(const FrameRenderRequest& req,
                                                            uint64_t& out_frame_timeline_value);

    /** Records Pass 13 (`Viewport Compose` + `128^2 Orbit Orb`) onto `cmd_ui` (swapchain). */
    void record_ui_composite(VkCommandBuffer cmd_ui, const FrameRenderRequest& req) noexcept;

    /** Performs `OfflineSdfGenerator::bake_face_sdf` and uploads the resulting UV-space R8 texture. */
    [[nodiscard]] Expected<SdfBakeOutput> bake_and_apply_face_sdf(const SdfBakeInput& input);

    /** Rebuilds VkDevice, FrameGraph, pipelines, and avatar GPU buffers on `VK_ERROR_DEVICE_LOST` (`<= 3.0 s`). */
    [[nodiscard]] DeviceRecoveryReport recover_from_device_lost(rhi::RhiDevice& device,
                                                                const rhi::RhiDeviceConfig& device_cfg,
                                                                const RendererConfig& render_cfg,
                                                                const GpuAvatar* avatar,
                                                                const std::filesystem::path& breadcrumb_dir = {});

    [[nodiscard]] FrameGraph& graph() noexcept { return graph_; }
    [[nodiscard]] const FrameGraph& graph() const noexcept { return graph_; }
    [[nodiscard]] LightingRig& lighting() noexcept { return lighting_; }
    [[nodiscard]] const LightingRig& lighting() const noexcept { return lighting_; }
    [[nodiscard]] uint64_t frame_timeline_value() const noexcept { return timeline_counter_; }
    [[nodiscard]] bool wireframe_supported() const noexcept { return graph_.wireframe_supported(); }

  private:
    struct CriticalCmdSlot {
        VkCommandPool pool{VK_NULL_HANDLE};
        VkCommandBuffer cmd{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
    };

    rhi::RhiDevice* device_{nullptr};
    FrameGraph graph_{};
    LightingRig lighting_{LightingRig::default_three_point()};
    std::array<CriticalCmdSlot, kFrameSlots> critical_slots_{};
    std::array<CriticalCmdSlot, kFrameSlots> ui_slots_{};
    uint64_t timeline_counter_{0};
    bool initialized_{false};
};

} // namespace aether::renderer
