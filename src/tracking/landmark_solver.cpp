#include "tracking/landmark_solver.hpp"
#include "animation/math_quat.hpp"
#include "tracking/canonical_face_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace aether::tracking {

namespace {

using animation::Quat;
using animation::Vec3;

[[nodiscard]] float sigmoid(float x) noexcept {
    const float clamped = std::clamp(x, -20.0f, 20.0f);
    return 1.0f / (1.0f + std::exp(-clamped));
}

[[nodiscard]] Quat quat_from_two_vecs(Vec3 from, Vec3 to) noexcept {
    const Vec3 a = animation::normalize_safe(from);
    const Vec3 b = animation::normalize_safe(to);
    const float d = std::clamp(animation::dot(a, b), -1.0f, 1.0f);
    if (d < -0.9995f) {
        Vec3 ax = animation::cross({1.0f, 0.0f, 0.0f}, a);
        if (animation::length_sq(ax) < 1e-4f) {
            ax = animation::cross({0.0f, 0.0f, 1.0f}, a);
        }
        return animation::from_axis_angle(animation::normalize_safe(ax), animation::kPi);
    }
    const Vec3 c = animation::cross(a, b);
    return animation::normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

[[nodiscard]] Quat mat3_to_quat(const float m[3][3]) noexcept {
    const float trace = m[0][0] + m[1][1] + m[2][2];
    Quat q{};
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return animation::normalize(q);
}

/**
 * Exact 3x3 SVD (`A = U * Sigma * V^T`) using symmetric Jacobi eigendecomposition of `A^T A`
 * for Kabsch/Procrustes SO(3) recovery without heap allocation (`HR-01`).
 */
void kabsch_3x3(const float cov[3][3], float out_r[3][3]) noexcept {
    // Symmetric matrix G = cov^T * cov
    double g[3][3]{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            g[i][j] = static_cast<double>(cov[0][i]) * cov[0][j] + static_cast<double>(cov[1][i]) * cov[1][j] +
                      static_cast<double>(cov[2][i]) * cov[2][j];
        }
    }

    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int sweep = 0; sweep < 20; ++sweep) {
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                const double gpq = g[p][q];
                if (std::abs(gpq) < 1e-14) {
                    continue;
                }
                const double tau = (g[q][q] - g[p][p]) / (2.0 * gpq);
                const double t = (tau >= 0.0) ? (1.0 / (tau + std::sqrt(1.0 + tau * tau)))
                                              : (-1.0 / (-tau + std::sqrt(1.0 + tau * tau)));
                const double c = 1.0 / std::sqrt(1.0 + t * t);
                const double s = t * c;

                const double gpp = g[p][p];
                const double gqq = g[q][q];
                g[p][p] = gpp - t * gpq;
                g[q][q] = gqq + t * gpq;
                g[p][q] = 0.0;
                g[q][p] = 0.0;
                for (int r = 0; r < 3; ++r) {
                    if (r != p && r != q) {
                        const double grp = g[r][p];
                        const double grq = g[r][q];
                        g[r][p] = c * grp - s * grq;
                        g[p][r] = g[r][p];
                        g[r][q] = s * grp + c * grq;
                        g[q][r] = g[r][q];
                    }
                    const double vrp = v[r][p];
                    const double vrq = v[r][q];
                    v[r][p] = c * vrp - s * vrq;
                    v[r][q] = s * vrp + c * vrq;
                }
            }
        }
    }

    // Sort eigenvalues descending so singular vectors have clean order
    int order[3] = {0, 1, 2};
    if (g[order[0]][order[0]] < g[order[1]][order[1]]) {
        std::swap(order[0], order[1]);
    }
    if (g[order[1]][order[1]] < g[order[2]][order[2]]) {
        std::swap(order[1], order[2]);
    }
    if (g[order[0]][order[0]] < g[order[1]][order[1]]) {
        std::swap(order[0], order[1]);
    }

    double v_sorted[3][3]{};
    double sigma[3]{};
    for (int col = 0; col < 3; ++col) {
        const int src = order[col];
        sigma[col] = std::sqrt(std::max(g[src][src], 0.0));
        for (int row = 0; row < 3; ++row) {
            v_sorted[row][col] = v[row][src];
        }
    }

    // U columns = cov * V[:, col] / sigma[col]
    double u[3][3]{};
    for (int col = 0; col < 3; ++col) {
        if (sigma[col] > 1e-10) {
            const double inv_s = 1.0 / sigma[col];
            for (int row = 0; row < 3; ++row) {
                u[row][col] = (static_cast<double>(cov[row][0]) * v_sorted[0][col] +
                               static_cast<double>(cov[row][1]) * v_sorted[1][col] +
                               static_cast<double>(cov[row][2]) * v_sorted[2][col]) *
                              inv_s;
            }
        } else {
            // Complete orthonormal U column if rank-deficient via cross product
            const int prev1 = (col + 1) % 3;
            const int prev2 = (col + 2) % 3;
            u[0][col] = u[1][prev1] * u[2][prev2] - u[2][prev1] * u[1][prev2];
            u[1][col] = u[2][prev1] * u[0][prev2] - u[0][prev1] * u[2][prev2];
            u[2][col] = u[0][prev1] * u[1][prev2] - u[1][prev1] * u[0][prev2];
        }
    }

    // Note: if rank 2, re-orthogonalize u[:, 2] cleanly as cross(u[:, 0], u[:, 1]) * det
    auto compute_r = [&](double sign_last) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                const double s0 = u[r][0] * v_sorted[c][0];
                const double s1 = u[r][1] * v_sorted[c][1];
                const double s2 = sign_last * u[r][2] * v_sorted[c][2];
                out_r[r][c] = static_cast<float>(s0 + s1 + s2);
            }
        }
    };
    compute_r(1.0);
    const double det = static_cast<double>(out_r[0][0]) * (out_r[1][1] * out_r[2][2] - out_r[1][2] * out_r[2][1]) -
                       static_cast<double>(out_r[0][1]) * (out_r[1][0] * out_r[2][2] - out_r[1][2] * out_r[2][0]) +
                       static_cast<double>(out_r[0][2]) * (out_r[1][0] * out_r[2][1] - out_r[1][1] * out_r[2][0]);
    if (det < 0.0) {
        compute_r(-1.0);
    }
}

