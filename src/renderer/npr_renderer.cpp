#include "renderer/npr_renderer.hpp"

#include "core/clock.hpp"
#include "core/log.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace aether::renderer {

Expected<void> NprRenderer::init(rhi::RhiDevice& device, const RendererConfig& cfg) {
    destroy();
    if (device.device() == VK_NULL_HANDLE) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "NprRenderer::init null RhiDevice")};
    }
    device_ = &device;

    if (auto res = graph_.init(device, cfg); !res) {
        destroy();
        return res;
    }

    auto init_slots = [&](std::array<CriticalCmdSlot, kFrameSlots>& slots) -> Expected<void> {
        for (auto& s : slots) {
            VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            cpci.queueFamilyIndex = device_->graphics_family();
            cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            if (vkCreateCommandPool(device_->device(), &cpci, nullptr, &s.pool) != VK_SUCCESS) {
                return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkCreateCommandPool failed")};
            }
            VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cbai.commandPool = s.pool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(device_->device(), &cbai, &s.cmd) != VK_SUCCESS) {
                return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkAllocateCommandBuffers failed")};
            }
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateFence(device_->device(), &fci, nullptr, &s.fence) != VK_SUCCESS) {
                return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkCreateFence failed")};
            }
        }
        return {};
    };

    if (auto res = init_slots(critical_slots_); !res) {
        destroy();
        return res;
    }
    if (auto res = init_slots(ui_slots_); !res) {
        destroy();
        return res;
    }

    timeline_counter_ = 0;
    if (device_->frame_timeline() != VK_NULL_HANDLE) {
        (void)vkGetSemaphoreCounterValue(device_->device(), device_->frame_timeline(), &timeline_counter_);
    }
    initialized_ = true;
    return {};
}

Expected<void> NprRenderer::resize(uint32_t viewport_w, uint32_t viewport_h, uint32_t output_w, uint32_t output_h) {
    if (!initialized_) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "NprRenderer::resize uninitialized")};
    }
    return graph_.resize(viewport_w, viewport_h, output_w, output_h);
}

void NprRenderer::destroy() noexcept {
    if (device_ != nullptr && device_->device() != VK_NULL_HANDLE) {
        if (!device_->is_device_lost()) {
            device_->wait_idle();
        }
        auto drop_slots = [&](std::array<CriticalCmdSlot, kFrameSlots>& slots) {
            for (auto& s : slots) {
                if (s.fence != VK_NULL_HANDLE) {
                    vkDestroyFence(device_->device(), s.fence, nullptr);
                    s.fence = VK_NULL_HANDLE;
                }
                if (s.pool != VK_NULL_HANDLE) {
                    vkDestroyCommandPool(device_->device(), s.pool, nullptr);
                    s.pool = VK_NULL_HANDLE;
                    s.cmd = VK_NULL_HANDLE;
                }
            }
        };
        drop_slots(ui_slots_);
        drop_slots(critical_slots_);
    }

    graph_.destroy();
    timeline_counter_ = 0;
    initialized_ = false;
    device_ = nullptr;
}

Expected<void> NprRenderer::bind_avatar(const GpuAvatar* avatar) {
    if (!initialized_) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "NprRenderer::bind_avatar uninitialized")};
    }
    return graph_.bind_avatar(avatar);
}

Expected<void> NprRenderer::submit_critical_and_signal(const FrameRenderRequest& req,
                                                       uint64_t& out_frame_timeline_value) {
    if (!initialized_ || device_ == nullptr || device_->is_device_lost()) {
        out_frame_timeline_value = 0;
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "Renderer unavailable for critical submit")};
    }

    const uint32_t slot = req.frame_slot % kFrameSlots;
    auto& cs = critical_slots_[slot];
    if (vkWaitForFences(device_->device(), 1, &cs.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkWaitForFences(critical) failed")};
    }
    vkResetFences(device_->device(), 1, &cs.fence);
    vkResetCommandPool(device_->device(), cs.pool, 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cs.cmd, &bi);
    graph_.record_critical_passes(cs.cmd, req);
    vkEndCommandBuffer(cs.cmd);

    out_frame_timeline_value = ++timeline_counter_;

    VkCommandBufferSubmitInfo cbsi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbsi.commandBuffer = cs.cmd;
    VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig.semaphore = device_->frame_timeline();
    sig.value = out_frame_timeline_value;
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbsi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sig;

    const VkResult res = vkQueueSubmit2(device_->graphics_queue(), 1, &si, cs.fence);
    if (res == VK_ERROR_DEVICE_LOST) {
        device_->mark_device_lost();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "VK_ERROR_DEVICE_LOST on critical submit")};
    }
    if (res != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkQueueSubmit2(critical) failed")};
    }
    return {};
}

