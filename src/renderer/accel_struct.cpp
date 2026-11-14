#include "renderer/accel_struct.hpp"

#include "core/log.hpp"
#include "renderer/gpu_structs.hpp"

#include <algorithm>
#include <cstring>

namespace aether::renderer {
namespace {

[[nodiscard]] VkDeviceAddress align_up_addr(VkDeviceAddress addr, VkDeviceSize alignment) noexcept {
    if (alignment <= 1) {
        return addr;
    }
    const VkDeviceSize mask = alignment - 1;
    return (addr + mask) & ~mask;
}

} // namespace

Expected<void> AccelStructManager::init(const rhi::RhiDevice& device) {
    destroy();
    device_ = &device;
    if (!device.rt_supported()) {
        return {};
    }
    const auto& ext = device.ext();
    if (ext.create_acceleration_structure == nullptr || ext.destroy_acceleration_structure == nullptr ||
        ext.get_acceleration_structure_build_sizes == nullptr || ext.cmd_build_acceleration_structures == nullptr ||
        ext.get_acceleration_structure_device_address == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "Missing Vulkan KHR ray-query function pointers")};
    }
    scratch_alignment_ = std::max<VkDeviceSize>(128u, device.gpu_info().min_accel_struct_scratch_offset_alignment);
    return {};
}

void AccelStructManager::destroy_avatar_structures() noexcept {
    if (device_ != nullptr && device_->device() != VK_NULL_HANDLE) {
        const VkDevice dev = device_->device();
        const auto& ext = device_->ext();
        if (ext.destroy_acceleration_structure != nullptr) {
            if (tlas_ != VK_NULL_HANDLE) {
                ext.destroy_acceleration_structure(dev, tlas_, nullptr);
            }
            if (blas_opaque_ != VK_NULL_HANDLE) {
                ext.destroy_acceleration_structure(dev, blas_opaque_, nullptr);
            }
            if (blas_hair_ != VK_NULL_HANDLE) {
                ext.destroy_acceleration_structure(dev, blas_hair_, nullptr);
            }
        }
        device_->destroy_buffer(instance_buf_);
        device_->destroy_buffer(scratch_buf_);
        device_->destroy_buffer(tlas_buf_);
        device_->destroy_buffer(blas_hair_buf_);
        device_->destroy_buffer(blas_opaque_buf_);
    }

    tlas_ = VK_NULL_HANDLE;
    blas_opaque_ = VK_NULL_HANDLE;
    blas_hair_ = VK_NULL_HANDLE;
    blas_opaque_size_ = 0;
    blas_hair_size_ = 0;
    tlas_size_ = 0;
    scratch_size_ = 0;
    prepared_vertex_count_ = 0;
    prepared_index_count_ = 0;
    opaque_tri_count_ = 0;
    hair_tri_count_ = 0;
    tlas_instance_count_ = 0;
    blas_opaque_addr_ = 0;
    blas_hair_addr_ = 0;
    opaque_geoms_.clear();
    opaque_ranges_.clear();
    opaque_range_ptrs_.clear();
    hair_geoms_.clear();
    hair_ranges_.clear();
    hair_range_ptrs_.clear();
    tlas_bound_ = false;
    ready_ = false;
    has_built_once_ = false;
    last_full_rebuild_ = false;
}

void AccelStructManager::destroy() noexcept {
    destroy_avatar_structures();
    device_ = nullptr;
    refit_count_ = 0;
    rebuild_count_ = 0;
}

