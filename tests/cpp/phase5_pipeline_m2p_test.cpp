#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "aether_ffi_contract.h"
#include "app/pipeline_skeleton.hpp"
#include "core/clock.hpp"
#include "core/hot_path_guard.hpp"
#include "core/hw_enum.hpp"
#include "output/dsp_sync_bridge.hpp"
#include "output/output_config.hpp"
#include "output/output_ring.hpp"
#include "renderer/npr_renderer.hpp"
#include "test_png_io.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace {

void publish_mock_dsp_line1(AetherDspSyncPacket* pkt, uint32_t seq, float rms_db, float aa, float oh,
                            uint64_t qpc_us) noexcept {
    volatile AetherDspSyncPacket* vp = pkt;
    const uint32_t s_odd = (seq * 2u) - 1u;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&vp->dsp_sequence_number), static_cast<LONG>(s_odd));
    MemoryBarrier();
    vp->dsp_input_rms_db = rms_db;
    vp->dsp_viseme_weights[0] = aa;
    vp->dsp_viseme_weights[1] = 0.05f;
    vp->dsp_viseme_weights[2] = 0.10f;
    vp->dsp_viseme_weights[3] = 0.15f;
    vp->dsp_viseme_weights[4] = oh;
    vp->dsp_reported_latency_ms = 4.2f;
    vp->dsp_write_qpc_us = qpc_us;
    vp->dsp_flags = 0x05u; // active + voice
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&vp->dsp_sequence_number), static_cast<LONG>(s_odd + 1u));
}

} // namespace

