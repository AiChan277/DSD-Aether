#pragma once

#include "rhi/rhi_device.hpp"

#include <cstdint>
#include <vector>

namespace aether::rhi {

struct AcquiredFrame {
    VkCommandBuffer cmd{VK_NULL_HANDLE};
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkExtent2D extent{};
    uint32_t image_index{0};
    uint32_t frame_slot{0};
    bool valid{false};
};

/**
 * Win32 Vulkan 1.3 swapchain implementing MAILBOX -> IMMEDIATE -> FIFO selection
 * (`REQ-CORE-014`), Vulkan 1.3 dynamic rendering + Synchronization2, and per-image
 * render-finished semaphores (`HR-08` zero sync warnings).
 */
class RhiSwapchain {
  public:
    static constexpr uint32_t kFramesInFlight = 2;

    RhiSwapchain() = default;
    ~RhiSwapchain();

    RhiSwapchain(const RhiSwapchain&) = delete;
    RhiSwapchain& operator=(const RhiSwapchain&) = delete;

    [[nodiscard]] Expected<void> init(RhiDevice& device, void* hinstance, void* hwnd, uint32_t width, uint32_t height);
    void destroy() noexcept;
    [[nodiscard]] Expected<void> resize(uint32_t width, uint32_t height);

    /** Waits for in-flight slot, acquires swapchain image, and starts primary command buffer. */
    [[nodiscard]] Expected<AcquiredFrame> acquire_and_begin_commands();

    /** Transitions swapchain image to COLOR_ATTACHMENT_OPTIMAL and begins dynamic rendering. */
    void begin_swapchain_pass(const AcquiredFrame& frame, const float clear_rgba[4]) noexcept;

    /** Waits for in-flight slot, acquires swapchain image, transitions to COLOR_ATTACHMENT_OPTIMAL. */
    [[nodiscard]] Expected<AcquiredFrame> begin_frame(const float clear_rgba[4]);

    /** Transitions swapchain image to PRESENT_SRC_KHR, submits via `vkQueueSubmit2`, and presents. */
    [[nodiscard]] Expected<void>
    end_frame(const AcquiredFrame& frame, VkSemaphore extra_wait_semaphore = VK_NULL_HANDLE,
              uint64_t extra_wait_value = 0,
              VkPipelineStageFlags2 extra_wait_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);

    [[nodiscard]] VkFormat format() const noexcept { return format_; }
    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
    [[nodiscard]] VkPresentModeKHR present_mode() const noexcept { return present_mode_; }
    [[nodiscard]] const char* present_mode_name() const noexcept;
    [[nodiscard]] uint32_t image_count() const noexcept { return static_cast<uint32_t>(images_.size()); }
    [[nodiscard]] bool vsync_enabled() const noexcept { return vsync_enabled_; }
    [[nodiscard]] Expected<void> set_vsync(bool enabled);

  private:
    struct FrameSync {
        VkCommandPool pool{VK_NULL_HANDLE};
        VkCommandBuffer cmd{VK_NULL_HANDLE};
        VkSemaphore image_available{VK_NULL_HANDLE};
        VkFence in_flight{VK_NULL_HANDLE};
    };

    Expected<void> recreate_swapchain_only(uint32_t width, uint32_t height);
    void destroy_swapchain_resources() noexcept;

    RhiDevice* device_{nullptr};
    void* hwnd_{nullptr};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    VkFormat format_{VK_FORMAT_B8G8R8A8_UNORM};
    VkColorSpaceKHR color_space_{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    VkPresentModeKHR present_mode_{VK_PRESENT_MODE_FIFO_KHR};
    bool vsync_enabled_{false};
    VkExtent2D extent_{};

    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    std::vector<VkSemaphore> render_finished_per_image_;
    std::vector<VkImageLayout> image_layouts_;

    FrameSync frames_[kFramesInFlight]{};
    uint32_t current_slot_{0};
    bool need_recreate_{false};
    uint32_t desired_w_{0};
    uint32_t desired_h_{0};
};

} // namespace aether::rhi
