#include "animation/ik_solvers.hpp"
#include "animation/sim_engine.hpp"
#include "output/vmc_sender.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aether::animation;
using namespace aether::output;

namespace {

uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24U) | (uint32_t{p[1]} << 16U) | (uint32_t{p[2]} << 8U) | uint32_t{p[3]};
}

float be_f32(const uint8_t* p) {
    const uint32_t u = be32(p);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

struct Decoded {
    std::string address;
    std::string tags;
    std::vector<std::string> strings;
    std::vector<float> floats;
};

size_t read_padded(const uint8_t* p, size_t n, std::string& out) {
    out.assign(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), n));
    return (out.size() + 4U) & ~size_t{3U};
}

Decoded decode_message(const uint8_t* p, size_t n) {
    Decoded d;
    size_t off = read_padded(p, n, d.address);
    off += read_padded(p + off, n - off, d.tags);
    for (size_t i = 1; i < d.tags.size(); ++i) {
        if (d.tags[i] == 's') {
            std::string s;
            off += read_padded(p + off, n - off, s);
            d.strings.push_back(s);
        } else if (d.tags[i] == 'f') {
            d.floats.push_back(be_f32(p + off));
            off += 4;
        } else if (d.tags[i] == 'i') {
            off += 4;
        }
    }
    REQUIRE(off == n);
    return d;
}

std::vector<Decoded> decode_bundle(const uint8_t* p, size_t n) {
    REQUIRE(n >= 16U);
    REQUIRE(std::memcmp(p, "#bundle\0", 8) == 0);
    std::vector<Decoded> msgs;
    size_t off = 16;
    while (off < n) {
        const uint32_t len = be32(p + off);
        REQUIRE(len % 4U == 0U);
        msgs.push_back(decode_message(p + off + 4U, len));
        off += 4U + len;
    }
    REQUIRE(off == n);
    return msgs;
}

} // namespace

TEST_CASE("VMC /VMC/Ext/Bone/Pos and /VMC/Ext/Blend/Val encode as OSC 1.0 big-endian messages", "[vmc][osc]") {
    std::array<uint8_t, 128> buf{};
    const float pos[3] = {-0.1f, 1.2f, 0.03f};
    const float rot[4] = {0.0f, 0.70710677f, 0.0f, 0.70710677f};
    const size_t n = encode_vmc_bone_pos("Head", pos, rot, buf.data(), buf.size());
    // "/VMC/Ext/Bone/Pos"(20) + ",sfffffff"(12) + "Head"(8) + 7 floats(28)
    REQUIRE(n == 68U);
    const Decoded d = decode_message(buf.data(), n);
    CHECK(d.address == "/VMC/Ext/Bone/Pos");
    CHECK(d.tags == ",sfffffff");
    REQUIRE(d.strings.size() == 1U);
    CHECK(d.strings[0] == "Head");
    REQUIRE(d.floats.size() == 7U);
    CHECK(d.floats[1] == 1.2f);
    CHECK(d.floats[4] == 0.70710677f);

    const size_t nb = encode_vmc_blend_val("Blink_L", 0.5f, buf.data(), buf.size());
    const Decoded b = decode_message(buf.data(), nb);
    CHECK(b.address == "/VMC/Ext/Blend/Val");
    CHECK(b.strings[0] == "Blink_L");
    CHECK(b.floats[0] == 0.5f);

    CHECK(encode_vmc_bone_pos("Head", pos, rot, buf.data(), 40U) == 0U); // overflow is reported, never written past
    CHECK(std::string(vmc_bone_name(AETHER_BONE_LEFT_THUMB_PROXIMAL)) == "LeftThumbProximal");
    CHECK(std::string(vmc_bone_name(AETHER_BONE_RIGHT_LITTLE_DISTAL)) == "RightLittleDistal");
}

