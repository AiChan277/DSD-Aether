#pragma once

#include "core/expected.hpp"

#include <array>
#include <cstdint>

namespace aether::output {

/**
 * Zero-alloc non-blocking WinSock2 UDP OSC 1.0 sender (`§7.8`, Appendix D).
 * Target default: `127.0.0.1:9001`:
 * - `/aether/telemetry/latency_ms` (f32, every frame)
 * - `/aether/telemetry/fps`        (f32, 4 Hz)
 * - `/aether/tracking/ep`          (i32, 4 Hz)
 */
class OscSender {
  public:
    OscSender() = default;
    ~OscSender() { shutdown(); }

    OscSender(const OscSender&) = delete;
    OscSender& operator=(const OscSender&) = delete;

    [[nodiscard]] Expected<void> init(const char* ipv4_host = "127.0.0.1", uint16_t port = 9001);
    void shutdown() noexcept;

    /** Sends `/aether/telemetry/latency_ms` (`f32`) with zero heap allocations (`HR-01`). */
    bool send_latency_ms(float latency_ms) noexcept;

    /** Sends `/aether/telemetry/fps` (`f32`) and `/aether/tracking/ep` (`i32`) at 4 Hz (`now_us`). */
    bool maybe_send_rate_and_ep(uint64_t now_us, float fps, int32_t active_ep) noexcept;

    /** Formats an OSC 1.0 float32 or int32 packet into caller's stack buffer; returns byte count (0 on overflow). */
    [[nodiscard]] static size_t encode_osc_float(const char* address, float value, uint8_t* out_buf,
                                                 size_t cap) noexcept;
    [[nodiscard]] static size_t encode_osc_int(const char* address, int32_t value, uint8_t* out_buf,
                                               size_t cap) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return socket_handle_ != UINT64_MAX; }
    [[nodiscard]] uint64_t packets_sent() const noexcept { return packets_sent_; }

  private:
    bool send_raw(const uint8_t* data, size_t bytes) noexcept;

    uint64_t socket_handle_{UINT64_MAX};
    bool wsa_initialized_{false};
    std::array<uint8_t, 64> target_sockaddr_{};
    int target_sockaddr_len_{0};
    uint64_t last_4hz_us_{0};
    uint64_t packets_sent_{0};
};

} // namespace aether::output
