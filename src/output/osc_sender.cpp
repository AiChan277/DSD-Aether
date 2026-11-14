#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "output/osc_sender.hpp"

#include <cstring>

namespace aether::output {

namespace {

constexpr uint64_t kFourHzIntervalUs = 250'000ULL;

size_t write_osc_padded_str(const char* s, uint8_t* out_buf, size_t cap) noexcept {
    if (s == nullptr || out_buf == nullptr) {
        return 0;
    }
    const size_t slen = std::strlen(s);
    const size_t padded = (slen + 4u) & ~size_t(3u);
    if (padded == 0 || padded > cap) {
        return 0;
    }
    std::memcpy(out_buf, s, slen + 1u);
    if (padded > slen + 1u) {
        std::memset(out_buf + slen + 1u, 0, padded - (slen + 1u));
    }
    return padded;
}

size_t encode_osc_scalar32(const char* address, char type_tag, uint32_t host_u32, uint8_t* out_buf,
                           size_t cap) noexcept {
    const size_t addr_bytes = write_osc_padded_str(address, out_buf, cap);
    if (addr_bytes == 0 || addr_bytes + 8u > cap) {
        return 0;
    }
    out_buf[addr_bytes + 0] = ',';
    out_buf[addr_bytes + 1] = static_cast<uint8_t>(type_tag);
    out_buf[addr_bytes + 2] = 0;
    out_buf[addr_bytes + 3] = 0;

    const uint32_t net_u32 = htonl(host_u32);
    std::memcpy(out_buf + addr_bytes + 4u, &net_u32, sizeof(net_u32));
    return addr_bytes + 8u;
}

} // namespace

Expected<void> OscSender::init(const char* ipv4_host, uint16_t port) {
    shutdown();

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return Unexpected<Error>{Error::make(ErrorCode::IoError, "WSAStartup failed for OscSender")};
    }
    wsa_initialized_ = true;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        shutdown();
        return Unexpected<Error>{Error::make(ErrorCode::IoError, "UDP socket creation failed")};
    }

    u_long non_blocking = 1;
    (void)ioctlsocket(s, FIONBIO, &non_blocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const char* host = (ipv4_host != nullptr && ipv4_host[0] != '\0') ? ipv4_host : "127.0.0.1";
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        closesocket(s);
        shutdown();
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "Invalid IPv4 address for OscSender")};
    }

    static_assert(sizeof(sockaddr_in) <= 64);
    target_sockaddr_.fill(0);
    std::memcpy(target_sockaddr_.data(), &addr, sizeof(addr));
    target_sockaddr_len_ = static_cast<int>(sizeof(addr));
    socket_handle_ = static_cast<uint64_t>(s);
    last_4hz_us_ = 0;
    packets_sent_ = 0;
    return {};
}

void OscSender::shutdown() noexcept {
    if (socket_handle_ != UINT64_MAX) {
        closesocket(static_cast<SOCKET>(socket_handle_));
        socket_handle_ = UINT64_MAX;
    }
    if (wsa_initialized_) {
        WSACleanup();
        wsa_initialized_ = false;
    }
    target_sockaddr_.fill(0);
    target_sockaddr_len_ = 0;
    last_4hz_us_ = 0;
}

bool OscSender::send_latency_ms(float latency_ms) noexcept {
    std::array<uint8_t, 128> pkt{};
    const size_t n = encode_osc_float("/aether/telemetry/latency_ms", latency_ms, pkt.data(), pkt.size());
    return (n > 0) && send_raw(pkt.data(), n);
}

bool OscSender::maybe_send_rate_and_ep(uint64_t now_us, float fps, int32_t active_ep) noexcept {
    if (last_4hz_us_ != 0 && (now_us < last_4hz_us_ || (now_us - last_4hz_us_) < kFourHzIntervalUs)) {
        return false;
    }
    last_4hz_us_ = now_us;

    std::array<uint8_t, 128> pkt_fps{};
    std::array<uint8_t, 128> pkt_ep{};
    const size_t n_fps = encode_osc_float("/aether/telemetry/fps", fps, pkt_fps.data(), pkt_fps.size());
    const size_t n_ep = encode_osc_int("/aether/tracking/ep", active_ep, pkt_ep.data(), pkt_ep.size());
    const bool ok_fps = (n_fps > 0) && send_raw(pkt_fps.data(), n_fps);
    const bool ok_ep = (n_ep > 0) && send_raw(pkt_ep.data(), n_ep);
    return ok_fps && ok_ep;
}

size_t OscSender::encode_osc_float(const char* address, float value, uint8_t* out_buf, size_t cap) noexcept {
    uint32_t raw_bits = 0;
    static_assert(sizeof(float) == sizeof(uint32_t));
    std::memcpy(&raw_bits, &value, sizeof(raw_bits));
    return encode_osc_scalar32(address, 'f', raw_bits, out_buf, cap);
}

size_t OscSender::encode_osc_int(const char* address, int32_t value, uint8_t* out_buf, size_t cap) noexcept {
    uint32_t raw_bits = 0;
    std::memcpy(&raw_bits, &value, sizeof(raw_bits));
    return encode_osc_scalar32(address, 'i', raw_bits, out_buf, cap);
}

bool OscSender::send_raw(const uint8_t* data, size_t bytes) noexcept {
    if (socket_handle_ == UINT64_MAX || data == nullptr || bytes == 0 || target_sockaddr_len_ <= 0) {
        return false;
    }
    const int sent =
        sendto(static_cast<SOCKET>(socket_handle_), reinterpret_cast<const char*>(data), static_cast<int>(bytes), 0,
               reinterpret_cast<const sockaddr*>(target_sockaddr_.data()), target_sockaddr_len_);
    if (sent == static_cast<int>(bytes)) {
        ++packets_sent_;
        return true;
    }
    return false;
}

} // namespace aether::output
