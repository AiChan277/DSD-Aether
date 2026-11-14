#include "aether_ffi_contract.h"
#include "core/clock.hpp"
#include "core/hot_path_guard.hpp"
#include "renderer/asset_upload/asset_job.hpp"
#include "renderer/preview_pass.hpp"
#include "rhi/rhi_device.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>

TEST_CASE("Async AssetJob + VMA GPU upload + PreviewPass offscreen render stays validation-clean",
          "[phase2][rhi][validation]") {
    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);

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

    const auto ref_vrm = std::filesystem::path(AETHER_REPO_ROOT) / "tests" / "data" / "reference_avatar.vrm";
    REQUIRE(std::filesystem::is_regular_file(ref_vrm));

    {
        aether::renderer::AssetJob job;
        aether::HighResTimer timer;

        job.start_load(*rhi, ref_vrm.string());
        const uint64_t deadline_us = aether::now_us() + 15000000ULL; // 15s generous CI timeout
        while (job.state() == aether::renderer::AssetJobState::Loading && aether::now_us() < deadline_us) {
            timer.sleep_us(500);
        }
        INFO("AssetJob last_error: " << job.last_error());
        REQUIRE(job.state() == aether::renderer::AssetJobState::ReadyForGpu);

        auto avatar_res = job.finalize_gpu_upload(*rhi);
        INFO("Upload error: " << (!avatar_res ? avatar_res.error().c_str() : ""));
        REQUIRE(avatar_res.has_value());
        auto avatar = std::move(avatar_res.value());
        REQUIRE(avatar->stats.pointer_identity_verified);
        REQUIRE(avatar->requirements.vertex_count > 30000U);
        REQUIRE(!avatar->textures.empty());

        std::printf("[PERF-GPU-UPLOAD] reference_avatar.vrm open+query=%.2f ms fill=%.2f ms gpu_upload=%.2f ms "
                    "staging=%.2f MiB\n",
                    avatar->stats.open_query_ms, avatar->stats.fill_ms, avatar->stats.gpu_upload_ms,
                    static_cast<double>(avatar->stats.staging_bytes) / (1024.0 * 1024.0));
        std::fflush(stdout);

#if defined(NDEBUG) && (!defined(AETHER_ENABLE_GPU_VALIDATION) || !AETHER_ENABLE_GPU_VALIDATION)
        CHECK(avatar->stats.gpu_upload_ms <= 50.0);
#endif

        aether::renderer::PreviewPass pass;
        REQUIRE(pass.init(*rhi, VK_FORMAT_B8G8R8A8_UNORM).has_value());
        bool recreated = false;
        REQUIRE(pass.ensure_size(512, 512, recreated).has_value());
        REQUIRE(recreated);
        REQUIRE(pass.bind_avatar(avatar.get()).has_value());

        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = rhi->graphics_family();
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        REQUIRE(vkCreateCommandPool(rhi->device(), &cpci, nullptr, &pool) == VK_SUCCESS);

        VkCommandBuffer cmds[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 2;
        REQUIRE(vkAllocateCommandBuffers(rhi->device(), &cbai, cmds) == VK_SUCCESS);

        VkFence fences[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        REQUIRE(vkCreateFence(rhi->device(), &fci, nullptr, &fences[0]) == VK_SUCCESS);
        REQUIRE(vkCreateFence(rhi->device(), &fci, nullptr, &fences[1]) == VK_SUCCESS);

        aether::HotPathGuard::reset_thread_violations();
        aether::renderer::PreviewCamera cam{};
        {
            AETHER_HOT_PATH_SCOPE;
            cam = aether::renderer::fit_camera_to_avatar(*avatar);
        }
        CHECK(aether::HotPathGuard::thread_violations() == 0ULL);

        aether::renderer::PreviewRenderOptions opts{};
        opts.show_bones = true;

        std::array<float[12], AETHER_HUMANOID_BONE_COUNT> pose_mats{};
        for (auto& m : pose_mats) {
            m[0] = 1.0f;
            m[5] = 1.0f;
            m[10] = 1.0f;
        }
        std::array<float, 64> expr_w{};
        expr_w[0] = 0.35f;

        for (uint32_t frame_idx = 0; frame_idx < 4; ++frame_idx) {
            const uint32_t slot = frame_idx % 2U;
            vkWaitForFences(rhi->device(), 1, &fences[slot], VK_TRUE, UINT64_MAX);
            vkResetFences(rhi->device(), 1, &fences[slot]);

            aether::HotPathGuard::reset_thread_violations();
            {
                AETHER_HOT_PATH_SCOPE;
                avatar->update_pose(slot, frame_idx + 1ULL, pose_mats.data(), AETHER_HUMANOID_BONE_COUNT, expr_w.data(),
                                    static_cast<uint32_t>(expr_w.size()), true);
            }
            CHECK(aether::HotPathGuard::thread_violations() == 0ULL);

            VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            REQUIRE(vkBeginCommandBuffer(cmds[slot], &cbbi) == VK_SUCCESS);

            // Vulkan validation layers allocate internally inside vkCmd* hooks, so `record()` runs
            // alongside `ImGui_ImplVulkan_RenderDrawData` outside the CRT malloc guard scope.
            pass.record(cmds[slot], slot, avatar.get(), cam, opts);

            REQUIRE(vkEndCommandBuffer(cmds[slot]) == VK_SUCCESS);

            VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            cmd_info.commandBuffer = cmds[slot];
            VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &cmd_info;
            REQUIRE(vkQueueSubmit2(rhi->graphics_queue(), 1, &submit, fences[slot]) == VK_SUCCESS);
        }

        vkWaitForFences(rhi->device(), 2, fences, VK_TRUE, UINT64_MAX);
        vkDestroyFence(rhi->device(), fences[0], nullptr);
        vkDestroyFence(rhi->device(), fences[1], nullptr);
        vkDestroyCommandPool(rhi->device(), pool, nullptr);

        pass.destroy();
        avatar.reset();

        // Cooperative cancellation check through C++ AssetJob wrapper
        job.start_load(*rhi, ref_vrm.string());
        job.cancel();
        job.reset();
        CHECK(job.state() == aether::renderer::AssetJobState::Idle);
    }

    CHECK(rhi->validation_error_count() == 0ULL);
    CHECK(rhi->validation_warning_count() == 0ULL);

    rhi->destroy();
    delete rhi;
    aether_runtime_shutdown();
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
}
