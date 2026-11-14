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

namespace {

[[nodiscard]] bool solve_linear_6x6(float a[6][6], float b[6], float x_out[6]) noexcept {
    for (int col = 0; col < 6; ++col) {
        int pivot_row = col;
        float best_abs = std::abs(a[col][col]);
        for (int row = col + 1; row < 6; ++row) {
            const float cand = std::abs(a[row][col]);
            if (cand > best_abs) {
                best_abs = cand;
                pivot_row = row;
            }
        }
        if (best_abs < 1e-10f) {
            return false;
        }
        if (pivot_row != col) {
            for (int c = col; c < 6; ++c) {
                std::swap(a[col][c], a[pivot_row][c]);
            }
            std::swap(b[col], b[pivot_row]);
        }
        const float inv_diag = 1.0f / a[col][col];
        for (int row = col + 1; row < 6; ++row) {
            const float factor = a[row][col] * inv_diag;
            if (factor == 0.0f) {
                continue;
            }
            for (int c = col; c < 6; ++c) {
                a[row][c] -= factor * a[col][c];
            }
            b[row] -= factor * b[col];
        }
    }
    for (int row = 5; row >= 0; --row) {
        float sum = b[row];
        for (int c = row + 1; c < 6; ++c) {
            sum -= a[row][c] * x_out[c];
        }
        x_out[row] = sum / a[row][row];
    }
    return true;
}

// Extracts (pitch, yaw, roll) in radians matching R = Ry(yaw) * Rx(pitch) * Rz(roll) in Y-up, +Z camera coords.
void quat_to_euler_yxz(Quat q, float& out_pitch, float& out_yaw, float& out_roll) noexcept {
    q = animation::normalize(q);
    const Vec3 ex = animation::rotate(q, {1.0f, 0.0f, 0.0f});
    const Vec3 ey = animation::rotate(q, {0.0f, 1.0f, 0.0f});
    const Vec3 ez = animation::rotate(q, {0.0f, 0.0f, 1.0f});
    const float sin_p = std::clamp(-ez.y, -1.0f, 1.0f);
    out_pitch = std::asin(sin_p);
    const float cos_p = std::cos(out_pitch);
    if (std::abs(cos_p) > 1e-3f) {
        out_yaw = std::atan2(ez.x, ez.z);
        out_roll = std::atan2(ex.y, ey.y);
    } else {
        out_yaw = std::atan2(-ex.z, ex.x);
        out_roll = 0.0f;
    }
}

[[nodiscard]] Quat euler_yxz_to_quat(float pitch_rad, float yaw_rad, float roll_rad) noexcept {
    const Quat qy = animation::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, yaw_rad);
    const Quat qx = animation::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, pitch_rad);
    const Quat qz = animation::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, roll_rad);
    return animation::normalize(qy * qx * qz);
}

void map_m2_crop_to_cam_px(float u_m2, float v_m2, const RoiRectNorm& face_roi, uint32_t cam_w, uint32_t cam_h,
                            float& out_u, float& out_v) noexcept {
    const float nx = (u_m2 / 256.0f) - 0.5f;
    const float ny = (v_m2 / 256.0f) - 0.5f;
    const float c = std::cos(face_roi.rotation_rad);
    const float s = std::sin(face_roi.rotation_rad);
    const float rx = (nx * c - ny * s) * face_roi.width;
    const float ry = (nx * s + ny * c) * face_roi.height;
    out_u = (face_roi.cx + rx) * static_cast<float>(cam_w);
    out_v = (face_roi.cy + ry) * static_cast<float>(cam_h);
}

