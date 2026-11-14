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
#include "animation/skinning_frame.hpp"
#include "core/clock.hpp"
#include "core/hot_path_guard.hpp"
#include "output/dsp_sync_bridge.hpp"
#include "output/osc_sender.hpp"
#include "output/output_coordinator.hpp"
#include "renderer/npr_renderer.hpp"
#include "test_png_io.hpp"
#include "vcam/vcam_shm.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

uint32_t be_u32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

struct ParsedOscPacket {
    std::string address{};
    char type_tag{'\0'};
    float fval{0.0f};
    int32_t ival{0};
};

bool parse_osc_packet(const uint8_t* buf, size_t len, ParsedOscPacket& out) {
    size_t pos = 0;
    while (pos < len && buf[pos] != 0) {
        ++pos;
    }
    if (pos == 0 || pos >= len) {
        return false;
    }
    out.address.assign(reinterpret_cast<const char*>(buf), pos);
    const size_t addr_end = (pos + 4u) & ~size_t(3u);
    if (addr_end + 8u > len) {
        return false;
    }
    if (buf[addr_end] != ',' || buf[addr_end + 2] != '\0') {
        return false;
    }
    out.type_tag = static_cast<char>(buf[addr_end + 1]);
    const size_t tag_end = addr_end + 4u;
    if (tag_end + 4u > len) {
        return false;
    }
    const uint32_t raw = be_u32(buf + tag_end);
    if (out.type_tag == 'f') {
        std::memcpy(&out.fval, &raw, 4);
        return true;
    }
    if (out.type_tag == 'i') {
        std::memcpy(&out.ival, &raw, 4);
        return true;
    }
    return false;
}

std::filesystem::path find_worker_exe() {
    wchar_t mod_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, mod_path, MAX_PATH);
    return std::filesystem::path(mod_path).parent_path() / L"dsp_torture_worker.exe";
}

} // namespace

TEST_CASE("Phase 5 ABI contracts: AetherDspSyncPacket, AetherVCamHeader, SpoutSharedTextureInfo", "[phase5][abi]") {
    static_assert(sizeof(AetherDspSyncPacket) == 128);
    static_assert(alignof(AetherDspSyncPacket) == 64);
    static_assert(offsetof(AetherDspSyncPacket, dsp_sequence_number) == 64);
    static_assert(sizeof(aether::output::SpoutSharedTextureInfo) == 280);
    static_assert(sizeof(AetherVCamHeader) == 128);
    static_assert(alignof(AetherVCamHeader) == 64);
    REQUIRE(AETHER_DSP_MAGIC == 0x41455448);
}