TEST_CASE("VMC frame: normalized identity rest bones, Unity handedness, all bones + blends in <= 1400 B bundles",
          "[vmc][normalized]") {
    // Rig with non-identity VRM bind rotations: VMC must still export identity (normalized) rest rotations.
    RestSkeletonMetrics rm = build_default_humanoid_rest_metrics();
    rm.rest_local_rotations[AETHER_BONE_SPINE] = from_euler_xyz(0.05f, -0.10f, 0.04f);
    rm.rest_local_rotations[AETHER_BONE_HEAD] = from_euler_xyz(-0.02f, -0.10f, 0.05f);
    rm.rest_local_rotations[AETHER_BONE_LEFT_UPPER_ARM] = from_euler_xyz(0.0f, 1.5707963f, 0.0f);
    for (size_t b = 1; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const int32_t p = rm.parent_indices[b];
        const size_t pi = (p >= 0 && p < static_cast<int32_t>(b)) ? static_cast<size_t>(p) : 0U;
        rm.rest_world_rotations[b] = normalize(rm.rest_world_rotations[pi] * rm.rest_local_rotations[b]);
    }
    SimEngine engine;
    engine.reset(rm);
    SimEngineConfig cfg{};
    cfg.visibility.enable_idle_breathing = false;
    SkinningFrame skin{};

    // Head yawed +30 deg toward +X (right-handed glTF frame).
    for (int f = 0; f < 240; ++f) {
        AetherTrackingFrame raw{};
        raw.frame_sequence = static_cast<uint32_t>(f / 2 + 1);
        raw.capture_timestamp_us = static_cast<uint64_t>(f / 2 * 2 + 1) * 16667ULL;
        raw.inference_complete_us = raw.capture_timestamp_us;
        raw.valid_flags = 0x01U;
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            raw.joint_rotations_quat[b][3] = 1.0f;
        }
        const Quat q = from_axis_angle({0.0f, 1.0f, 0.0f}, 30.0f * kDegToRad);
        raw.joint_rotations_quat[AETHER_BONE_HEAD][1] = q.y;
        raw.joint_rotations_quat[AETHER_BONE_HEAD][3] = q.w;
        raw.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.5f;
        raw.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.99f;
        engine.tick(raw, AetherDspSyncPacket{}, AetherGraphOutputs{}, static_cast<uint64_t>(f + 1) * 16667ULL, 0.0f,
                    cfg, skin);
    }
    VmcPoseFrame frame{};
    engine.export_vmc_pose(skin, 4.0f, frame);
    REQUIRE(frame.valid);

    // Untracked legs export identity (normalized rest) and the A-pose arm exports only its ~69 deg relaxed fold, even though
    // the raw VRM bind rotation of LeftUpperArm is 90 deg.
    CHECK(std::abs(frame.local_rot[AETHER_BONE_LEFT_UPPER_LEG][3]) > 0.9999f);
    const float arm_angle_deg =
        2.0f * std::acos(std::min(1.0f, std::abs(frame.local_rot[AETHER_BONE_LEFT_UPPER_ARM][3]))) * kRadToDeg;
    CHECK(arm_angle_deg > 60.0f);
    CHECK(arm_angle_deg < 78.0f);
    // The whole chain composes to the tracked 30 deg yaw; mirrored into Unity's left-handed frame (q.y flips).
    Quat chain = Quat::identity();
    for (uint32_t b : {AETHER_BONE_HIPS, AETHER_BONE_SPINE, AETHER_BONE_CHEST, AETHER_BONE_UPPER_CHEST,
                       AETHER_BONE_NECK, AETHER_BONE_HEAD}) {
        const auto& r = frame.local_rot[b];
        chain = normalize(chain * Quat{r[0], r[1], r[2], r[3]});
    }
    const float yaw_unity = 2.0f * std::atan2(chain.y, chain.w) * kRadToDeg;
    CHECK(std::abs(yaw_unity + 30.0f) < 0.5f);
    // Positions: X mirrored (left arm offset +X in glTF -> -X in Unity).
    CHECK(frame.local_pos[AETHER_BONE_LEFT_UPPER_ARM][0] < 0.0f);

    struct Sink {
        std::vector<std::vector<uint8_t>> grams;
    } sink;
    VmcSender sender;
    const uint32_t grams = sender.send_frame(
        frame,
        [](const uint8_t* d, size_t n, void* u) {
            static_cast<Sink*>(u)->grams.emplace_back(d, d + n);
        },
        &sink);
    REQUIRE(grams == sink.grams.size());
    REQUIRE(grams >= 2U);
    std::vector<std::string> bones;
    std::vector<std::string> blends;
    bool ok = false, applied = false, root = false, time = false;
    for (const auto& g : sink.grams) {
        CHECK(g.size() <= OscBundleBuilder::kMaxDatagram);
        for (const Decoded& m : decode_bundle(g.data(), g.size())) {
            if (m.address == "/VMC/Ext/Bone/Pos") {
                bones.push_back(m.strings.at(0));
            } else if (m.address == "/VMC/Ext/Blend/Val") {
                blends.push_back(m.strings.at(0));
            } else if (m.address == "/VMC/Ext/OK") {
                ok = true;
            } else if (m.address == "/VMC/Ext/Blend/Apply") {
                applied = true;
            } else if (m.address == "/VMC/Ext/Root/Pos") {
                root = true;
            } else if (m.address == "/VMC/Ext/T") {
                time = true;
                CHECK(m.floats.at(0) == 4.0f);
            }
        }
    }
    CHECK(ok);
    CHECK(time);
    CHECK(root);
    CHECK(applied);
    CHECK(bones.size() == AETHER_HUMANOID_BONE_COUNT);
    CHECK(blends == std::vector<std::string>{"A", "I", "U", "E", "O", "Blink_L", "Blink_R"});
}