[[nodiscard]] float dist2d_px(const float* lm, int a, int b) noexcept {
    const float dx = lm[a * 3 + 0] - lm[b * 3 + 0];
    const float dy = lm[a * 3 + 1] - lm[b * 3 + 1];
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

HeadPose6D solve_pnp_head_pose(const float* landmarks_478x3, const RoiRectNorm& face_roi, uint32_t cam_w,
                               uint32_t cam_h, const HeadPose6D* warm_start) noexcept {
    HeadPose6D out{};
    if (landmarks_478x3 == nullptr || !face_roi.valid || cam_w == 0U || cam_h == 0U) {
        return out;
    }

    constexpr uint32_t kPnpCount = 17U;
    static constexpr uint16_t kSkullIdx[kPnpCount] = {
        4, 5, 197, 6, 168, 10, 33, 133, 263, 362, 127, 356, 234, 454, 61, 291, 152,
    };
    static constexpr float kSkullWeights[kPnpCount] = {
        1.60f, 1.30f, 1.30f, 1.40f, 1.40f, 0.90f, 1.60f, 1.60f, 1.60f,
        1.60f, 0.80f, 0.80f, 0.80f, 0.80f, 0.35f, 0.35f, 0.40f,
    };

    const float wf = static_cast<float>(cam_w);
    const float hf = static_cast<float>(cam_h);
    const float fx = wf;
    const float fy = wf;
    const float cx = 0.5f * wf;
    const float cy = 0.5f * hf;

    float obs_x[kPnpCount]{};
    float obs_y[kPnpCount]{};
    Vec3 obj_pts[kPnpCount]{};
    float weights[kPnpCount]{};

    for (uint32_t k = 0; k < kPnpCount; ++k) {
        const uint16_t idx = kSkullIdx[k];
        float u = 0.0f;
        float v = 0.0f;
        map_m2_crop_to_cam_px(landmarks_478x3[idx * 3U + 0U], landmarks_478x3[idx * 3U + 1U], face_roi, cam_w, cam_h, u,
                              v);
        obs_x[k] = (u - cx) / fx;
        obs_y[k] = (cy - v) / fy; // +Y UP normalized pinhole plane
        obj_pts[k] = {kCanonicalFaceVerticesM[idx][0], kCanonicalFaceVerticesM[idx][1],
                      kCanonicalFaceVerticesM[idx][2]};
        weights[k] = kSkullWeights[k];
    }

    // Analytic weak-perspective depth & translation seed (index 6=L eye outer 33, index 8=R eye outer 263)
    const float dx_eye = obs_x[8] - obs_x[6];
    const float dy_eye = obs_y[8] - obs_y[6];
    const float d2d_eye = std::sqrt(dx_eye * dx_eye + dy_eye * dy_eye);
    if (!std::isfinite(d2d_eye) || d2d_eye < 1e-4f) {
        return out;
    }
    constexpr float kCanonicalOuterEyeSpanM = 0.088918f;
    const float est_depth = std::clamp(kCanonicalOuterEyeSpanM / d2d_eye, 0.22f, 2.50f);
    const float mid_eye_x = 0.5f * (obs_x[6] + obs_x[8]);
    const float mid_eye_y = 0.5f * (obs_y[6] + obs_y[8]);
    const float init_roll = std::atan2(dy_eye, dx_eye);

    Quat q_analytic = animation::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, init_roll);
    Vec3 t_analytic{mid_eye_x * est_depth, (mid_eye_y - 0.026f / est_depth) * est_depth, -est_depth};

    auto eval_cost = [&](Quat q_test, Vec3 t_test) noexcept -> float {
        float cost = 0.0f;
        for (uint32_t k = 0; k < kPnpCount; ++k) {
            const Vec3 r = animation::rotate(q_test, obj_pts[k]);
            const Vec3 p = r + t_test;
            const float d = -p.z;
            if (d < 0.08f) {
                return 1e9f;
            }
            const float ex = (p.x / d) - obs_x[k];
            const float ey = (p.y / d) - obs_y[k];
            cost += weights[k] * (ex * ex + ey * ey);
        }
        return cost;
    };

    Quat q = q_analytic;
    Vec3 t = t_analytic;
    float cur_cost = eval_cost(q, t);

    if (warm_start != nullptr && warm_start->valid) {
        const Quat q_w = animation::normalize(
            {warm_start->rotation_xyzw[0], warm_start->rotation_xyzw[1], warm_start->rotation_xyzw[2],
             warm_start->rotation_xyzw[3]});
        const Vec3 t_w{warm_start->position_m[0], warm_start->position_m[1], warm_start->position_m[2]};
        const float warm_cost = eval_cost(q_w, t_w);
        if (warm_cost < cur_cost * 1.25f) {
            q = q_w;
            t = t_w;
            cur_cost = warm_cost;
        }
    }

    // Damped Levenberg-Marquardt IRLS (14 iterations, analytical 6-DoF pinhole Jacobian, zero heap allocs)
    float lambda = 1e-3f;
    for (int iter = 0; iter < 14; ++iter) {
        float h[6][6]{};
        float g[6]{};

        for (uint32_t k = 0; k < kPnpCount; ++k) {
            const Vec3 r = animation::rotate(q, obj_pts[k]);
            const Vec3 p = r + t;
            const float d = std::max(-p.z, 0.08f);
            const float inv_d = 1.0f / d;
            const float inv_d2 = inv_d * inv_d;

            const float rx_e = (p.x * inv_d) - obs_x[k];
            const float ry_e = (p.y * inv_d) - obs_y[k];
            const float err_mag = std::sqrt(rx_e * rx_e + ry_e * ry_e);
            const float huber = (err_mag > 0.015f) ? (0.015f / err_mag) : 1.0f;
            const float w = weights[k] * huber;

            const float j0[6] = {
                p.x * inv_d2 * r.y,
                inv_d * r.z - p.x * inv_d2 * r.x,
                -inv_d * r.y,
                inv_d,
                0.0f,
                p.x * inv_d2,
            };
            const float j1[6] = {
                -inv_d * r.z + p.y * inv_d2 * r.y,
                -p.y * inv_d2 * r.x,
                inv_d * r.x,
                0.0f,
                inv_d,
                p.y * inv_d2,
            };

            for (int c = 0; c < 6; ++c) {
                g[c] += w * (j0[c] * rx_e + j1[c] * ry_e);
                for (int d_col = c; d_col < 6; ++d_col) {
                    h[c][d_col] += w * (j0[c] * j0[d_col] + j1[c] * j1[d_col]);
                }
            }
        }

        for (int c = 0; c < 6; ++c) {
            for (int d_col = 0; d_col < c; ++d_col) {
                h[c][d_col] = h[d_col][c];
            }
            h[c][c] += lambda * (1.0f + h[c][c]);
        }

        float rhs[6]{-g[0], -g[1], -g[2], -g[3], -g[4], -g[5]};
        float step[6]{};
        if (!solve_linear_6x6(h, rhs, step)) {
            lambda = std::min(lambda * 10.0f, 1e4f);
            continue;
        }

        const Vec3 d_rot{step[0], step[1], step[2]};
        const Vec3 d_trans{step[3], step[4], step[5]};
        const float step_norm2 = animation::dot(d_rot, d_rot) + animation::dot(d_trans, d_trans);
        if (step_norm2 < 1e-12f) {
            break;
        }

        const Quat q_cand = animation::normalize(animation::rotvec_to_quat(d_rot) * q);
        const Vec3 t_cand = t + d_trans;
        const float cand_cost = eval_cost(q_cand, t_cand);
        if (cand_cost < cur_cost) {
            q = q_cand;
            t = t_cand;
            cur_cost = cand_cost;
            lambda = std::max(lambda * 0.35f, 1e-6f);
        } else {
            lambda = std::min(lambda * 5.0f, 1e4f);
        }
    }

    if (!std::isfinite(t.z) || t.z > -0.10f || t.z < -4.0f) {
        return out;
    }

    double sq_px_err = 0.0;
    for (uint32_t k = 0; k < kPnpCount; ++k) {
        const Vec3 p = animation::rotate(q, obj_pts[k]) + t;
        const float d = -p.z;
        if (d < 0.08f) {
            return out;
        }
        const float du = ((p.x / d) - obs_x[k]) * fx;
        const float dv = ((p.y / d) - obs_y[k]) * fy;
        sq_px_err += static_cast<double>(du) * du + static_cast<double>(dv) * dv;
    }
    const float rms_px = static_cast<float>(std::sqrt(sq_px_err / kPnpCount));
    if (!std::isfinite(rms_px) || rms_px > 64.0f) {
        return out;
    }

    out.rotation_xyzw[0] = q.x;
    out.rotation_xyzw[1] = q.y;
    out.rotation_xyzw[2] = q.z;
    out.rotation_xyzw[3] = q.w;
    out.position_m[0] = t.x;
    out.position_m[1] = t.y;
    out.position_m[2] = t.z;
    out.reproj_rms_px = rms_px;
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
    stable_lock_frames_ = 0U;
    neutral_ear_l_ = 0.28f;
    neutral_ear_r_ = 0.28f;
    neutral_mar_open_ = 0.04f;
    neutral_mouth_width_ratio_ = 0.52f;
    blink_closed_l_ = false;
    blink_closed_r_ = false;
    last_pnp_pose_ = HeadPose6D{};
    prev_raw_head_quat_[0] = 0.0f;
    prev_raw_head_quat_[1] = 0.0f;
    prev_raw_head_quat_[2] = 0.0f;
    prev_raw_head_quat_[3] = 1.0f;
    has_prev_raw_quat_ = false;
    last_emitted_head_quat_[0] = 0.0f;
    last_emitted_head_quat_[1] = 0.0f;
    last_emitted_head_quat_[2] = 0.0f;
    last_emitted_head_quat_[3] = 1.0f;
    has_emitted_head_quat_ = false;
    overlay_ = TrackingOverlaySnapshot{};
}