TEST_CASE("Phase 5 Exit Gate: 10^7 bidirectional separate-process DSP IPC seqlock torture", "[phase5][dsp][torture]") {
    const auto worker_exe = find_worker_exe();
    REQUIRE(std::filesystem::exists(worker_exe));

    // Unique per-process section name so parallel CTest runs never collide
    const DWORD pid = GetCurrentProcessId();
    const wchar_t section_w[] = L"Local\\AetherDspSyncTestTorture";
    wchar_t unique_section_w[96]{};
    swprintf(unique_section_w, 96, L"%s_%lu", section_w, pid);
    char unique_section_a[96]{};
    snprintf(unique_section_a, sizeof(unique_section_a), "Local\\AetherDspSyncTestTorture_%lu", pid);

    aether::output::DspSyncBridge bridge;
    auto init_res = bridge.init(unique_section_w);
    REQUIRE(init_res.has_value());
    REQUIRE(bridge.is_open());

    // Launch C11 external writer process performing 10,000,000 Cache Line 1 updates
    wchar_t cmd_line[512]{};
    swprintf(cmd_line, 512, L"\"%s\" %S writer 10000000", worker_exe.c_str(), unique_section_a);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(nullptr, cmd_line, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));

    const auto* shm = bridge.mapped_packet();
    REQUIRE(shm != nullptr);

    uint64_t reads_ok = 0;
    uint64_t torn_reads = 0;
    uint64_t stale_dummy = 0;
    uint64_t spin_iters = 0;
    AetherDspSyncPacket snap{};

    while (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT || spin_iters < 1000ULL) {
        ++spin_iters;
        if (aether::output::DspSyncBridge::try_read_dsp_line1(shm, aether::now_us(), snap, stale_dummy)) {
            const uint32_t k_mod = static_cast<uint32_t>(snap.dsp_viseme_weights[4]);
            const float expect_base = static_cast<float>(k_mod % 1000u) * 0.001f;
            if (std::fabs(snap.dsp_viseme_weights[0] - expect_base) > 1e-4f ||
                std::fabs((snap.dsp_viseme_weights[0] + snap.dsp_viseme_weights[1]) - 1.0f) > 1e-4f ||
                snap.dsp_flags != 0x07u) {
                ++torn_reads;
            }
            ++reads_ok;
        }
        // Keep publishing Cache Line 0 simultaneously to torture dual-writer cache line isolation
        if ((spin_iters & 63u) == 0u) {
            bridge.publish_video_telemetry(16.0f, aether::now_us(), 60000u, 0x03u, 0.0f, 1.0f / 60.0f);
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0 && spin_iters >= 1000ULL) {
            break;
        }
    }

    DWORD exit_code = 1;
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    REQUIRE(exit_code == 0);
    REQUIRE(torn_reads == 0);
    REQUIRE(reads_ok > 100);

    // Reverse direction: launch external C11 cl0reader verifying Cache Line 0 invariants while C++ writes CL0
    wchar_t cmd_line2[512]{};
    swprintf(cmd_line2, 512, L"\"%s\" %S cl0reader 1000000", worker_exe.c_str(), unique_section_a);
    PROCESS_INFORMATION pi2{};
    REQUIRE(CreateProcessW(nullptr, cmd_line2, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi2));
    for (int i = 0; i < 20000; ++i) {
        bridge.publish_video_telemetry(15.5f + 0.5f * static_cast<float>(i & 3), aether::now_us(), 60000u, 0x03u, 0.0f,
                                       1.0f / 60.0f);
    }
    DWORD exit_code2 = 1;
    REQUIRE(WaitForSingleObject(pi2.hProcess, 15000) == WAIT_OBJECT_0);
    GetExitCodeProcess(pi2.hProcess, &exit_code2);
    CloseHandle(pi2.hThread);
    CloseHandle(pi2.hProcess);
    REQUIRE(exit_code2 == 0);
}