[[nodiscard]] Quat solve_finger_curl(const float* lm21_x3, int mcp_i, int pip_i, int dip_i, const Vec3& palm_dir,
                                     const Vec3& palm_normal, float handed_sign) noexcept {
    const Vec3 mcp{lm21_x3[mcp_i * 3 + 0], lm21_x3[mcp_i * 3 + 1], lm21_x3[mcp_i * 3 + 2]};
    const Vec3 pip{lm21_x3[pip_i * 3 + 0], lm21_x3[pip_i * 3 + 1], lm21_x3[pip_i * 3 + 2]};
    const Vec3 dip{lm21_x3[dip_i * 3 + 0], lm21_x3[dip_i * 3 + 1], lm21_x3[dip_i * 3 + 2]};
    const Vec3 seg1 = animation::normalize_safe(pip - mcp, palm_dir);
    const Vec3 seg2 = animation::normalize_safe(dip - pip, seg1);
    const float cos_c = std::clamp(animation::dot(seg1, seg2), -1.0f, 1.0f);
    const float angle = std::acos(cos_c) * ((animation::dot(seg2, palm_normal) < 0.0f) ? 1.0f : -0.3f);
    return animation::from_euler_xyz(std::clamp(angle, -0.3f, 1.55f), 0.0f, handed_sign * 0.02f);
}

} // namespace

