#include <array>
#include <cstdint>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/garbage_collector.hpp"
#include "core/hot_path_guard.hpp"
#include "renderer/dx11_renderer.hpp"

namespace {

struct ReclaimCounter {
    uint32_t count{0};
    uint64_t bytes{0};
    uint64_t last_tag{0};
};

void record_reclaim(void* handle, uint64_t user_tag, uint64_t byte_size) noexcept {
    if (handle != nullptr) {
        auto* counter = static_cast<ReclaimCounter*>(handle);
        ++counter->count;
        counter->bytes += byte_size;
        counter->last_tag = user_tag;
    }
}

} // namespace

TEST_CASE("GarbageCollector epoch-fenced retirement and incremental tri-color mark-sweep are allocation-free",
          "[gc][core]") {
    aether::HotPathGuard::install();
    aether::HotPathGuard::reset_thread_violations();

    aether::GarbageCollector gc{};
    ReclaimCounter counter{};

    {
        aether::HotPathScope guard{};

        // 1. Epoch-fenced retirement
        REQUIRE(gc.retire(&counter, 1024, 10, &record_reclaim, 101));
        REQUIRE(gc.retire(&counter, 2048, 12, &record_reclaim, 102));
        REQUIRE(gc.retire(&counter, 4096, 15, &record_reclaim, 103));

        CHECK(gc.collect_epoch(9) == 0);
        CHECK(counter.count == 0);

        CHECK(gc.collect_epoch(12) == 2);
        CHECK(counter.count == 2);
        CHECK(counter.bytes == 3072);
        CHECK(counter.last_tag == 102);

        // 2. Tri-color mark-and-sweep with reachable chain and unreachable reference cycle
        const uint32_t root = gc.alloc_node(&counter, 512, &record_reclaim, true, 1);
        const uint32_t child = gc.alloc_node(&counter, 512, &record_reclaim, false, 2);
        const uint32_t cycle_a = gc.alloc_node(&counter, 1000, &record_reclaim, false, 3);
        const uint32_t cycle_b = gc.alloc_node(&counter, 2000, &record_reclaim, false, 4);

        REQUIRE(root != aether::GarbageCollector::kInvalidNode);
        REQUIRE(child != aether::GarbageCollector::kInvalidNode);
        REQUIRE(cycle_a != aether::GarbageCollector::kInvalidNode);
        REQUIRE(cycle_b != aether::GarbageCollector::kInvalidNode);

        REQUIRE(gc.add_edge(root, child));
        // Detached reference cycle: cycle_a <-> cycle_b
        REQUIRE(gc.add_edge(cycle_a, cycle_b));
        REQUIRE(gc.add_edge(cycle_b, cycle_a));

        gc.begin_mark();
        CHECK(gc.node_color(root) == aether::GcColor::Gray);
        CHECK(gc.mark_step(1) == false); // Scans root -> colors child Gray
        CHECK(gc.mark_step(8) == true);  // Finishes marking reachable subgraph

        const uint32_t swept = gc.sweep();
        CHECK(swept == 2); // cycle_a and cycle_b collected
        CHECK(gc.is_node_alive(root));
        CHECK(gc.is_node_alive(child));
        CHECK_FALSE(gc.is_node_alive(cycle_a));
        CHECK_FALSE(gc.is_node_alive(cycle_b));

        // Unroot `root` and run collect_full(epoch=20) -> reclaims remaining epoch 15 + both graph nodes
        REQUIRE(gc.set_root(root, false));
        const uint32_t total_reclaimed = gc.collect_full(20);
        CHECK(total_reclaimed == 3);
        CHECK(gc.stats().live_graph_objects == 0);
        CHECK(gc.stats().retired_pending == 0);
        CHECK(gc.stats().bytes_reclaimed_total == (1024 + 2048 + 4096 + 1000 + 2000 + 512 + 512));
    }

    CHECK(aether::HotPathGuard::thread_violations() == 0);
}

TEST_CASE("Dx11NprRenderer compiles SM5.0 2.5D NPR shaders and renders 64B AetherVertex25D mesh", "[dx11][renderer]") {
    aether::renderer::Dx11NprRenderer dx11{};
    aether::renderer::Dx11RendererConfig cfg{};
    cfg.width = 128;
    cfg.height = 128;
    cfg.max_vertices = 256;
    cfg.max_indices = 512;
    cfg.allow_warp_fallback = true;
    cfg.enable_shared_texture = true;

    const auto init_res = dx11.init(cfg);
    REQUIRE(init_res.has_value());
    CHECK(dx11.is_initialized());
    CHECK(dx11.width() == 128);
    CHECK(dx11.height() == 128);
    CHECK(dx11.device() != nullptr);
    CHECK(dx11.color_target() != nullptr);

    // Full-viewport quad in clip space using the frozen 64-byte AetherVertex25D layout
    std::array<AetherVertex25D, 4> verts{};
    verts[0].position[0] = -0.8f;
    verts[0].position[1] = -0.8f;
    verts[0].position[2] = 0.5f;
    verts[1].position[0] = -0.8f;
    verts[1].position[1] = 0.8f;
    verts[1].position[2] = 0.5f;
    verts[2].position[0] = 0.8f;
    verts[2].position[1] = 0.8f;
    verts[2].position[2] = 0.5f;
    verts[3].position[0] = 0.8f;
    verts[3].position[1] = -0.8f;
    verts[3].position[2] = 0.5f;

    for (auto& v : verts) {
        v.outline_width_scale = 1.0f;
        v.true_normal[0] = 0.0f;
        v.true_normal[1] = 0.0f;
        v.true_normal[2] = -1.0f;
        v.z_flatten_weight = 0.5f;
        v.proxy_normal[0] = 0.0f;
        v.proxy_normal[1] = 0.2f;
        v.proxy_normal[2] = -0.98f;
        v.sdf_face_mask = 1.0f;
        v.smooth_tangent[0] = 0.0f;
        v.smooth_tangent[1] = 0.0f;
        v.smooth_tangent[2] = -1.0f;
        v.smooth_tangent[3] = 1.0f;
    }

    constexpr std::array<uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    aether::renderer::Dx11NprFrameParams params{};
    params.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
    params.lit_color = {0.90f, 0.80f, 0.75f, 1.0f};

    REQUIRE(dx11.render_npr_mesh(verts, indices, params));
    REQUIRE(dx11.wait_for_gpu(1000));
    CHECK(dx11.rendered_frames() == 1);

    std::vector<uint8_t> rgba(static_cast<size_t>(cfg.width) * static_cast<size_t>(cfg.height) * 4U, 0U);
    REQUIRE(dx11.readback_rgba8(rgba));

    // Center pixel (64, 64) must be lit and opaque (non-zero RGBA); corner pixel (2, 2) must be clear (0, 0, 0, 0)
    const size_t center_off = (64U * 128U + 64U) * 4U;
    const size_t corner_off = (2U * 128U + 2U) * 4U;
    CHECK(rgba[center_off + 0] > 100);
    CHECK(rgba[center_off + 1] > 80);
    CHECK(rgba[center_off + 2] > 80);
    CHECK(rgba[center_off + 3] == 255);

    CHECK(rgba[corner_off + 0] == 0);
    CHECK(rgba[corner_off + 1] == 0);
    CHECK(rgba[corner_off + 2] == 0);
    CHECK(rgba[corner_off + 3] == 0);

    REQUIRE(dx11.resize(64, 64).has_value());
    CHECK(dx11.width() == 64);
    CHECK(dx11.height() == 64);

    dx11.shutdown();
    CHECK_FALSE(dx11.is_initialized());
}
