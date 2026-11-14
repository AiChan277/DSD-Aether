#include "output/output_coordinator.hpp"

#include "core/clock.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace aether::output {

namespace {

OutputConfig normalize_config(OutputConfig cfg) noexcept {
    if (cfg.width == 0u) {
        cfg.width = output_preset_width(cfg.resolution);
    }
    if (cfg.height == 0u) {
        cfg.height = output_preset_height(cfg.resolution);
    }
    cfg.fps = clamp_output_fps_for_resolution(cfg.width, cfg.height, cfg.fps);
    return cfg;
}

} // namespace

Expected<void> OutputCoordinator::init(rhi::RhiDevice& device, const OutputConfig& cfg) {
    shutdown();

    device_ = &device;
    cfg_ = normalize_config(cfg);

    if (auto r = dsp_.init(); !r) {
        AETHER_LOG_WARN("output", "DspSyncBridge init failed: %s", r.error().message.data());
    }
    if (cfg_.osc_enabled) {
        if (auto r = osc_.init(cfg_.osc_host, cfg_.osc_port); !r) {
            AETHER_LOG_WARN("output", "OscSender init failed: %s", r.error().message.data());
        }
    }
    if (cfg_.spout_enabled) {
        if (auto r = spout_.init(device, cfg_.width, cfg_.height, cfg_.spout_sender_name, cfg_.spout_mode); !r) {
            AETHER_LOG_WARN("output", "SpoutOutput init failed: %s", r.error().message.data());
        }
    }
    if (cfg_.vcam_enabled) {
        if (auto r = vcam_.init(device); !r) {
            AETHER_LOG_WARN("output", "VcamTransport init failed: %s", r.error().message.data());
        }
    }
    if (cfg_.ndi_enabled && NdiOutput::is_runtime_dll_present()) {
        if (auto r = ndi_.init(device, cfg_.width, cfg_.height, cfg_.ndi_format); !r) {
            AETHER_LOG_WARN("output", "NdiOutput init failed: %s", r.error().message.data());
        }
    }

    last_process_qpc_us_ = now_us();
    initialized_ = true;
    return {};
}

void OutputCoordinator::shutdown() noexcept {
    ndi_.shutdown();
    vcam_.shutdown();
    spout_.shutdown();
    osc_.shutdown();
    dsp_.shutdown();
    status_ = OutputRuntimeStatus{};
    last_process_qpc_us_ = 0u;
    initialized_ = false;
    device_ = nullptr;
}

void OutputCoordinator::apply_config(const OutputConfig& cfg) noexcept {
    const OutputConfig next = normalize_config(cfg);
    if (!initialized_ || device_ == nullptr) {
        cfg_ = next;
        return;
    }

    const bool dims_changed = (next.width != cfg_.width || next.height != cfg_.height);

    // Spout2 reconfig
    const bool spout_name_changed =
        std::strncmp(next.spout_sender_name, cfg_.spout_sender_name, sizeof(next.spout_sender_name)) != 0;
    if (!next.spout_enabled) {
        spout_.shutdown();
    } else if (!spout_.is_active() || spout_name_changed || next.spout_mode != cfg_.spout_mode) {
        (void)spout_.init(*device_, next.width, next.height, next.spout_sender_name, next.spout_mode);
    } else if (dims_changed) {
        (void)spout_.resize(next.width, next.height);
    }

    // VCam reconfig
    if (!next.vcam_enabled) {
        vcam_.shutdown();
    } else if (!vcam_.is_open()) {
        (void)vcam_.init(*device_);
    }

    // NDI reconfig
    if (!next.ndi_enabled) {
        ndi_.shutdown();
    } else if ((!ndi_.is_active() || next.ndi_format != cfg_.ndi_format) && NdiOutput::is_runtime_dll_present()) {
        (void)ndi_.init(*device_, next.width, next.height, next.ndi_format);
    } else if (ndi_.is_active() && dims_changed) {
        (void)ndi_.resize(next.width, next.height);
    }

    // OSC reconfig
    const bool osc_target_changed =
        std::strncmp(next.osc_host, cfg_.osc_host, sizeof(next.osc_host)) != 0 || (next.osc_port != cfg_.osc_port);
    if (!next.osc_enabled) {
        osc_.shutdown();
    } else if (!osc_.is_open() || osc_target_changed) {
        (void)osc_.init(next.osc_host, next.osc_port);
    }

    cfg_ = next;
}