void LandmarkPoseSolver::calibrate_neutral() noexcept {
    calibrate_next_ = true;
}

RoiRectNorm LandmarkPoseSolver::compute_next_face_roi(const float* m2_landmarks_478x3, const RoiRectNorm& prev_face_roi,
                                                      float face_flag, uint32_t cam_w, uint32_t cam_h) const noexcept {
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
    RoiRectNorm cand{};
    cand.cx = std::clamp(prev_face_roi.cx + (lc_x * c - lc_y * s) * prev_face_roi.width, 0.08f, 0.92f);
    cand.cy = std::clamp(prev_face_roi.cy + (lc_x * s + lc_y * c) * prev_face_roi.height, 0.08f, 0.92f);
    // MediaPipe FaceLandmarksToRoi convention: square crop of the landmark bbox scaled 1.5x, rotated so the
    // eye line (33 -> 263) is horizontal. The face-mesh network was trained on exactly that framing; a tighter
    // (1.32x) or un-rotated crop pushes its regression toward the mean face, so the dots stop following the
    // user's real eyes / brows / lips and roll tracking degrades.
    const float span_x_px = ((max_x - min_x) / 256.0f) * prev_face_roi.width * static_cast<float>(std::max(cam_w, 1U));
    const float span_y_px = ((max_y - min_y) / 256.0f) * prev_face_roi.height * static_cast<float>(std::max(cam_h, 1U));
    const float target_px = std::max(span_x_px, span_y_px) * 1.5f;
    cand.width = target_px / static_cast<float>(std::max(cam_w, 1U));
    cand.height = target_px / static_cast<float>(std::max(cam_h, 1U));
    const float eye_dx = m2_landmarks_478x3[263U * 3U + 0U] - m2_landmarks_478x3[33U * 3U + 0U];
    const float eye_dy = m2_landmarks_478x3[263U * 3U + 1U] - m2_landmarks_478x3[33U * 3U + 1U];
    float rot = prev_face_roi.rotation_rad;
    if (eye_dx * eye_dx + eye_dy * eye_dy > 4.0f) {
        rot += std::atan2(eye_dy, eye_dx); // crop is square in pixels, so in-crop angles compose additively
    }
    rot = std::remainder(rot, 2.0f * animation::kPi);
    cand.rotation_rad = std::clamp(rot, -1.2f, 1.2f);
    cand.valid = true;
    cand = enforce_square_pixel_roi(cand, cam_w, cam_h, 0.16f, 0.56f);

    // Critical anti-jitter hysteresis gate: freeze crop completely when movement < 2.5% of box width and the
    // eye-line roll changed by < 2 degrees (a static crop gives the network a static input -> zero dot shimmer).
    const float dx_ratio = std::abs(cand.cx - prev_face_roi.cx) / std::max(prev_face_roi.width, 1e-4f);
    const float dy_ratio = std::abs(cand.cy - prev_face_roi.cy) / std::max(prev_face_roi.height, 1e-4f);
    const float dw_ratio = std::abs(cand.width - prev_face_roi.width) / std::max(prev_face_roi.width, 1e-4f);
    const float dh_ratio = std::abs(cand.height - prev_face_roi.height) / std::max(prev_face_roi.height, 1e-4f);
    const float drot = std::abs(std::remainder(cand.rotation_rad - prev_face_roi.rotation_rad, 2.0f * animation::kPi));
    if (dx_ratio < 0.025f && dy_ratio < 0.025f && dw_ratio < 0.05f && dh_ratio < 0.05f &&
        drot < 2.0f * animation::kDegToRad) {
        return prev_face_roi;
    }
    return cand;
}

