#pragma once

#include "aether_ffi_contract.h"
#include "animation/expression_mixer.hpp"
#include "animation/ik_solvers.hpp"
#include "animation/kalman.hpp"
#include "animation/one_euro.hpp"
#include "animation/skinning_frame.hpp"
#include "animation/spring_bone.hpp"
#include "animation/visibility_first.hpp"

#include <cstdint>

namespace aether::animation {

struct SimEngineConfig {
    KalmanConfig kalman{};
    VisibilityFirstConfig visibility{};
    float spring_fixed_dt_s{1.0f / 60.0f};
    uint32_t spring_substeps_max{4};
    SpringBoneSimulator::SimdPath spring_simd_path{SpringBoneSimulator::SimdPath::Avx2};
};

/**
 * Unified T2 Animation & Simulation Engine (`AETHER-TS-001` §9).
 * Budget: `<= 0.8 ms` per 240 Hz tick on P-core, strictly zero heap allocations (`HR-01`).
 * Combines 1-Euro smoothing -> Kalman dead-reckoning -> Visibility-First posture ->
 * 2-bone/FABRIK IK + Appendix C joint limits -> SIMD SpringBone Verlet -> ExpressionMixer.
 */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

class alignas(64) SimEngine {
  public:
    SimEngine() noexcept;

    void reset(const RestSkeletonMetrics& rest = build_default_humanoid_rest_metrics()) noexcept;
    void set_rest_metrics(const RestSkeletonMetrics& rest) noexcept;

    void tick(const AetherTrackingFrame& raw_tracking, const AetherDspSyncPacket& dsp,
              const AetherGraphOutputs& graph_out, uint64_t now_us, float horizon_override_s,
              const SimEngineConfig& cfg, SkinningFrame& out_skinning) noexcept;

    [[nodiscard]] SpringBoneSimulator& spring_simulator() noexcept { return spring_; }
    [[nodiscard]] const SpringBoneSimulator& spring_simulator() const noexcept { return spring_; }
    [[nodiscard]] const VisibilityFirstSolver& visibility_solver() const noexcept { return vis_solver_; }
    [[nodiscard]] const FilteredTrackingState& last_predicted() const noexcept { return predicted_state_; }
    [[nodiscard]] const std::array<Vec3, kMaxBones>& bone_pos_ws() const noexcept { return bone_pos_ws_; }
    [[nodiscard]] const std::array<Quat, kMaxBones>& bone_rot_ws() const noexcept { return bone_rot_ws_; }

    void export_spring_overlay(SpringColliderOverlay* out_cols, uint32_t max_cols, uint32_t& out_col_count,
                               SpringSegmentOverlay* out_segs, uint32_t max_segs,
                               uint32_t& out_seg_count) const noexcept;

  private:
    OneEuroPoseBank one_euro_{};
    KalmanPosePredictor kalman_{};
    VisibilityFirstSolver vis_solver_{};
    FullBodyIkSolver ik_solver_{};
    SpringBoneSimulator spring_{};
    ExpressionMixer expr_mixer_{};

    FilteredTrackingState filtered_state_{};
    FilteredTrackingState predicted_state_{};
    SolvedBodyPose solved_body_{};
    std::array<Vec3, AETHER_HUMANOID_BONE_COUNT> ik_world_pos_{};
    std::array<Quat, AETHER_HUMANOID_BONE_COUNT> ik_world_rot_{};
    std::array<Vec3, kMaxBones> bone_pos_ws_{};
    std::array<Quat, kMaxBones> bone_rot_ws_{};

    uint32_t last_seen_frame_seq_{0};
    uint64_t last_tick_us_{0};
    float spring_accum_s_{0.0f};
    bool has_tracking_frame_{false};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace aether::animation
