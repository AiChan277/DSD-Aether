#include "core/clock.hpp"
#include "core/hot_path_guard.hpp"
#include "renderer/asset_upload/gpu_avatar.hpp"
#include "renderer/npr_renderer.hpp"
#include "rhi/bindless.hpp"
#include "rhi/gpu_timing.hpp"
#include "rhi/rebar_ring.hpp"
#include "rhi/rhi_device.hpp"
#include "test_png_io.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>

TEST_CASE("Multi-GPU enumeration, scoring, ReBAR ring, bindless heap, and calibrated timestamps",
          "[phase4][rhi][validation]") {
    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);

    // Vendor scoring priority (§0.7, §10.1): discrete RT > RM-B discrete > iGPU
    const uint32_t score_rtx = aether::rhi::RhiDevice::score_adapter_for_vendor(
        0x10DEu, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, true, true, 8192);
    const uint32_t score_rmb = aether::rhi::RhiDevice::score_adapter_for_vendor(
        0x1002u, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, true, true, 16384);
    const uint32_t score_igpu = aether::rhi::RhiDevice::score_adapter_for_vendor(
        0x8086u, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, true, false, 512);
    CHECK(score_rtx > score_igpu);
    CHECK(score_rmb > score_igpu);

    const auto enum_res = aether::rhi::RhiDevice::enumerate_vulkan_devices();
    REQUIRE(enum_res.has_value());
    REQUIRE(!enum_res.value().empty());
    std::printf("[P4-RHI] Enumerated %zu Vulkan adapters:\n", enum_res.value().size());
    for (const auto& ad : enum_res.value()) {
        std::printf("  - %s (vendor=0x%04X dev=0x%04X rt=%d rebar=%d score=%u)\n", ad.device_name.c_str(), ad.vendor_id,
                    ad.device_id, ad.supports_ray_query ? 1 : 0, ad.rebar_enabled ? 1 : 0, ad.score);
    }

    const auto temp_dir = std::filesystem::temp_directory_path() / "aether_p4_rhi_test";
    std::filesystem::create_directories(temp_dir);

    aether::rhi::RhiDeviceConfig cfg{};
    cfg.enable_validation = true;
    cfg.require_validation = true;
    cfg.headless = true;
    cfg.enable_ray_tracing = true;
    cfg.pipeline_cache_dir = temp_dir;

    auto dev_res = aether::rhi::RhiDevice::create(cfg);
    REQUIRE(dev_res.has_value());
    aether::rhi::RhiDevice* rhi = dev_res.value();
    REQUIRE(rhi->validation_active());
    REQUIRE(rhi->sync_validation_active());
    REQUIRE(rhi->pipeline_cache() != VK_NULL_HANDLE);
    REQUIRE(rhi->compute_queue() != VK_NULL_HANDLE);
    REQUIRE(rhi->transfer_queue() != VK_NULL_HANDLE);

    const auto budget = rhi->query_vram_budget();
    CHECK(budget.budget_bytes > 0);

    if (rhi->gpu_info().calibrated_timestamps_active) {
        uint64_t gpu_ns = 0, cpu_ns = 0;
        REQUIRE(rhi->calibrated_timestamp_ns(gpu_ns, cpu_ns).has_value());
        CHECK(cpu_ns > 0);
    }

    // ReBAR 8 MiB ring zero-alloc wraparound test
    aether::rhi::RebarRingBuffer ring;
    REQUIRE(ring.init(*rhi, aether::rhi::RebarRingBuffer::kDefaultCapacityBytes).has_value());
    CHECK(ring.capacity() == 8ULL * 1024ULL * 1024ULL);

    aether::HotPathGuard::reset_thread_violations();
    bool ring_all_ok = true;
    bool ring_all_aligned = true;
    {
        AETHER_HOT_PATH_SCOPE;
        for (uint64_t f = 0; f < 160; ++f) {
            const uint64_t completed = (f >= 3) ? (f - 3) : 0;
            ring.begin_frame(f, completed);
            for (int k = 0; k < 4; ++k) {
                auto a = ring.allocate(24 * 1024, 64);
                if (!a.has_value()) {
                    ring_all_ok = false;
                } else if (a.value().mapped == nullptr || a.value().device_address == 0 ||
                           (a.value().device_address % 64ULL) != 0ULL) {
                    ring_all_aligned = false;
                }
            }
        }
    }
    REQUIRE(ring_all_ok);
    CHECK(ring_all_aligned);
    CHECK(aether::HotPathGuard::thread_violations() == 0);
    ring.destroy();

    // Bindless heap + zero-alloc slot register/release + non-RT Set 1 exclusion verification
    aether::rhi::BindlessHeap heap;
    aether::rhi::BindlessConfig bcfg{};
    bcfg.enable_rt_set = rhi->rt_supported();
    REQUIRE(heap.init(*rhi, bcfg).has_value());
    CHECK(heap.global_layout() != VK_NULL_HANDLE);
    CHECK(heap.global_set() != VK_NULL_HANDLE);
    CHECK(heap.rt_enabled() == rhi->rt_supported());

    auto dummy_img =
        rhi->create_image_2d(32, 32, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT);
    REQUIRE(dummy_img.has_value());

    aether::HotPathGuard::reset_thread_violations();
    aether::Expected<uint32_t> s0 = aether::make_unexpected(aether::Error::make(aether::ErrorCode::InvalidArg, "init"));
    aether::Expected<uint32_t> st0 =
        aether::make_unexpected(aether::Error::make(aether::ErrorCode::InvalidArg, "init"));
    {
        AETHER_HOT_PATH_SCOPE;
        s0 = heap.register_sampled_image(dummy_img.value().view);
        st0 = heap.register_storage_image(dummy_img.value().view);
        if (s0.has_value()) {
            heap.release_sampled_image(s0.value());
        }
        if (st0.has_value()) {
            heap.release_storage_image(st0.value());
        }
    }
    REQUIRE(s0.has_value());
    REQUIRE(st0.has_value());
