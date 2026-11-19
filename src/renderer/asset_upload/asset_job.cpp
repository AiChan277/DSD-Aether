#include "renderer/asset_upload/asset_job.hpp"

#include "core/clock.hpp"
#include "core/log.hpp"

#include <immintrin.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace aether::renderer {
namespace {

[[nodiscard]] uint64_t align_up_256(uint64_t val) noexcept {
    return (val + 255ULL) & ~255ULL;
}

} // namespace

AssetJob::~AssetJob() {
    reset();
}

void AssetJob::set_error(AetherStatus status, const char* msg) noexcept {
    last_status_.store(status, std::memory_order_release);
    if (msg != nullptr) {
        std::snprintf(error_buf_, sizeof(error_buf_), "%s", msg);
    } else {
        error_buf_[0] = '\0';
    }
}

void AssetJob::cancel() noexcept {
    cancel_requested_.store(true, std::memory_order_release);
    active_polls_.fetch_add(1, std::memory_order_acq_rel);
    if (AetherLoaderHandle h = active_loader_.load(std::memory_order_acquire); h != nullptr) {
        aether_loader_cancel(h);
    }
    active_polls_.fetch_sub(1, std::memory_order_release);
}

void AssetJob::reset() noexcept {
    cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (StagedAvatarUpload* leftover = staged_ptr_.exchange(nullptr, std::memory_order_acq_rel); leftover != nullptr) {
        leftover->release();
        delete leftover;
    }
    state_.store(AssetJobState::Idle, std::memory_order_release);
    last_progress_.store(0.0f, std::memory_order_release);
    last_status_.store(AETHER_OK, std::memory_order_release);
    cancel_requested_.store(false, std::memory_order_release);
    error_buf_[0] = '\0';
}

float AssetJob::progress() const noexcept {
    active_polls_.fetch_add(1, std::memory_order_acq_rel);
    float p = last_progress_.load(std::memory_order_acquire);
    if (AetherLoaderHandle h = active_loader_.load(std::memory_order_acquire); h != nullptr) {
        p = aether_loader_progress(h);
        last_progress_.store(p, std::memory_order_release);
    }
    active_polls_.fetch_sub(1, std::memory_order_release);
    return p;
}

void AssetJob::start_load(rhi::RhiDevice& rhi, std::string path_utf8, uint32_t load_flags) {
    reset();
    state_.store(AssetJobState::Loading, std::memory_order_release);
    worker_ = std::thread(&AssetJob::worker_main, this, &rhi, std::move(path_utf8), load_flags);
}