HeadPose6D solve_procrustes_head_pose(const float* landmarks_478x3, float aspect_w_over_h) noexcept {
    HeadPose6D out{};
    if (landmarks_478x3 == nullptr) {
        return out;
    }

    // Use weighted Kabsch over all Procrustes basis landmarks (`kProcrustesBasisCount = 33`)
    float w_sum = 0.0f;
    Vec3 src_mean{};
    Vec3 dst_mean{};

    for (uint32_t k = 0; k < kProcrustesBasisCount; ++k) {
        const uint32_t idx = kProcrustesBasisIndices[k];
        if (idx >= kCanonicalFaceVertexCount) {
            continue;
        }
        const float w = kProcrustesBasisWeights[k];
        const Vec3 src{kCanonicalFaceVerticesM[idx][0], kCanonicalFaceVerticesM[idx][1],
                       kCanonicalFaceVerticesM[idx][2]};
        const Vec3 dst{landmarks_478x3[idx * 3U + 0U] * aspect_w_over_h, landmarks_478x3[idx * 3U + 1U],
                       landmarks_478x3[idx * 3U + 2U]};
        src_mean = src_mean + src * w;
        dst_mean = dst_mean + dst * w;
        w_sum += w;
    }

    if (w_sum <= 1e-6f) {
        return out;
    }
    const float inv_w = 1.0f / w_sum;
    src_mean = src_mean * inv_w;
    dst_mean = dst_mean * inv_w;

    float cov[3][3]{};
    float src_energy = 0.0f;
    for (uint32_t k = 0; k < kProcrustesBasisCount; ++k) {
        const uint32_t idx = kProcrustesBasisIndices[k];
        if (idx >= kCanonicalFaceVertexCount) {
            continue;
        }
        const float w = kProcrustesBasisWeights[k];
        const Vec3 sc{
            kCanonicalFaceVerticesM[idx][0] - src_mean.x,
            kCanonicalFaceVerticesM[idx][1] - src_mean.y,
            kCanonicalFaceVerticesM[idx][2] - src_mean.z,
        };
        const Vec3 dc{
            landmarks_478x3[idx * 3U + 0U] * aspect_w_over_h - dst_mean.x,
            landmarks_478x3[idx * 3U + 1U] - dst_mean.y,
            landmarks_478x3[idx * 3U + 2U] - dst_mean.z,
        };
        src_energy += w * animation::dot(sc, sc);
        // Cov = sum w * dc * sc^T solves dc = s * R * sc
        cov[0][0] += w * dc.x * sc.x;
        cov[0][1] += w * dc.x * sc.y;
        cov[0][2] += w * dc.x * sc.z;
        cov[1][0] += w * dc.y * sc.x;
        cov[1][1] += w * dc.y * sc.y;
        cov[1][2] += w * dc.y * sc.z;
        cov[2][0] += w * dc.z * sc.x;
        cov[2][1] += w * dc.z * sc.y;
        cov[2][2] += w * dc.z * sc.z;
    }

    float r[3][3]{};
    kabsch_3x3(cov, r);

    // Estimate uniform scale s = trace(R^T * Cov) / src_energy and recover translation t = dst_mean - s * R * src_mean
    float tr = 0.0f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            tr += r[i][j] * cov[i][j];
        }
    }
    const float scale = (src_energy > 1e-8f) ? std::max(tr / src_energy, 1e-3f) : 1.0f;
    const Vec3 r_src_mean{
        r[0][0] * src_mean.x + r[0][1] * src_mean.y + r[0][2] * src_mean.z,
        r[1][0] * src_mean.x + r[1][1] * src_mean.y + r[1][2] * src_mean.z,
        r[2][0] * src_mean.x + r[2][1] * src_mean.y + r[2][2] * src_mean.z,
    };
    const Vec3 t = (dst_mean - r_src_mean * scale) * (1.0f / scale);

    const Quat q = mat3_to_quat(r);
    out.rotation_xyzw[0] = q.x;
    out.rotation_xyzw[1] = q.y;
    out.rotation_xyzw[2] = q.z;
    out.rotation_xyzw[3] = q.w;
    out.position_m[0] = t.x;
    out.position_m[1] = t.y;
    out.position_m[2] = t.z;
    out.valid = true;
    return out;
}

