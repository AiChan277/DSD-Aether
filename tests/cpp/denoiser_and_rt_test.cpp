#include "core/hot_path_guard.hpp"
#include "renderer/denoise/denoiser_manager.hpp"
#include "renderer/npr_renderer.hpp"
#include "synthetic_avatar_helper.hpp"
#include "test_png_io.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <vector>

TEST_CASE("Denoiser auto-selection per vendor and zero-alloc <= 0.8 ms p95 / error fallback", "[phase4][denoiser]") {
    using aether::renderer::DenoiserFallbackReason;
    using aether::renderer::DenoiserKind;
    using aether::renderer::DenoiserManager;

    CHECK(DenoiserManager::select_default_for_vendor(0x10DEu, false, false, false) == DenoiserKind::NrdSigma);
    CHECK(DenoiserManager::select_default_for_vendor(0x10DEu, false, true, false) == DenoiserKind::Optix);
    CHECK(DenoiserManager::select_default_for_vendor(0x1002u, false, false, false) == DenoiserKind::FfxCompute);
    CHECK(DenoiserManager::select_default_for_vendor(0x8086u, false, false, true) == DenoiserKind::Oidn);
    CHECK(DenoiserManager::select_default_for_vendor(0x8086u, false, false, false) == DenoiserKind::SvgfLite);
    CHECK(DenoiserManager::select_default_for_vendor(0x10DEu, true, true, true) == DenoiserKind::SvgfLite);

    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);

    aether::rhi::RhiDeviceConfig dcfg{};
    dcfg.enable_validation = true;
    dcfg.require_validation = true;
    dcfg.headless = true;
    dcfg.preferred_vendor_id = 0x10DEu;
    auto dev_res = aether::rhi::RhiDevice::create(dcfg);
    REQUIRE(dev_res.has_value());
    std::unique_ptr<aether::rhi::RhiDevice> rhi(dev_res.value());

    aether::rhi::BindlessHeap bindless;
    REQUIRE(bindless.init(*rhi, {}).has_value());

    DenoiserManager mgr;
    REQUIRE(mgr.init(*rhi, bindless, 640, 360, DenoiserKind::Auto).has_value());
    if (rhi->gpu_info().vendor_id == 0x10DEu) {
        CHECK(mgr.active_kind() == DenoiserKind::NrdSigma);
    }

    // 1) Budget exceedance across 120 frames triggers zero-alloc fallback to SvgfLite
    aether::HotPathGuard::reset_thread_violations();
    {
        AETHER_HOT_PATH_SCOPE;
        for (int i = 0; i < 120; ++i) {
            mgr.observe_gpu_time_ms(0.95f);
        }
    }
    CHECK(aether::HotPathGuard::thread_violations() == 0);
    CHECK(mgr.fallback_triggered());
    CHECK(mgr.fallback_reason() == DenoiserFallbackReason::BudgetExceeded);
    CHECK(mgr.active_kind() == DenoiserKind::SvgfLite);
    CHECK(mgr.rolling_p95_ms() > 0.80f);

    // 2) Re-select vendor backend cleanly and test injected runtime execution failure -> zero-alloc fallback
    mgr.reset_budget_monitor();
    if (rhi->gpu_info().vendor_id == 0x10DEu) {
        REQUIRE(mgr.select_backend(DenoiserKind::NrdSigma).has_value());
        CHECK(mgr.active_kind() == DenoiserKind::NrdSigma);
    }

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = rhi->graphics_family();
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    REQUIRE(vkCreateCommandPool(rhi->device(), &cpci, nullptr, &pool) == VK_SUCCESS);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    REQUIRE(vkAllocateCommandBuffers(rhi->device(), &cbai, &cmd) == VK_SUCCESS);

    auto dummy_raw =
        rhi->create_image_2d(640, 360, VK_FORMAT_R16G16B16A16_SFLOAT,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    auto dummy_dst =
        rhi->create_image_2d(640, 360, VK_FORMAT_R16G16B16A16_SFLOAT,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    REQUIRE(dummy_raw.has_value());
    REQUIRE(dummy_dst.has_value());

    const auto raw_slot = bindless.register_sampled_image(dummy_raw.value().view);
    const auto dst_slot = bindless.register_storage_image(dummy_dst.value().view);
    REQUIRE(raw_slot.has_value());
    REQUIRE(dst_slot.has_value());

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    REQUIRE(vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS);

    rhi->cmd_transition_image(cmd, dummy_raw.value().handle, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    rhi->cmd_transition_image(cmd, dummy_dst.value().handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_WRITE_BIT);

    VkDescriptorSet set0 = bindless.global_set();
    aether::renderer::DenoiserInputView inputs{};
    inputs.width = 640;
    inputs.height = 360;
    inputs.raw_shadow_sampled_slot = raw_slot.value();
    inputs.denoised_shadow_storage_slot = dst_slot.value();
    inputs.raw_rim_sampled_slot = raw_slot.value();
    inputs.denoised_rim_storage_slot = dst_slot.value();
    inputs.history_shadow_sampled_slot = raw_slot.value();
    inputs.history_rim_sampled_slot = raw_slot.value();
    inputs.gb_normal_sampled_slot = raw_slot.value();
    inputs.gb_mv_sampled_slot = raw_slot.value();
    inputs.depth_sampled_slot = raw_slot.value();
    inputs.prev_depth_sampled_slot = raw_slot.value();
    inputs.prev_normal_sampled_slot = raw_slot.value();
    (void)set0;

    mgr.inject_failure_next_execute();
    aether::HotPathGuard::reset_thread_violations();
    bool exec_ok = false;
    {
        AETHER_HOT_PATH_SCOPE;
        exec_ok = mgr.execute(cmd, inputs).has_value();
    }
    REQUIRE(exec_ok);
#if !defined(__SANITIZE_ADDRESS__)
    // Under ASan, process-wide malloc hooks count internal allocations inside Vulkan layers/drivers
    // invoked by vkCmdDispatch / vkUpdateDescriptorSets; MSVC Debug CRT checks zero allocs in AETHER C++.
    CHECK(aether::HotPathGuard::thread_violations() == 0);
#endif
    CHECK(mgr.fallback_triggered());
    CHECK(mgr.fallback_reason() == DenoiserFallbackReason::RuntimeError);
    CHECK(mgr.active_kind() == DenoiserKind::SvgfLite);

    REQUIRE(vkEndCommandBuffer(cmd) == VK_SUCCESS);

    vkDestroyCommandPool(rhi->device(), pool, nullptr);
    rhi->destroy_image(dummy_raw.value());
    rhi->destroy_image(dummy_dst.value());
    mgr.destroy();
    bindless.destroy();
    CHECK(rhi->validation_error_count() == 0);
    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("1080p RT + BLAS refit + 300-frame rebuild + GPU frame budget verification", "[phase4][rt][perf]") {
    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);

    aether::rhi::RhiDeviceConfig dcfg{};
#if defined(NDEBUG)
    dcfg.enable_validation = false;
#else
    dcfg.enable_validation = true;
    dcfg.require_validation = true;
#endif
    dcfg.headless = true;
    dcfg.enable_ray_tracing = true;
    dcfg.preferred_vendor_id = 0x10DEu;

    auto dev_res = aether::rhi::RhiDevice::create(dcfg);
    REQUIRE(dev_res.has_value());
    std::unique_ptr<aether::rhi::RhiDevice> rhi(dev_res.value());
    REQUIRE(rhi->rt_supported());

    auto avatar_res = aether::test::load_reference_vrm_avatar(*rhi);
    REQUIRE(avatar_res.has_value());
    auto avatar = std::move(avatar_res.value());

    aether::renderer::NprRenderer renderer;
    aether::renderer::RendererConfig rcfg{};
    rcfg.width = 1920;
    rcfg.height = 1080;
    rcfg.enable_rt = true;
    rcfg.denoiser = aether::renderer::DenoiserKind::Auto;
    REQUIRE(renderer.init(*rhi, rcfg).has_value());
    REQUIRE(renderer.bind_avatar(avatar.get()).has_value());

    aether::renderer::FrameRenderRequest req{};
    req.avatar = avatar.get();
    req.rig = &renderer.lighting();
    req.camera = aether::renderer::PreviewCamera{0.0f, 1.32f, 0.0f, 0.25f, 0.06f, 1.35f, 0.55f};
    req.rt_enabled = true;

    std::vector<float> total_ms;
    std::vector<float> rt_ms;
    std::vector<float> denoise_ms;
    std::vector<float> npr_ms;

    constexpr uint64_t kFrames = 305;
    for (uint64_t f = 0; f < kFrames; ++f) {
        req.frame_index = f;
        req.completed_frame_index = (f >= 2) ? (f - 2) : 0;
        req.frame_slot = static_cast<uint32_t>(f % aether::rhi::RhiSwapchain::kFramesInFlight);

        REQUIRE(renderer.render_headless_frame(req).has_value());

        if (f == 0) {
            CHECK(renderer.graph().accel_struct().last_was_full_rebuild());
        } else if (f == 150) {
            CHECK(!renderer.graph().accel_struct().last_was_full_rebuild());
        } else if (f == 300) {
            CHECK(renderer.graph().accel_struct().last_was_full_rebuild());
        }

        const auto& snap = renderer.graph().timing().latest_snapshot();
        if (f >= 15 && snap.valid) {
            total_ms.push_back(snap.gpu_total_ms);
            rt_ms.push_back(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::RayTrace)]);
            denoise_ms.push_back(snap.denoiser_ms);
            npr_ms.push_back(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::Live2dNpr)]);
        }
    }

    CHECK(renderer.graph().accel_struct().full_rebuild_count() >= 2);
    CHECK(renderer.graph().accel_struct().refit_count() >= 300);
    REQUIRE(!total_ms.empty());

    std::sort(total_ms.begin(), total_ms.end());
    std::sort(rt_ms.begin(), rt_ms.end());
    std::sort(denoise_ms.begin(), denoise_ms.end());
    std::sort(npr_ms.begin(), npr_ms.end());

    const size_t p95_idx = (total_ms.size() - 1) * 95 / 100;
    const float gpu_p95 = total_ms[p95_idx];
    const float rt_p95 = rt_ms[p95_idx];
    const float denoise_p95 = denoise_ms[p95_idx];
    const float npr_p95 = npr_ms[p95_idx];

    std::printf("[PERF-PHASE4-1080P] GPU(%s) total_p95=%.3f ms (rt=%.3f ms denoise=%.3f ms npr=%.3f ms)\n",
                rhi->gpu_info().device_name.c_str(), gpu_p95, rt_p95, denoise_p95, npr_p95);
    std::fflush(stdout);

