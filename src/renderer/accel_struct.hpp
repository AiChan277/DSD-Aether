#pragma once

#include "core/expected.hpp"
#include "renderer/asset_upload/gpu_avatar.hpp"
#include "rhi/bindless.hpp"
#include "rhi/rhi_device.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace aether::renderer {

enum class RtInstanceMask : uint8_t {
    OpaqueAvatar = 0x01,
    Translucent = 0x02,
    HairCast = 0x04,
    Prop = 0x08,
    All = 0xFF,
};

/**
 * Manages hardware ray-tracing BLAS & TLAS for AETHER-2.5D (`§10.5`, `P4-03`):
 * - Avatar BLAS uses `ALLOW_UPDATE | PREFER_FAST_BUILD` for fast per-frame refits (`~0.35 ms` target),
 *   performing a full rebuild every `300` frames or upon vertex/index count change.
 * - TLAS holds up to `64` instances partitioned by `0x01`/`0x02`/`0x04` visibility masks.
 * - Pre-allocates all buffers at `init()`/`prepare_avatar()` so per-frame updates perform zero heap allocs (`HR-01`).
 */
class AccelStructManager {
  public:
    static constexpr uint32_t kFullRebuildInterval = 300;
    static constexpr uint32_t kMaxTlasInstances = 64;
    static constexpr uint32_t kMaxSubmeshRanges = 128;
    static constexpr uint32_t kFrameSlots = 3;

    AccelStructManager() = default;
    ~AccelStructManager() { destroy(); }

    AccelStructManager(const AccelStructManager&) = delete;
    AccelStructManager& operator=(const AccelStructManager&) = delete;

    [[nodiscard]] Expected<void> init(const rhi::RhiDevice& device);
    void destroy() noexcept;

    /** Pre-allocates BLAS/TLAS & scratch buffers for `avatar` (zero per-frame allocations thereafter). */
    [[nodiscard]] Expected<void> prepare_avatar(const GpuAvatar& avatar);

    /**
     * Records Pass 2 BLAS refit (or periodic 300-frame rebuild) + TLAS rebuild + Sync2 barrier into `cmd`.
     */
    void record_update(VkCommandBuffer cmd, const GpuAvatar& avatar, VkDeviceAddress skinned_vertices_bda,
                       uint32_t frame_slot, uint64_t frame_index, rhi::BindlessHeap& bindless,
                       bool force_full_rebuild = false) noexcept;

    [[nodiscard]] VkAccelerationStructureKHR tlas() const noexcept { return tlas_; }
    [[nodiscard]] bool is_ready() const noexcept { return ready_; }
    [[nodiscard]] bool last_was_full_rebuild() const noexcept { return last_full_rebuild_; }
    [[nodiscard]] uint64_t refit_count() const noexcept { return refit_count_; }
    [[nodiscard]] uint64_t full_rebuild_count() const noexcept { return rebuild_count_; }

  private:
    void destroy_avatar_structures() noexcept;

    const rhi::RhiDevice* device_{nullptr};

    VkAccelerationStructureKHR blas_opaque_{VK_NULL_HANDLE};
    VkAccelerationStructureKHR blas_hair_{VK_NULL_HANDLE};
    VkAccelerationStructureKHR tlas_{VK_NULL_HANDLE};

    rhi::RhiBuffer blas_opaque_buf_{};
    rhi::RhiBuffer blas_hair_buf_{};
    rhi::RhiBuffer tlas_buf_{};
    rhi::RhiBuffer scratch_buf_{};
    rhi::RhiBuffer instance_buf_{};

    VkDeviceSize blas_opaque_size_{0};
    VkDeviceSize blas_hair_size_{0};
    VkDeviceSize tlas_size_{0};
    VkDeviceSize scratch_size_{0};
    VkDeviceSize scratch_alignment_{128};

    uint32_t prepared_vertex_count_{0};
    uint32_t prepared_index_count_{0};
    uint32_t opaque_tri_count_{0};
    uint32_t hair_tri_count_{0};
    uint32_t tlas_instance_count_{0};

    // Pre-allocated during `prepare_avatar()` so `record_update()` does zero heap allocs (`HR-01`).
    std::vector<VkAccelerationStructureGeometryKHR> opaque_geoms_{};
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> opaque_ranges_{};
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> opaque_range_ptrs_{};
    std::vector<VkAccelerationStructureGeometryKHR> hair_geoms_{};
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> hair_ranges_{};
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> hair_range_ptrs_{};

    VkDeviceAddress blas_opaque_addr_{0};
    VkDeviceAddress blas_hair_addr_{0};
    bool tlas_bound_{false};
    bool ready_{false};
    bool has_built_once_{false};
    bool last_full_rebuild_{false};
    uint64_t refit_count_{0};
    uint64_t rebuild_count_{0};
};

} // namespace aether::renderer