void AssetJob::worker_main(rhi::RhiDevice* rhi, std::string path_utf8, uint32_t load_flags) {
    aether_set_thread_role(AETHER_THREAD_ROLE_LOADER);
    auto restore_role = []() { aether_set_thread_role(AETHER_THREAD_ROLE_ANY); };

    const uint64_t t_open_start = now_us();

    AetherLoadRequest req{};
    req.path_utf8 = path_utf8.c_str();
    req.flags = load_flags;
    req.max_threads = 0;

    AetherLoaderHandle loader = nullptr;
    const AetherStatus open_rc = aether_loader_open(&req, &loader);
    if (open_rc != AETHER_OK || loader == nullptr) {
        char err[256]{};
        (void)aether_last_error(err, sizeof(err));
        set_error(open_rc, err[0] != '\0' ? err : "aether_loader_open failed");
        state_.store(cancel_requested_.load(std::memory_order_acquire) ? AssetJobState::Cancelled
                                                                       : AssetJobState::Failed,
                     std::memory_order_release);
        restore_role();
        return;
    }

    auto close_loader = [&]() {
        active_loader_.store(nullptr, std::memory_order_release);
        while (active_polls_.load(std::memory_order_acquire) != 0) {
            _mm_pause();
        }
        aether_loader_close(loader);
        loader = nullptr;
    };

    active_loader_.store(loader, std::memory_order_release);
    if (cancel_requested_.load(std::memory_order_acquire)) {
        aether_loader_cancel(loader);
    }

    AetherAssetRequirements reqs{};
    const AetherStatus query_rc = aether_loader_query(loader, &reqs);
    const double open_query_ms = static_cast<double>(now_us() - t_open_start) / 1000.0;
    if (query_rc != AETHER_OK) {
        char err[256]{};
        (void)aether_last_error(err, sizeof(err));
        close_loader();
        set_error(query_rc, err[0] != '\0' ? err : "aether_loader_query failed");
        state_.store((query_rc == AETHER_E_CANCELLED || cancel_requested_.load(std::memory_order_acquire))
                         ? AssetJobState::Cancelled
                         : AssetJobState::Failed,
                     std::memory_order_release);
        restore_role();
        return;
    }

    auto staged = std::make_unique<StagedAvatarUpload>();
    staged->device = rhi;
    staged->requirements = reqs;
    staged->open_query_ms = open_query_ms;
    staged->source_path = std::move(path_utf8);

    // Reserve at least 256 bytes per non-empty stream, aligned to 256 B (satisfies both 64 B FFI
    // alignment and Vulkan vkCmdCopyBufferToImage 16 B BC7 texel-block alignment).
    uint64_t total_staging = 0;
    for (uint32_t s = 0; s < AETHER_STREAM_COUNT; ++s) {
        staged->stream_offsets[s] = total_staging;
        const uint64_t len = reqs.stream_bytes[s];
        if (len > 0) {
            total_staging += align_up_256(len);
        }
    }
    staged->fallback_white_offset = total_staging;
    total_staging += 256ULL;

    auto buf_res = rhi->create_buffer(total_staging, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!buf_res) {
        close_loader();
        set_error(AETHER_E_IO, buf_res.error().c_str());
        state_.store(AssetJobState::Failed, std::memory_order_release);
        restore_role();
        return;
    }
    staged->staging_buf = buf_res.value();

    auto* base_ptr = static_cast<uint8_t*>(staged->staging_buf.info.pMappedData);
    AetherArenaSlice slices[AETHER_STREAM_COUNT]{};
    for (uint32_t s = 0; s < AETHER_STREAM_COUNT; ++s) {
        slices[s].stream_kind = s;
        slices[s].size_bytes = reqs.stream_bytes[s];
        if (reqs.stream_bytes[s] > 0) {
            slices[s].ptr = base_ptr + staged->stream_offsets[s];
            staged->lent_ptrs[s] = reinterpret_cast<uint64_t>(slices[s].ptr);
        } else {
            slices[s].ptr = nullptr;
            staged->lent_ptrs[s] = 0;
        }
    }

    const uint64_t t_fill_start = now_us();
    const AetherStatus fill_rc = aether_loader_fill(loader, slices, AETHER_STREAM_COUNT, &staged->desc);
    staged->fill_ms = static_cast<double>(now_us() - t_fill_start) / 1000.0;
    last_progress_.store(aether_loader_progress(loader), std::memory_order_release);

    if (fill_rc != AETHER_OK) {
        char err[256]{};
        (void)aether_last_error(err, sizeof(err));
        close_loader();
        staged->release();
        set_error(fill_rc, err[0] != '\0' ? err : "aether_loader_fill failed");
        state_.store((fill_rc == AETHER_E_CANCELLED || cancel_requested_.load(std::memory_order_acquire))
                         ? AssetJobState::Cancelled
                         : AssetJobState::Failed,
                     std::memory_order_release);
        restore_role();
        return;
    }

    // Query and fill optional PMX 2.0/2.1 auxiliary data (SDEF, Bone Ext, CCD-IK, Morphs, Physics).
    if (aether_loader_query_pmx_aux(loader, &staged->pmx_aux_header) == AETHER_OK &&
        staged->pmx_aux_header.is_pmx != 0U) {
        const auto& ph = staged->pmx_aux_header;
        staged->pmx_sdef_vertices.resize(ph.sdef_vertex_count);
        staged->pmx_bone_exts.resize(ph.bone_ext_count);
        staged->pmx_ik_links.resize(ph.ik_link_count);
        staged->pmx_morph_channels.resize(ph.morph_channel_count);
        staged->pmx_group_children.resize(ph.group_child_count);
        staged->pmx_uv_morphs.resize(ph.uv_morph_count);
        staged->pmx_mat_morphs.resize(ph.mat_morph_count);
        staged->pmx_bone_morphs.resize(ph.bone_morph_count);
        staged->pmx_rigid_bodies.resize(ph.rigid_body_count);
        staged->pmx_constraints_6dof.resize(ph.constraint_6dof_count);

        AetherPmxAuxSlices pmx_slices{};
        pmx_slices.struct_size = sizeof(AetherPmxAuxSlices);
        pmx_slices.sdef_vertices = staged->pmx_sdef_vertices.empty() ? nullptr : staged->pmx_sdef_vertices.data();
        pmx_slices.sdef_vertex_cap = ph.sdef_vertex_count;
        pmx_slices.bone_exts = staged->pmx_bone_exts.empty() ? nullptr : staged->pmx_bone_exts.data();
        pmx_slices.bone_ext_cap = ph.bone_ext_count;
        pmx_slices.ik_links = staged->pmx_ik_links.empty() ? nullptr : staged->pmx_ik_links.data();
        pmx_slices.ik_link_cap = ph.ik_link_count;
        pmx_slices.morph_channels = staged->pmx_morph_channels.empty() ? nullptr : staged->pmx_morph_channels.data();
        pmx_slices.morph_channel_cap = ph.morph_channel_count;
        pmx_slices.group_children = staged->pmx_group_children.empty() ? nullptr : staged->pmx_group_children.data();
        pmx_slices.group_child_cap = ph.group_child_count;
        pmx_slices.uv_morphs = staged->pmx_uv_morphs.empty() ? nullptr : staged->pmx_uv_morphs.data();
        pmx_slices.uv_morph_cap = ph.uv_morph_count;
        pmx_slices.mat_morphs = staged->pmx_mat_morphs.empty() ? nullptr : staged->pmx_mat_morphs.data();
        pmx_slices.mat_morph_cap = ph.mat_morph_count;
        pmx_slices.bone_morphs = staged->pmx_bone_morphs.empty() ? nullptr : staged->pmx_bone_morphs.data();
        pmx_slices.bone_morph_cap = ph.bone_morph_count;
        pmx_slices.rigid_bodies = staged->pmx_rigid_bodies.empty() ? nullptr : staged->pmx_rigid_bodies.data();
        pmx_slices.rigid_body_cap = ph.rigid_body_count;
        pmx_slices.constraints_6dof =
            staged->pmx_constraints_6dof.empty() ? nullptr : staged->pmx_constraints_6dof.data();
        pmx_slices.constraint_6dof_cap = ph.constraint_6dof_count;

        (void)aether_loader_fill_pmx_aux(loader, &pmx_slices);
    }

    close_loader();

    // Verify REQ-FFI-010 zero-copy pointer identity for every populated stream.
    bool ptr_match = true;
    for (uint32_t s = 0; s < AETHER_STREAM_COUNT; ++s) {
        if (reqs.stream_bytes[s] > 0 && staged->desc.debug_stream_ptrs[s] != staged->lent_ptrs[s]) {
            ptr_match = false;
            AETHER_LOG_ERROR("renderer", "Zero-copy pointer identity mismatch on stream %u", s);
        }
    }
    staged->pointer_identity_verified = ptr_match;
    if (!ptr_match) {
        staged->release();
        set_error(AETHER_E_INTERNAL, "FFI zero-copy pointer identity check failed");
        state_.store(AssetJobState::Failed, std::memory_order_release);
        restore_role();
        return;
    }

    if (cancel_requested_.load(std::memory_order_acquire)) {
        staged->release();
        set_error(AETHER_E_CANCELLED, "Asset load cancelled");
        state_.store(AssetJobState::Cancelled, std::memory_order_release);
        restore_role();
        return;
    }

    StagedAvatarUpload* raw = staged.release();
    StagedAvatarUpload* prev = staged_ptr_.exchange(raw, std::memory_order_acq_rel);
    if (prev != nullptr) {
        prev->release();
        delete prev;
    }
    last_progress_.store(1.0f, std::memory_order_release);
    last_status_.store(AETHER_OK, std::memory_order_release);
    state_.store(AssetJobState::ReadyForGpu, std::memory_order_release);
    restore_role();
}

std::unique_ptr<StagedAvatarUpload> AssetJob::take_staged() noexcept {
    StagedAvatarUpload* raw = staged_ptr_.exchange(nullptr, std::memory_order_acq_rel);
    return std::unique_ptr<StagedAvatarUpload>(raw);
}

Expected<std::unique_ptr<GpuAvatar>> AssetJob::finalize_gpu_upload(rhi::RhiDevice& rhi) {
    auto staged = take_staged();
    if (!staged) {
        return fail(ErrorCode::InvalidArg, "No staged avatar ready for GPU upload");
    }
    auto res = upload_staged_avatar(rhi, std::move(staged));
    if (!res) {
        set_error(static_cast<AetherStatus>(res.error().code), res.error().c_str());
        state_.store(AssetJobState::Failed, std::memory_order_release);
        return res;
    }
    state_.store(AssetJobState::Uploaded, std::memory_order_release);
    return res;
}

} // namespace aether::renderer
