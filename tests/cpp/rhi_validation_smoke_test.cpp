#include "core/log.hpp"
#include "rhi/rhi_device.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

TEST_CASE("Vulkan 1.3 RHI initializes with sync validation active and zero hazards", "[rhi][validation]") {
    aether::rhi::RhiDeviceConfig cfg{};
    cfg.enable_validation = true;
    cfg.require_validation = true;
#if defined(AETHER_ENABLE_GPU_VALIDATION) && AETHER_ENABLE_GPU_VALIDATION
    cfg.enable_gpu_assisted = true;
#endif
    cfg.headless = true;

    auto dev_res = aether::rhi::RhiDevice::create(cfg);
    REQUIRE(dev_res.has_value());
    aether::rhi::RhiDevice* rhi = dev_res.value();

    REQUIRE(rhi->validation_active());
    REQUIRE(rhi->sync_validation_active());

    auto buf_res = rhi->create_buffer(4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VMA_MEMORY_USAGE_AUTO);
    REQUIRE(buf_res.has_value());
    auto buf = buf_res.value();

    auto img_res = rhi->create_image_2d(256, 256, VK_FORMAT_B8G8R8A8_UNORM,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(img_res.has_value());
    auto img = img_res.value();

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = rhi->graphics_family();
    REQUIRE(vkCreateCommandPool(rhi->device(), &cpci, nullptr, &pool) == VK_SUCCESS);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    REQUIRE(vkAllocateCommandBuffers(rhi->device(), &cbai, &cmd) == VK_SUCCESS);

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    REQUIRE(vkBeginCommandBuffer(cmd, &cbbi) == VK_SUCCESS);

    vkCmdFillBuffer(cmd, buf.handle, 0, 4096, 0x11223344U);

    aether::rhi::RhiDevice::cmd_transition_image(
        cmd, img.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color_att.imageView = img.view;
    color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.clearValue.color.float32[0] = 0.067f;
    color_att.clearValue.color.float32[1] = 0.075f;
    color_att.clearValue.color.float32[2] = 0.094f;
    color_att.clearValue.color.float32[3] = 1.0f;

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = VkRect2D{{0, 0}, {256, 256}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color_att;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdEndRendering(cmd);

    aether::rhi::RhiDevice::cmd_transition_image(
        cmd, img.handle, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    REQUIRE(vkEndCommandBuffer(cmd) == VK_SUCCESS);

    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSemaphoreSubmitInfo sig_tl{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig_tl.semaphore = rhi->frame_timeline();
    sig_tl.value = 1;
    sig_tl.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &sig_tl;
    REQUIRE(vkQueueSubmit2(rhi->graphics_queue(), 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);

    const VkSemaphore tl = rhi->frame_timeline();
    const uint64_t wait_val = 1;
    VkSemaphoreWaitInfo swi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    swi.semaphoreCount = 1;
    swi.pSemaphores = &tl;
    swi.pValues = &wait_val;
    REQUIRE(vkWaitSemaphores(rhi->device(), &swi, UINT64_MAX) == VK_SUCCESS);

    vkDestroyCommandPool(rhi->device(), pool, nullptr);
    rhi->destroy_image(img);
    rhi->destroy_buffer(buf);

    aether::LogSystem::drain(
        [](const aether::LogRecord& rec, void*) {
            if (rec.level >= aether::LogLevel::Warn) {
                std::fprintf(stderr, "[%s][%s] %s\n", aether::LogSystem::level_name(rec.level), rec.category,
                             rec.message);
            }
        },
        nullptr, 512);

    CHECK(rhi->validation_error_count() == 0);
    CHECK(rhi->validation_warning_count() == 0);

    rhi->destroy();
    delete rhi;
}