RoiRectNorm LandmarkPoseSolver::estimate_face_roi_from_m1(const float* m1_body_lm_39x5, const RoiRectNorm& lb_roi,
                                                            float pose_flag, uint32_t cam_w,
                                                            uint32_t cam_h) const noexcept {
    if (m1_body_lm_39x5 == nullptr || pose_flag < 0.35f) {
        return {};
    }
    float min_u = 1.0f;
    float min_v = 1.0f;
    float max_u = 0.0f;
    float max_v = 0.0f;
    float sum_u = 0.0f;
    float sum_v = 0.0f;
    uint32_t vis_count = 0U;
    for (int i = 0; i <= 10; ++i) {
        const float vis = sigmoid(m1_body_lm_39x5[i * 5 + 3]);
        const float pres = sigmoid(m1_body_lm_39x5[i * 5 + 4]);
        if (std::min(vis, pres) < 0.40f) {
            continue;
        }
        const float lu = m1_body_lm_39x5[i * 5 + 0] / 256.0f;
        const float lv = m1_body_lm_39x5[i * 5 + 1] / 256.0f;
        const float u = lb_roi.cx + (lu - 0.5f) * lb_roi.width;
        const float v = lb_roi.cy + (lv - 0.5f) * lb_roi.height;
        if (u < 0.02f || u > 0.98f || v < 0.02f || v > 0.92f) {
            continue;
        }
        min_u = std::min(min_u, u);
        min_v = std::min(min_v, v);
        max_u = std::max(max_u, u);
        max_v = std::max(max_v, v);
        sum_u += u;
        sum_v += v;
        ++vis_count;
    }
    if (vis_count < 3U) {
        return {};
    }
    const float fw = static_cast<float>(std::max(cam_w, 1U));
    const float fh = static_cast<float>(std::max(cam_h, 1U));
    const float span_px = std::max((max_u - min_u) * fw, (max_v - min_v) * fh) * 1.95f;
    RoiRectNorm roi{};
    roi.cx = std::clamp(sum_u / static_cast<float>(vis_count), 0.10f, 0.90f);
    roi.cy = std::clamp(sum_v / static_cast<float>(vis_count), 0.10f, 0.85f);
    roi.width = span_px / fw;
    roi.height = span_px / fh;
    roi.rotation_rad = 0.0f;
    roi.valid = true;
    return enforce_square_pixel_roi(roi, cam_w, cam_h, 0.18f, 0.55f);
}

