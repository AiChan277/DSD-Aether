#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <intrin.h>

#include "animation/expression_mixer.hpp"
#include "output/dsp_sync_bridge.hpp"

#include <algorithm>
#include <cstring>

namespace aether::output {

namespace {

uint32_t read_atomic_u32(const volatile uint32_t* ptr) noexcept {
    const LONG val =
        InterlockedCompareExchange(const_cast<volatile LONG*>(reinterpret_cast<const volatile LONG*>(ptr)), 0, 0);
    MemoryBarrier();
    return static_cast<uint32_t>(val);
}

void write_atomic_u32(volatile uint32_t* ptr, uint32_t val) noexcept {
    MemoryBarrier();
    InterlockedExchange(const_cast<volatile LONG*>(reinterpret_cast<volatile LONG*>(ptr)), static_cast<LONG>(val));
}

void clear_cl1_payload(AetherDspSyncPacket& pkt) noexcept {
    for (float& w : pkt.dsp_viseme_weights) {
        w = 0.0f;
    }
    pkt.dsp_flags &= ~0x05u;
}

} // namespace

Expected<void> DspSyncBridge::init(const wchar_t* section_name) {
    shutdown();

    const wchar_t* name = (section_name != nullptr && section_name[0] != L'\0') ? section_name : kDspSyncShmName;
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(kDspSyncShmBytes), name);
    if (mapping == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::IoError, "CreateFileMappingW(AetherDspSyncSharedMem) failed")};
    }
    const bool already_exists = (GetLastError() == ERROR_ALREADY_EXISTS);

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kDspSyncShmBytes);
    if (view == nullptr) {
        CloseHandle(mapping);
        return Unexpected<Error>{Error::make(ErrorCode::IoError, "MapViewOfFile(AetherDspSyncSharedMem) failed")};
    }

    file_mapping_ = mapping;
    packet_ = static_cast<AetherDspSyncPacket*>(view);

    if (!already_exists) {
        std::memset(view, 0, kDspSyncShmBytes);
    }
    if (packet_->magic_header != static_cast<uint32_t>(AETHER_DSP_MAGIC)) {
        packet_->magic_header = static_cast<uint32_t>(AETHER_DSP_MAGIC);
        packet_->protocol_version = 1u;
    }

    last_good_ = {};
    lat_window_.fill(0.0f);
    lat_sort_scratch_.fill(0.0f);
    lat_count_ = 0;
    lat_cursor_ = 0;
    ema_latency_ms_ = 0.0f;
    smoothed_delay_ms_ = 0.0f;
    latest_jitter_ms_ = 0.0f;
    has_ema_ = false;
    has_delay_ = false;
    stale_rejects_ = 0;
    return {};
}

void DspSyncBridge::shutdown() noexcept {
    if (packet_ != nullptr) {
        UnmapViewOfFile(packet_);
        packet_ = nullptr;
    }
    if (file_mapping_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(file_mapping_));
        file_mapping_ = nullptr;
    }
}

void DspSyncBridge::publish_video_telemetry(float video_latency_ms, uint64_t present_qpc_us, uint32_t fps_milli,
                                            uint32_t video_flags, float user_offset_ms, float dt_sec) noexcept {
    const float clamped_lat = std::max(0.0f, video_latency_ms);
    if (!has_ema_) {
        ema_latency_ms_ = clamped_lat;
        has_ema_ = true;
    } else {
        ema_latency_ms_ += kDspDelayEmaAlpha * (clamped_lat - ema_latency_ms_);
    }

    lat_window_[lat_cursor_] = clamped_lat;
    lat_cursor_ = (lat_cursor_ + 1u) % static_cast<uint32_t>(lat_window_.size());
    if (lat_count_ < static_cast<uint32_t>(lat_window_.size())) {
        ++lat_count_;
    }

    if (lat_count_ > 0) {
        std::copy_n(lat_window_.begin(), lat_count_, lat_sort_scratch_.begin());
        std::sort(lat_sort_scratch_.begin(), lat_sort_scratch_.begin() + lat_count_);
        const uint32_t idx_p50 = (lat_count_ - 1u) / 2u;
        const uint32_t idx_p95 = std::min<uint32_t>(lat_count_ - 1u, (lat_count_ * 95u) / 100u);
        latest_jitter_ms_ = std::max(0.0f, lat_sort_scratch_[idx_p95] - lat_sort_scratch_[idx_p50]);
    }

    float dsp_reported_ms = 0.0f;
    if (packet_ != nullptr) {
        AetherDspSyncPacket dsp_tmp{};
        uint64_t dummy_stale = 0;
        if (try_read_dsp_line1(packet_, present_qpc_us, dsp_tmp, dummy_stale)) {
            dsp_reported_ms = std::max(0.0f, dsp_tmp.dsp_reported_latency_ms);
        }
    }

    const float raw_target = std::clamp(ema_latency_ms_ - dsp_reported_ms + user_offset_ms, 0.0f, kDspMaxDelayMs);
    if (!has_delay_) {
        smoothed_delay_ms_ = raw_target;
        has_delay_ = true;
    } else {
        const float max_delta = (dt_sec > 0.0f) ? (kDspMaxSlewMsPerSec * dt_sec) : 0.0f;
        const float diff = std::clamp(raw_target - smoothed_delay_ms_, -max_delta, max_delta);
        smoothed_delay_ms_ = std::clamp(smoothed_delay_ms_ + diff, 0.0f, kDspMaxDelayMs);
    }

    if (packet_ == nullptr) {
        return;
    }

    volatile AetherDspSyncPacket* vp = packet_;
    uint32_t seq = read_atomic_u32(&vp->sequence_number);
    if ((seq & 1u) != 0u) {
        ++seq;
    }

    // Odd seqlock transition: begin Cache Line 0 update
    write_atomic_u32(&vp->sequence_number, seq + 1u);
    MemoryBarrier();

    vp->magic_header = static_cast<uint32_t>(AETHER_DSP_MAGIC);
    vp->total_video_latency_ms = ema_latency_ms_;
    vp->recommended_dsp_delay_ms = smoothed_delay_ms_;
    vp->video_present_qpc_us = (present_qpc_us != 0ULL) ? present_qpc_us : 1ULL;
    vp->protocol_version = 1u;
    vp->video_fps_milli = fps_milli;
    vp->latency_jitter_ms = latest_jitter_ms_;
    vp->video_flags = video_flags;

    // Even seqlock transition: commit Cache Line 0 atomically
    write_atomic_u32(&vp->sequence_number, seq + 2u);
}