RoiRectNorm decode_m0_best_face_roi(const float* regressors_896x16, const float* classificators_896x1,
                                    float min_score) noexcept {
    RoiRectNorm best{};
    if (regressors_896x16 == nullptr || classificators_896x1 == nullptr) {
        return best;
    }

    // Generate official BlazeFace 128x128 SSD anchors (`strides = [8, 16, 16, 16]`, 2 anchors/cell -> 512+384 = 896)
    float best_score = min_score;
    uint32_t anchor_idx = 0;
    constexpr int kStrides[2] = {8, 16};
    constexpr int kAnchorsPerCell[2] = {2, 6}; // 16*16*2 = 512; 8*8*6 = 384 -> total 896 anchors!

    for (int stage = 0; stage < 2; ++stage) {
        const int grid = 128 / kStrides[stage];
        const int count = kAnchorsPerCell[stage];
        for (int gy = 0; gy < grid; ++gy) {
            const float ay = (static_cast<float>(gy) + 0.5f) / static_cast<float>(grid);
            for (int gx = 0; gx < grid; ++gx) {
                const float ax = (static_cast<float>(gx) + 0.5f) / static_cast<float>(grid);
                for (int k = 0; k < count; ++k) {
                    const float score = sigmoid(classificators_896x1[anchor_idx]);
                    if (score > best_score) {
                        best_score = score;
                        const float* reg = &regressors_896x16[static_cast<size_t>(anchor_idx) * 16U];
                        // BlazeFace x/y/w/h_scale = 128.0
                        const float cx = ax + reg[0] / 128.0f;
                        const float cy = ay + reg[1] / 128.0f;
                        const float bw = std::abs(reg[2]) / 128.0f;
                        const float bh = std::abs(reg[3]) / 128.0f;
                        const float side = std::clamp(std::max(bw, bh) * 1.50f, 0.12f, 1.0f);
                        // Approximate roll angle from eye keypoints (kp0 = left eye, kp1 = right eye at reg[4..7])
                        const float ex0 = ax + reg[4] / 128.0f;
                        const float ey0 = ay + reg[5] / 128.0f;
                        const float ex1 = ax + reg[6] / 128.0f;
                        const float ey1 = ay + reg[7] / 128.0f;
                        best.cx = std::clamp(cx, 0.05f, 0.95f);
                        best.cy = std::clamp(cy, 0.05f, 0.95f);
                        best.width = side;
                        best.height = side;
                        best.rotation_rad = std::atan2(ey1 - ey0, ex1 - ex0);
                        best.valid = true;
                    }
                    ++anchor_idx;
                }
            }
        }
    }
    return best;
}

HandRoiDecision compute_hand_rois_from_pose(const float* pose_landmarks_39x5, float pose_flag, float min_conf,
                                            float border_margin) noexcept {
    HandRoiDecision out{};
    if (pose_landmarks_39x5 == nullptr || pose_flag < min_conf) {
        return out;
    }

    const auto eval_side = [&](int wrist_i, int pinky_i, int index_i, RoiRectNorm& dst_roi) -> bool {
        const float wx = pose_landmarks_39x5[wrist_i * 5 + 0] / 256.0f;
        const float wy = pose_landmarks_39x5[wrist_i * 5 + 1] / 256.0f;
        const float w_vis = sigmoid(pose_landmarks_39x5[wrist_i * 5 + 3]);
        const float w_pres = sigmoid(pose_landmarks_39x5[wrist_i * 5 + 4]);
        if (std::min(w_vis, w_pres) < min_conf) {
            return false;
        }

        const float px = pose_landmarks_39x5[pinky_i * 5 + 0] / 256.0f;
        const float py = pose_landmarks_39x5[pinky_i * 5 + 1] / 256.0f;
        const float ix = pose_landmarks_39x5[index_i * 5 + 0] / 256.0f;
        const float iy = pose_landmarks_39x5[index_i * 5 + 1] / 256.0f;

        const float mid_x = 0.5f * (px + ix);
        const float mid_y = 0.5f * (py + iy);
        const float cx = wx + (mid_x - wx) * 0.65f;
        const float cy = wy + (mid_y - wy) * 0.65f;

        // 5% frame-border exclusion margin (`P3-02`)
        if (cx < border_margin || cx > (1.0f - border_margin) || cy < border_margin || cy > (1.0f - border_margin)) {
            return false;
        }

        const float hand_span = std::hypot(mid_x - wx, mid_y - wy);
        const float side = std::clamp(hand_span * 2.6f, 0.16f, 0.55f);
        dst_roi.cx = cx;
        dst_roi.cy = cy;
        dst_roi.width = side;
        dst_roi.height = side;
        dst_roi.rotation_rad = std::atan2(mid_y - wy, mid_x - wx) + animation::kPi * 0.5f;
        dst_roi.valid = true;
        return true;
    };

    out.run_left = eval_side(15, 17, 19, out.left_roi);
    out.run_right = eval_side(16, 18, 20, out.right_roi);
    return out;
}

