#include "rhi/rhi_swapchain.hpp"

#include "core/log.hpp"

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <algorithm>
#include <array>

namespace aether::rhi {

namespace {

VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes, bool vsync_enabled) noexcept {
    if (vsync_enabled) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    // REQ-CORE-014 priority: MAILBOX -> IMMEDIATE -> FIFO.
    for (const auto m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
            return m;
        }
    }
    for (const auto m : modes) {
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            return m;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkSurfaceFormatKHR choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats) noexcept {
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                           : formats.front();
}

} // namespace

RhiSwapchain::~RhiSwapchain() {
    destroy();
}

Expected<void> RhiSwapchain::init(RhiDevice& device, void* hinstance, void* hwnd, uint32_t width, uint32_t height) {
    device_ = &device;
    hwnd_ = hwnd;
#if defined(_WIN32)
    if (hwnd_ != nullptr && (width == 0U || height == 0U)) {
        RECT cr{};
        if (::GetClientRect(static_cast<HWND>(hwnd_), &cr) != FALSE) {
            width = static_cast<uint32_t>(std::max<LONG>(0L, cr.right - cr.left));
            height = static_cast<uint32_t>(std::max<LONG>(0L, cr.bottom - cr.top));
        }
    }
#endif
    desired_w_ = width > 0 ? width : 1280U;
    desired_h_ = height > 0 ? height : 720U;

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = static_cast<HINSTANCE>(hinstance);
    sci.hwnd = static_cast<HWND>(hwnd);
    if (vkCreateWin32SurfaceKHR(device_->instance(), &sci, nullptr, &surface_) != VK_SUCCESS) {
        return fail(ErrorCode::VulkanError, "vkCreateWin32SurfaceKHR failed");
    }

    VkBool32 present_supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device_->physical_device(), device_->graphics_family(), surface_,
                                         &present_supported);
    if (present_supported != VK_TRUE) {
        return fail(ErrorCode::VulkanError, "Graphics queue family does not support Win32 present");
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = device_->graphics_family();
        if (vkCreateCommandPool(device_->device(), &cpci, nullptr, &frames_[i].pool) != VK_SUCCESS) {
            return fail(ErrorCode::VulkanError, "vkCreateCommandPool failed");
        }
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = frames_[i].pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_->device(), &cbai, &frames_[i].cmd) != VK_SUCCESS) {
            return fail(ErrorCode::VulkanError, "vkAllocateCommandBuffers failed");
        }
        VkSemaphoreCreateInfo smci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(device_->device(), &smci, nullptr, &frames_[i].image_available) != VK_SUCCESS) {
            return fail(ErrorCode::VulkanError, "vkCreateSemaphore failed");
        }
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(device_->device(), &fci, nullptr, &frames_[i].in_flight) != VK_SUCCESS) {
            return fail(ErrorCode::VulkanError, "vkCreateFence failed");
        }
    }

    return recreate_swapchain_only(desired_w_, desired_h_);
}

void RhiSwapchain::destroy_swapchain_resources() noexcept {
    if (device_ == nullptr || device_->device() == VK_NULL_HANDLE) {
        return;
    }
    VkDevice dev = device_->device();
    for (VkSemaphore s : render_finished_per_image_) {
        if (s != VK_NULL_HANDLE) {
            vkDestroySemaphore(dev, s, nullptr);
        }
    }
    render_finished_per_image_.clear();

    for (VkImageView v : image_views_) {
        if (v != VK_NULL_HANDLE) {
            vkDestroyImageView(dev, v, nullptr);
        }
    }
    image_views_.clear();
    images_.clear();
    image_layouts_.clear();

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(dev, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void RhiSwapchain::destroy() noexcept {
    if (device_ != nullptr && device_->device() != VK_NULL_HANDLE) {
        device_->wait_idle();
        destroy_swapchain_resources();
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (frames_[i].in_flight != VK_NULL_HANDLE) {
                vkDestroyFence(device_->device(), frames_[i].in_flight, nullptr);
            }
            if (frames_[i].image_available != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_->device(), frames_[i].image_available, nullptr);
            }
            if (frames_[i].pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device_->device(), frames_[i].pool, nullptr);
            }
            frames_[i] = FrameSync{};
        }
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(device_->instance(), surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
    }
    device_ = nullptr;
}

Expected<void> RhiSwapchain::recreate_swapchain_only(uint32_t width, uint32_t height) {
    device_->wait_idle();
    destroy_swapchain_resources();

    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_->physical_device(), surface_, &caps) != VK_SUCCESS) {
        return fail(ErrorCode::VulkanError, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
    }

    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) {
        extent_ = VkExtent2D{0, 0};
        need_recreate_ = true;
        return Expected<void>{};
    }

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device_->physical_device(), surface_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device_->physical_device(), surface_, &fmt_count, formats.data());

    uint32_t pm_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device_->physical_device(), surface_, &pm_count, nullptr);
    std::vector<VkPresentModeKHR> modes(pm_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device_->physical_device(), surface_, &pm_count, modes.data());

    const VkSurfaceFormatKHR chosen_fmt = choose_surface_format(formats);
    format_ = chosen_fmt.format;
    color_space_ = chosen_fmt.colorSpace;
    present_mode_ = choose_present_mode(modes, vsync_enabled_);

