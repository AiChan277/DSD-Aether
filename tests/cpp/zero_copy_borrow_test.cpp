#include "aether_ffi_contract.h"
#include "aether_test_hooks.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

TEST_CASE("Two-phase C++ arena zero-copy borrow and zero-alloc graph eval", "[ffi][zero-copy]") {
#ifndef AETHER_ENABLE_TEST_HOOKS
    SUCCEED("Test hooks disabled in this configuration");
#else
    constexpr uint32_t kVerts = 512;
    constexpr uint32_t kIndices = 1536;

    aether_set_thread_role(AETHER_THREAD_ROLE_LOADER);
    AetherLoaderHandle loader = nullptr;
    REQUIRE(aether_test_loader_open_synthetic(kVerts, kIndices, &loader) == AETHER_OK);
    REQUIRE(loader != nullptr);

    AetherAssetRequirements req{};
    REQUIRE(aether_loader_query(loader, &req) == AETHER_OK);
    REQUIRE(req.vertex_count == kVerts);
    REQUIRE(req.index_count == kIndices);
    REQUIRE(req.stream_bytes[AETHER_STREAM_VERTEX25D] == kVerts * sizeof(AetherVertex25D));

    std::vector<AetherVertex25D> v25(kVerts);
    std::vector<AetherVertexSurface> surf(kVerts);
    std::vector<AetherVertexSkin> skin(kVerts);
    std::vector<uint32_t> idx(kIndices);

    std::array<AetherArenaSlice, 4> slices{{
        {reinterpret_cast<uint8_t*>(v25.data()), req.stream_bytes[AETHER_STREAM_VERTEX25D], AETHER_STREAM_VERTEX25D, 0},
        {reinterpret_cast<uint8_t*>(surf.data()), req.stream_bytes[AETHER_STREAM_SURFACE], AETHER_STREAM_SURFACE, 0},
        {reinterpret_cast<uint8_t*>(skin.data()), req.stream_bytes[AETHER_STREAM_SKIN], AETHER_STREAM_SKIN, 0},
        {reinterpret_cast<uint8_t*>(idx.data()), req.stream_bytes[AETHER_STREAM_INDEX_U32], AETHER_STREAM_INDEX_U32, 0},
    }};

    AetherAvatarDesc desc{};

    // Undersized slice rejected cleanly
    auto short_slices = slices;
    short_slices[0].size_bytes -= 4;
    REQUIRE(aether_loader_fill(loader, short_slices.data(), 4, &desc) == AETHER_E_BUFFER_TOO_SMALL);

    // Overlapping slices rejected cleanly before mutable borrowing
    auto overlap_slices = slices;
    overlap_slices[1].ptr = slices[0].ptr + 64;
    REQUIRE(aether_loader_fill(loader, overlap_slices.data(), 4, &desc) == AETHER_E_INVALID_ARG);

    aether_test_alloc_reset();
    REQUIRE(aether_loader_fill(loader, slices.data(), 4, &desc) == AETHER_OK);

    // Zero Rust heap allocation during zero-copy fill & pointer identity preserved (REQ-FFI-010, HR-05).
    CHECK(aether_test_thread_alloc_calls() == 0);
    CHECK(aether_test_alloc_peak_bytes() < (1024ULL * 1024ULL));
    CHECK(aether_loader_progress(loader) == 1.0f);

    for (size_t i = 0; i < slices.size(); ++i) {
        CHECK(desc.debug_stream_ptrs[i] == reinterpret_cast<uint64_t>(slices[i].ptr));
    }
    CHECK(v25[kVerts - 1].position[0] > 0.0f);
    CHECK(idx[kIndices - 1] == ((kIndices - 1) % kVerts));

    aether_loader_close(loader);

    // Verify graph evaluate executes with zero heap allocation on T2.
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
    AetherPlanHandle plan = nullptr;
    REQUIRE(aether_test_graph_noop_plan(&plan) == AETHER_OK);
    REQUIRE(plan != nullptr);

    aether_set_thread_role(AETHER_THREAD_ROLE_T2);
    AetherTrackingFrame track{};
    track.arkit_blendshapes[3] = 0.8f;

    AetherGraphInputs inputs{};
    inputs.tracking = &track;
    inputs.now_us = 123456;
    inputs.dt_s = 1.0f / 240.0f;
    inputs.tick_index = 42;
    inputs.dsp_visemes[0] = 0.6f;

    AetherGraphOutputs outputs{};
    aether_test_alloc_reset();
    REQUIRE(aether_graph_evaluate(plan, &inputs, &outputs) == AETHER_OK);
    CHECK(aether_test_thread_alloc_calls() == 0);
    CHECK(outputs.expression_weights[3] > 0.79f);
    CHECK(outputs.expression_weights[52] > 0.59f);

    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
    aether_graph_release(plan);
#endif
}