void LandmarkPoseSolver::reset() noexcept {
    neutral_calibrated_ = false;
    neutral_head_quat_[0] = 0.0f;
    neutral_head_quat_[1] = 0.0f;
    neutral_head_quat_[2] = 0.0f;
    neutral_head_quat_[3] = 1.0f;
    neutral_head_pos_[0] = 0.0f;
    neutral_head_pos_[1] = 0.0f;
    neutral_head_pos_[2] = 0.0f;
    calibrate_next_ = false;
}

void LandmarkPoseSolver::calibrate_neutral() noexcept {
    calibrate_next_ = true;
}

RoiRectNorm LandmarkPoseSolver::compute_next_face_roi(const float* m2_landmarks_478x3, const RoiRectNorm& prev_face_roi,
                                                      float face_flag) const noexcept {
    if (m2_landmarks_478x3 == nullptr || !prev_face_roi.valid || face_flag < 0.45f) {
        return {};
    }
    float min_x = 256.0f, min_y = 256.0f, max_x = 0.0f, max_y = 0.0f;
    for (uint32_t i = 0; i < 468U; ++i) {
        const float x = m2_landmarks_478x3[i * 3U + 0U];
        const float y = m2_landmarks_478x3[i * 3U + 1U];
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    }
    const float lc_x = ((min_x + max_x) * 0.5f) / 256.0f - 0.5f;
    const float lc_y = ((min_y + max_y) * 0.5f) / 256.0f - 0.5f;
    const float c = std::cos(prev_face_roi.rotation_rad);
    const float s = std::sin(prev_face_roi.rotation_rad);
    RoiRectNorm next{};
    next.cx = std::clamp(prev_face_roi.cx + (lc_x * c - lc_y * s) * prev_face_roi.width, 0.05f, 0.95f);
    next.cy = std::clamp(prev_face_roi.cy + (lc_x * s + lc_y * c) * prev_face_roi.height, 0.05f, 0.95f);
    const float span = std::max(max_x - min_x, max_y - min_y) / 256.0f;
    const float side = std::clamp(prev_face_roi.width * span * 1.45f, 0.14f, 0.90f);
    next.width = side;
    next.height = side;
    // Left eye outer corner = 33, Right eye outer corner = 263
    const float ex = m2_landmarks_478x3[263U * 3U + 0U] - m2_landmarks_478x3[33U * 3U + 0U];
    const float ey = m2_landmarks_478x3[263U * 3U + 1U] - m2_landmarks_478x3[33U * 3U + 1U];
    next.rotation_rad = prev_face_roi.rotation_rad + std::atan2(ey, ex);
    next.valid = true;
    return next;
}