TEST_CASE("P5-M2P-PIPELINE: Full T1->T2->T3(LateLatch+Passes1..12)->T4(Spout+VCam+DSP+OSC) M2P p95 <= 20.0 ms",
          "[phase5][m2p]") {
    aether::HotPathGuard::install();
    aether::HotPathGuard::reset_total_violations();

    WSADATA wsa{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    SOCKET osc_rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(osc_rx != INVALID_SOCKET);
    u_long nonblock = 1;
    ioctlsocket(osc_rx, FIONBIO, &nonblock);
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(0);
    inet_pton(AF_INET, "127.0.0.1", &bind_addr.sin_addr);
    REQUIRE(bind(osc_rx, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) == 0);
    sockaddr_in bound_addr{};
    int addr_len = sizeof(bound_addr);
    REQUIRE(getsockname(osc_rx, reinterpret_cast<sockaddr*>(&bound_addr), &addr_len) == 0);
    const uint16_t osc_port = ntohs(bound_addr.sin_port);

    aether::rhi::RhiDeviceConfig dcfg{};
    dcfg.headless = true;
    dcfg.enable_validation = true;
    dcfg.require_validation = true;
    auto dev_res = aether::rhi::RhiDevice::create(dcfg);
    REQUIRE(dev_res.has_value());
    std::unique_ptr<aether::rhi::RhiDevice> rhi(dev_res.value());

    auto av_res = aether::test::load_reference_vrm_avatar(*rhi);
    REQUIRE(av_res.has_value());
    auto avatar = std::move(av_res.value());

    aether::renderer::RendererConfig rcfg{};
    rcfg.width = 960;
    rcfg.height = 540;
    rcfg.output_width = 1920;
    rcfg.output_height = 1080;
    rcfg.enable_rt = rhi->rt_supported();

    aether::renderer::NprRenderer renderer;
    REQUIRE(renderer.init(*rhi, rcfg).has_value());
    REQUIRE(renderer.bind_avatar(avatar.get()).has_value());

    aether::output::OutputConfig out_cfg{};
    out_cfg.resolution = aether::output::OutputResolutionPreset::Fhd1080;
    out_cfg.width = 1920;
    out_cfg.height = 1080;
    out_cfg.fps = 60;
    out_cfg.premul_alpha = true;
    out_cfg.spout_enabled = true;
    out_cfg.spout_mode = aether::output::SpoutSharingMode::GpuDefault;
    std::snprintf(out_cfg.spout_sender_name, sizeof(out_cfg.spout_sender_name), "AetherM2PTest");
    out_cfg.vcam_enabled = true;
    out_cfg.ndi_enabled = false;
    out_cfg.osc_enabled = true;
    out_cfg.osc_port = osc_port;
    out_cfg.dsp_user_offset_ms = 0.0f;

    // Open DSP writer view BEFORE starting PipelineSkeleton so T2 & T3 see live DSP audio from frame 1.
    aether::output::DspSyncBridge dsp_writer;
    REQUIRE(dsp_writer.init().has_value());
    AetherDspSyncPacket* shm_pkt = dsp_writer.mapped_packet();
    REQUIRE(shm_pkt != nullptr);
    publish_mock_dsp_line1(shm_pkt, 1u, -16.5f, 0.80f, 0.35f, aether::now_us());

    const auto hw = aether::enumerate_hardware();
    aether::app::PipelineConfig pipe_cfg{};
    pipe_cfg.enable_live_tracking = false; // Deterministic synthetic T1 60 Hz capture -> real T2 SimEngine
    pipe_cfg.use_synthetic_camera = true;
    pipe_cfg.preferred_ep = -1;
    pipe_cfg.output_device = rhi.get();
    pipe_cfg.output_cfg = out_cfg;

    auto pipeline = std::make_unique<aether::app::PipelineSkeleton>();
    REQUIRE(pipeline->start(hw, pipe_cfg));

    const auto scenes = aether::test::make_canonical_scenes();
    aether::renderer::FrameRenderRequest req{};
    req.avatar = avatar.get();
    req.rig = &scenes[5].rig;
    req.camera = scenes[5].camera;
    req.rt_enabled = rcfg.enable_rt;
    req.premul_alpha = true;
    req.need_straight_alpha = false;
    req.need_bottom_up = false;

    constexpr uint32_t kWarmupFrames = 8;
    constexpr uint32_t kMeasuredFrames = 45;
    std::vector<double> m2p_samples_ms;
    m2p_samples_ms.reserve(kMeasuredFrames);

    uint64_t last_seen_t1_seq = 0;
    uint32_t dsp_seq = 2u;
    aether::HighResTimer timer;

    for (uint32_t f = 0; f < kWarmupFrames + kMeasuredFrames; ++f) {
        // Wait for the next T1->T2 published frame so M2P reflects T1 capture -> T4 completion cleanly
        aether::app::SkinningSnapshot skin_snap{};
        const uint64_t wait_deadline = aether::now_us() + 200'000ULL;
        while (aether::now_us() < wait_deadline) {
            publish_mock_dsp_line1(shm_pkt, dsp_seq++, -14.0f, 0.85f, 0.40f, aether::now_us());
            const auto& cur = pipeline->acquire_skinning_ref();
            if (cur.frame_sequence != 0ULL && cur.frame_sequence != last_seen_t1_seq) {
                skin_snap = cur;
                last_seen_t1_seq = cur.frame_sequence;
                break;
            }
            timer.sleep_us(250ULL);
        }
        REQUIRE(skin_snap.frame_sequence != 0ULL);

        const uint64_t t3_start = aether::now_us();
        const uint32_t slot_idx = f % aether::renderer::NprRenderer::kFrameSlots;
        req.frame_index = f;
        req.completed_frame_index =
            (f >= aether::renderer::NprRenderer::kFrameSlots) ? (f - aether::renderer::NprRenderer::kFrameSlots) : 0;
        req.frame_slot = slot_idx;
        req.bone_matrices = skin_snap.frame.bone_matrices;
        req.bone_count = skin_snap.frame.bone_count;

        uint64_t ll_us = 0;
        {
            aether::HotPathScope guard;
            const uint64_t ll_begin = aether::now_us();
            AetherDspSyncPacket t3_dsp{};
            const bool fresh_dsp = pipeline->try_read_latest_dsp(t3_dsp);
            std::array<float, aether::animation::kMaxMorphs> morphs{};
            const uint32_t mc = std::min<uint32_t>(skin_snap.frame.morph_count, aether::animation::kMaxMorphs);
            if (mc > 0) {
                std::memcpy(morphs.data(), skin_snap.frame.morph_weights, mc * sizeof(float));
            }
            if (fresh_dsp && ((t3_dsp.dsp_flags & 0x05u) == 0x05u)) {
                const float owner = std::clamp(skin_snap.frame.mouth_owner_blend, 0.0f, 1.0f);
                if (!skin_snap.dsp_attenuated_on_t2) {
                    aether::output::DspSyncBridge::apply_late_latch_visemes(t3_dsp, owner, morphs.data(), mc);
                } else {
                    for (uint32_t v = 0; v < aether::animation::kVisemeCount; ++v) {
                        const uint32_t dst = aether::animation::kVisemeSlotOffset + v;
                        if (dst >= mc) {
                            break;
                        }
                        morphs[dst] = std::clamp(t3_dsp.dsp_viseme_weights[v], 0.0f, 1.0f) * owner;
                    }
                }
            }
            avatar->update_pose(slot_idx, skin_snap.frame_sequence, skin_snap.frame.bone_matrices,
                                skin_snap.frame.bone_count, morphs.data(), mc, true);
            ll_us = aether::now_us() - ll_begin;
            REQUIRE(aether::HotPathGuard::thread_violations() == 0);
        }

        uint64_t tl_val = 0;
        REQUIRE(renderer.submit_critical_and_signal(req, tl_val).has_value());
        const uint64_t t3_submit = aether::now_us();

        const auto gout = renderer.graph().outputs();
        aether::output::OutputRingSlot slot{};
        slot.frame_sequence = skin_snap.frame_sequence;
        slot.frame_timeline_value = tl_val;
        slot.output_slot = gout.output_slot;
        slot.width = gout.output_width;
        slot.height = gout.output_height;
        slot.fps = 60u;
        slot.bgra_image = gout.bgra8_image;
        slot.bgra_straight_image = gout.bgra8_straight_image;
        slot.bottom_up_image = gout.bottom_up_bgra8_image;
        slot.nv12_y_image = gout.nv12_y_image;
        slot.nv12_uv_image = gout.nv12_uv_image;
        slot.has_bottom_up = false;
        slot.tracking_active = true;
        slot.t1_capture_us = skin_snap.t1_capture_us;
        slot.t2_start_us = skin_snap.t2_start_us;
        slot.t2_done_us = skin_snap.t2_done_us;
        slot.t3_start_us = t3_start;
        slot.t3_submit_us = t3_submit;
        slot.ui_cpu_us = 120ULL;
        slot.late_latch_sample_to_write_us = ll_us;
        slot.active_ep = skin_snap.frame.active_ep;
        pipeline->submit_output_slot(slot);

        const uint64_t drain_t0 = aether::now_us();
        const bool drained = pipeline->wait_output_slots_drained(f == 0u ? 1000u : 250u);
        const double drain_ms = static_cast<double>(aether::now_us() - drain_t0) / 1000.0;
        INFO("frame=" << f << " drain_ms=" << drain_ms
                      << " spout_active=" << pipeline->latest_output_status().spout_active);
        REQUIRE(drained);
        const auto telem = pipeline->latest_telemetry();
        if (f >= kWarmupFrames && telem.last_frame.frame_sequence == skin_snap.frame_sequence) {
            m2p_samples_ms.push_back(static_cast<double>(telem.last_frame.motion_to_photon_ms()));
        }
    }

    const auto out_st = pipeline->latest_output_status();
    REQUIRE(out_st.spout_active);
    REQUIRE(out_st.spout_frames_published >= kMeasuredFrames);
    REQUIRE(out_st.vcam_active);
    REQUIRE(out_st.vcam_frames_published >= kMeasuredFrames);
    REQUIRE(out_st.osc_active);
    REQUIRE(out_st.osc_packets_sent >= kMeasuredFrames);
    REQUIRE(out_st.dsp_connected);
    REQUIRE(out_st.dsp_voice_active);

    int udp_rx_count = 0;
    std::array<uint8_t, 512> udp_buf{};
    while (recvfrom(osc_rx, reinterpret_cast<char*>(udp_buf.data()), static_cast<int>(udp_buf.size()), 0, nullptr,
                    nullptr) > 0) {
        ++udp_rx_count;
    }
    REQUIRE(udp_rx_count >= static_cast<int>(kMeasuredFrames));

    REQUIRE(m2p_samples_ms.size() >= kMeasuredFrames - 2u);
    std::sort(m2p_samples_ms.begin(), m2p_samples_ms.end());
    const double m2p_p50 = m2p_samples_ms[m2p_samples_ms.size() * 50u / 100u];
    const double m2p_p95 = m2p_samples_ms[m2p_samples_ms.size() * 95u / 100u];
    const double m2p_max = m2p_samples_ms.back();
    std::printf(
        "[P5-M2P-PIPELINE] 1080p60 RT=%s p50=%.3f ms p95=%.3f ms max=%.3f ms (spout=%.3f ms vcam=%.3f ms ll=%llu us)\n",
        rcfg.enable_rt ? "ON" : "OFF", m2p_p50, m2p_p95, m2p_max, out_st.spout_publish_ms, out_st.vcam_dma_ms,
        static_cast<unsigned long long>(out_st.late_latch_us));
    std::fflush(stdout);
    INFO("Phase 5 End-to-End 1080p M2P (validation=" << (rhi->validation_error_count() == 0 ? "clean" : "err")
                                                     << "): p50=" << m2p_p50 << " ms, p95=" << m2p_p95
                                                     << " ms, max=" << m2p_max << " ms");

#if defined(NDEBUG)
    REQUIRE(m2p_p95 <= 20.0);
#else
    // Debug builds run unoptimized CPU/VK-SyncValidation instrumentation; ensure hard bound check holds cleanly too
    REQUIRE(m2p_p95 <= 35.0);
#endif
#if !defined(__SANITIZE_ADDRESS__)
    // Under ASan + GPU-Assisted Validation, __sanitizer_install_malloc_and_free_hooks intercepts internal
    // malloc() inside VkLayer_khronos_validation.dll on T4 Vulkan submits; Debug CRT checks app allocations.
    REQUIRE(aether::HotPathGuard::total_violations() == 0);
#endif
    REQUIRE(rhi->validation_error_count() == 0u);

    pipeline->stop();
    pipeline.reset();
    dsp_writer.shutdown();
    renderer.destroy();
    avatar->destroy();
    rhi->destroy();
    closesocket(osc_rx);
    WSACleanup();
}