uint64_t OutputCoordinator::process_slot(const OutputRingSlot& slot, float observed_fps) noexcept {
    if (!initialized_ || device_ == nullptr) {
        return 0u;
    }

    if (slot.frame_timeline_value > 0u && device_->frame_timeline() != VK_NULL_HANDLE) {
        const VkSemaphore sem = device_->frame_timeline();
        VkSemaphoreWaitInfo swi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        swi.semaphoreCount = 1u;
        swi.pSemaphores = &sem;
        swi.pValues = &slot.frame_timeline_value;
        (void)vkWaitSemaphores(device_->device(), &swi, 1'000'000'000ULL);
    }

    const uint64_t publish_qpc_us = now_us();

    if (cfg_.spout_enabled && spout_.is_active() && slot.bgra_image != VK_NULL_HANDLE) {
        (void)spout_.publish_frame(slot.bgra_image, slot.width, slot.height, 0u);
    }

    if (cfg_.vcam_enabled && vcam_.is_open()) {
        const VkImage vcam_bgra =
            (slot.has_bottom_up && slot.bottom_up_image != VK_NULL_HANDLE) ? slot.bottom_up_image : slot.bgra_image;
        (void)vcam_.publish_frame(vcam_bgra, slot.nv12_y_image, slot.nv12_uv_image, slot.width, slot.height, slot.fps,
                                  slot.has_bottom_up, slot.frame_sequence, publish_qpc_us, 0u);
    }

    if (cfg_.ndi_enabled && ndi_.is_active()) {
        const VkImage ndi_src =
            (slot.bgra_straight_image != VK_NULL_HANDLE) ? slot.bgra_straight_image : slot.bgra_image;
        if (ndi_src != VK_NULL_HANDLE) {
            (void)ndi_.send_frame(ndi_src, slot.width, slot.height, slot.fps, publish_qpc_us, 0u);
        }
    }

    const uint64_t done_qpc_us = now_us();
    const float dt_sec = (last_process_qpc_us_ > 0u && done_qpc_us > last_process_qpc_us_)
                             ? std::clamp(static_cast<float>(done_qpc_us - last_process_qpc_us_) * 1e-6f, 0.001f, 0.1f)
                             : (1.0f / static_cast<float>(std::max(slot.fps, 1u)));
    last_process_qpc_us_ = done_qpc_us;

    const float video_latency_ms = (slot.t3_start_us > 0u && done_qpc_us > slot.t3_start_us)
                                       ? static_cast<float>(done_qpc_us - slot.t3_start_us) * 0.001f
                                       : 0.5f;
    const float m2p_ms = (slot.t1_capture_us > 0u && done_qpc_us > slot.t1_capture_us)
                             ? static_cast<float>(done_qpc_us - slot.t1_capture_us) * 0.001f
                             : video_latency_ms;

    uint32_t video_flags = 0x01u;
    if (spout_.is_active()) {
        video_flags |= 0x02u;
    }
    if (vcam_.is_rgb_pin_connected()) {
        video_flags |= 0x04u;
    }
    if (slot.tracking_active) {
        video_flags |= 0x08u;
    }

    if (dsp_.is_open()) {
        dsp_.publish_video_telemetry(video_latency_ms, done_qpc_us, slot.fps * 1000u, video_flags,
                                     cfg_.dsp_user_offset_ms, dt_sec);
    }

    AetherDspSyncPacket dsp_pkt{};
    const bool dsp_ok = dsp_.is_open() ? dsp_.read_dsp_packet(dsp_pkt, done_qpc_us) : false;

    if (cfg_.osc_enabled && osc_.is_open()) {
        (void)osc_.send_latency_ms(m2p_ms);
        (void)osc_.maybe_send_rate_and_ep(done_qpc_us, observed_fps, static_cast<int32_t>(slot.active_ep));
    }

    status_.spout_active = spout_.is_active();
    status_.spout_using_cpu_fallback = spout_.using_cpu_fallback();
    status_.spout_adapter_mismatch = spout_.adapter_mismatch_detected();
    status_.spout_frames_published = spout_.frames_published();
    status_.spout_skipped_mutex = spout_.skipped_frames();
    status_.spout_publish_ms = spout_.last_publish_ms();

    status_.vcam_active = vcam_.is_open();
    status_.vcam_using_ext_host_mem = vcam_.using_external_host_mem();
    status_.vcam_rgb_pin_connected = vcam_.is_rgb_pin_connected();
    status_.vcam_frames_published = vcam_.frames_published();
    status_.vcam_dma_ms = vcam_.last_dma_ms();

    status_.ndi_active = ndi_.is_active();
    status_.ndi_runtime_available = NdiOutput::is_runtime_dll_present();
    status_.ndi_frames_sent = ndi_.frames_sent();
    status_.ndi_send_ms = ndi_.last_send_ms();

    status_.osc_active = osc_.is_open();
    status_.osc_packets_sent = osc_.packets_sent();

    status_.dsp_connected = dsp_ok && ((dsp_pkt.dsp_flags & 0x01u) != 0u);
    status_.dsp_voice_active = dsp_ok && ((dsp_pkt.dsp_flags & 0x04u) != 0u);
    status_.dsp_input_rms_db = dsp_ok ? dsp_pkt.dsp_input_rms_db : -96.0f;
    status_.dsp_reported_latency_ms = dsp_ok ? dsp_pkt.dsp_reported_latency_ms : 0.0f;
    status_.dsp_recommended_delay_ms = dsp_.current_recommended_delay_ms();
    status_.video_latency_ms = dsp_.current_ema_latency_ms();
    status_.latency_jitter_ms = dsp_.current_jitter_ms();
    status_.dsp_stale_count = dsp_.stale_reject_count();
    status_.late_latch_us = slot.late_latch_sample_to_write_us;

    return done_qpc_us;
}

} // namespace aether::output