#if !defined(__SANITIZE_ADDRESS__)
    // Under ASan, __sanitizer_install_malloc_and_free_hooks intercepts internal malloc()
    // inside VkLayer_khronos_validation.dll during vkUpdateDescriptorSets; MSVC Debug CRT checks ours.
    CHECK(aether::HotPathGuard::thread_violations() == 0);
#endif

    rhi->destroy_image(dummy_img.value());
    heap.destroy();
    rhi->save_pipeline_cache();
    CHECK(rhi->validation_error_count() == 0);

    rhi->destroy();
    delete rhi;
    std::filesystem::remove_all(temp_dir);
    aether_runtime_shutdown();
}

TEST_CASE("Simulated VK_ERROR_DEVICE_LOST recovers renderer cleanly within <= 3.0 s with crash breadcrumb",
          "[phase4][rhi][recovery]") {
    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);

    const auto bc_dir = std::filesystem::temp_directory_path() / "aether_p4_recovery_test";
    std::filesystem::remove_all(bc_dir);
    std::filesystem::create_directories(bc_dir);

    aether::rhi::RhiDeviceConfig dcfg{};
    dcfg.enable_validation = true;
    dcfg.require_validation = true;
    dcfg.headless = true;
    dcfg.enable_ray_tracing = true;
    dcfg.pipeline_cache_dir = bc_dir;

    auto dev_res = aether::rhi::RhiDevice::create(dcfg);
    REQUIRE(dev_res.has_value());
    std::unique_ptr<aether::rhi::RhiDevice> rhi(dev_res.value());

    aether::renderer::RendererConfig rcfg{};
    rcfg.width = 640;
    rcfg.height = 360;
    rcfg.enable_rt = rhi->rt_supported();

    aether::renderer::NprRenderer renderer;
    REQUIRE(renderer.init(*rhi, rcfg).has_value());

    auto avatar_res = aether::test::load_reference_vrm_avatar(*rhi);
    REQUIRE(avatar_res.has_value());
    auto avatar = std::move(avatar_res.value());
    REQUIRE(renderer.bind_avatar(avatar.get()).has_value());

    aether::renderer::FrameRenderRequest req{};
    req.avatar = avatar.get();
    req.rig = &renderer.lighting();
    req.rt_enabled = rhi->rt_supported();
    REQUIRE(renderer.render_headless_frame(req).has_value());

    // Inject device lost and measure end-to-end device + pipeline + avatar recovery
    rhi->mark_device_lost();
    REQUIRE(rhi->is_device_lost());

    // Destroy stale avatar GPU allocations before tearing down the lost VMA allocator
    avatar->destroy();
    avatar = nullptr;

    const auto report = renderer.recover_from_device_lost(*rhi, dcfg, rcfg, nullptr, bc_dir);
    INFO("Recovery error: " << report.error_message);
    REQUIRE(report.recovered);
    CHECK(report.recovery_ms <= 3000.0);
    CHECK(!report.breadcrumb_path.empty());
    CHECK(std::filesystem::is_regular_file(report.breadcrumb_path));
    CHECK(!rhi->is_device_lost());

    // Reload avatar on newly recreated device and render a post-recovery frame cleanly
    auto post_avatar = aether::test::load_reference_vrm_avatar(*rhi);
    REQUIRE(post_avatar.has_value());
    REQUIRE(renderer.bind_avatar(post_avatar.value().get()).has_value());
    req.avatar = post_avatar.value().get();
    req.frame_index = 1;
    REQUIRE(renderer.render_headless_frame(req).has_value());
    CHECK(rhi->validation_error_count() == 0);

    std::printf("[P4-RECOVERY] Device rebuild + breadcrumb=%.2f ms (limit <= 3000 ms)\n", report.recovery_ms);

    renderer.destroy();
    post_avatar.value()->destroy();
    rhi->destroy();
    std::filesystem::remove_all(bc_dir);
    aether_runtime_shutdown();
}