Expected<void> AccelStructManager::prepare_avatar(const GpuAvatar& avatar) {
    destroy_avatar_structures();
    if (device_ == nullptr || !device_->rt_supported()) {
        return {};
    }
    const uint32_t vertex_count = avatar.requirements.vertex_count;
    const uint32_t index_count = avatar.requirements.index_count;
    if (vertex_count == 0 || index_count < 3 || avatar.index_buf.handle == VK_NULL_HANDLE) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "GpuAvatar has empty geometry for AS build")};
    }

    const auto& ext = device_->ext();
    const VkDeviceAddress index_bda = avatar.index_buf.device_address != 0
                                          ? avatar.index_buf.device_address
                                          : device_->buffer_device_address(avatar.index_buf.handle);
    if (index_bda == 0) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "GpuAvatar index buffer lacks device address")};
    }

    // Partition submeshes into Opaque/Body (0x01) vs Hair Caster (0x04) (`§10.5`).
    auto append_geom = [&](uint32_t idx_offset, uint32_t idx_count, bool is_hair) {
        if (idx_count < 3) {
            return;
        }
        const uint32_t tri_cnt = idx_count / 3u;
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        g.geometry.triangles.vertexStride = sizeof(GpuSkinnedVertex);
        g.geometry.triangles.maxVertex = vertex_count - 1u;
        g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        g.geometry.triangles.indexData.deviceAddress = index_bda;

        VkAccelerationStructureBuildRangeInfoKHR rng{};
        rng.primitiveCount = tri_cnt;
        rng.primitiveOffset = idx_offset * static_cast<uint32_t>(sizeof(uint32_t));
        rng.firstVertex = 0;
        rng.transformOffset = 0;

        if (is_hair) {
            hair_geoms_.push_back(g);
            hair_ranges_.push_back(rng);
            hair_tri_count_ += tri_cnt;
        } else {
            opaque_geoms_.push_back(g);
            opaque_ranges_.push_back(rng);
            opaque_tri_count_ += tri_cnt;
        }
    };

    if (avatar.submeshes.empty()) {
        append_geom(0, index_count, false);
    } else {
        for (const auto& sm : avatar.submeshes) {
            bool is_hair = false;
            if (sm.material_index < avatar.materials.size()) {
                is_hair = (avatar.materials[sm.material_index].flags & AETHER_MAT_FLAG_HAIR_FRONT_STENCIL) != 0u;
            }
            append_geom(sm.index_offset, sm.index_count, is_hair);
        }
    }

    if (opaque_geoms_.empty() && hair_geoms_.empty()) {
        append_geom(0, index_count, false);
    }

    opaque_range_ptrs_.reserve(opaque_ranges_.size());
    for (const auto& r : opaque_ranges_) {
        opaque_range_ptrs_.push_back(&r);
    }
    hair_range_ptrs_.reserve(hair_ranges_.size());
    for (const auto& r : hair_ranges_) {
        hair_range_ptrs_.push_back(&r);
    }

    VkDeviceSize max_scratch = 0;

    auto create_blas = [&](const std::vector<VkAccelerationStructureGeometryKHR>& geoms,
                           const std::vector<VkAccelerationStructureBuildRangeInfoKHR>& ranges,
                           VkAccelerationStructureKHR& out_as, rhi::RhiBuffer& out_buf, VkDeviceSize& out_size,
                           VkDeviceAddress& out_addr) -> Expected<void> {
        if (geoms.empty()) {
            return {};
        }
        std::vector<uint32_t> prim_counts(ranges.size());
        for (size_t i = 0; i < ranges.size(); ++i) {
            prim_counts[i] = ranges[i].primitiveCount;
        }

        VkAccelerationStructureBuildGeometryInfoKHR bi{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                   VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        bi.geometryCount = static_cast<uint32_t>(geoms.size());
        bi.pGeometries = geoms.data();

        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        ext.get_acceleration_structure_build_sizes(device_->device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                   &bi, prim_counts.data(), &sizes);

        out_size = sizes.accelerationStructureSize;
        max_scratch = std::max(max_scratch, std::max(sizes.buildScratchSize, sizes.updateScratchSize));

        auto buf_res = device_->create_buffer(out_size,
                                              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                              VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
        if (!buf_res) {
            return Unexpected<Error>{buf_res.error()};
        }
        out_buf = buf_res.value();

        VkAccelerationStructureCreateInfoKHR asci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        asci.buffer = out_buf.handle;
        asci.size = out_size;
        asci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (ext.create_acceleration_structure(device_->device(), &asci, nullptr, &out_as) != VK_SUCCESS) {
            return Unexpected<Error>{
                Error::make(ErrorCode::VulkanError, "vkCreateAccelerationStructureKHR(BLAS) failed")};
        }

        VkAccelerationStructureDeviceAddressInfoKHR dai{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        dai.accelerationStructure = out_as;
        out_addr = ext.get_acceleration_structure_device_address(device_->device(), &dai);
        return {};
    };

    if (auto r = create_blas(opaque_geoms_, opaque_ranges_, blas_opaque_, blas_opaque_buf_, blas_opaque_size_,
                             blas_opaque_addr_);
        !r) {
        destroy_avatar_structures();
        return r;
    }
    if (auto r = create_blas(hair_geoms_, hair_ranges_, blas_hair_, blas_hair_buf_, blas_hair_size_, blas_hair_addr_);
        !r) {
        destroy_avatar_structures();
        return r;
    }

    tlas_instance_count_ = (blas_opaque_ != VK_NULL_HANDLE ? 1u : 0u) + (blas_hair_ != VK_NULL_HANDLE ? 1u : 0u);

    // Pre-allocate TLAS for up to kMaxTlasInstances (`64`)
    VkAccelerationStructureGeometryKHR tlas_geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlas_geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;

    VkAccelerationStructureBuildGeometryInfoKHR tlas_bi{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlas_bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlas_bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    tlas_bi.geometryCount = 1;
    tlas_bi.pGeometries = &tlas_geom;

    const uint32_t max_inst = kMaxTlasInstances;
    VkAccelerationStructureBuildSizesInfoKHR tlas_sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    ext.get_acceleration_structure_build_sizes(device_->device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                               &tlas_bi, &max_inst, &tlas_sizes);

    tlas_size_ = tlas_sizes.accelerationStructureSize;
    max_scratch = std::max(max_scratch, std::max(tlas_sizes.buildScratchSize, tlas_sizes.updateScratchSize));

    auto tlas_buf_res = device_->create_buffer(
        tlas_size_, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    if (!tlas_buf_res) {
        destroy_avatar_structures();
        return Unexpected<Error>{tlas_buf_res.error()};
    }
    tlas_buf_ = tlas_buf_res.value();

    VkAccelerationStructureCreateInfoKHR tlas_ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    tlas_ci.buffer = tlas_buf_.handle;
    tlas_ci.size = tlas_size_;
    tlas_ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (ext.create_acceleration_structure(device_->device(), &tlas_ci, nullptr, &tlas_) != VK_SUCCESS) {
        destroy_avatar_structures();
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkCreateAccelerationStructureKHR(TLAS) failed")};
    }

    // Double/triple-buffered host-mapped TLAS instance array (`3 * 64 * 64B = 12 KiB`)
    const VkDeviceSize inst_bytes =
        static_cast<VkDeviceSize>(kFrameSlots) * kMaxTlasInstances * sizeof(VkAccelerationStructureInstanceKHR);
    auto inst_res = device_->create_buffer(inst_bytes,
                                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                           VMA_MEMORY_USAGE_AUTO,
                                           VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                               VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!inst_res) {
        destroy_avatar_structures();
        return Unexpected<Error>{inst_res.error()};
    }
    instance_buf_ = inst_res.value();

    scratch_size_ = max_scratch + scratch_alignment_;
    auto scratch_res = device_->create_buffer(
        scratch_size_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    if (!scratch_res) {
        destroy_avatar_structures();
        return Unexpected<Error>{scratch_res.error()};
    }
    scratch_buf_ = scratch_res.value();

    prepared_vertex_count_ = vertex_count;
    prepared_index_count_ = index_count;
    ready_ = true;
    has_built_once_ = false;
    AETHER_LOG_INFO("rt", "AccelStruct prepared: opaque_tris=%u hair_tris=%u blas_kb=%.1f scratch_kb=%.1f",
                    opaque_tri_count_, hair_tri_count_,
                    static_cast<double>(blas_opaque_size_ + blas_hair_size_) / 1024.0,
                    static_cast<double>(scratch_size_) / 1024.0);
    return {};
}

void AccelStructManager::record_update(VkCommandBuffer cmd, const GpuAvatar& avatar,
                                       VkDeviceAddress skinned_vertices_bda, uint32_t frame_slot, uint64_t frame_index,
                                       rhi::BindlessHeap& bindless, bool force_full_rebuild) noexcept {
    (void)avatar;
    if (!ready_ || device_ == nullptr || !device_->rt_supported() || cmd == VK_NULL_HANDLE ||
        skinned_vertices_bda == 0) {
        return;
    }

    if (!tlas_bound_ && tlas_ != VK_NULL_HANDLE) {
        (void)bindless.update_tlas(tlas_);
        tlas_bound_ = true;
    }

    const auto& ext = device_->ext();
    const VkDeviceAddress scratch_bda = align_up_addr(scratch_buf_.device_address, scratch_alignment_);
    const bool do_full_rebuild =
        force_full_rebuild || !has_built_once_ || ((frame_index % kFullRebuildInterval) == 0ULL);

    last_full_rebuild_ = do_full_rebuild;
    has_built_once_ = true;
    if (do_full_rebuild) {
        ++rebuild_count_;
    } else {
        ++refit_count_;
    }

    // 1. Update/Build BLAS structures sequentially reusing aligned scratch with AS barriers (`HR-08`)
    auto record_blas = [&](std::vector<VkAccelerationStructureGeometryKHR>& geoms,
                           const VkAccelerationStructureBuildRangeInfoKHR* const* range_ptrs,
                           VkAccelerationStructureKHR target_as) noexcept {
        if (geoms.empty() || target_as == VK_NULL_HANDLE) {
            return;
        }
        for (auto& g : geoms) {
            g.geometry.triangles.vertexData.deviceAddress = skinned_vertices_bda;
        }

        VkAccelerationStructureBuildGeometryInfoKHR bi{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                   VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        bi.mode = do_full_rebuild ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR
                                  : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        bi.srcAccelerationStructure = do_full_rebuild ? VK_NULL_HANDLE : target_as;
        bi.dstAccelerationStructure = target_as;
        bi.geometryCount = static_cast<uint32_t>(geoms.size());
        bi.pGeometries = geoms.data();
        bi.scratchData.deviceAddress = scratch_bda;

        ext.cmd_build_acceleration_structures(cmd, 1, &bi, range_ptrs);

        VkMemoryBarrier2 as_bar{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        as_bar.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        as_bar.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        as_bar.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        as_bar.dstAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &as_bar;
        vkCmdPipelineBarrier2(cmd, &dep);
    };

    record_blas(opaque_geoms_, opaque_range_ptrs_.data(), blas_opaque_);
    record_blas(hair_geoms_, hair_range_ptrs_.data(), blas_hair_);

    // 2. Write per-slot TLAS instances (Opaque 0x01 + HairCaster 0x04) & rebuild TLAS
    const uint32_t slot = frame_slot % kFrameSlots;
    const VkDeviceSize slot_byte_offset =
        static_cast<VkDeviceSize>(slot) * kMaxTlasInstances * sizeof(VkAccelerationStructureInstanceKHR);
    auto* inst_ptr = reinterpret_cast<VkAccelerationStructureInstanceKHR*>(
        static_cast<uint8_t*>(instance_buf_.info.pMappedData) + slot_byte_offset);

    uint32_t active_inst = 0;
    auto push_instance = [&](VkDeviceAddress blas_addr, uint32_t custom_idx, uint8_t mask) noexcept {
        if (blas_addr == 0 || active_inst >= kMaxTlasInstances) {
            return;
        }
        VkAccelerationStructureInstanceKHR inst{};
        inst.transform.matrix[0][0] = 1.0f;
        inst.transform.matrix[1][1] = 1.0f;
        inst.transform.matrix[2][2] = 1.0f;
        inst.instanceCustomIndex = custom_idx;
        inst.mask = mask;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = blas_addr;
        inst_ptr[active_inst++] = inst;
    };

    push_instance(blas_opaque_addr_, 0u, static_cast<uint8_t>(RtInstanceMask::OpaqueAvatar));
    push_instance(blas_hair_addr_, 1u, static_cast<uint8_t>(RtInstanceMask::HairCast));
    vmaFlushAllocation(device_->allocator(), instance_buf_.allocation, slot_byte_offset,
                       active_inst * sizeof(VkAccelerationStructureInstanceKHR));

    VkAccelerationStructureGeometryKHR tlas_geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlas_geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlas_geom.geometry.instances.arrayOfPointers = VK_FALSE;
    tlas_geom.geometry.instances.data.deviceAddress = instance_buf_.device_address + slot_byte_offset;

    VkAccelerationStructureBuildGeometryInfoKHR tlas_bi{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlas_bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlas_bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    tlas_bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlas_bi.dstAccelerationStructure = tlas_;
    tlas_bi.geometryCount = 1;
    tlas_bi.pGeometries = &tlas_geom;
    tlas_bi.scratchData.deviceAddress = scratch_bda;

    VkAccelerationStructureBuildRangeInfoKHR tlas_rng{};
    tlas_rng.primitiveCount = active_inst;
    const VkAccelerationStructureBuildRangeInfoKHR* tlas_rng_ptr = &tlas_rng;
    ext.cmd_build_acceleration_structures(cmd, 1, &tlas_bi, &tlas_rng_ptr);

    // Barrier before Pass 4 RayQuery reads TLAS in Compute/Fragment (`HR-08`)
    VkMemoryBarrier2 post_tlas{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    post_tlas.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    post_tlas.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    post_tlas.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    post_tlas.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo post_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    post_dep.memoryBarrierCount = 1;
    post_dep.pMemoryBarriers = &post_tlas;
    vkCmdPipelineBarrier2(cmd, &post_dep);
}

} // namespace aether::renderer