#if defined(_WIN32)
    if (hwnd_ != nullptr) {
        RECT cr{};
        if (::GetClientRect(static_cast<HWND>(hwnd_), &cr) != FALSE) {
            const uint32_t phys_w = static_cast<uint32_t>(std::max<LONG>(0L, cr.right - cr.left));
            const uint32_t phys_h = static_cast<uint32_t>(std::max<LONG>(0L, cr.bottom - cr.top));
            if (phys_w > 0U && phys_h > 0U) {
                width = phys_w;
                height = phys_h;
                desired_w_ = phys_w;
                desired_h_ = phys_h;
            }
        }
    }
#endif

    if (width > 0U && height > 0U) {
        extent_.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    } else if (caps.currentExtent.width != UINT32_MAX) {
        extent_ = caps.currentExtent;
    } else {
        extent_.width = std::clamp(desired_w_, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(desired_h_, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t req_images = caps.minImageCount + 1U;
    if (caps.maxImageCount > 0 && req_images > caps.maxImageCount) {
        req_images = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swci.surface = surface_;
    swci.minImageCount = req_images;
    swci.imageFormat = format_;
    swci.imageColorSpace = color_space_;
    swci.imageExtent = extent_;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = present_mode_;
    swci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_->device(), &swci, nullptr, &swapchain_) != VK_SUCCESS) {
        return fail(ErrorCode::VulkanError, "vkCreateSwapchainKHR failed");
    }

    uint32_t img_count = 0;
    vkGetSwapchainImagesKHR(device_->device(), swapchain_, &img_count, nullptr);
    images_.resize(img_count);
    vkGetSwapchainImagesKHR(device_->device(), swapchain_, &img_count, images_.data());
    image_views_.resize(img_count);
    render_finished_per_image_.resize(img_count, VK_NULL_HANDLE);
    image_layouts_.assign(img_count, VK_IMAGE_LAYOUT_UNDEFINED);

    for (uint32_t i = 0; i < img_count; ++i) {
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = images_[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = format_;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.baseMipLevel = 0;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.baseArrayLayer = 0;
        ivci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_->device(), &ivci, nullptr, &image_views_[i]) != VK_SUCCESS) {
            return fail(ErrorCode::VulkanError, "vkCreateImageView(swapchain) failed");
        }
        VkSemaphoreCreateInfo smci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(device_->device(), &smci, nullptr, &render_finished_per_image_[i]) != VK_SUCCESS) {
            return fail(ErrorCode::VulkanError, "vkCreateSemaphore(swapchain) failed");
        }
    }

    need_recreate_ = false;
    AETHER_LOG_INFO("rhi", "Swapchain ready: %ux%u, %u images, mode=%s", extent_.width, extent_.height, img_count,
                    present_mode_name());
    return Expected<void>{};
}

Expected<void> RhiSwapchain::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        need_recreate_ = true;
        return Expected<void>{};
    }
    desired_w_ = width;
    desired_h_ = height;
    return recreate_swapchain_only(width, height);
}

Expected<void> RhiSwapchain::set_vsync(bool enabled) {
    if (vsync_enabled_ == enabled) {
        return Expected<void>{};
    }
    vsync_enabled_ = enabled;
    if (device_ == nullptr || swapchain_ == VK_NULL_HANDLE) {
        return Expected<void>{};
    }
    return recreate_swapchain_only(desired_w_, desired_h_);
}

Expected<AcquiredFrame> RhiSwapchain::acquire_and_begin_commands() {
    if (need_recreate_) {
        auto rc = recreate_swapchain_only(desired_w_, desired_h_);
        if (!rc) {
            return make_unexpected(rc.error());
        }
        if (need_recreate_ || swapchain_ == VK_NULL_HANDLE) {
            return AcquiredFrame{}; // Minimized window
        }
    }

    FrameSync& sync = frames_[current_slot_];
    vkWaitForFences(device_->device(), 1, &sync.in_flight, VK_TRUE, UINT64_MAX);

    uint32_t img_idx = 0;
    const VkResult acq = vkAcquireNextImageKHR(device_->device(), swapchain_, UINT64_MAX, sync.image_available,
                                               VK_NULL_HANDLE, &img_idx);
    if (acq == VK_ERROR_DEVICE_LOST) {
        device_->mark_device_lost();
        return fail(ErrorCode::VulkanError, "vkAcquireNextImageKHR failed (VK_ERROR_DEVICE_LOST)");
    }
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        need_recreate_ = true;
        return AcquiredFrame{};
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        return fail(ErrorCode::VulkanError, "vkAcquireNextImageKHR failed");
    }

    vkResetFences(device_->device(), 1, &sync.in_flight);
    vkResetCommandPool(device_->device(), sync.pool, 0);

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(sync.cmd, &cbbi) != VK_SUCCESS) {
        return fail(ErrorCode::VulkanError, "vkBeginCommandBuffer failed");
    }

    AcquiredFrame frame{};
    frame.cmd = sync.cmd;
    frame.image = images_[img_idx];
    frame.view = image_views_[img_idx];
    frame.extent = extent_;
    frame.image_index = img_idx;
    frame.frame_slot = current_slot_;
    frame.valid = true;
    return frame;
}

