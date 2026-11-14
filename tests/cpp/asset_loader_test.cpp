#include "aether_ffi_contract.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

std::filesystem::path data_root() {
    const std::filesystem::path repo_root(AETHER_REPO_ROOT);
    return repo_root / "tests" / "data";
}

struct LoadedCxxArena {
    AetherAssetRequirements reqs{};
    AetherAvatarDesc desc{};
    std::array<std::vector<uint8_t>, AETHER_STREAM_COUNT> buffers;
    std::array<uint64_t, AETHER_STREAM_COUNT> lent_ptrs{};
};

AetherStatus load_model_two_phase(const std::filesystem::path& path, uint32_t flags, LoadedCxxArena& out) {
    const std::string path_u8 = path.string();
    AetherLoadRequest req{};
    req.path_utf8 = path_u8.c_str();
    req.flags = flags;
    req.max_threads = 0;

    AetherLoaderHandle loader = nullptr;
    AetherStatus rc = aether_loader_open(&req, &loader);
    if (rc != AETHER_OK || loader == nullptr) {
        return rc != AETHER_OK ? rc : AETHER_E_INTERNAL;
    }

    rc = aether_loader_query(loader, &out.reqs);
    if (rc != AETHER_OK) {
        aether_loader_close(loader);
        return rc;
    }

    AetherArenaSlice slices[AETHER_STREAM_COUNT]{};
    for (uint32_t s = 0; s < AETHER_STREAM_COUNT; ++s) {
        slices[s].stream_kind = s;
        slices[s].size_bytes = out.reqs.stream_bytes[s];
        if (out.reqs.stream_bytes[s] > 0) {
            // Allocate 64-byte aligned arena storage cleanly.
            out.buffers[s].resize(static_cast<size_t>(out.reqs.stream_bytes[s]) + 64U);
            uintptr_t raw_addr = reinterpret_cast<uintptr_t>(out.buffers[s].data());
            uintptr_t aligned_addr = (raw_addr + 63ULL) & ~63ULL;
            slices[s].ptr = reinterpret_cast<uint8_t*>(aligned_addr);
            out.lent_ptrs[s] = static_cast<uint64_t>(aligned_addr);
        } else {
            slices[s].ptr = nullptr;
            out.lent_ptrs[s] = 0;
        }
    }

    rc = aether_loader_fill(loader, slices, AETHER_STREAM_COUNT, &out.desc);
    aether_loader_close(loader);
    return rc;
}

uint32_t count_mapped_humanoid(const AetherAvatarDesc& desc) {
    uint32_t count = 0;
    for (uint16_t j : desc.humanoid_to_joint) {
        if (j != 0xFFFFU) {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_CASE("C++ two-phase loader loads reference avatar and corpus with zero-copy identity", "[phase2][loader]") {
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    aether_set_thread_role(AETHER_THREAD_ROLE_LOADER);

    const uint32_t default_flags = AETHER_LOAD_FLAG_BAKE_PROXY_NORMALS | AETHER_LOAD_FLAG_BAKE_SMOOTH_TANGENTS |
                                   AETHER_LOAD_FLAG_MORTON_SORT | AETHER_LOAD_FLAG_DECODE_TEXTURES |
                                   AETHER_LOAD_FLAG_TEXTURE_CACHE;

    {
        LoadedCxxArena ref_out;
        const auto ref_path = data_root() / "reference_avatar.vrm";
        REQUIRE(std::filesystem::is_regular_file(ref_path));
        REQUIRE(load_model_two_phase(ref_path, default_flags, ref_out) == AETHER_OK);
        REQUIRE(ref_out.reqs.vertex_count > 30000U);
        REQUIRE(ref_out.reqs.index_count / 3U >= 65000U);
        REQUIRE(ref_out.reqs.texture_count >= 10U);
        REQUIRE(count_mapped_humanoid(ref_out.desc) == AETHER_HUMANOID_BONE_COUNT);

        for (uint32_t s = 0; s < AETHER_STREAM_COUNT; ++s) {
            if (ref_out.reqs.stream_bytes[s] > 0) {
                CHECK(ref_out.desc.debug_stream_ptrs[s] == ref_out.lent_ptrs[s]);
            }
        }
    }

    {
        const auto corpus_dir = data_root() / "corpus";
        REQUIRE(std::filesystem::is_directory(corpus_dir));
        size_t corpus_count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(corpus_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            ++corpus_count;
            LoadedCxxArena arena;
            INFO("Corpus model: " << entry.path().filename().string());
            REQUIRE(load_model_two_phase(entry.path(), default_flags, arena) == AETHER_OK);
            CHECK(arena.reqs.vertex_count > 0U);
            CHECK(arena.reqs.index_count >= 3U);
            const uint32_t mapped = count_mapped_humanoid(arena.desc);
            CHECK(mapped >= 53U); // >= 95% of 55 humanoid slots
            for (uint32_t s = 0; s < AETHER_STREAM_COUNT; ++s) {
                if (arena.reqs.stream_bytes[s] > 0) {
                    CHECK(arena.desc.debug_stream_ptrs[s] == arena.lent_ptrs[s]);
                }
            }
        }
        REQUIRE(corpus_count == 20U);
    }

    {
        LoadedCxxArena err_out;
        REQUIRE(load_model_two_phase(data_root() / "edge_cases" / "draco_rejected.glb", default_flags, err_out) ==
                AETHER_E_UNSUPPORTED);
        REQUIRE(load_model_two_phase(data_root() / "edge_cases" / "limit_bones_1025.pmx", default_flags, err_out) ==
                AETHER_E_LIMIT_EXCEEDED);
        REQUIRE(load_model_two_phase(data_root() / "edge_cases" / "meshopt_quad.glb", default_flags, err_out) ==
                AETHER_OK);
        CHECK(err_out.reqs.vertex_count == 8U);
        CHECK(err_out.reqs.index_count == 6U);
    }

    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    aether_runtime_shutdown();
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
}