bool DspSyncBridge::read_dsp_packet(AetherDspSyncPacket& out_packet, uint64_t now_qpc_us) noexcept {
    if (try_read_dsp_line1(packet_, now_qpc_us, out_packet, stale_rejects_)) {
        last_good_ = out_packet;
        return true;
    }
    return false;
}

bool DspSyncBridge::try_read_dsp_line1(const AetherDspSyncPacket* shm_ptr, uint64_t now_qpc_us,
                                       AetherDspSyncPacket& out_packet, uint64_t& stale_counter) noexcept {
    clear_cl1_payload(out_packet);
    if (shm_ptr == nullptr) {
        return false;
    }

    const volatile AetherDspSyncPacket* vp = shm_ptr;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t s0 = read_atomic_u32(&vp->dsp_sequence_number);
        if (s0 == 0u || (s0 & 1u) != 0u) {
            _mm_pause();
            continue;
        }

        const float rms_db = vp->dsp_input_rms_db;
        std::array<float, 5> visemes{};
        for (size_t i = 0; i < 5; ++i) {
            visemes[i] = vp->dsp_viseme_weights[i];
        }
        const float rep_lat = vp->dsp_reported_latency_ms;
        const uint64_t write_qpc = vp->dsp_write_qpc_us;
        const uint32_t flags = vp->dsp_flags;

        MemoryBarrier();
        const uint32_t s1 = read_atomic_u32(&vp->dsp_sequence_number);
        if (s0 != s1) {
            _mm_pause();
            continue;
        }

        const bool stale =
            (write_qpc == 0ULL) || (now_qpc_us > write_qpc && (now_qpc_us - write_qpc) > kDspStaleThresholdUs);
        out_packet.dsp_sequence_number = s1;
        out_packet.dsp_input_rms_db = rms_db;
        out_packet.dsp_reported_latency_ms = rep_lat;
        out_packet.dsp_write_qpc_us = write_qpc;
        if (stale) {
            ++stale_counter;
            out_packet.dsp_flags = flags & ~0x05u;
            for (float& w : out_packet.dsp_viseme_weights) {
                w = 0.0f;
            }
            return false;
        }

        for (size_t i = 0; i < 5; ++i) {
            out_packet.dsp_viseme_weights[i] = visemes[i];
        }
        out_packet.dsp_flags = flags;
        return true;
    }

    return false;
}

void DspSyncBridge::apply_late_latch_visemes(const AetherDspSyncPacket& dsp, float mouth_owner_blend,
                                             float* morph_weights, uint32_t morph_count) noexcept {
    if (morph_weights == nullptr || morph_count == 0) {
        return;
    }

    const float owner = std::clamp(mouth_owner_blend, 0.0f, 1.0f);
    const bool active = ((dsp.dsp_flags & 0x01u) != 0u) && ((dsp.dsp_flags & 0x04u) != 0u);
    if (!active || owner <= 0.0f) {
        return;
    }

    const float arkit_scale = 1.0f - owner;
    const uint32_t arkit_limit = std::min<uint32_t>(morph_count, animation::kVisemeSlotOffset);
    for (uint32_t slot = 0; slot < arkit_limit; ++slot) {
        if (animation::ExpressionMixer::is_mouth_arkit_slot(slot)) {
            morph_weights[slot] *= arkit_scale;
        }
    }

    for (uint32_t v = 0; v < animation::kVisemeCount; ++v) {
        const uint32_t dst = animation::kVisemeSlotOffset + v;
        if (dst >= morph_count) {
            break;
        }
        const float dsp_w = std::clamp(dsp.dsp_viseme_weights[v], 0.0f, 1.0f);
        morph_weights[dst] = std::clamp(dsp_w * owner + morph_weights[dst] * arkit_scale, 0.0f, 1.0f);
    }
}

} // namespace aether::output
