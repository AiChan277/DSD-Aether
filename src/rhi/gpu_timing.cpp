#include "rhi/gpu_timing.hpp"

#include <algorithm>
#include <cmath>

namespace aether::rhi {

GpuTimingRing::~GpuTimingRing() {
    destroy();
}

Expected<void> GpuTimingRing::init(const RhiDevice& device) {
    destroy();
    if (device.device() == VK_NULL_HANDLE) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "GpuTimingRing::init null RhiDevice"));
    }
    device_ = &device;

    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = kQueriesPerSlot;

    for (uint32_t s = 0; s < kFrameSlots; ++s) {
        if (vkCreateQueryPool(device_->device(), &qpci, nullptr, &query_pools_[s]) != VK_SUCCESS) {
            destroy();
            return make_unexpected(Error::make(ErrorCode::VulkanError, "vkCreateQueryPool(TIMESTAMP) failed"));
        }
        vkResetQueryPool(device_->device(), query_pools_[s], 0, kQueriesPerSlot);
        recorded_mask_[s] = 0;
    }

    denoiser_history_count_ = 0;
    denoiser_history_cursor_ = 0;
    latest_ = GpuPassTimingSnapshot{};
    return {};
}

void GpuTimingRing::destroy() noexcept {
    if (device_ != nullptr && device_->device() != VK_NULL_HANDLE) {
        for (auto& pool : query_pools_) {
            if (pool != VK_NULL_HANDLE) {
                vkDestroyQueryPool(device_->device(), pool, nullptr);
                pool = VK_NULL_HANDLE;
            }
        }
    }
    recorded_mask_.fill(0);
    denoiser_history_count_ = 0;
    denoiser_history_cursor_ = 0;
    latest_ = GpuPassTimingSnapshot{};
    device_ = nullptr;
}

void GpuTimingRing::cmd_reset_frame(VkCommandBuffer cmd, uint32_t frame_slot) noexcept {
    if (device_ == nullptr || cmd == VK_NULL_HANDLE) {
        return;
    }
    const uint32_t slot = frame_slot % kFrameSlots;
    vkCmdResetQueryPool(cmd, query_pools_[slot], 0, kQueriesPerSlot);
    recorded_mask_[slot] = 0;
}

void GpuTimingRing::cmd_reset_scope(VkCommandBuffer cmd, uint32_t frame_slot, GpuPassId pass) noexcept {
    if (device_ == nullptr || cmd == VK_NULL_HANDLE) {
        return;
    }
    const uint32_t slot = frame_slot % kFrameSlots;
    const uint32_t p = static_cast<uint32_t>(pass);
    vkCmdResetQueryPool(cmd, query_pools_[slot], p * 2u, 2u);
    recorded_mask_[slot] &= ~(1u << p);
}

void GpuTimingRing::begin_scope(VkCommandBuffer cmd, uint32_t frame_slot, GpuPassId pass) const noexcept {
    if (device_ == nullptr || cmd == VK_NULL_HANDLE) {
        return;
    }
    const uint32_t slot = frame_slot % kFrameSlots;
    const uint32_t q = static_cast<uint32_t>(pass) * 2u;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, query_pools_[slot], q);
}

void GpuTimingRing::end_scope(VkCommandBuffer cmd, uint32_t frame_slot, GpuPassId pass) noexcept {
    if (device_ == nullptr || cmd == VK_NULL_HANDLE) {
        return;
    }
    const uint32_t slot = frame_slot % kFrameSlots;
    const uint32_t p = static_cast<uint32_t>(pass);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, query_pools_[slot], p * 2u + 1u);
    recorded_mask_[slot] |= (1u << p);
}

const GpuPassTimingSnapshot& GpuTimingRing::resolve_frame(uint32_t frame_slot) noexcept {
    if (device_ == nullptr) {
        return latest_;
    }
    const uint32_t slot = frame_slot % kFrameSlots;
    if (recorded_mask_[slot] == 0) {
        return latest_;
    }

    std::array<uint64_t, static_cast<size_t>(kQueriesPerSlot) * 2U> results{};
    const VkResult res = vkGetQueryPoolResults(device_->device(), query_pools_[slot], 0, kQueriesPerSlot,
                                               sizeof(results), results.data(), sizeof(uint64_t) * 2u,
                                               VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (res != VK_SUCCESS && res != VK_NOT_READY) {
        return latest_;
    }

    const double period_ms = static_cast<double>(device_->gpu_info().timestamp_period_ns) * 1e-6;
    GpuPassTimingSnapshot snap{};
    double sum_critical_ms = 0.0;
    bool any_valid = false;

    for (uint32_t p = 0; p < kPassCount; ++p) {
        if ((recorded_mask_[slot] & (1u << p)) == 0) {
            continue;
        }
        const uint64_t t0 = results[static_cast<size_t>(p) * 4u + 0u];
        const uint64_t av0 = results[static_cast<size_t>(p) * 4u + 1u];
        const uint64_t t1 = results[static_cast<size_t>(p) * 4u + 2u];
        const uint64_t av1 = results[static_cast<size_t>(p) * 4u + 3u];
        if (av0 != 0 && av1 != 0 && t1 >= t0) {
            any_valid = true;
            const float ms = static_cast<float>(static_cast<double>(t1 - t0) * period_ms);
            snap.pass_ms[p] = ms;
            if (p <= static_cast<uint32_t>(GpuPassId::OutputEncode)) {
                sum_critical_ms += ms;
            }
        }
    }
    if (!any_valid) {
        return latest_;
    }

    snap.denoiser_ms = snap.pass_ms[static_cast<size_t>(GpuPassId::Denoise)];
    const float frame_total = snap.pass_ms[static_cast<size_t>(GpuPassId::FrameTotal)];
    snap.gpu_total_ms = (frame_total > 0.0f) ? frame_total : static_cast<float>(sum_critical_ms);

    denoiser_history_ms_[denoiser_history_cursor_] = snap.denoiser_ms;
    denoiser_history_cursor_ = (denoiser_history_cursor_ + 1u) % kRollingWindowFrames;
    if (denoiser_history_count_ < kRollingWindowFrames) {
        ++denoiser_history_count_;
    }

    std::array<float, kRollingWindowFrames> scratch = denoiser_history_ms_;
    std::sort(scratch.begin(), scratch.begin() + denoiser_history_count_);
    const uint32_t p95_idx =
        std::min(denoiser_history_count_ - 1u,
                 static_cast<uint32_t>(std::floor(0.95f * static_cast<float>(denoiser_history_count_ - 1u))));
    snap.denoiser_p95_120f_ms = scratch[p95_idx];

    uint64_t gpu_ns = 0;
    uint64_t cpu_ns = 0;
    if (device_->calibrated_timestamp_ns(gpu_ns, cpu_ns)) {
        snap.gpu_cpu_clock_offset_ns = static_cast<int64_t>(gpu_ns) - static_cast<int64_t>(cpu_ns);
    }

    snap.valid = true;
    latest_ = snap;
    return latest_;
}

} // namespace aether::rhi
