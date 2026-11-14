#pragma once

#include "animation/vmc_pose.hpp"
#include "core/expected.hpp"
#include "output/osc_sender.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace aether::output {

/** Default VMC Protocol performer -> marionette port (vmc.info). */
inline constexpr uint16_t kVmcDefaultPort = 39539;

/** Unity `HumanBodyBones` name for an Appendix A humanoid bone (`nullptr` if out of range). */
[[nodiscard]] const char* vmc_bone_name(uint32_t bone) noexcept;

/** Big-endian OSC 1.0 message writer over a caller-owned buffer (zero heap allocations). */
class OscMessageWriter {
  public:
    OscMessageWriter(uint8_t* buf, size_t cap) noexcept : buf_(buf), cap_(cap) {}

    /** Starts a message with `address` and OSC type tags (e.g. `"sfffffff"`, without the leading comma). */
    void begin(const char* address, const char* type_tags) noexcept;
    void add_string(const char* s) noexcept;
    void add_float(float v) noexcept;
    void add_int(int32_t v) noexcept;

    /** Total byte length, or 0 if the buffer overflowed. */
    [[nodiscard]] size_t size() const noexcept { return ok_ ? len_ : 0U; }

  private:
    void put_padded_string(const char* s) noexcept;
    void put_u32(uint32_t v) noexcept;

    uint8_t* buf_{nullptr};
    size_t cap_{0};
    size_t len_{0};
    bool ok_{true};
};

/** `/VMC/Ext/Bone/Pos (s name, f px, f py, f pz, f qx, f qy, f qz, f qw)`. */
[[nodiscard]] size_t encode_vmc_bone_pos(const char* name, const float pos[3], const float rot_xyzw[4], uint8_t* out,
                                         size_t cap) noexcept;
/** `/VMC/Ext/Blend/Val (s name, f value)`. */
[[nodiscard]] size_t encode_vmc_blend_val(const char* name, float value, uint8_t* out, size_t cap) noexcept;

/** Appends OSC messages into `#bundle` datagrams of at most `kMaxDatagram` bytes (immediate time tag). */
class OscBundleBuilder {
  public:
    static constexpr size_t kMaxDatagram = 1400;

    void reset() noexcept;
    /** Returns false if `msg` does not fit; callers then flush and retry. */
    [[nodiscard]] bool append(const uint8_t* msg, size_t len) noexcept;
    [[nodiscard]] bool empty() const noexcept { return count_ == 0U; }
    [[nodiscard]] const uint8_t* data() const noexcept { return buf_.data(); }
    [[nodiscard]] size_t size() const noexcept { return len_; }

  private:
    std::array<uint8_t, kMaxDatagram> buf_{};
    size_t len_{0};
    uint32_t count_{0};
};

/**
 * VMC Protocol performer: sends `/VMC/Ext/OK`, `/VMC/Ext/T`, `/VMC/Ext/Root/Pos`, 55 x `/VMC/Ext/Bone/Pos`,
 * `/VMC/Ext/Blend/Val` (`A, I, U, E, O, Blink_L, Blink_R`) and `/VMC/Ext/Blend/Apply` as OSC bundles.
 */
class VmcSender {
  public:
    [[nodiscard]] Expected<void> init(const char* ipv4_host = "127.0.0.1", uint16_t port = kVmcDefaultPort);
    void shutdown() noexcept { osc_.shutdown(); }
    [[nodiscard]] bool is_open() const noexcept { return osc_.is_open(); }
    [[nodiscard]] uint64_t packets_sent() const noexcept { return osc_.packets_sent(); }

    /** Encodes one frame into bundles; with `sink == nullptr` they go to the socket. Returns datagram count. */
    uint32_t send_frame(const animation::VmcPoseFrame& frame,
                        void (*sink)(const uint8_t*, size_t, void*) = nullptr, void* sink_user = nullptr) noexcept;

  private:
    OscSender osc_{};
    OscBundleBuilder bundle_{};
};

} // namespace aether::output
