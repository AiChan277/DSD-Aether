#include "output/vmc_sender.hpp"

#include <cstring>

namespace aether::output {

namespace {

constexpr std::array<const char*, AETHER_HUMANOID_BONE_COUNT> kVmcBoneNames = {
    "Hips",
    "Spine",
    "Chest",
    "UpperChest",
    "Neck",
    "Head",
    "LeftEye",
    "RightEye",
    "Jaw",
    "LeftUpperLeg",
    "LeftLowerLeg",
    "LeftFoot",
    "LeftToes",
    "RightUpperLeg",
    "RightLowerLeg",
    "RightFoot",
    "RightToes",
    "LeftShoulder",
    "LeftUpperArm",
    "LeftLowerArm",
    "LeftHand",
    "RightShoulder",
    "RightUpperArm",
    "RightLowerArm",
    "RightHand",
    "LeftThumbProximal",
    "LeftThumbIntermediate",
    "LeftThumbDistal",
    "LeftIndexProximal",
    "LeftIndexIntermediate",
    "LeftIndexDistal",
    "LeftMiddleProximal",
    "LeftMiddleIntermediate",
    "LeftMiddleDistal",
    "LeftRingProximal",
    "LeftRingIntermediate",
    "LeftRingDistal",
    "LeftLittleProximal",
    "LeftLittleIntermediate",
    "LeftLittleDistal",
    "RightThumbProximal",
    "RightThumbIntermediate",
    "RightThumbDistal",
    "RightIndexProximal",
    "RightIndexIntermediate",
    "RightIndexDistal",
    "RightMiddleProximal",
    "RightMiddleIntermediate",
    "RightMiddleDistal",
    "RightRingProximal",
    "RightRingIntermediate",
    "RightRingDistal",
    "RightLittleProximal",
    "RightLittleIntermediate",
    "RightLittleDistal",
};

// VRM 0.x BlendShapePreset names (the VMC de-facto standard for /VMC/Ext/Blend/Val).
constexpr std::array<const char*, 5> kVowelNames = {"A", "I", "U", "E", "O"};

} // namespace

const char* vmc_bone_name(uint32_t bone) noexcept {
    return (bone < kVmcBoneNames.size()) ? kVmcBoneNames[bone] : nullptr;
}

void OscMessageWriter::put_padded_string(const char* s) noexcept {
    if (!ok_ || s == nullptr) {
        ok_ = false;
        return;
    }
    const size_t slen = std::strlen(s);
    const size_t padded = (slen + 4U) & ~size_t{3U};
    if (len_ + padded > cap_) {
        ok_ = false;
        return;
    }
    std::memcpy(buf_ + len_, s, slen);
    std::memset(buf_ + len_ + slen, 0, padded - slen);
    len_ += padded;
}

void OscMessageWriter::put_u32(uint32_t v) noexcept {
    if (!ok_ || len_ + 4U > cap_) {
        ok_ = false;
        return;
    }
    buf_[len_ + 0U] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
    buf_[len_ + 1U] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    buf_[len_ + 2U] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    buf_[len_ + 3U] = static_cast<uint8_t>(v & 0xFFU);
    len_ += 4U;
}

void OscMessageWriter::begin(const char* address, const char* type_tags) noexcept {
    len_ = 0;
    ok_ = (buf_ != nullptr);
    put_padded_string(address);
    char tags[32]{};
    const size_t n = (type_tags != nullptr) ? std::strlen(type_tags) : 0U;
    if (n + 2U > sizeof(tags)) {
        ok_ = false;
        return;
    }
    std::snprintf(tags, sizeof(tags), ",%s", (type_tags != nullptr) ? type_tags : "");
    put_padded_string(tags);
}

void OscMessageWriter::add_string(const char* s) noexcept {
    put_padded_string(s);
}

void OscMessageWriter::add_float(float v) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    put_u32(u);
}

void OscMessageWriter::add_int(int32_t v) noexcept {
    put_u32(static_cast<uint32_t>(v));
}

size_t encode_vmc_bone_pos(const char* name, const float pos[3], const float rot_xyzw[4], uint8_t* out,
                           size_t cap) noexcept {
    OscMessageWriter w(out, cap);
    w.begin("/VMC/Ext/Bone/Pos", "sfffffff");
    w.add_string(name);
    for (int i = 0; i < 3; ++i) {
        w.add_float(pos[i]);
    }
    for (int i = 0; i < 4; ++i) {
        w.add_float(rot_xyzw[i]);
    }
    return w.size();
}