void RhiSwapchain::begin_swapchain_pass(const AcquiredFrame& frame, const float clear_rgba[4]) noexcept {
    if (!frame.valid) {
        return;
    }

    RhiDevice::cmd_transition_image(
        frame.cmd, frame.image, image_layouts_[frame.image_index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    image_layouts_[frame.image_index] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkRenderingAttachmentInfo color_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color_att.imageView = frame.view;
    color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.clearValue.color.float32[0] = clear_rgba[0];
    color_att.clearValue.color.float32[1] = clear_rgba[1];
    color_att.clearValue.color.float32[2] = clear_rgba[2];
    color_att.clearValue.color.float32[3] = clear_rgba[3];

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = VkRect2D{{0, 0}, frame.extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color_att;
    vkCmdBeginRendering(frame.cmd, &ri);

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<float>(frame.extent.width);
    vp.height = static_cast<float>(frame.extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &vp);

    VkRect2D scissor{{0, 0}, frame.extent};
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
}

Expected<AcquiredFrame> RhiSwapchain::begin_frame(const float clear_rgba[4]) {
    auto acq = acquire_and_begin_commands();
    if (!acq) {
        return make_unexpected(acq.error());
    }
    const AcquiredFrame& frame = acq.value();
    if (frame.valid) {
        begin_swapchain_pass(frame, clear_rgba);
    }
    return frame;
}

Expected<void> RhiSwapchain::end_frame(const AcquiredFrame& frame, VkSemaphore extra_wait_semaphore,
                                       uint64_t extra_wait_value, VkPipelineStageFlags2 extra_wait_stage) {
    if (!frame.valid) {
        return Expected<void>{};
    }

    FrameSync& sync = frames_[frame.frame_slot];
    vkCmdEndRendering(frame.cmd);

    RhiDevice::cmd_transition_image(frame.cmd, frame.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    image_layouts_[frame.image_index] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    if (vkEndCommandBuffer(frame.cmd) != VK_SUCCESS) {
        return fail(ErrorCode::VulkanError, "vkEndCommandBuffer failed");
    }

    VkSemaphoreSubmitInfo wait_sems[2]{};
    wait_sems[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait_sems[0].semaphore = sync.image_available;
    wait_sems[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    uint32_t wait_count = 1;
    if (extra_wait_semaphore != VK_NULL_HANDLE) {
        wait_sems[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_sems[1].semaphore = extra_wait_semaphore;
        wait_sems[1].value = extra_wait_value;
        wait_sems[1].stageMask = extra_wait_stage;
        wait_count = 2;
    }

    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = frame.cmd;

    VkSemaphoreSubmitInfo sig_sem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig_sem.semaphore = render_finished_per_image_[frame.image_index];
    sig_sem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = wait_count;
    submit.pWaitSemaphoreInfos = wait_sems;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &sig_sem;

    const VkResult sub_rc = vkQueueSubmit2(device_->graphics_queue(), 1, &submit, sync.in_flight);
    if (sub_rc != VK_SUCCESS) {
        if (sub_rc == VK_ERROR_DEVICE_LOST) {
            device_->mark_device_lost();
        }
        return fail(ErrorCode::VulkanError, "vkQueueSubmit2 failed");
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_finished_per_image_[frame.image_index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &frame.image_index;

    const VkResult pr = vkQueuePresentKHR(device_->graphics_queue(), &pi);
    if (pr == VK_ERROR_DEVICE_LOST) {
        device_->mark_device_lost();
        return fail(ErrorCode::VulkanError, "vkQueuePresentKHR failed (VK_ERROR_DEVICE_LOST)");
    }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        need_recreate_ = true;
    } else if (pr != VK_SUCCESS) {
        return fail(ErrorCode::VulkanError, "vkQueuePresentKHR failed");
    }

    current_slot_ = (current_slot_ + 1U) % kFramesInFlight;
    return Expected<void>{};
}

const char* RhiSwapchain::present_mode_name() const noexcept {
    switch (present_mode_) {
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return "MAILBOX";
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "IMMEDIATE";
    case VK_PRESENT_MODE_FIFO_KHR:
        return "FIFO";
    default:
        return "OTHER";
    }
}

} // namespace aether::rhi