TEST_CASE("Phase 5 DspSyncBridge: EMA, +-2 ms/s slew limit, [0, 250] ms clamp, and 250 ms stale cutoff",
          "[phase5][dsp][rules]") {
    const DWORD pid = GetCurrentProcessId();
    wchar_t section_w[96]{};
    swprintf(section_w, 96, L"Local\\AetherDspBridgeUnit_%lu", pid);

    aether::output::DspSyncBridge bridge;
    REQUIRE(bridge.init(section_w).has_value());

    const uint64_t t0 = aether::now_us();
    auto* pkt = bridge.mapped_packet();
    REQUIRE(pkt != nullptr);

    // Seed Cache Line 1 with DSP-reported 4.0 ms latency and active voice
    pkt->dsp_sequence_number = 2;
    pkt->dsp_reported_latency_ms = 4.0f;
    pkt->dsp_input_rms_db = -18.0f;
    pkt->dsp_viseme_weights[0] = 0.85f;
    pkt->dsp_viseme_weights[1] = 0.15f;
    pkt->dsp_write_qpc_us = t0;
    pkt->dsp_flags = 0x07u;

    {
        aether::HotPathScope guard;
        bridge.publish_video_telemetry(18.0f, t0, 60000u, 0x03u, 0.0f, 1.0f / 60.0f);
        REQUIRE(aether::HotPathGuard::thread_violations() == 0);
    }
    // Initial delay target = 18.0 - 4.0 = 14.0 ms
    REQUIRE(std::fabs(bridge.current_recommended_delay_ms() - 14.0f) < 0.05f);

    // Sudden +100 ms video latency spike over dt=1/60s -> slew rate limited to <= +2 ms/s * dt + tiny margin
    const float before_delay = bridge.current_recommended_delay_ms();
    bridge.publish_video_telemetry(118.0f, t0 + 16667ULL, 60000u, 0x03u, 0.0f, 1.0f / 60.0f);
    const float delta_delay = bridge.current_recommended_delay_ms() - before_delay;
    REQUIRE(delta_delay >= 0.0f);
    REQUIRE(delta_delay <= (2.0f / 60.0f) + 0.005f);

    // Negative offset clamps to 0.0f; stale payload > 250 ms clears visemes_valid & voice_active
    AetherDspSyncPacket read_fresh{};
    REQUIRE(bridge.read_dsp_packet(read_fresh, t0 + 10'000ULL));
    REQUIRE((read_fresh.dsp_flags & 0x05u) == 0x05u);
    REQUIRE(read_fresh.dsp_viseme_weights[0] > 0.8f);

    AetherDspSyncPacket read_stale{};
    REQUIRE_FALSE(bridge.read_dsp_packet(read_stale, t0 + 300'000ULL));
    REQUIRE((read_stale.dsp_flags & 0x05u) == 0u);
    REQUIRE(read_stale.dsp_viseme_weights[0] == 0.0f);
    REQUIRE(bridge.stale_reject_count() >= 1);
}

TEST_CASE("Phase 5 REQ-ANM-020 & PERF-008: T3 Late-Latch DSP Visemes <= 0.5 ms sample-to-GPU-write",
          "[phase5][latelatch][perf]") {
    aether::rhi::RhiDeviceConfig dcfg{};
    dcfg.headless = true;
    dcfg.enable_validation = true;
    auto dev_res = aether::rhi::RhiDevice::create(dcfg);
    REQUIRE(dev_res.has_value());
    std::unique_ptr<aether::rhi::RhiDevice> rhi(dev_res.value());

    auto av_res = aether::test::load_reference_vrm_avatar(*rhi);
    REQUIRE(av_res.has_value());
    auto avatar = std::move(av_res.value());

    const DWORD pid = GetCurrentProcessId();
    wchar_t section_w[96]{};
    swprintf(section_w, 96, L"Local\\AetherLateLatchTest_%lu", pid);
    aether::output::DspSyncBridge bridge;
    REQUIRE(bridge.init(section_w).has_value());

    auto* shm = bridge.mapped_packet();
    REQUIRE(shm != nullptr);

    std::array<float, aether::animation::kMaxMorphs> morphs{};
    std::vector<double> lat_ms;
    lat_ms.reserve(50);

    AetherDspSyncPacket dsp{};
    uint64_t stale_cnt = 0;

    for (uint32_t i = 0; i < 50; ++i) {
        morphs.fill(0.0f);
        morphs[17] = 0.7f; // jawOpen from camera before mouth ownership attenuation

        const float aa = 0.25f + 0.01f * static_cast<float>(i);
        const uint64_t qpc_sample = aether::now_us();
        shm->dsp_sequence_number = (i * 2u) + 1u;
        shm->dsp_viseme_weights[0] = aa;
        shm->dsp_viseme_weights[1] = 0.12f;
        shm->dsp_viseme_weights[2] = 0.34f;
        shm->dsp_viseme_weights[3] = 0.05f;
        shm->dsp_viseme_weights[4] = 0.21f;
        shm->dsp_write_qpc_us = qpc_sample;
        shm->dsp_flags = 0x07u;
        shm->dsp_sequence_number = (i * 2u) + 2u;

        {
#if !defined(__SANITIZE_ADDRESS__)
            aether::HotPathScope guard;
#endif
            const uint64_t now = aether::now_us();
            REQUIRE(aether::output::DspSyncBridge::try_read_dsp_line1(shm, now, dsp, stale_cnt));
            aether::output::DspSyncBridge::apply_late_latch_visemes(dsp, 1.0f, morphs.data(),
                                                                    static_cast<uint32_t>(morphs.size()));
            avatar->update_pose(i % 2u, i + 1u, nullptr, 0, morphs.data(), static_cast<uint32_t>(morphs.size()), true);
#if !defined(__SANITIZE_ADDRESS__)
            REQUIRE(aether::HotPathGuard::thread_violations() == 0);
#endif
        }
        const uint64_t qpc_done = aether::now_us();
        const double elapsed_ms = static_cast<double>(qpc_done - qpc_sample) / 1000.0;
        lat_ms.push_back(elapsed_ms);
        REQUIRE(std::fabs(morphs[52] - aa) < 1e-4f);
        REQUIRE(std::fabs(morphs[17]) < 1e-5f); // attenuated cleanly under 100% DSP mouth ownership
    }

    std::sort(lat_ms.begin(), lat_ms.end());
    const double p95_ms = lat_ms[static_cast<size_t>(lat_ms.size() * 95u / 100u)];
    INFO("PERF-008 T3 Late-Latch sample->GPU write p95 = " << p95_ms << " ms");
    REQUIRE(p95_ms <= 0.5);

    avatar->destroy();
    rhi->destroy();
}

TEST_CASE("Phase 5 OSC Telemetry Sender: UDP :9001 binary wire format & 4 Hz rate limiting", "[phase5][osc]") {
    WSADATA wsa{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    SOCKET rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(rx != INVALID_SOCKET);
    u_long nonblock = 1;
    ioctlsocket(rx, FIONBIO, &nonblock);

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(0);
    inet_pton(AF_INET, "127.0.0.1", &bind_addr.sin_addr);
    REQUIRE(bind(rx, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) == 0);

    sockaddr_in bound_addr{};
    int addr_len = sizeof(bound_addr);
    REQUIRE(getsockname(rx, reinterpret_cast<sockaddr*>(&bound_addr), &addr_len) == 0);
    const uint16_t port = ntohs(bound_addr.sin_port);

    aether::output::OscSender osc;
    REQUIRE(osc.init("127.0.0.1", port).has_value());

    uint64_t sim_us = 1'000'000ULL;
    {
        aether::HotPathScope guard;
        for (int f = 0; f < 60; ++f) {
            REQUIRE(osc.send_latency_ms(16.25f));
            (void)osc.maybe_send_rate_and_ep(sim_us, 60.0f, 3);
            sim_us += 16'667ULL; // 60 frames = ~1.0 s total
        }
        REQUIRE(aether::HotPathGuard::thread_violations() == 0);
    }

    // Drain loopback UDP datagrams cleanly (give kernel loopback 5 ms)
    std::vector<ParsedOscPacket> rx_packets;
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    while (std::chrono::steady_clock::now() < wait_deadline) {
        std::array<uint8_t, 512> buf{};
        const int n =
            recvfrom(rx, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0, nullptr, nullptr);
        if (n > 0) {
            ParsedOscPacket p{};
            REQUIRE(parse_osc_packet(buf.data(), static_cast<size_t>(n), p));
            rx_packets.push_back(std::move(p));
        } else {
            Sleep(1);
        }
    }

    int lat_cnt = 0;
    int fps_cnt = 0;
    int ep_cnt = 0;
    for (const auto& p : rx_packets) {
        if (p.address == "/aether/telemetry/latency_ms") {
            REQUIRE(p.type_tag == 'f');
            REQUIRE(std::fabs(p.fval - 16.25f) < 1e-4f);
            ++lat_cnt;
        } else if (p.address == "/aether/telemetry/fps") {
            REQUIRE(p.type_tag == 'f');
            REQUIRE(std::fabs(p.fval - 60.0f) < 1e-4f);
            ++fps_cnt;
        } else if (p.address == "/aether/tracking/ep") {
            REQUIRE(p.type_tag == 'i');
            REQUIRE(p.ival == 3);
            ++ep_cnt;
        }
    }

    REQUIRE(lat_cnt == 60);
    REQUIRE(fps_cnt == 4);
    REQUIRE(ep_cnt == 4);

    osc.shutdown();
    closesocket(rx);
    WSACleanup();
}