#if defined(NDEBUG)
    CHECK(gpu_p95 <= 4.2f);
    CHECK(denoise_p95 <= 0.8f);
#endif
    if (dcfg.enable_validation) {
        CHECK(rhi->validation_error_count() == 0);
    }

    renderer.destroy();
    avatar->destroy();
    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("Rev 3.1 Empirical Test #7: LightingStageView 1x Skin+BLAS, TransparentPass Timing & 10x Swap Zero-Leak",
          "[phase4][rt][rev31][empirical]") {
    using namespace aether::renderer;

    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);

    aether::rhi::RhiDeviceConfig dcfg{};
    dcfg.enable_validation = true;
    dcfg.require_validation = true;
    dcfg.headless = true;
    auto dev_res = aether::rhi::RhiDevice::create(dcfg);
    REQUIRE(dev_res.has_value());
    std::unique_ptr<aether::rhi::RhiDevice> rhi(dev_res.value());

    auto build_avatar = [&](uint8_t tint_r) {
        aether::test::SyntheticMeshSpec spec{};
        spec.materials.push_back(
            aether::test::make_basic_npr_material(static_cast<float>(tint_r) / 255.0f, 0.70f, 0.68f));
        auto mat_tr = aether::test::make_basic_npr_material(0.55f, 0.78f, 1.0f, 0.65f);
        mat_tr.alpha_mode = AETHER_ALPHA_MODE_BLEND;
        spec.materials.push_back(mat_tr);
        const float p0[3] = {-0.45f, 0.70f, 0.0f};
        const float p1[3] = {0.45f, 0.70f, 0.0f};
        const float p2[3] = {0.45f, 1.80f, 0.0f};
        const float p3[3] = {-0.45f, 1.80f, 0.0f};
        const float n[3] = {0.0f, 0.0f, 1.0f};
        const float t[3] = {1.0f, 0.0f, 0.0f};
        aether::test::append_quad(spec, p0, p1, p2, p3, n, t, 0.0f, 0.0f, 0.0f, 0u, AETHER_SUBMESH_FLAG_OPAQUE);
        const float q0[3] = {-0.25f, 0.90f, 0.04f};
        const float q1[3] = {0.25f, 0.90f, 0.04f};
        const float q2[3] = {0.25f, 1.60f, 0.04f};
        const float q3[3] = {-0.25f, 1.60f, 0.04f};
        aether::test::append_quad(spec, q0, q1, q2, q3, n, t, 0.0f, 0.0f, 0.0f, 1u, AETHER_SUBMESH_FLAG_TRANSPARENT);
        auto res = aether::test::upload_synthetic_avatar(*rhi, spec);
        REQUIRE(res.has_value());
        return std::move(res.value());
    };

    NprRenderer renderer;
    RendererConfig rcfg{};
    rcfg.width = 640u;
    rcfg.height = 360u;
    rcfg.enable_rt = rhi->rt_supported();
    REQUIRE(renderer.init(*rhi, rcfg).has_value());

    // 1) Broadcast + MainStage + QuadTop + QuadRight share exactly 1 Skin & <= 1 BLAS dispatch per frame
    {
        auto av = build_avatar(220u);
        REQUIRE(renderer.bind_avatar(av.get()).has_value());

        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = rhi->graphics_family();
        REQUIRE(vkCreateCommandPool(rhi->device(), &cpci, nullptr, &pool) == VK_SUCCESS);
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        REQUIRE(vkAllocateCommandBuffers(rhi->device(), &cbai, &cmd) == VK_SUCCESS);

        for (uint64_t f = 0; f < 3u; ++f) {
            const uint64_t skin0 = renderer.graph().skin_dispatch_count();
            const uint64_t blas0 = renderer.graph().blas_update_count();

            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            REQUIRE(vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS);

            FrameRenderRequest req{};
            req.avatar = av.get();
            req.rig = &renderer.lighting();
            req.camera = PreviewCamera{0.0f, 1.25f, 0.0f, 0.1f, 0.05f, 1.8f, 0.55f};
            req.frame_index = f;
            req.frame_slot = static_cast<uint32_t>(f % 3u);
            req.rt_enabled = rhi->rt_supported();
            renderer.graph().record_critical_passes(cmd, req);

            for (auto slot :
                 {FrameRenderRequest::StageOutputSlot::MainStage, FrameRenderRequest::StageOutputSlot::QuadTop,
                  FrameRenderRequest::StageOutputSlot::QuadRight}) {
                FrameRenderRequest sreq = req;
                sreq.view_kind = RenderViewKind::LightingStage;
                sreq.stage_output_slot = slot;
                sreq.stage_viewport_w = 320u;
                sreq.stage_viewport_h = 180u;
                sreq.skip_skin_and_blas = true;
                renderer.graph().record_critical_passes(cmd, sreq);
            }
            REQUIRE(vkEndCommandBuffer(cmd) == VK_SUCCESS);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            REQUIRE(vkQueueSubmit(rhi->graphics_queue(), 1, &si, VK_NULL_HANDLE) == VK_SUCCESS);
            rhi->wait_idle();

            CHECK(renderer.graph().skin_dispatch_count() - skin0 == 1ULL);
            CHECK(renderer.graph().blas_update_count() - blas0 <= 1ULL);

            const auto& snap = renderer.graph().timing().resolve_frame(req.frame_slot);
            CHECK(snap.valid);
            CHECK(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::TransparentPass)] >= 0.0f);
            CHECK(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::LightingStage)] >= 0.0f);
        }

        vkDestroyCommandPool(rhi->device(), pool, nullptr);
        REQUIRE(renderer.bind_avatar(nullptr).has_value());
        CHECK_FALSE(renderer.graph().accel_struct().is_ready());
        av->destroy();
    }

    // 2) 10x sequential avatar load/bind/render/unbind/wait_idle cycles reclaim BLAS & VRAM cleanly
    auto av_a = build_avatar(210u);
    auto av_b = build_avatar(160u);
    uint64_t baseline_vram = 0;

    for (uint32_t cycle = 0; cycle < 10u; ++cycle) {
        GpuAvatar* cur = ((cycle & 1u) == 0u) ? av_a.get() : av_b.get();
        REQUIRE(renderer.bind_avatar(cur).has_value());
        renderer.graph().invalidate_temporal_history();

        FrameRenderRequest req{};
        req.avatar = cur;
        req.rig = &renderer.lighting();
        req.camera = PreviewCamera{0.0f, 1.25f, 0.0f, 0.1f, 0.05f, 1.8f, 0.55f};
        req.frame_index = static_cast<uint64_t>(cycle);
        req.completed_frame_index = (cycle > 0u) ? static_cast<uint64_t>(cycle - 1u) : 0ULL;
        req.frame_slot = cycle % 3u;
        req.rt_enabled = rhi->rt_supported();
        REQUIRE(renderer.render_headless_frame(req).has_value());

        rhi->wait_idle();
        REQUIRE(renderer.bind_avatar(nullptr).has_value());
        CHECK_FALSE(renderer.graph().accel_struct().is_ready());

        const auto vb = rhi->query_vram_budget();
        if (cycle == 1u) {
            baseline_vram = vb.usage_bytes;
        } else if (cycle == 9u && baseline_vram > 0ULL && vb.usage_bytes > baseline_vram) {
            // Allow <= 4 MiB driver allocator pooling jitter across 10 swap/unload cycles; BLAS/vertex must reclaim.
            CHECK((vb.usage_bytes - baseline_vram) <= (4ULL * 1024ULL * 1024ULL));
        }
    }

    CHECK(rhi->validation_error_count() == 0);
    av_a->destroy();
    av_b->destroy();
    renderer.destroy();
    rhi->destroy();
    aether_runtime_shutdown();
}