void LandmarkPoseSolver::populate_tracking_frame(
    const RoiRectNorm& face_roi, const float* m2_landmarks_478x3, float face_flag, const float* m3_blendshapes_52,
    const float* m1_body_world_39x3, const float* m1_body_lm_39x5, float pose_flag, bool run_left_hand,
    bool run_right_hand, const float* m1_hands_lm_2x63, const float* m1_hands_flag_2,
    const float* /*m1_hands_handed_2*/, uint64_t capture_timestamp_us, uint64_t inference_complete_us, EpTier active_ep,
    uint32_t frame_sequence, bool roi_reset_this_frame, AetherTrackingFrame& out_frame, uint32_t cam_w,
    uint32_t cam_h) noexcept {
    out_frame = AetherTrackingFrame{};
    out_frame.capture_timestamp_us = capture_timestamp_us;
    out_frame.inference_complete_us = inference_complete_us;
    out_frame.active_execution_provider = to_ffi_ep(active_ep);
    out_frame.frame_sequence = frame_sequence;

    overlay_ = TrackingOverlaySnapshot{};
    overlay_.face_roi = face_roi;
    overlay_.neutral_calibrated = neutral_calibrated_;
    overlay_.stable_lock_frames = stable_lock_frames_;

    uint32_t valid_flags = roi_reset_this_frame ? 0x10U : 0U;
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        out_frame.joint_rotations_quat[b][3] = 1.0f;
    }

    // 1. Classic Rigid Skull solvePnP + Auto-Neutral Calibration + Live2D EAR/MAR Geometric Ratios
    if (m2_landmarks_478x3 != nullptr && face_flag >= 0.45f && face_roi.valid) {
        valid_flags |= 0x01U;
        overlay_.face_locked = true;

        HeadPose6D hp =
            solve_pnp_head_pose(m2_landmarks_478x3, face_roi, cam_w, cam_h, last_pnp_pose_.valid ? &last_pnp_pose_ : nullptr);
        if (hp.valid) {
            last_pnp_pose_ = hp;
        } else if (last_pnp_pose_.valid) {
            hp = last_pnp_pose_;
        }

        const Quat q_raw = hp.valid
                               ? animation::normalize(
                                     {hp.rotation_xyzw[0], hp.rotation_xyzw[1], hp.rotation_xyzw[2], hp.rotation_xyzw[3]})
                               : Quat::identity();
        const Vec3 t_raw = hp.valid ? Vec3{hp.position_m[0], hp.position_m[1], hp.position_m[2]} : Vec3{0.0f, 0.0f, -0.55f};

        // Check inter-frame stability for 15-frame auto-neutral lock
        if (hp.valid && !roi_reset_this_frame) {
            if (has_prev_raw_quat_) {
                const Quat prev_q{prev_raw_head_quat_[0], prev_raw_head_quat_[1], prev_raw_head_quat_[2],
                                  prev_raw_head_quat_[3]};
                const float d_q = std::clamp(std::abs(animation::dot(q_raw, prev_q)), 0.0f, 1.0f);
                const float delta_deg = (2.0f * std::acos(d_q)) * animation::kRadToDeg;
                if (delta_deg <= 2.5f) {
                    stable_lock_frames_ = std::min(stable_lock_frames_ + 1U, 1000U);
                } else {
                    stable_lock_frames_ = 1U;
                }
            } else {
                stable_lock_frames_ = 1U;
            }
            prev_raw_head_quat_[0] = q_raw.x;
            prev_raw_head_quat_[1] = q_raw.y;
            prev_raw_head_quat_[2] = q_raw.z;
            prev_raw_head_quat_[3] = q_raw.w;
            has_prev_raw_quat_ = true;
        } else {
            stable_lock_frames_ = 0U;
            has_prev_raw_quat_ = false;
        }

        // Compute geometric EAR & MAR normalized by 2D inter-ocular distance
        const float iod = std::max(dist2d_px(m2_landmarks_478x3, 33, 263), 4.0f);
        const float ear_l = 0.5f * (dist2d_px(m2_landmarks_478x3, 160, 144) + dist2d_px(m2_landmarks_478x3, 158, 153)) /
                            std::max(dist2d_px(m2_landmarks_478x3, 33, 133), 2.0f);
        const float ear_r = 0.5f * (dist2d_px(m2_landmarks_478x3, 385, 380) + dist2d_px(m2_landmarks_478x3, 387, 373)) /
                            std::max(dist2d_px(m2_landmarks_478x3, 263, 362), 2.0f);
        const float mar_open = dist2d_px(m2_landmarks_478x3, 13, 14) / std::max(dist2d_px(m2_landmarks_478x3, 78, 308), 2.0f);
        const float mouth_width_ratio = dist2d_px(m2_landmarks_478x3, 61, 291) / iod;

        if (calibrate_next_ || (!neutral_calibrated_ && stable_lock_frames_ >= kAutoNeutralStableFrames)) {
            neutral_head_quat_[0] = q_raw.x;
            neutral_head_quat_[1] = q_raw.y;
            neutral_head_quat_[2] = q_raw.z;
            neutral_head_quat_[3] = q_raw.w;
            neutral_head_pos_[0] = t_raw.x;
            neutral_head_pos_[1] = t_raw.y;
            neutral_head_pos_[2] = t_raw.z;
            neutral_ear_l_ = std::clamp(ear_l, 0.18f, 0.42f);
            neutral_ear_r_ = std::clamp(ear_r, 0.18f, 0.42f);
            neutral_mar_open_ = std::clamp(mar_open, 0.0f, 0.18f);
            neutral_mouth_width_ratio_ = std::clamp(mouth_width_ratio, 0.35f, 0.72f);
            neutral_calibrated_ = true;
            calibrate_next_ = false;
            last_emitted_head_quat_[0] = 0.0f;
            last_emitted_head_quat_[1] = 0.0f;
            last_emitted_head_quat_[2] = 0.0f;
            last_emitted_head_quat_[3] = 1.0f;
            has_emitted_head_quat_ = true;
        }

        Quat q_head = Quat::identity();
        Vec3 t_head{};
        if (neutral_calibrated_) {
            const Quat q_neutral{neutral_head_quat_[0], neutral_head_quat_[1], neutral_head_quat_[2],
                                 neutral_head_quat_[3]};
            q_head = animation::normalize(animation::conjugate(q_neutral) * q_raw);
            t_head = {t_raw.x - neutral_head_pos_[0], t_raw.y - neutral_head_pos_[1],
                      t_raw.z - neutral_head_pos_[2]};
        }

        // Strict anatomical limits: Pitch [-35, +35], Yaw [-45, +45], Roll [-30, +30] degrees
        float pitch_r = 0.0f;
        float yaw_r = 0.0f;
        float roll_r = 0.0f;
        quat_to_euler_yxz(q_head, pitch_r, yaw_r, roll_r);
        pitch_r = std::clamp(pitch_r, -35.0f * animation::kDegToRad, 35.0f * animation::kDegToRad);
        yaw_r = std::clamp(yaw_r, -45.0f * animation::kDegToRad, 45.0f * animation::kDegToRad);
        roll_r = std::clamp(roll_r, -30.0f * animation::kDegToRad, 30.0f * animation::kDegToRad);
        q_head = euler_yxz_to_quat(pitch_r, yaw_r, roll_r);

        // 0.15 deg angular deadband on R_delta so sitting still yields zero head tremor
        constexpr float kDeadbandRad = 0.15f * animation::kDegToRad;
        if (has_emitted_head_quat_) {
            const Quat prev_e{last_emitted_head_quat_[0], last_emitted_head_quat_[1], last_emitted_head_quat_[2],
                              last_emitted_head_quat_[3]};
            const float d = std::clamp(std::abs(animation::dot(q_head, prev_e)), 0.0f, 1.0f);
            const float ang = 2.0f * std::acos(d);
            if (ang <= kDeadbandRad) {
                q_head = prev_e;
            } else {
                q_head = animation::slerp(prev_e, q_head, (ang - kDeadbandRad) / ang);
            }
        }
        last_emitted_head_quat_[0] = q_head.x;
        last_emitted_head_quat_[1] = q_head.y;
        last_emitted_head_quat_[2] = q_head.z;
        last_emitted_head_quat_[3] = q_head.w;
        has_emitted_head_quat_ = true;

        quat_to_euler_yxz(q_head, pitch_r, yaw_r, roll_r);
        overlay_.neutral_calibrated = neutral_calibrated_;
        overlay_.stable_lock_frames = stable_lock_frames_;
        overlay_.head_euler_deg = {pitch_r * animation::kRadToDeg, yaw_r * animation::kRadToDeg,
                                   roll_r * animation::kRadToDeg};

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

        // Geometric iris gaze reinforcement for VRM blendshapes (`eyeLookDown/In/Out/Up`, indices 1..4 L, 8..11 R)
        const auto blend_eye_gaze_bs = [&](int iris, int outer_c, int inner_c, int top_l, int bot_l, size_t bs_base,
                                           bool is_left_screen) noexcept {
            const float cx = 0.5f * (m2_landmarks_478x3[outer_c * 3 + 0] + m2_landmarks_478x3[inner_c * 3 + 0]);
            const float cy = 0.5f * (m2_landmarks_478x3[top_l * 3 + 1] + m2_landmarks_478x3[bot_l * 3 + 1]);
            const float ew =
                std::max(1.0f, std::abs(m2_landmarks_478x3[inner_c * 3 + 0] - m2_landmarks_478x3[outer_c * 3 + 0]));
            const float eh =
                std::max(1.0f, std::abs(m2_landmarks_478x3[bot_l * 3 + 1] - m2_landmarks_478x3[top_l * 3 + 1]));
            const float gx = std::clamp((m2_landmarks_478x3[iris * 3 + 0] - cx) / (0.28f * ew), -1.0f, 1.0f);
            const float gy = std::clamp((m2_landmarks_478x3[iris * 3 + 1] - cy) / (0.28f * eh), -1.0f, 1.0f);
            const float g_down = std::max(0.0f, gy);
            const float g_up = std::max(0.0f, -gy);
            const float g_in = is_left_screen ? std::max(0.0f, -gx) : std::max(0.0f, gx);
            const float g_out = is_left_screen ? std::max(0.0f, gx) : std::max(0.0f, -gx);
            out_frame.arkit_blendshapes[bs_base + 0] =
                std::clamp(0.70f * g_down + 0.30f * out_frame.arkit_blendshapes[bs_base + 0], 0.0f, 1.0f);
            out_frame.arkit_blendshapes[bs_base + 1] =
                std::clamp(0.70f * g_in + 0.30f * out_frame.arkit_blendshapes[bs_base + 1], 0.0f, 1.0f);
            out_frame.arkit_blendshapes[bs_base + 2] =
                std::clamp(0.70f * g_out + 0.30f * out_frame.arkit_blendshapes[bs_base + 2], 0.0f, 1.0f);
            out_frame.arkit_blendshapes[bs_base + 3] =
                std::clamp(0.70f * g_up + 0.30f * out_frame.arkit_blendshapes[bs_base + 3], 0.0f, 1.0f);
        };
        blend_eye_gaze_bs(468, 33, 133, 159, 145, 1U, true);
        blend_eye_gaze_bs(473, 263, 362, 386, 374, 8U, false);

        // Hysteresis EAR blink & Live2D MAR overrides (prevents eyelid flicker & rest mouth drift)
        auto eval_hyst_blink = [](float ear, float neutral_ear, bool& closed_state) noexcept -> float {
            const float close_t = neutral_ear * 0.65f;
            const float open_t = neutral_ear * 0.78f;
            if (closed_state) {
                if (ear >= open_t) {
                    closed_state = false;
                }
            } else {
                if (ear <= close_t) {
                    closed_state = true;
                }
            }
            const float raw_blink =
                std::clamp((neutral_ear * 0.88f - ear) / std::max(neutral_ear * 0.30f, 0.04f), 0.0f, 1.0f);
            if (closed_state) {
                return std::max(raw_blink, 0.94f);
            }
            return (raw_blink < 0.08f) ? 0.0f : (raw_blink * 0.80f);
        };
        const float blink_l = eval_hyst_blink(ear_l, neutral_ear_l_, blink_closed_l_);
        const float blink_r = eval_hyst_blink(ear_r, neutral_ear_r_, blink_closed_r_);
        out_frame.arkit_blendshapes[0] = std::clamp(blink_l * 0.80f + out_frame.arkit_blendshapes[0] * 0.20f, 0.0f, 1.0f);
        out_frame.arkit_blendshapes[7] = std::clamp(blink_r * 0.80f + out_frame.arkit_blendshapes[7] * 0.20f, 0.0f, 1.0f);

        const float mar_delta = std::max(0.0f, mar_open - neutral_mar_open_ - 0.025f);
        const float jaw_open_geom = std::clamp(mar_delta / 0.38f, 0.0f, 1.0f);
        const float w_delta = mouth_width_ratio - neutral_mouth_width_ratio_;
        const float smile_geom = std::clamp((w_delta - 0.020f) / 0.13f, 0.0f, 1.0f);
        const float pucker_geom = std::clamp((-w_delta - 0.020f) / 0.11f, 0.0f, 1.0f);
        const float narrow_ratio = std::clamp(
            1.0f - ((mouth_width_ratio / std::max(neutral_mouth_width_ratio_, 0.25f)) - 0.75f) * 3.0f, 0.0f, 1.0f);
        const float funnel_geom = std::clamp(jaw_open_geom * narrow_ratio, 0.0f, 1.0f);
        const float stretch_geom =
            std::clamp((w_delta - 0.012f) / 0.11f * (1.0f - funnel_geom * 0.6f), 0.0f, 1.0f);
        out_frame.arkit_blendshapes[17] =
            std::clamp(jaw_open_geom * 0.72f + out_frame.arkit_blendshapes[17] * 0.28f, 0.0f, 1.0f);
        out_frame.arkit_blendshapes[19] =
            std::clamp(funnel_geom * 0.68f + out_frame.arkit_blendshapes[19] * 0.32f, 0.0f, 1.0f);
        out_frame.arkit_blendshapes[20] =
            std::clamp(pucker_geom * 0.68f + out_frame.arkit_blendshapes[20] * 0.32f, 0.0f, 1.0f);
        out_frame.arkit_blendshapes[23] =
            std::clamp(smile_geom * 0.65f + out_frame.arkit_blendshapes[23] * 0.35f, 0.0f, 1.0f);
        out_frame.arkit_blendshapes[24] =
            std::clamp(smile_geom * 0.65f + out_frame.arkit_blendshapes[24] * 0.35f, 0.0f, 1.0f);
        out_frame.arkit_blendshapes[29] =
            std::clamp(stretch_geom * 0.60f + out_frame.arkit_blendshapes[29] * 0.40f, 0.0f, 1.0f);
        out_frame.arkit_blendshapes[30] =
            std::clamp(stretch_geom * 0.60f + out_frame.arkit_blendshapes[30] * 0.40f, 0.0f, 1.0f);
        out_frame.arkit_blendshapes[37] =
            std::clamp(jaw_open_geom * 0.45f + out_frame.arkit_blendshapes[37] * 0.55f, 0.0f, 1.0f);
        out_frame.arkit_blendshapes[38] =
            std::clamp(jaw_open_geom * 0.45f + out_frame.arkit_blendshapes[38] * 0.55f, 0.0f, 1.0f);
        out_frame.arkit_blendshapes[39] =
            std::clamp(jaw_open_geom * 0.30f + out_frame.arkit_blendshapes[39] * 0.70f, 0.0f, 1.0f);
        out_frame.arkit_blendshapes[40] =
            std::clamp(jaw_open_geom * 0.30f + out_frame.arkit_blendshapes[40] * 0.70f, 0.0f, 1.0f);

        // Export HUD overlay landmarks (normalized [0..1] camera space) & 3D head orientation triad
        static constexpr uint16_t kHudKeyIndices[24] = {
            33,  133, 159, 145, // L eye outer/inner/top/bot
            263, 362, 386, 374, // R eye outer/inner/top/bot
            468, 473,           // L/R iris centers
            4,   6,   168, 10,  // Nose tip/bridge, glabella, forehead
            61,  291, 13,  14,  // Mouth corners, inner upper/lower
            152, 234, 454,      // Chin, L/R cheeks
            70,  300, 197       // L/R outer brows, mid nose
        };
        const float inv_w = 1.0f / static_cast<float>(std::max(cam_w, 1U));
        const float inv_h = 1.0f / static_cast<float>(std::max(cam_h, 1U));
        overlay_.keypoint_count = 24U;
        for (uint32_t k = 0; k < 24U; ++k) {
            const uint16_t idx = kHudKeyIndices[k];
            float u = 0.0f;
            float v = 0.0f;
            map_m2_crop_to_cam_px(m2_landmarks_478x3[idx * 3U + 0U], m2_landmarks_478x3[idx * 3U + 1U], face_roi, cam_w,
                                  cam_h, u, v);
            overlay_.keypoints_norm[k * 2U + 0U] = std::clamp(u * inv_w, 0.0f, 1.0f);
            overlay_.keypoints_norm[k * 2U + 1U] = std::clamp(v * inv_h, 0.0f, 1.0f);
        }
        if (hp.valid) {
            const float fx = static_cast<float>(std::max(cam_w, 1U));
            const float fy = fx;
            const float cx = 0.5f * static_cast<float>(std::max(cam_w, 1U));
            const float cy = 0.5f * static_cast<float>(std::max(cam_h, 1U));
            const Vec3 p_nose_obj{kCanonicalFaceVerticesM[4][0], kCanonicalFaceVerticesM[4][1],
                                  kCanonicalFaceVerticesM[4][2]};
            constexpr float kAxisLenM = 0.075f;
            const Vec3 pts[4] = {
                p_nose_obj,
                p_nose_obj + Vec3{kAxisLenM, 0.0f, 0.0f},
                p_nose_obj + Vec3{0.0f, kAxisLenM, 0.0f},
                p_nose_obj + Vec3{0.0f, 0.0f, kAxisLenM},
            };
            bool axes_ok = true;
            for (int a = 0; a < 4; ++a) {
                const Vec3 p = animation::rotate(q_raw, pts[a]) + t_raw;
                const float d = -p.z;
                if (d < 0.08f) {
                    axes_ok = false;
                    break;
                }
                const float u = cx + fx * (p.x / d);
                const float v = cy - fy * (p.y / d);
                overlay_.head_axes_norm[static_cast<size_t>(a) * 2U + 0U] = std::clamp(u * inv_w, 0.0f, 1.0f);
                overlay_.head_axes_norm[static_cast<size_t>(a) * 2U + 1U] = std::clamp(v * inv_h, 0.0f, 1.0f);
            }
            overlay_.head_axes_valid = axes_ok;
        }
    } else {
        stable_lock_frames_ = 0U;
        has_prev_raw_quat_ = false;
        blink_closed_l_ = false;
        blink_closed_r_ = false;
    }

    // 2. Upper-Body Pose (strictly gate arms on in-frame elbow+wrist visibility; never overwrite CHEST/HIPS from out-of-frame M1 hips)
    const float body_conf = std::clamp(pose_flag, 0.0f, 1.0f);
    out_frame.upper_body_confidence = body_conf;
    if (m1_body_world_39x3 != nullptr && m1_body_lm_39x5 != nullptr && body_conf >= 0.30f) {
        valid_flags |= 0x08U;
        const Vec3 l_hip{m1_body_world_39x3[23 * 3 + 0], -m1_body_world_39x3[23 * 3 + 1],
                         -m1_body_world_39x3[23 * 3 + 2]};
        const Vec3 r_hip{m1_body_world_39x3[24 * 3 + 0], -m1_body_world_39x3[24 * 3 + 1],
                         -m1_body_world_39x3[24 * 3 + 2]};
        const Vec3 hip_mid = (l_hip + r_hip) * 0.5f;
        const Vec3 l_w{m1_body_world_39x3[15 * 3 + 0], -m1_body_world_39x3[15 * 3 + 1],
                       -m1_body_world_39x3[15 * 3 + 2]};
        const Vec3 r_w{m1_body_world_39x3[16 * 3 + 0], -m1_body_world_39x3[16 * 3 + 1],
                       -m1_body_world_39x3[16 * 3 + 2]};

        const Vec3 hips_ws{0.0f, 0.90f, 0.0f};
        out_frame.joint_positions_ws[AETHER_BONE_HIPS][0] = hips_ws.x;
        out_frame.joint_positions_ws[AETHER_BONE_HIPS][1] = hips_ws.y;
        out_frame.joint_positions_ws[AETHER_BONE_HIPS][2] = hips_ws.z;
        out_frame.joint_positions_ws[AETHER_BONE_HIPS][3] = body_conf;
        out_frame.joint_positions_ws[AETHER_BONE_SPINE][3] = body_conf;

        const RoiRectNorm lb_roi = make_letterbox_square_roi(cam_w, cam_h);
        auto arm_in_frame_conf = [&](int elbow_idx, int wrist_idx) noexcept -> float {
            const float e_vis = sigmoid(m1_body_lm_39x5[elbow_idx * 5 + 3]);
            const float w_vis = sigmoid(m1_body_lm_39x5[wrist_idx * 5 + 3]);
            const float w_pres = sigmoid(m1_body_lm_39x5[wrist_idx * 5 + 4]);
            const float lu = m1_body_lm_39x5[wrist_idx * 5 + 0] / 256.0f;
            const float lv = m1_body_lm_39x5[wrist_idx * 5 + 1] / 256.0f;
            const float u = lb_roi.cx + (lu - 0.5f) * lb_roi.width;
            const float v = lb_roi.cy + (lv - 0.5f) * lb_roi.height;
            if (u < 0.05f || u > 0.95f || v < 0.05f || v > 0.88f) {
                return 0.0f;
            }
            const float min_c = std::min(e_vis, std::min(w_vis, w_pres));
            return (min_c >= 0.65f) ? min_c : 0.0f;
        };

        // Place wrist IK targets relative to hips in avatar world frame only when wrist+elbow are clearly visible in-frame
        const Vec3 lw_ws = hips_ws + (l_w - hip_mid);
        const Vec3 rw_ws = hips_ws + (r_w - hip_mid);
        out_frame.joint_positions_ws[AETHER_BONE_LEFT_HAND][0] = lw_ws.x;
        out_frame.joint_positions_ws[AETHER_BONE_LEFT_HAND][1] = lw_ws.y;
        out_frame.joint_positions_ws[AETHER_BONE_LEFT_HAND][2] = lw_ws.z;
        out_frame.joint_positions_ws[AETHER_BONE_LEFT_HAND][3] = arm_in_frame_conf(13, 15);
        out_frame.joint_positions_ws[AETHER_BONE_RIGHT_HAND][0] = rw_ws.x;
        out_frame.joint_positions_ws[AETHER_BONE_RIGHT_HAND][1] = rw_ws.y;
        out_frame.joint_positions_ws[AETHER_BONE_RIGHT_HAND][2] = rw_ws.z;
        out_frame.joint_positions_ws[AETHER_BONE_RIGHT_HAND][3] = arm_in_frame_conf(14, 16);
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
