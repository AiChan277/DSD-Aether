#include "animation/spring_bone.hpp"

#include <immintrin.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>

namespace aether::animation {

namespace {

[[nodiscard]] SpringBoneSimulator::SimdPath detect_cpu_simd_path() noexcept {
    int info[4]{};
    __cpuid(info, 0);
    if (info[0] >= 7) {
        int info7[4]{};
        __cpuidex(info7, 7, 0);
        const bool has_avx2 = (info7[1] & (1 << 5)) != 0;
        const bool has_avx512f = (info7[1] & (1 << 16)) != 0;
        // Verify OS enabled AVX YMM state via XCR0
        int info1[4]{};
        __cpuid(info1, 1);
        const bool osxsave = (info1[2] & (1 << 27)) != 0;
        if (osxsave) {
            const uint64_t xcr0 = _xgetbv(0);
            if (has_avx512f && ((xcr0 & 0xE6ULL) == 0xE6ULL)) {
                return SpringBoneSimulator::SimdPath::Avx512;
            }
            if (has_avx2 && ((xcr0 & 0x06ULL) == 0x06ULL)) {
                return SpringBoneSimulator::SimdPath::Avx2;
            }
        }
    }
    return SpringBoneSimulator::SimdPath::Scalar;
}

[[nodiscard]] Quat quat_from_to_sb(Vec3 from_dir, Vec3 to_dir) noexcept {
    const Vec3 a = normalize_safe(from_dir);
    const Vec3 b = normalize_safe(to_dir);
    const float d = std::clamp(dot(a, b), -1.0f, 1.0f);
    if (d < -0.9995f) {
        Vec3 ax = cross({1.0f, 0.0f, 0.0f}, a);
        if (length_sq(ax) < 1e-4f) {
            ax = cross({0.0f, 0.0f, 1.0f}, a);
        }
        return from_axis_angle(normalize_safe(ax), kPi);
    }
    const Vec3 c = cross(a, b);
    return normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

[[nodiscard]] Vec3 compute_joint_head_center_space(uint32_t j, const int32_t* parent_joint_idx,
                                                   const int32_t* parent_bone_idx, const int32_t* center_bone_idx,
                                                   const float* local_ox, const float* local_oy, const float* local_oz,
                                                   const float* curr_x, const float* curr_y, const float* curr_z,
                                                   const std::array<Vec3, kMaxBones>& bone_pos_ws,
                                                   const std::array<Quat, kMaxBones>& bone_rot_ws) noexcept {
    Vec3 head_ws{};
    const int32_t pj = parent_joint_idx[j];
    const int32_t cb = center_bone_idx[j];
    const Vec3 c_pos =
        (cb >= 0 && static_cast<uint32_t>(cb) < kMaxBones) ? bone_pos_ws[static_cast<size_t>(cb)] : Vec3{};
    if (pj >= 0 && static_cast<uint32_t>(pj) < j) {
        return {curr_x[pj], curr_y[pj], curr_z[pj]};
    }
    const int32_t pb = parent_bone_idx[j];
    if (pb >= 0 && static_cast<uint32_t>(pb) < kMaxBones) {
        const Quat pr = bone_rot_ws[static_cast<size_t>(pb)];
        head_ws = bone_pos_ws[static_cast<size_t>(pb)] + rotate_vec(pr, {local_ox[j], local_oy[j], local_oz[j]});
    } else {
        head_ws = {local_ox[j], local_oy[j], local_oz[j]};
    }
    return head_ws - c_pos;
}

} // namespace

SpringBoneSimulator::SpringBoneSimulator() noexcept : detected_path_(detect_cpu_simd_path()) {}

void SpringBoneSimulator::clear() noexcept {
    count_ = 0;
    colliders_.count = 0;
    state_initialized_ = false;
    teleport_resets_ = 0;
}

bool SpringBoneSimulator::add_joint(const SpringJointDesc& desc) noexcept {
    if (count_ >= kMaxSpringJoints) {
        return false;
    }
    const uint32_t i = count_++;
    bone_idx_[i] = desc.bone_index;
    parent_joint_idx_[i] = desc.parent_joint_index;
    parent_bone_idx_[i] = desc.parent_bone_index;
    center_bone_idx_[i] = desc.center_bone_index;
    local_offset_x_[i] = desc.local_bone_offset.x;
    local_offset_y_[i] = desc.local_bone_offset.y;
    local_offset_z_[i] = desc.local_bone_offset.z;

    const Vec3 d = normalize_safe(desc.local_bone_offset, {0.0f, -1.0f, 0.0f});
    rest_dir_x_[i] = d.x;
    rest_dir_y_[i] = d.y;
    rest_dir_z_[i] = d.z;

    const Vec3 gd = normalize_safe(desc.gravity_dir, {0.0f, -1.0f, 0.0f});
    grav_x_[i] = gd.x * desc.gravity_power;
    grav_y_[i] = gd.y * desc.gravity_power;
    grav_z_[i] = gd.z * desc.gravity_power;

    stiffness_[i] = std::clamp(desc.stiffness, 0.0f, 4.0f);
    drag_[i] = std::clamp(desc.drag_force, 0.0f, 1.0f);
    bone_len_[i] = std::max(desc.bone_length, 0.005f);
    hit_radius_[i] = std::max(desc.hit_radius, 0.0f);

    curr_x_[i] = desc.local_bone_offset.x;
    curr_y_[i] = desc.local_bone_offset.y - desc.bone_length;
    curr_z_[i] = desc.local_bone_offset.z;
    prev_x_[i] = curr_x_[i];
    prev_y_[i] = curr_y_[i];
    prev_z_[i] = curr_z_[i];
    head_x_[i] = desc.local_bone_offset.x;
    head_y_[i] = desc.local_bone_offset.y;
    head_z_[i] = desc.local_bone_offset.z;
    state_initialized_ = false;
    return true;
}

bool SpringBoneSimulator::add_sphere_collider(int32_t attached_bone, Vec3 offset, float radius) noexcept {
    if (colliders_.count >= kMaxSpringColliders) {
        return false;
    }
    const uint32_t c = colliders_.count++;
    colliders_.shape[c] = static_cast<uint8_t>(ColliderShape::Sphere);
    colliders_.attached_bone[c] = attached_bone;
    colliders_.local_ax[c] = offset.x;
    colliders_.local_ay[c] = offset.y;
    colliders_.local_az[c] = offset.z;
    colliders_.local_bx[c] = offset.x;
    colliders_.local_by[c] = offset.y;
    colliders_.local_bz[c] = offset.z;
    colliders_.radius[c] = std::max(radius, 0.001f);
    return true;
}

bool SpringBoneSimulator::add_capsule_collider(int32_t attached_bone, Vec3 head_offset, Vec3 tail_offset,
                                               float radius) noexcept {
    if (colliders_.count >= kMaxSpringColliders) {
        return false;
    }
    const uint32_t c = colliders_.count++;
    colliders_.shape[c] = static_cast<uint8_t>(ColliderShape::Capsule);
    colliders_.attached_bone[c] = attached_bone;
    colliders_.local_ax[c] = head_offset.x;
    colliders_.local_ay[c] = head_offset.y;
    colliders_.local_az[c] = head_offset.z;
    colliders_.local_bx[c] = tail_offset.x;
    colliders_.local_by[c] = tail_offset.y;
    colliders_.local_bz[c] = tail_offset.z;
    colliders_.radius[c] = std::max(radius, 0.001f);
    return true;
}

void SpringBoneSimulator::configure_default_skirt_and_hair(const RestSkeletonMetrics& /*rest*/) noexcept {
    clear();
    // Add torso, head & shoulder colliders so procedural hair & clothing respond smoothly
    (void)add_sphere_collider(static_cast<int32_t>(AETHER_BONE_HEAD), {0.0f, 0.08f, 0.01f}, 0.11f);
    (void)add_capsule_collider(static_cast<int32_t>(AETHER_BONE_CHEST), {0.0f, -0.05f, 0.0f}, {0.0f, 0.14f, 0.0f},
                               0.12f);
    (void)add_sphere_collider(static_cast<int32_t>(AETHER_BONE_LEFT_SHOULDER), {0.06f, 0.0f, 0.0f}, 0.07f);
    (void)add_sphere_collider(static_cast<int32_t>(AETHER_BONE_RIGHT_SHOULDER), {-0.06f, 0.0f, 0.0f}, 0.07f);

    // 8 hair strands x 4 joints (indices 64..95) so secondary motion populates virtual hair bones when avatar loads
    for (int strand = 0; strand < 8; ++strand) {
        const float angle = static_cast<float>(strand) * (kTwoPi / 8.0f);
        const Vec3 dir_out{std::cos(angle) * 0.25f, -0.95f, std::sin(angle) * 0.25f};
        for (int link = 0; link < 4; ++link) {
            const uint32_t j = static_cast<uint32_t>(strand * 4 + link);
            SpringJointDesc d{};
            d.bone_index = static_cast<int32_t>(64U + j);
            d.parent_joint_index = (link == 0) ? -1 : static_cast<int32_t>(j - 1U);
            d.parent_bone_index =
                (link == 0) ? static_cast<int32_t>(AETHER_BONE_HEAD) : static_cast<int32_t>(64U + j - 1U);
            d.center_bone_index = static_cast<int32_t>(AETHER_BONE_HIPS);
            d.local_bone_offset = (link == 0) ? Vec3{0.08f * std::cos(angle), 0.06f, 0.08f * std::sin(angle)}
                                              : (normalize_safe(dir_out) * 0.06f);
            d.bone_length = 0.065f;
            d.hit_radius = 0.018f;
            d.stiffness = 0.42f;
            d.drag_force = 0.45f;
            d.gravity_power = 0.12f;
            (void)add_joint(d);
        }
    }
}

void SpringBoneSimulator::reset_state(const std::array<Vec3, kMaxBones>& bone_pos_ws,
                                      const std::array<Quat, kMaxBones>& bone_rot_ws,
                                      uint32_t /*bone_count*/) noexcept {
    for (uint32_t j = 0; j < count_; ++j) {
        const Vec3 head = compute_joint_head_center_space(j, parent_joint_idx_, parent_bone_idx_, center_bone_idx_,
                                                          local_offset_x_, local_offset_y_, local_offset_z_, curr_x_,
                                                          curr_y_, curr_z_, bone_pos_ws, bone_rot_ws);
        head_x_[j] = head.x;
        head_y_[j] = head.y;
        head_z_[j] = head.z;
        const int32_t pb = parent_bone_idx_[j];
        const Quat pr = (pb >= 0 && static_cast<uint32_t>(pb) < kMaxBones) ? bone_rot_ws[static_cast<size_t>(pb)]
                                                                           : Quat::identity();
        const Vec3 tail = head + rotate_vec(pr, {rest_dir_x_[j], rest_dir_y_[j], rest_dir_z_[j]}) * bone_len_[j];
        curr_x_[j] = tail.x;
        curr_y_[j] = tail.y;
        curr_z_[j] = tail.z;
        prev_x_[j] = tail.x;
        prev_y_[j] = tail.y;
        prev_z_[j] = tail.z;
    }
    prev_root_pos_ = bone_pos_ws[AETHER_BONE_HIPS];
    state_initialized_ = true;
}

void SpringBoneSimulator::evaluate_colliders(const std::array<Vec3, kMaxBones>& bone_pos_ws,
                                             const std::array<Quat, kMaxBones>& bone_rot_ws) noexcept {
    // Evaluate collider spheres/capsules in `center` (root hips) space when center_bone_index >= 0 on rig
    const Vec3 center_pos = bone_pos_ws[AETHER_BONE_HIPS];
    for (uint32_t c = 0; c < colliders_.count; ++c) {
        const int32_t b = colliders_.attached_bone[c];
        Vec3 bp{};
        Quat br = Quat::identity();
        if (b >= 0 && static_cast<uint32_t>(b) < kMaxBones) {
            bp = bone_pos_ws[static_cast<size_t>(b)];
            br = bone_rot_ws[static_cast<size_t>(b)];
        }
        const Vec3 pa =
            (bp + rotate_vec(br, {colliders_.local_ax[c], colliders_.local_ay[c], colliders_.local_az[c]})) -
            center_pos;
        const Vec3 pb =
            (bp + rotate_vec(br, {colliders_.local_bx[c], colliders_.local_by[c], colliders_.local_bz[c]})) -
            center_pos;
        colliders_.center_ax[c] = pa.x;
        colliders_.center_ay[c] = pa.y;
        colliders_.center_az[c] = pa.z;
        colliders_.center_bx[c] = pb.x;
        colliders_.center_by[c] = pb.y;
        colliders_.center_bz[c] = pb.z;
    }
}

void SpringBoneSimulator::step_scalar(float dt_sq, Vec3 ext_accel, float ext_drag_scale,
                                      std::array<Vec3, kMaxBones>& io_bone_pos_ws,
                                      std::array<Quat, kMaxBones>& io_bone_rot_ws) noexcept {
    for (uint32_t j = 0; j < count_; ++j) {
        const Vec3 head = compute_joint_head_center_space(j, parent_joint_idx_, parent_bone_idx_, center_bone_idx_,
                                                          local_offset_x_, local_offset_y_, local_offset_z_, curr_x_,
                                                          curr_y_, curr_z_, io_bone_pos_ws, io_bone_rot_ws);
        head_x_[j] = head.x;
        head_y_[j] = head.y;
        head_z_[j] = head.z;

        const float drag = std::clamp(drag_[j] * ext_drag_scale, 0.0f, 0.99f);
        const float one_minus_drag = 1.0f - drag;
        const Vec3 cur{curr_x_[j], curr_y_[j], curr_z_[j]};
        const Vec3 prv{prev_x_[j], prev_y_[j], prev_z_[j]};

        const int32_t pb = parent_bone_idx_[j];
        const Quat pr = (pb >= 0 && static_cast<uint32_t>(pb) < kMaxBones) ? io_bone_rot_ws[static_cast<size_t>(pb)]
                                                                           : Quat::identity();
        const Vec3 world_rest_dir = rotate_vec(pr, {rest_dir_x_[j], rest_dir_y_[j], rest_dir_z_[j]});
        const Vec3 accel{
            world_rest_dir.x * stiffness_[j] + grav_x_[j] + ext_accel.x,
            world_rest_dir.y * stiffness_[j] + grav_y_[j] + ext_accel.y,
            world_rest_dir.z * stiffness_[j] + grav_z_[j] + ext_accel.z,
        };

        Vec3 next = cur + (cur - prv) * one_minus_drag + accel * dt_sq;
        const float len = bone_len_[j];
        Vec3 dir = normalize_safe(next - head, world_rest_dir);
        next = head + dir * len;

        const float hr = hit_radius_[j];
        for (uint32_t c = 0; c < colliders_.count; ++c) {
            const float sum_r = hr + colliders_.radius[c];
            const Vec3 ca{colliders_.center_ax[c], colliders_.center_ay[c], colliders_.center_az[c]};
            Vec3 closest = ca;
            if (colliders_.shape[c] == static_cast<uint8_t>(ColliderShape::Capsule)) {
                const Vec3 cb{colliders_.center_bx[c], colliders_.center_by[c], colliders_.center_bz[c]};
                const Vec3 ab = cb - ca;
                const float ab2 = dot(ab, ab);
                const float t = (ab2 > 1e-10f) ? std::clamp(dot(next - ca, ab) / ab2, 0.0f, 1.0f) : 0.0f;
                closest = ca + ab * t;
            }
            const Vec3 diff = next - closest;
            const float d2 = dot(diff, diff);
            if (d2 < sum_r * sum_r) {
                const float d = std::sqrt(std::max(d2, 1e-12f));
                next = closest + diff * (sum_r / d);
                next = head + normalize_safe(next - head, world_rest_dir) * len;
            }
        }

        prev_x_[j] = cur.x;
        prev_y_[j] = cur.y;
        prev_z_[j] = cur.z;
        curr_x_[j] = next.x;
        curr_y_[j] = next.y;
        curr_z_[j] = next.z;

        const int32_t bi = bone_idx_[j];
        if (bi >= 0 && static_cast<uint32_t>(bi) < kMaxBones) {
            const int32_t cb = center_bone_idx_[j];
            const Vec3 c_pos =
                (cb >= 0 && static_cast<uint32_t>(cb) < kMaxBones) ? io_bone_pos_ws[static_cast<size_t>(cb)] : Vec3{};
            io_bone_pos_ws[static_cast<size_t>(bi)] = head + c_pos;
            const Vec3 final_dir = normalize_safe(next - head, world_rest_dir);
            io_bone_rot_ws[static_cast<size_t>(bi)] = normalize(quat_from_to_sb(world_rest_dir, final_dir) * pr);
        }
    }
}

void SpringBoneSimulator::step_avx2(float dt_sq, Vec3 ext_accel, float ext_drag_scale,
                                    std::array<Vec3, kMaxBones>& io_bone_pos_ws,
                                    std::array<Quat, kMaxBones>& io_bone_rot_ws) noexcept {
    // 8-wide AVX2 collider evaluation per joint + scalar topological length-constraint match (`<= 1e-5 m` parity).
    const __m256 v_zero = _mm256_setzero_ps();
    const __m256 v_one = _mm256_set1_ps(1.0f);
    const __m256 v_eps = _mm256_set1_ps(1e-10f);

    for (uint32_t j = 0; j < count_; ++j) {
        const Vec3 head = compute_joint_head_center_space(j, parent_joint_idx_, parent_bone_idx_, center_bone_idx_,
                                                          local_offset_x_, local_offset_y_, local_offset_z_, curr_x_,
                                                          curr_y_, curr_z_, io_bone_pos_ws, io_bone_rot_ws);
        head_x_[j] = head.x;
        head_y_[j] = head.y;
        head_z_[j] = head.z;

        const float drag = std::clamp(drag_[j] * ext_drag_scale, 0.0f, 0.99f);
        const float one_minus_drag = 1.0f - drag;
        const Vec3 cur{curr_x_[j], curr_y_[j], curr_z_[j]};
        const Vec3 prv{prev_x_[j], prev_y_[j], prev_z_[j]};

        const int32_t pb = parent_bone_idx_[j];
        const Quat pr = (pb >= 0 && static_cast<uint32_t>(pb) < kMaxBones) ? io_bone_rot_ws[static_cast<size_t>(pb)]
                                                                           : Quat::identity();
        const Vec3 world_rest_dir = rotate_vec(pr, {rest_dir_x_[j], rest_dir_y_[j], rest_dir_z_[j]});
        const Vec3 accel{
            world_rest_dir.x * stiffness_[j] + grav_x_[j] + ext_accel.x,
            world_rest_dir.y * stiffness_[j] + grav_y_[j] + ext_accel.y,
            world_rest_dir.z * stiffness_[j] + grav_z_[j] + ext_accel.z,
        };

        Vec3 next = cur + (cur - prv) * one_minus_drag + accel * dt_sq;
        const float len = bone_len_[j];
        next = head + normalize_safe(next - head, world_rest_dir) * len;

        const float hr = hit_radius_[j];
        const __m256 v_hr = _mm256_set1_ps(hr);

        uint32_t c = 0;
        for (; c + 8U <= colliders_.count; c += 8U) {
            // Test 8 colliders against `next` concurrently in AVX2 registers
            const __m256 nx = _mm256_set1_ps(next.x);
            const __m256 ny = _mm256_set1_ps(next.y);
            const __m256 nz = _mm256_set1_ps(next.z);

            const __m256 ax = _mm256_load_ps(&colliders_.center_ax[c]);
            const __m256 ay = _mm256_load_ps(&colliders_.center_ay[c]);
            const __m256 az = _mm256_load_ps(&colliders_.center_az[c]);
            const __m256 bx = _mm256_load_ps(&colliders_.center_bx[c]);
            const __m256 by = _mm256_load_ps(&colliders_.center_by[c]);
            const __m256 bz = _mm256_load_ps(&colliders_.center_bz[c]);

            const __m256 abx = _mm256_sub_ps(bx, ax);
            const __m256 aby = _mm256_sub_ps(by, ay);
            const __m256 abz = _mm256_sub_ps(bz, az);

            const __m256 apx = _mm256_sub_ps(nx, ax);
            const __m256 apy = _mm256_sub_ps(ny, ay);
            const __m256 apz = _mm256_sub_ps(nz, az);

            const __m256 ab2 = _mm256_fmadd_ps(abx, abx, _mm256_fmadd_ps(aby, aby, _mm256_mul_ps(abz, abz)));
            const __m256 ap_dot_ab = _mm256_fmadd_ps(apx, abx, _mm256_fmadd_ps(apy, aby, _mm256_mul_ps(apz, abz)));

            const __m256 safe_ab2 = _mm256_max_ps(ab2, v_eps);
            __m256 t = _mm256_div_ps(ap_dot_ab, safe_ab2);
            t = _mm256_min_ps(v_one, _mm256_max_ps(v_zero, t));
            // For sphere colliders (ab == 0), mask t -> 0 cleanly
            const __m256 has_len = _mm256_cmp_ps(ab2, v_eps, _CMP_GT_OQ);
            t = _mm256_and_ps(t, has_len);

            const __m256 cx = _mm256_fmadd_ps(abx, t, ax);
            const __m256 cy = _mm256_fmadd_ps(aby, t, ay);
            const __m256 cz = _mm256_fmadd_ps(abz, t, az);

            const __m256 dx = _mm256_sub_ps(nx, cx);
            const __m256 dy = _mm256_sub_ps(ny, cy);
            const __m256 dz = _mm256_sub_ps(nz, cz);
            const __m256 d2 = _mm256_fmadd_ps(dx, dx, _mm256_fmadd_ps(dy, dy, _mm256_mul_ps(dz, dz)));

            const __m256 sum_r = _mm256_add_ps(v_hr, _mm256_load_ps(&colliders_.radius[c]));
            const __m256 sum_r2 = _mm256_mul_ps(sum_r, sum_r);
            const __m256 hit_mask = _mm256_cmp_ps(d2, sum_r2, _CMP_LT_OQ);
            const int mask = _mm256_movemask_ps(hit_mask);

            if (mask != 0) {
                // Push against hitting lanes in ascending order (exact sequential parity)
                for (int k = 0; k < 8; ++k) {
                    if (mask & (1 << k)) {
                        const uint32_t ck = c + static_cast<uint32_t>(k);
                        const float sum_rk = hr + colliders_.radius[ck];
                        const Vec3 ca{colliders_.center_ax[ck], colliders_.center_ay[ck], colliders_.center_az[ck]};
                        Vec3 closest = ca;
                        if (colliders_.shape[ck] == static_cast<uint8_t>(ColliderShape::Capsule)) {
                            const Vec3 cb{colliders_.center_bx[ck], colliders_.center_by[ck], colliders_.center_bz[ck]};
                            const Vec3 ab = cb - ca;
                            const float ab2s = dot(ab, ab);
                            const float ts = (ab2s > 1e-10f) ? std::clamp(dot(next - ca, ab) / ab2s, 0.0f, 1.0f) : 0.0f;
                            closest = ca + ab * ts;
                        }
                        const Vec3 diff = next - closest;
                        const float d2s = dot(diff, diff);
                        if (d2s < sum_rk * sum_rk) {
                            const float d = std::sqrt(std::max(d2s, 1e-12f));
                            next = closest + diff * (sum_rk / d);
                            next = head + normalize_safe(next - head, world_rest_dir) * len;
                        }
                    }
                }
            }
        }
        // Tail scalar colliders (< 8 remainder)
        for (; c < colliders_.count; ++c) {
            const float sum_r = hr + colliders_.radius[c];
            const Vec3 ca{colliders_.center_ax[c], colliders_.center_ay[c], colliders_.center_az[c]};
            Vec3 closest = ca;
            if (colliders_.shape[c] == static_cast<uint8_t>(ColliderShape::Capsule)) {
                const Vec3 cb{colliders_.center_bx[c], colliders_.center_by[c], colliders_.center_bz[c]};
                const Vec3 ab = cb - ca;
                const float ab2s = dot(ab, ab);
                const float ts = (ab2s > 1e-10f) ? std::clamp(dot(next - ca, ab) / ab2s, 0.0f, 1.0f) : 0.0f;
                closest = ca + ab * ts;
            }
            const Vec3 diff = next - closest;
            const float d2s = dot(diff, diff);
            if (d2s < sum_r * sum_r) {
                const float d = std::sqrt(std::max(d2s, 1e-12f));
                next = closest + diff * (sum_r / d);
                next = head + normalize_safe(next - head, world_rest_dir) * len;
            }
        }

        prev_x_[j] = cur.x;
        prev_y_[j] = cur.y;
        prev_z_[j] = cur.z;
        curr_x_[j] = next.x;
        curr_y_[j] = next.y;
        curr_z_[j] = next.z;

        const int32_t bi = bone_idx_[j];
        if (bi >= 0 && static_cast<uint32_t>(bi) < kMaxBones) {
            const int32_t cb = center_bone_idx_[j];
            const Vec3 c_pos =
                (cb >= 0 && static_cast<uint32_t>(cb) < kMaxBones) ? io_bone_pos_ws[static_cast<size_t>(cb)] : Vec3{};
            io_bone_pos_ws[static_cast<size_t>(bi)] = head + c_pos;
            const Vec3 final_dir = normalize_safe(next - head, world_rest_dir);
            io_bone_rot_ws[static_cast<size_t>(bi)] = normalize(quat_from_to_sb(world_rest_dir, final_dir) * pr);
        }
    }
}

void SpringBoneSimulator::step_avx512(float dt_sq, Vec3 ext_accel, float ext_drag_scale,
                                      std::array<Vec3, kMaxBones>& io_bone_pos_ws,
                                      std::array<Quat, kMaxBones>& io_bone_rot_ws) noexcept {
    // Dispatches to wide SIMD lane path safely cleanly without requiring AVX-512 compiler codegen flags
    step_avx2(dt_sq, ext_accel, ext_drag_scale, io_bone_pos_ws, io_bone_rot_ws);
}

void SpringBoneSimulator::step(float fixed_dt_s, Vec3 external_accel_ws, float external_drag_scale,
                               std::array<Vec3, kMaxBones>& io_bone_pos_ws, std::array<Quat, kMaxBones>& io_bone_rot_ws,
                               uint32_t bone_count, SimdPath force_path) noexcept {
    if (count_ == 0U) {
        return;
    }
    const Vec3 root_now = io_bone_pos_ws[AETHER_BONE_HIPS];
    if (!state_initialized_) {
        reset_state(io_bone_pos_ws, io_bone_rot_ws, bone_count);
        return;
    }

    // §9.6 Teleportation threshold (> 0.5 m step -> reset Verlet velocities cleanly)
    if (length(root_now - prev_root_pos_) > 0.5f) {
        ++teleport_resets_;
        reset_state(io_bone_pos_ws, io_bone_rot_ws, bone_count);
        return;
    }
    prev_root_pos_ = root_now;

    evaluate_colliders(io_bone_pos_ws, io_bone_rot_ws);

    const float dt = std::clamp(fixed_dt_s, 1.0f / 240.0f, 1.0f / 30.0f);
    const float dt_sq = dt * dt;

    SimdPath effective = force_path;
    if (effective == SimdPath::Avx512 && detected_path_ != SimdPath::Avx512) {
        effective = detected_path_;
    }
    if (effective == SimdPath::Avx2 && detected_path_ == SimdPath::Scalar) {
        effective = SimdPath::Scalar;
    }

    switch (effective) {
    case SimdPath::Scalar:
        step_scalar(dt_sq, external_accel_ws, external_drag_scale, io_bone_pos_ws, io_bone_rot_ws);
        break;
    case SimdPath::Avx2:
        step_avx2(dt_sq, external_accel_ws, external_drag_scale, io_bone_pos_ws, io_bone_rot_ws);
        break;
    case SimdPath::Avx512:
        step_avx512(dt_sq, external_accel_ws, external_drag_scale, io_bone_pos_ws, io_bone_rot_ws);
        break;
    }
}

} // namespace aether::animation
