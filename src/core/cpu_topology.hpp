#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aether {

enum class ThreadRole : uint8_t {
    MainT0 = 0,
    TrackingT1 = 1,
    SimT2 = 2,
    RenderT3 = 3,
    OutputT4 = 4,
    AssetLoader = 5,
    Background = 6,
};

struct CpuCoreInfo {
    uint32_t physical_index{0};
    uint8_t efficiency_class{0}; // Higher values are higher performance (P-cores on Intel hybrid)
    uint32_t logical_threads{1};
    uint64_t affinity_mask{0};
    bool is_p_core{true};
};

struct CpuTopology {
    std::string brand_string;
    uint32_t physical_cores{0};
    uint32_t logical_processors{0};
    uint32_t p_core_count{0};
    uint32_t e_core_count{0};
    uint32_t p_logical_count{0};
    uint32_t e_logical_count{0};
    bool is_hybrid{false};
    std::vector<CpuCoreInfo> cores;
    std::vector<uint32_t> p_cpu_set_ids;
    std::vector<uint32_t> e_cpu_set_ids;
};

struct ThreadPlacementResult {
    ThreadRole role{ThreadRole::MainT0};
    bool cpu_set_applied{false};
    bool priority_applied{false};
    bool mmcss_applied{false};
    uint32_t assigned_cpu_sets{0};
    const char* policy_summary{"default"};
};

/** Queries CPU brand, physical/logical counts, P/E efficiency classes, and CPU Set IDs (`§4.2`). */
[[nodiscard]] CpuTopology detect_cpu_topology();

/** Applies thread placement (P-core/E-core soft CPU set, priority, name) for `role` (`§4.2`). */
[[nodiscard]] ThreadPlacementResult apply_thread_placement(ThreadRole role, const CpuTopology& topology) noexcept;

/** Returns display name for `ThreadRole`. */
[[nodiscard]] const char* thread_role_name(ThreadRole role) noexcept;

} // namespace aether
