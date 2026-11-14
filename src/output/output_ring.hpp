#pragma once

#include "core/telemetry.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace aether::output {

/**
 * Work item handed from T3 (`NprRenderer::submit_critical_and_signal`) to T4 (`OutputCoordinator`)
 * through `SpscRing<OutputRingSlot, 16>`. Zero heap allocations (`HR-01`).
 */
struct OutputRingSlot {
    uint64_t frame_sequence{0};
    uint64_t frame_timeline_value{0}; // Wait target on RhiDevice::frame_timeline() before reading GPU images
    uint32_t output_slot{0};          // 0..2 inside FrameGraph OutputRing
    uint32_t width{1920};
    uint32_t height{1080};
    uint32_t fps{60};

    // Vulkan handles owned by FrameGraph::output_ring_[output_slot]
    VkImage bgra_image{VK_NULL_HANDLE};          // Premul or straight depending on request
    VkImage bgra_straight_image{VK_NULL_HANDLE}; // Straight sRGB BGRA for NDI (`§11.4`)
    VkImage bottom_up_image{VK_NULL_HANDLE};     // Bottom-up BGRA when DirectShow RGB pin active
    VkImage nv12_y_image{VK_NULL_HANDLE};
    VkImage nv12_uv_image{VK_NULL_HANDLE};
    bool has_bottom_up{false};
    bool tracking_active{false};

    // Pipeline stamps accumulated along T1 -> T2 -> T3 (`§4.6`)
    uint64_t t1_capture_us{0};
    uint64_t t2_start_us{0};
    uint64_t t2_done_us{0};
    uint64_t t3_start_us{0};
    uint64_t t3_submit_us{0};
    uint64_t ui_cpu_us{0};
    uint64_t late_latch_sample_to_write_us{0};
    uint32_t active_ep{0};
};

} // namespace aether::output
