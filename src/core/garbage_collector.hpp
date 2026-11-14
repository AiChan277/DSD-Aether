#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "spsc_ring.hpp"

namespace aether {

/** Tri-color marking states for incremental mark-and-sweep collection. */
enum class GcColor : uint8_t {
    White = 0, // Condemned / unvisited
    Gray = 1,  // Discovered, outgoing references not yet scanned
    Black = 2, // Reachable and fully scanned
};

/** Non-throwing reclamation callback invoked when a retired resource or unreachable node is collected. */
using GcFinalizerFn = void (*)(void* handle, uint64_t user_tag, uint64_t byte_size) noexcept;

/** Deferred epoch-fenced resource retirement descriptor (`HR-01`, `HR-02`). */
struct RetiredResource {
    void* handle{nullptr};
    uint64_t user_tag{0};
    uint64_t byte_size{0};
    uint64_t retire_epoch{0};
    GcFinalizerFn finalizer{nullptr};
};

/** Snapshot of Garbage Collector telemetry counters. */
struct GcStats {
    uint64_t retired_pending{0};
    uint64_t retired_reclaimed_total{0};
    uint64_t bytes_reclaimed_total{0};
    uint32_t live_graph_objects{0};
    uint64_t graph_collected_total{0};
    uint64_t cycles_completed{0};
};

/**
 * Deterministic C++20 Engine Garbage Collector (`HR-01`, `HR-02`, ADR-0015).
 *
 * Combines:
 * 1. A wait-free SPSC epoch-fenced deferred resource collector for GPU/engine handles retired on
 *    hot-path threads (`T1–T4`) with zero heap allocations (`HotPathGuard`-safe).
 * 2. A fixed-slab incremental tri-color (`White`/`Gray`/`Black`) mark-and-sweep collector that
 *    reclaims unreachable object subgraphs and reference cycles without dynamic memory allocation.
 */
class GarbageCollector {
  public:
    static constexpr uint32_t kInvalidNode = 0xFFFFFFFFU;
    static constexpr size_t kMaxRetiredQueue = 512;
    static constexpr size_t kMaxNodes = 256;
    static constexpr size_t kMaxEdgesPerNode = 8;

    GarbageCollector() noexcept = default;
    ~GarbageCollector() noexcept;

    GarbageCollector(const GarbageCollector&) = delete;
    GarbageCollector& operator=(const GarbageCollector&) = delete;

    /**
     * Enqueues a resource for deferred reclamation once `completed_epoch >= retire_epoch`.
     * Wait-free and allocation-free (`HR-01`, `HR-02`).
     */
    [[nodiscard]] bool retire(void* handle, uint64_t byte_size, uint64_t retire_epoch, GcFinalizerFn finalizer,
                              uint64_t user_tag = 0) noexcept;

    /**
     * Drains the wait-free retirement ring and reclaims up to `max_reclaim` resources whose
     * `retire_epoch <= completed_epoch`. Returns the number of resources reclaimed in this call.
     */
    uint32_t collect_epoch(uint64_t completed_epoch, uint32_t max_reclaim = 64) noexcept;

    /** Reclaims all pending epoch-retired resources unconditionally (used at engine shutdown). */
    uint32_t flush_all_epochs() noexcept;

    /**
     * Allocates a managed graph node from the fixed slab (`0` heap allocations).
     * Returns `kInvalidNode` if the slab is full.
     */
    [[nodiscard]] uint32_t alloc_node(void* payload, uint64_t byte_size, GcFinalizerFn finalizer, bool is_root = false,
                                      uint64_t user_tag = 0) noexcept;

    /** Marks or unmarks a managed node as a GC root. */
    bool set_root(uint32_t node_id, bool is_root) noexcept;

    /** Adds a directed reference `from_id -> to_id` for tri-color traversal. */
    bool add_edge(uint32_t from_id, uint32_t to_id) noexcept;

    /** Removes a directed reference `from_id -> to_id`. */
    bool remove_edge(uint32_t from_id, uint32_t to_id) noexcept;

    /** Initializes a new tri-color mark phase, coloring roots `Gray` and non-roots `White`. */
    void begin_mark() noexcept;

    /**
     * Incremental tri-color mark step scanning up to `budget_nodes` gray objects.
     * Returns `true` when the gray worklist is empty and marking is complete.
     */
    bool mark_step(uint32_t budget_nodes = 16) noexcept;

    /**
     * Sweeps all unreachable (`White`) managed nodes, invoking their finalizers and freeing slab
     * slots, while resetting surviving (`Black`) nodes to `White`. Returns count of swept nodes.
     */
    uint32_t sweep() noexcept;

    /**
     * Runs both epoch-gated resource reclamation and a complete mark-and-sweep cycle.
     * Returns total items reclaimed (`collect_epoch` + `sweep`).
     */
    uint32_t collect_full(uint64_t completed_epoch, uint32_t max_epoch_reclaim = 256) noexcept;

    [[nodiscard]] bool is_node_alive(uint32_t node_id) const noexcept;
    [[nodiscard]] GcColor node_color(uint32_t node_id) const noexcept;
    [[nodiscard]] GcStats stats() const noexcept;

  private:
    struct GraphNode {
        bool allocated{false};
        bool is_root{false};
        GcColor color{GcColor::White};
        uint8_t edge_count{0};
        std::array<uint32_t, kMaxEdgesPerNode> edges{};
        void* payload{nullptr};
        uint64_t user_tag{0};
        uint64_t byte_size{0};
        GcFinalizerFn finalizer{nullptr};
    };

    void drain_retired_ring() noexcept;
    void push_gray(uint32_t node_id) noexcept;

    SpscRing<RetiredResource, kMaxRetiredQueue> retire_ring_{};
    std::array<RetiredResource, kMaxRetiredQueue> pending_retired_{};
    size_t pending_retired_count_{0};

    std::array<GraphNode, kMaxNodes> nodes_{};
    std::array<uint32_t, kMaxNodes> gray_stack_{};
    size_t gray_count_{0};
    bool mark_in_progress_{false};

    uint32_t live_node_count_{0};
    uint64_t retired_reclaimed_total_{0};
    uint64_t bytes_reclaimed_total_{0};
    uint64_t graph_collected_total_{0};
    uint64_t cycles_completed_{0};
};

} // namespace aether