Expected<void> NprRenderer::render_headless_frame(const FrameRenderRequest& req) {
    uint64_t tl_val = 0;
    if (auto res = submit_critical_and_signal(req, tl_val); !res) {
        return res;
    }

    const uint32_t slot = req.frame_slot % kFrameSlots;
    auto& us = ui_slots_[slot];
    if (vkWaitForFences(device_->device(), 1, &us.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkWaitForFences(ui) failed")};
    }
    vkResetFences(device_->device(), 1, &us.fence);
    vkResetCommandPool(device_->device(), us.pool, 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(us.cmd, &bi);
    graph_.record_ui_passes(us.cmd, req);
    vkEndCommandBuffer(us.cmd);

    VkSemaphoreSubmitInfo wait_tl{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait_tl.semaphore = device_->frame_timeline();
    wait_tl.value = tl_val;
    wait_tl.stageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;

    VkCommandBufferSubmitInfo cbsi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbsi.commandBuffer = us.cmd;

    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait_tl;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbsi;

    const VkResult res = vkQueueSubmit2(device_->graphics_queue(), 1, &si, us.fence);
    if (res == VK_ERROR_DEVICE_LOST) {
        device_->mark_device_lost();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "VK_ERROR_DEVICE_LOST on ui submit")};
    }
    if (res != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkQueueSubmit2(ui) failed")};
    }

    if (vkWaitForFences(device_->device(), 1, &us.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkWaitForFences(headless wait) failed")};
    }

    const auto& snap = graph_.timing().resolve_frame(req.frame_slot);
    if (snap.valid) {
        graph_.denoiser().observe_gpu_time_ms(snap.denoiser_ms);
    }
    return {};
}

void NprRenderer::record_ui_composite(VkCommandBuffer cmd_ui, const FrameRenderRequest& req) noexcept {
    graph_.record_ui_passes(cmd_ui, req);
}

Expected<SdfBakeOutput> NprRenderer::bake_and_apply_face_sdf(const SdfBakeInput& input) {
    auto baked = OfflineSdfGenerator::bake_face_sdf(input);
    if (!baked) {
        return Unexpected<Error>{baked.error()};
    }
    auto applied = graph_.apply_face_sdf(baked.value());
    if (!applied) {
        return Unexpected<Error>{applied.error()};
    }
    return baked;
}

DeviceRecoveryReport NprRenderer::recover_from_device_lost(rhi::RhiDevice& device,
                                                           const rhi::RhiDeviceConfig& device_cfg,
                                                           const RendererConfig& render_cfg, const GpuAvatar* avatar,
                                                           const std::filesystem::path& breadcrumb_dir) {
    DeviceRecoveryReport report{};
    const uint64_t t0_us = aether::now_us();

    const std::filesystem::path bc_dir =
        breadcrumb_dir.empty() ? std::filesystem::path("logs/crash_breadcrumbs") : breadcrumb_dir;
    std::error_code ec;
    std::filesystem::create_directories(bc_dir, ec);

    const auto ts =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const std::filesystem::path bc_file = bc_dir / ("aether_device_lost_" + std::to_string(ts) + ".json");
    {
        std::ofstream ofs(bc_file, std::ios::out | std::ios::trunc);
        if (ofs.good()) {
            ofs << "{\n"
                << "  \"event\": \"VK_ERROR_DEVICE_LOST\",\n"
                << "  \"timestamp_ms\": " << ts << ",\n"
                << "  \"adapter\": \"" << device.gpu_info().device_name << "\",\n"
                << "  \"vendor_id\": \"0x" << std::hex << device.gpu_info().vendor_id << std::dec << "\",\n"
                << "  \"last_timeline_value\": " << timeline_counter_ << "\n"
                << "}\n";
            report.breadcrumb_path = bc_file.string();
        }
    }

    destroy();

    if (auto dev_res = device.recreate_after_device_lost(device_cfg); !dev_res) {
        report.error_message = dev_res.error().c_str();
        report.recovery_ms = static_cast<double>(aether::now_us() - t0_us) * 0.001;
        return report;
    }

    if (auto init_res = init(device, render_cfg); !init_res) {
        report.error_message = init_res.error().c_str();
        report.recovery_ms = static_cast<double>(aether::now_us() - t0_us) * 0.001;
        return report;
    }

    if (avatar != nullptr) {
        if (auto b_res = bind_avatar(avatar); !b_res) {
            report.error_message = b_res.error().c_str();
            report.recovery_ms = static_cast<double>(aether::now_us() - t0_us) * 0.001;
            return report;
        }
    }

    report.recovered = true;
    report.recovery_ms = static_cast<double>(aether::now_us() - t0_us) * 0.001;
    AETHER_LOG_WARN("renderer", "Recovered from VK_ERROR_DEVICE_LOST cleanly");
    return report;
}

} // namespace aether::renderer
