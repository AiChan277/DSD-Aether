#pragma once

#include "aether_ffi_contract.h"

#include <array>

namespace aether::animation {

/**
 * One VMC Protocol (vmc.info) frame in Unity conventions: left-handed, +Y up, +Z forward, and *normalized* humanoid
 * bones (every bone has identity rotation in the T-pose rest, exactly like UniVRM's normalized avatar). Rotations are
 * bone-local; positions are bone-local offsets (Hips = root-relative position).
 */
struct VmcPoseFrame {
    std::array<std::array<float, 3>, AETHER_HUMANOID_BONE_COUNT> local_pos{};
    std::array<std::array<float, 4>, AETHER_HUMANOID_BONE_COUNT> local_rot{};
    std::array<float, 5> visemes{}; // A, I, U, E, O
    float blink_l{0.0f};
    float blink_r{0.0f};
    float time_s{0.0f};
    bool valid{false};
};

} // namespace aether::animation