void LandmarkPoseSolver::populate_tracking_frame(
    const RoiRectNorm& /*face_roi*/, const float* m2_landmarks_478x3, float face_flag, const float* m3_blendshapes_52,
    const float* m1_body_world_39x3, const float* m1_body_lm_39x5, float pose_flag, bool run_left_hand,
    bool run_right_hand, const float* m1_hands_lm_2x63, const float* m1_hands_flag_2,
    const float* /*m1_hands_handed_2*/, uint64_t capture_timestamp_us, uint64_t inference_complete_us, EpTier active_ep,
    uint32_t frame_sequence, bool roi_reset_this_frame, AetherTrackingFrame& out_frame) noexcept {
    out_frame = AetherTrackingFrame{};
    out_frame.capture_timestamp_us = capture_timestamp_us;
    out_frame.inference_complete_us = inference_complete_us;
    out_frame.active_execution_provider = to_ffi_ep(active_ep);
    out_frame.frame_sequence = frame_sequence;

    uint32_t valid_flags = roi_reset_this_frame ? 0x10U : 0U;
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        out_frame.joint_rotations_quat[b][3] = 1.0f;
    }

    // 1. Face (Procrustes 6-DoF + Iris Gaze + M3 -> Appendix B 52 Blendshapes)
    if (m2_landmarks_478x3 != nullptr && face_flag >= 0.45f) {
        valid_flags |= 0x01U;
        const HeadPose6D hp = solve_procrustes_head_pose(m2_landmarks_478x3, 1.0f);
        Quat q_head{hp.rotation_xyzw[0], hp.rotation_xyzw[1], hp.rotation_xyzw[2], hp.rotation_xyzw[3]};
        Vec3 t_head{hp.position_m[0], hp.position_m[1], hp.position_m[2]};
        if (calibrate_next_) {
            neutral_head_quat_[0] = q_head.x;
            neutral_head_quat_[1] = q_head.y;
            neutral_head_quat_[2] = q_head.z;
            neutral_head_quat_[3] = q_head.w;
            neutral_head_pos_[0] = t_head.x;
            neutral_head_pos_[1] = t_head.y;
            neutral_head_pos_[2] = t_head.z;
            neutral_calibrated_ = true;
            calibrate_next_ = false;
        }
        if (neutral_calibrated_) {
            const Quat q_neutral{neutral_head_quat_[0], neutral_head_quat_[1], neutral_head_quat_[2],
                                 neutral_head_quat_[3]};
            q_head = animation::normalize(q_head * animation::conjugate(q_neutral));
            t_head = {t_head.x - neutral_head_pos_[0], t_head.y - neutral_head_pos_[1],
                      t_head.z - neutral_head_pos_[2]};
        }
        out_frame.joint_rotations_quat[AETHER_BONE_HEAD][0] = q_head.x;
        out_frame.joint_rotations_quat[AETHER_BONE_HEAD][1] = q_head.y;
        out_frame.joint_rotations_quat[AETHER_BONE_HEAD][2] = q_head.z;
        out_frame.joint_rotations_quat[AETHER_BONE_HEAD][3] = q_head.w;
        out_frame.joint_positions_ws[AETHER_BONE_HEAD][0] = t_head.x;
        out_frame.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f + t_head.y;
        out_frame.joint_positions_ws[AETHER_BONE_HEAD][2] = t_head.z;
        out_frame.joint_positions_ws[AETHER_BONE_HEAD][3] = std::clamp(face_flag, 0.0f, 1.0f);
        out_frame.joint_positions_ws[AETHER_BONE_NECK][3] = std::clamp(face_flag, 0.0f, 1.0f);

        // Iris landmarks: left iris center=468 (corners 33,133; lids 159,145); right center=473 (362,263; 386,374)
        const auto compute_eye_quat = [&](int iris, int outer_c, int inner_c, int top_l, int bot_l) noexcept -> Quat {
            const float cx = 0.5f * (m2_landmarks_478x3[outer_c * 3 + 0] + m2_landmarks_478x3[inner_c * 3 + 0]);
            const float cy = 0.5f * (m2_landmarks_478x3[top_l * 3 + 1] + m2_landmarks_478x3[bot_l * 3 + 1]);
            const float ew =
                std::max(1.0f, std::abs(m2_landmarks_478x3[inner_c * 3 + 0] - m2_landmarks_478x3[outer_c * 3 + 0]));
            const float eh =
                std::max(1.0f, std::abs(m2_landmarks_478x3[bot_l * 3 + 1] - m2_landmarks_478x3[top_l * 3 + 1]));
            const float yaw = std::clamp((m2_landmarks_478x3[iris * 3 + 0] - cx) / ew, -0.5f, 0.5f) * 0.55f;
            const float pitch = std::clamp((m2_landmarks_478x3[iris * 3 + 1] - cy) / eh, -0.5f, 0.5f) * 0.35f;
            return animation::from_euler_xyz(pitch, yaw, 0.0f);
        };
        const Quat ql_eye = compute_eye_quat(468, 33, 133, 159, 145);
        const Quat qr_eye = compute_eye_quat(473, 263, 362, 386, 374);
        out_frame.joint_rotations_quat[AETHER_BONE_LEFT_EYE][0] = ql_eye.x;
        out_frame.joint_rotations_quat[AETHER_BONE_LEFT_EYE][1] = ql_eye.y;
        out_frame.joint_rotations_quat[AETHER_BONE_LEFT_EYE][2] = ql_eye.z;
        out_frame.joint_rotations_quat[AETHER_BONE_LEFT_EYE][3] = ql_eye.w;
        out_frame.joint_rotations_quat[AETHER_BONE_RIGHT_EYE][0] = qr_eye.x;
        out_frame.joint_rotations_quat[AETHER_BONE_RIGHT_EYE][1] = qr_eye.y;
        out_frame.joint_rotations_quat[AETHER_BONE_RIGHT_EYE][2] = qr_eye.z;
        out_frame.joint_rotations_quat[AETHER_BONE_RIGHT_EYE][3] = qr_eye.w;

        if (m3_blendshapes_52 != nullptr) {
            for (size_t s = 0; s < 52U; ++s) {
                const int8_t src_idx = kAppendixBFromMediaPipeM3[s];
                out_frame.arkit_blendshapes[s] =
                    (src_idx >= 0) ? std::clamp(m3_blendshapes_52[src_idx], 0.0f, 1.0f) : 0.0f;
            }
        }
    }

    // 2. Upper-Body Pose (MediaPipe 39 world landmarks -> Hips, Chest, Arms, Wrists)
    const float body_conf = std::clamp(pose_flag, 0.0f, 1.0f);
    out_frame.upper_body_confidence = body_conf;
    if (m1_body_world_39x3 != nullptr && m1_body_lm_39x5 != nullptr && body_conf >= 0.30f) {
        valid_flags |= 0x08U;
        // Hip midpoint (indices 23, 24), shoulder midpoint (11, 12), wrists (15, 16)
        const Vec3 l_hip{m1_body_world_39x3[23 * 3 + 0], -m1_body_world_39x3[23 * 3 + 1],
                         -m1_body_world_39x3[23 * 3 + 2]};
        const Vec3 r_hip{m1_body_world_39x3[24 * 3 + 0], -m1_body_world_39x3[24 * 3 + 1],
                         -m1_body_world_39x3[24 * 3 + 2]};
        const Vec3 hip_mid = (l_hip + r_hip) * 0.5f;
        const Vec3 l_sh{m1_body_world_39x3[11 * 3 + 0], -m1_body_world_39x3[11 * 3 + 1],
                        -m1_body_world_39x3[11 * 3 + 2]};
        const Vec3 r_sh{m1_body_world_39x3[12 * 3 + 0], -m1_body_world_39x3[12 * 3 + 1],
                        -m1_body_world_39x3[12 * 3 + 2]};
        const Vec3 sh_mid = (l_sh + r_sh) * 0.5f;
        const Vec3 l_w{m1_body_world_39x3[15 * 3 + 0], -m1_body_world_39x3[15 * 3 + 1],
                       -m1_body_world_39x3[15 * 3 + 2]};
        const Vec3 r_w{m1_body_world_39x3[16 * 3 + 0], -m1_body_world_39x3[16 * 3 + 1],
                       -m1_body_world_39x3[16 * 3 + 2]};

        const Vec3 hips_ws{std::clamp(hip_mid.x, -0.6f, 0.6f), 0.90f, std::clamp(hip_mid.z, -0.5f, 0.5f)};
        out_frame.joint_positions_ws[AETHER_BONE_HIPS][0] = hips_ws.x;
        out_frame.joint_positions_ws[AETHER_BONE_HIPS][1] = hips_ws.y;
        out_frame.joint_positions_ws[AETHER_BONE_HIPS][2] = hips_ws.z;
        out_frame.joint_positions_ws[AETHER_BONE_HIPS][3] = body_conf;
        out_frame.joint_positions_ws[AETHER_BONE_SPINE][3] = body_conf;

        const Vec3 torso_up = animation::normalize_safe(sh_mid - hip_mid, {0.0f, 1.0f, 0.0f});
        const Quat chest_q = quat_from_two_vecs({0.0f, 1.0f, 0.0f}, torso_up);
        out_frame.joint_rotations_quat[AETHER_BONE_CHEST][0] = chest_q.x;
        out_frame.joint_rotations_quat[AETHER_BONE_CHEST][1] = chest_q.y;
        out_frame.joint_rotations_quat[AETHER_BONE_CHEST][2] = chest_q.z;
        out_frame.joint_rotations_quat[AETHER_BONE_CHEST][3] = chest_q.w;

        // Place wrist IK targets relative to hips in avatar world frame
        const Vec3 lw_ws = hips_ws + (l_w - hip_mid);
        const Vec3 rw_ws = hips_ws + (r_w - hip_mid);
        out_frame.joint_positions_ws[AETHER_BONE_LEFT_HAND][0] = lw_ws.x;
        out_frame.joint_positions_ws[AETHER_BONE_LEFT_HAND][1] = lw_ws.y;
        out_frame.joint_positions_ws[AETHER_BONE_LEFT_HAND][2] = lw_ws.z;
        out_frame.joint_positions_ws[AETHER_BONE_LEFT_HAND][3] = sigmoid(m1_body_lm_39x5[15 * 5 + 3]);
        out_frame.joint_positions_ws[AETHER_BONE_RIGHT_HAND][0] = rw_ws.x;
        out_frame.joint_positions_ws[AETHER_BONE_RIGHT_HAND][1] = rw_ws.y;
        out_frame.joint_positions_ws[AETHER_BONE_RIGHT_HAND][2] = rw_ws.z;
        out_frame.joint_positions_ws[AETHER_BONE_RIGHT_HAND][3] = sigmoid(m1_body_lm_39x5[16 * 5 + 3]);
    }

    // 3. Batched Hands (slots 0 = left, 1 = right; finger curl -> 30 finger bones)
    const auto process_hand = [&](bool active, int batch_idx, uint32_t thumb_base, float handed_sign,
                                  float& out_hand_conf, uint32_t flag_bit) {
        if (!active || m1_hands_lm_2x63 == nullptr || m1_hands_flag_2 == nullptr) {
            out_hand_conf = 0.0f;
            return;
        }
        const float hconf = std::clamp(m1_hands_flag_2[batch_idx], 0.0f, 1.0f);
        out_hand_conf = hconf;
        if (hconf < 0.30f) {
            return;
        }
        valid_flags |= flag_bit;
        const float* lm = &m1_hands_lm_2x63[static_cast<ptrdiff_t>(batch_idx) * 63];
        const Vec3 wrist{lm[0], lm[1], lm[2]};
        const Vec3 mid_mcp{lm[9 * 3 + 0], lm[9 * 3 + 1], lm[9 * 3 + 2]};
        const Vec3 idx_mcp{lm[5 * 3 + 0], lm[5 * 3 + 1], lm[5 * 3 + 2]};
        const Vec3 pnk_mcp{lm[17 * 3 + 0], lm[17 * 3 + 1], lm[17 * 3 + 2]};
        const Vec3 palm_dir = animation::normalize_safe(mid_mcp - wrist, {0.0f, -1.0f, 0.0f});
        const Vec3 palm_norm =
            animation::normalize_safe(animation::cross(idx_mcp - wrist, pnk_mcp - wrist), {0.0f, 0.0f, 1.0f});

        static constexpr int kMcp[5] = {1, 5, 9, 13, 17};
        for (int f = 0; f < 5; ++f) {
            const int b0 = kMcp[f];
            const Quat curl = solve_finger_curl(lm, b0, b0 + 1, b0 + 2, palm_dir, palm_norm, handed_sign);
            for (int ph = 0; ph < 3; ++ph) {
                const uint32_t bone = thumb_base + static_cast<uint32_t>(f * 3 + ph);
                out_frame.joint_rotations_quat[bone][0] = curl.x;
                out_frame.joint_rotations_quat[bone][1] = curl.y;
                out_frame.joint_rotations_quat[bone][2] = curl.z;
                out_frame.joint_rotations_quat[bone][3] = curl.w;
                out_frame.joint_positions_ws[bone][3] = hconf;
            }
        }
    };
    process_hand(run_left_hand, 0, AETHER_BONE_LEFT_THUMB_PROXIMAL, 1.0f, out_frame.left_hand_confidence, 0x02U);
    process_hand(run_right_hand, 1, AETHER_BONE_RIGHT_THUMB_PROXIMAL, -1.0f, out_frame.right_hand_confidence, 0x04U);

    out_frame.valid_flags = valid_flags;
}

} // namespace aether::tracking