size_t encode_vmc_blend_val(const char* name, float value, uint8_t* out, size_t cap) noexcept {
    OscMessageWriter w(out, cap);
    w.begin("/VMC/Ext/Blend/Val", "sf");
    w.add_string(name);
    w.add_float(value);
    return w.size();
}

void OscBundleBuilder::reset() noexcept {
    static constexpr char kTag[8] = {'#', 'b', 'u', 'n', 'd', 'l', 'e', '\0'};
    std::memcpy(buf_.data(), kTag, sizeof(kTag));
    // OSC time tag 1 = "immediately"
    std::memset(buf_.data() + 8, 0, 7);
    buf_[15] = 1U;
    len_ = 16;
    count_ = 0;
}

bool OscBundleBuilder::append(const uint8_t* msg, size_t len) noexcept {
    if (msg == nullptr || len == 0U || len_ + 4U + len > buf_.size()) {
        return false;
    }
    const uint32_t n = static_cast<uint32_t>(len);
    buf_[len_ + 0U] = static_cast<uint8_t>((n >> 24U) & 0xFFU);
    buf_[len_ + 1U] = static_cast<uint8_t>((n >> 16U) & 0xFFU);
    buf_[len_ + 2U] = static_cast<uint8_t>((n >> 8U) & 0xFFU);
    buf_[len_ + 3U] = static_cast<uint8_t>(n & 0xFFU);
    std::memcpy(buf_.data() + len_ + 4U, msg, len);
    len_ += 4U + len;
    ++count_;
    return true;
}

Expected<void> VmcSender::init(const char* ipv4_host, uint16_t port) {
    return osc_.init(ipv4_host, port);
}

uint32_t VmcSender::send_frame(const animation::VmcPoseFrame& frame, void (*sink)(const uint8_t*, size_t, void*),
                               void* sink_user) noexcept {
    if (!frame.valid || (sink == nullptr && !osc_.is_open())) {
        return 0U;
    }
    uint32_t datagrams = 0;
    const auto flush = [&]() noexcept {
        if (bundle_.empty()) {
            return;
        }
        if (sink != nullptr) {
            sink(bundle_.data(), bundle_.size(), sink_user);
        } else {
            (void)osc_.send_packet(bundle_.data(), bundle_.size());
        }
        ++datagrams;
        bundle_.reset();
    };
    const auto push = [&](const uint8_t* msg, size_t len) noexcept {
        if (len == 0U) {
            return;
        }
        if (!bundle_.append(msg, len)) {
            flush();
            (void)bundle_.append(msg, len);
        }
    };

    bundle_.reset();
    std::array<uint8_t, 160> msg{};
    {
        OscMessageWriter w(msg.data(), msg.size());
        w.begin("/VMC/Ext/OK", "i");
        w.add_int(1);
        push(msg.data(), w.size());
    }
    {
        OscMessageWriter w(msg.data(), msg.size());
        w.begin("/VMC/Ext/T", "f");
        w.add_float(frame.time_s);
        push(msg.data(), w.size());
    }
    {
        OscMessageWriter w(msg.data(), msg.size());
        w.begin("/VMC/Ext/Root/Pos", "sfffffff");
        w.add_string("root");
        for (float v : {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}) {
            w.add_float(v);
        }
        push(msg.data(), w.size());
    }
    for (uint32_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        push(msg.data(), encode_vmc_bone_pos(kVmcBoneNames[b], frame.local_pos[b].data(), frame.local_rot[b].data(),
                                             msg.data(), msg.size()));
    }
    for (size_t v = 0; v < kVowelNames.size(); ++v) {
        push(msg.data(), encode_vmc_blend_val(kVowelNames[v], frame.visemes[v], msg.data(), msg.size()));
    }
    push(msg.data(), encode_vmc_blend_val("Blink_L", frame.blink_l, msg.data(), msg.size()));
    push(msg.data(), encode_vmc_blend_val("Blink_R", frame.blink_r, msg.data(), msg.size()));
    {
        OscMessageWriter w(msg.data(), msg.size());
        w.begin("/VMC/Ext/Blend/Apply", "");
        push(msg.data(), w.size());
    }
    flush();
    return datagrams;
}

} // namespace aether::output
