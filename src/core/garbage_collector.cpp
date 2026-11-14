#include "garbage_collector.hpp"

#include <limits>

namespace aether {

GarbageCollector::~GarbageCollector() noexcept {
    (void)flush_all_epochs();
    for (auto& node : nodes_) {
        if (node.allocated) {
            if (node.finalizer != nullptr) {
                node.finalizer(node.payload, node.user_tag, node.byte_size);
            }
            node = GraphNode{};
        }
    }
    live_node_count_ = 0;
}

bool GarbageCollector::retire(void* handle, uint64_t byte_size, uint64_t retire_epoch, GcFinalizerFn finalizer,
                              uint64_t user_tag) noexcept {
    RetiredResource item{};
    item.handle = handle;
    item.user_tag = user_tag;
    item.byte_size = byte_size;
    item.retire_epoch = retire_epoch;
    item.finalizer = finalizer;
    return retire_ring_.try_push(item);
}

void GarbageCollector::drain_retired_ring() noexcept {
    while (pending_retired_count_ < pending_retired_.size()) {
        RetiredResource item{};
        if (!retire_ring_.try_pop(item)) {
            break;
        }
        pending_retired_[pending_retired_count_] = item;
        ++pending_retired_count_;
    }
}

uint32_t GarbageCollector::collect_epoch(uint64_t completed_epoch, uint32_t max_reclaim) noexcept {
    drain_retired_ring();
    if (max_reclaim == 0U || pending_retired_count_ == 0U) {
        return 0U;
    }

    uint32_t reclaimed = 0U;
    size_t write_idx = 0U;
    for (size_t read_idx = 0U; read_idx < pending_retired_count_; ++read_idx) {
        const RetiredResource item = pending_retired_[read_idx];
        if (reclaimed < max_reclaim && item.retire_epoch <= completed_epoch) {
            if (item.finalizer != nullptr) {
                item.finalizer(item.handle, item.user_tag, item.byte_size);
            }
            ++reclaimed;
            ++retired_reclaimed_total_;
            bytes_reclaimed_total_ += item.byte_size;
        } else {
            pending_retired_[write_idx] = item;
            ++write_idx;
        }
    }
    pending_retired_count_ = write_idx;
    drain_retired_ring();
    return reclaimed;
}

uint32_t GarbageCollector::flush_all_epochs() noexcept {
    uint32_t total = 0U;
    for (;;) {
        const uint32_t step =
            collect_epoch(std::numeric_limits<uint64_t>::max(), static_cast<uint32_t>(kMaxRetiredQueue));
        if (step == 0U) {
            break;
        }
        total += step;
    }
    return total;
}

uint32_t GarbageCollector::alloc_node(void* payload, uint64_t byte_size, GcFinalizerFn finalizer, bool is_root,
                                      uint64_t user_tag) noexcept {
    for (size_t i = 0U; i < nodes_.size(); ++i) {
        if (!nodes_[i].allocated) {
            GraphNode& n = nodes_[i];
            n = GraphNode{};
            n.allocated = true;
            n.is_root = is_root;
            n.color = GcColor::White;
            n.payload = payload;
            n.user_tag = user_tag;
            n.byte_size = byte_size;
            n.finalizer = finalizer;
            ++live_node_count_;
            return static_cast<uint32_t>(i);
        }
    }
    return kInvalidNode;
}

bool GarbageCollector::set_root(uint32_t node_id, bool is_root) noexcept {
    if (node_id >= nodes_.size() || !nodes_[node_id].allocated) {
        return false;
    }
    nodes_[node_id].is_root = is_root;
    if (mark_in_progress_ && is_root && nodes_[node_id].color == GcColor::White) {
        push_gray(node_id);
    }
    return true;
}

bool GarbageCollector::add_edge(uint32_t from_id, uint32_t to_id) noexcept {
    if (from_id >= nodes_.size() || to_id >= nodes_.size()) {
        return false;
    }
    GraphNode& src = nodes_[from_id];
    const GraphNode& dst = nodes_[to_id];
    if (!src.allocated || !dst.allocated) {
        return false;
    }
    for (uint8_t i = 0U; i < src.edge_count; ++i) {
        if (src.edges[i] == to_id) {
            return true;
        }
    }
    if (src.edge_count >= kMaxEdgesPerNode) {
        return false;
    }
    src.edges[src.edge_count] = to_id;
    ++src.edge_count;

    // Dijkstra write barrier: if mutator links a Black node to a White node during incremental marking,
    // shade the target Gray so it cannot be missed by the current mark cycle.
    if (mark_in_progress_ && src.color == GcColor::Black && nodes_[to_id].color == GcColor::White) {
        push_gray(to_id);
    }
    return true;
}

bool GarbageCollector::remove_edge(uint32_t from_id, uint32_t to_id) noexcept {
    if (from_id >= nodes_.size() || !nodes_[from_id].allocated) {
        return false;
    }
    GraphNode& src = nodes_[from_id];
    for (uint8_t i = 0U; i < src.edge_count; ++i) {
        if (src.edges[i] == to_id) {
            const uint8_t last = static_cast<uint8_t>(src.edge_count - 1U);
            src.edges[i] = src.edges[last];
            src.edges[last] = 0U;
            src.edge_count = last;
            return true;
        }
    }
    return false;
}

void GarbageCollector::push_gray(uint32_t node_id) noexcept {
    if (node_id >= nodes_.size() || !nodes_[node_id].allocated) {
        return;
    }
    if (nodes_[node_id].color != GcColor::White) {
        return;
    }
    nodes_[node_id].color = GcColor::Gray;
    if (gray_count_ < gray_stack_.size()) {
        gray_stack_[gray_count_] = node_id;
        ++gray_count_;
    }
}

void GarbageCollector::begin_mark() noexcept {
    gray_count_ = 0U;
    mark_in_progress_ = true;
    for (size_t i = 0U; i < nodes_.size(); ++i) {
        if (nodes_[i].allocated) {
            nodes_[i].color = GcColor::White;
        }
    }
    for (size_t i = 0U; i < nodes_.size(); ++i) {
        if (nodes_[i].allocated && nodes_[i].is_root) {
            push_gray(static_cast<uint32_t>(i));
        }
    }
}

bool GarbageCollector::mark_step(uint32_t budget_nodes) noexcept {
    if (!mark_in_progress_) {
        begin_mark();
    }
    uint32_t processed = 0U;
    while (gray_count_ > 0U && processed < budget_nodes) {
        --gray_count_;
        const uint32_t current_id = gray_stack_[gray_count_];
        GraphNode& current = nodes_[current_id];
        if (!current.allocated) {
            continue;
        }
        for (uint8_t i = 0U; i < current.edge_count; ++i) {
            const uint32_t child_id = current.edges[i];
            if (child_id < nodes_.size() && nodes_[child_id].allocated && nodes_[child_id].color == GcColor::White) {
                push_gray(child_id);
            }
        }
        current.color = GcColor::Black;
        ++processed;
    }
    return gray_count_ == 0U;
}

uint32_t GarbageCollector::sweep() noexcept {
    while (gray_count_ > 0U) {
        (void)mark_step(static_cast<uint32_t>(kMaxNodes));
    }

    uint32_t swept = 0U;
    for (size_t i = 0U; i < nodes_.size(); ++i) {
        GraphNode& n = nodes_[i];
        if (!n.allocated) {
            continue;
        }
        if (n.color == GcColor::White) {
            if (n.finalizer != nullptr) {
                n.finalizer(n.payload, n.user_tag, n.byte_size);
            }
            bytes_reclaimed_total_ += n.byte_size;
            n = GraphNode{};
            if (live_node_count_ > 0U) {
                --live_node_count_;
            }
            ++swept;
            ++graph_collected_total_;
        } else {
            n.color = GcColor::White;
        }
    }
    mark_in_progress_ = false;
    ++cycles_completed_;
    return swept;
}

uint32_t GarbageCollector::collect_full(uint64_t completed_epoch, uint32_t max_epoch_reclaim) noexcept {
    const uint32_t epoch_reclaimed = collect_epoch(completed_epoch, max_epoch_reclaim);
    begin_mark();
    (void)mark_step(static_cast<uint32_t>(kMaxNodes));
    const uint32_t graph_swept = sweep();
    return epoch_reclaimed + graph_swept;
}

bool GarbageCollector::is_node_alive(uint32_t node_id) const noexcept {
    return node_id < nodes_.size() && nodes_[node_id].allocated;
}

GcColor GarbageCollector::node_color(uint32_t node_id) const noexcept {
    if (node_id >= nodes_.size() || !nodes_[node_id].allocated) {
        return GcColor::White;
    }
    return nodes_[node_id].color;
}

GcStats GarbageCollector::stats() const noexcept {
    GcStats out{};
    out.retired_pending = pending_retired_count_ + retire_ring_.size_approx();
    out.retired_reclaimed_total = retired_reclaimed_total_;
    out.bytes_reclaimed_total = bytes_reclaimed_total_;
    out.live_graph_objects = live_node_count_;
    out.graph_collected_total = graph_collected_total_;
    out.cycles_completed = cycles_completed_;
    return out;
}

} // namespace aether
