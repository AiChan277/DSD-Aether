#pragma once

#include "core/expected.hpp"
#include "renderer/asset_upload/gpu_avatar.hpp"
#include "rhi/rhi_device.hpp"
#include "rhi/rhi_swapchain.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace aether::renderer {

struct PreviewCamera {
    float target_x{0.0f};
    float target_y{0.85f};
    float target_z{0.0f};
    float yaw_rad{0.35f};
    float pitch_rad{0.10f};
    float distance{4.70f};
    float fovy_rad{0.2805f}; // ~85mm portrait telephoto FOV (minimizes 3D perspective distortion)
};

struct PreviewRenderOptions {
    bool show_bones{true};
    bool wireframe{false};
    float clear_rgba[4]{0.067f, 0.075f, 0.094f, 1.0f}; // Precision Dark bg_viewport (#111318)
};

/**
 * Offscreen Vulkan 1.3 dynamic-rendering preview pass for Phase 2 Dev Shell (`P2-07`, `HR-08`).
 *
 * Uses per-frame-slot (`kFramesInFlight = 2`) color/depth attachments so ImGui sampling in
 * frame N never races with offscreen rendering in frame N+1, maintaining 0 sync validation
 * hazards and 0 heap allocations during steady-state rendering (`HR-01`).
 */
class PreviewPass {
  public:
    static constexpr uint32_t kSlotCount = rhi::RhiSwapchain::kFramesInFlight;

    PreviewPass() = default;
    ~PreviewPass() { destroy(); }

    PreviewPass(const PreviewPass&) = delete;
    PreviewPass& operator=(const PreviewPass&) = delete;

    [[nodiscard]] Expected<void> init(rhi::RhiDevice& rhi, VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM);
    void destroy() noexcept;

    /** Ensures offscreen targets match `[width, height]`; sets `out_recreated = true` if recreated. */
    [[nodiscard]] Expected<void> ensure_size(uint32_t width, uint32_t height, bool& out_recreated);

    /** Binds descriptor sets for all materials of `avatar` once on load (zero per-frame alloc). */
    [[nodiscard]] Expected<void> bind_avatar(const GpuAvatar* avatar);

    /** Records dynamic render pass + submesh MToon preview + bone overlay into `cmd` with 0 allocs. */
    void record(VkCommandBuffer cmd, uint32_t frame_slot, const GpuAvatar* avatar, const PreviewCamera& cam,
                const PreviewRenderOptions& opts) noexcept;

    [[nodiscard]] VkSampler sampler() const noexcept { return linear_sampler_; }
    [[nodiscard]] VkImageView color_view(uint32_t slot) const noexcept {
        return slot < kSlotCount ? color_targets_[slot].view : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkImage color_image(uint32_t slot) const noexcept {
        return slot < kSlotCount ? color_targets_[slot].handle : VK_NULL_HANDLE;
    }
    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
    [[nodiscard]] bool wireframe_supported() const noexcept { return mesh_pipe_wireframe_ != VK_NULL_HANDLE; }

  private:
    void destroy_targets() noexcept;
    void destroy_descriptors() noexcept;
    [[nodiscard]] Expected<void> create_pipelines();

    rhi::RhiDevice* device_{nullptr};
    VkFormat color_format_{VK_FORMAT_B8G8R8A8_UNORM};
    VkFormat depth_format_{VK_FORMAT_D32_SFLOAT};
    uint32_t width_{0};
    uint32_t height_{0};

    std::array<rhi::RhiImage, kSlotCount> color_targets_{};
    std::array<rhi::RhiImage, kSlotCount> depth_targets_{};
    std::array<VkImageLayout, kSlotCount> color_layouts_{VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED};

    VkSampler linear_sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout mat_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool mat_desc_pool_{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> mat_desc_sets_;
    VkDescriptorSet mat_fallback_set_{VK_NULL_HANDLE};

    VkPipelineLayout mesh_pipe_layout_{VK_NULL_HANDLE};
    VkPipeline mesh_pipe_cull_{VK_NULL_HANDLE};
    VkPipeline mesh_pipe_double_sided_{VK_NULL_HANDLE};
    VkPipeline mesh_pipe_blend_{VK_NULL_HANDLE};
    VkPipeline mesh_pipe_wireframe_{VK_NULL_HANDLE};

    VkPipelineLayout bone_pipe_layout_{VK_NULL_HANDLE};
    VkPipeline bone_pipe_{VK_NULL_HANDLE};
};

/** Helper to frame a normalized avatar's AABB cleanly inside `PreviewCamera`. */
[[nodiscard]] PreviewCamera fit_camera_to_avatar(const GpuAvatar& avatar) noexcept;

/** Critically-damped (`SmoothDamp`) camera & avatar root transition controller for framing presets & reset. */
struct CameraTransitionState {
    PreviewCamera target_cam{};
    PreviewCamera vel_cam{};
    std::array<float, 3> target_avatar_pos{0.0f, 0.0f, 0.0f};
    std::array<float, 3> vel_avatar_pos{0.0f, 0.0f, 0.0f};
    float target_avatar_rot_y_deg{0.0f};
    float vel_avatar_rot_y_deg{0.0f};
    float target_avatar_scale{1.0f};
    float vel_avatar_scale{0.0f};
    float smooth_time_s{0.22f};
    bool animate_avatar{false};
    bool active{false};

    void start(const PreviewCamera& current_cam, const PreviewCamera& goal_cam, float transition_time_s) noexcept;
    void start_with_avatar(const PreviewCamera& current_cam, const PreviewCamera& goal_cam,
                           const std::array<float, 3>& cur_pos, float cur_rot_y_deg, float cur_scale,
                           const std::array<float, 3>& goal_pos, float goal_rot_y_deg, float goal_scale,
                           float transition_time_s) noexcept;
    void cancel() noexcept;
    bool step(float dt_s, PreviewCamera& in_out_cam, std::array<float, 3>* in_out_avatar_pos = nullptr,
              float* in_out_avatar_rot_y_deg = nullptr, float* in_out_avatar_scale = nullptr) noexcept;
};

} // namespace aether::renderer
