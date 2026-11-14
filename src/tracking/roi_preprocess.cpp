#include "tracking/roi_preprocess.hpp"
#include "tracking/canonical_face_model.hpp"

#include <immintrin.h>

#include <algorithm>
#include <cmath>

namespace aether::tracking {

namespace {

struct AffineMap2D {
    float orig_x{0.0f};
    float orig_y{0.0f};
    float ux{1.0f};
    float uy{0.0f};
    float vx{0.0f};
    float vy{1.0f};
};

[[nodiscard]] AffineMap2D make_affine_map(const RoiRectNorm& roi, uint32_t src_w, uint32_t src_h, uint32_t dst_w,
                                          uint32_t dst_h) noexcept {
    const float sw = static_cast<float>(src_w);
    const float sh = static_cast<float>(src_h);
    const float rw = (roi.valid ? std::max(roi.width, 0.05f) : 1.0f) * sw;
    const float rh = (roi.valid ? std::max(roi.height, 0.05f) : 1.0f) * sh;
    const float cx = (roi.valid ? roi.cx : 0.5f) * sw;
    const float cy = (roi.valid ? roi.cy : 0.5f) * sh;
    const float ang = roi.valid ? roi.rotation_rad : 0.0f;
    const float c = std::cos(ang);
    const float s = std::sin(ang);

    const float inv_dw = 1.0f / static_cast<float>(std::max(1U, dst_w));
    const float inv_dh = 1.0f / static_cast<float>(std::max(1U, dst_h));

    AffineMap2D m{};
    m.ux = (c * rw) * inv_dw;
    m.uy = (s * rw) * inv_dw;
    m.vx = (-s * rh) * inv_dh;
    m.vy = (c * rh) * inv_dh;
    // Center pixel (dst_w*0.5 - 0.5, dst_h*0.5 - 0.5) maps to (cx, cy)
    const float half_x = 0.5f * static_cast<float>(dst_w);
    const float half_y = 0.5f * static_cast<float>(dst_h);
    m.orig_x = cx - m.ux * half_x - m.vx * half_y;
    m.orig_y = cy - m.uy * half_x - m.vy * half_y;
    return m;
}

void warp_nv12_scalar(const CapturedFrameBuffer& src, const AffineMap2D& map, uint32_t dst_w, uint32_t dst_h,
                      float scale_mul, float offset_add, float* dst_nchw) noexcept {
    const uint8_t* y_plane = src.bytes.data();
    const uint8_t* uv_plane = src.bytes.data() + static_cast<size_t>(src.stride_y) * src.height;
    const size_t plane_stride = static_cast<size_t>(dst_w) * dst_h;
    float* dst_r = dst_nchw;
    float* dst_g = dst_nchw + plane_stride;
    float* dst_b = dst_nchw + plane_stride * 2U;

    const int max_x = static_cast<int>(src.width) - 1;
    const int max_y = static_cast<int>(src.height) - 1;
    const int max_uv_x = std::max(0, static_cast<int>(src.width / 2U) - 1);
    const int max_uv_y = std::max(0, static_cast<int>(src.height / 2U) - 1);
    const int sy_stride = static_cast<int>(src.stride_y);
    const int suv_stride = static_cast<int>(src.stride_uv);

    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        const float fy_base = map.orig_y + map.vy * (static_cast<float>(dy) + 0.5f);
        const float fx_base = map.orig_x + map.vx * (static_cast<float>(dy) + 0.5f);
        const size_t row_off = static_cast<size_t>(dy) * dst_w;
        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            const float fdx = static_cast<float>(dx) + 0.5f;
            const float sx = fx_base + map.ux * fdx;
            const float sy = fy_base + map.uy * fdx;
            if (sx < 0.0f || sy < 0.0f || sx > static_cast<float>(max_x) || sy > static_cast<float>(max_y)) {
                dst_r[row_off + dx] = offset_add;
                dst_g[row_off + dx] = offset_add;
                dst_b[row_off + dx] = offset_add;
                continue;
            }
            const int ix0 = std::clamp(static_cast<int>(sx), 0, max_x);
            const int iy0 = std::clamp(static_cast<int>(sy), 0, max_y);
            const int ix1 = std::min(ix0 + 1, max_x);
            const int iy1 = std::min(iy0 + 1, max_y);
            const float wx = sx - static_cast<float>(ix0);
            const float wy = sy - static_cast<float>(iy0);

            const float y00 = static_cast<float>(y_plane[iy0 * sy_stride + ix0]);
            const float y10 = static_cast<float>(y_plane[iy0 * sy_stride + ix1]);
            const float y01 = static_cast<float>(y_plane[iy1 * sy_stride + ix0]);
            const float y11 = static_cast<float>(y_plane[iy1 * sy_stride + ix1]);
            const float luma = ((y00 * (1.0f - wx) + y10 * wx) * (1.0f - wy) + (y01 * (1.0f - wx) + y11 * wx) * wy);

            const int uvx = std::clamp(ix0 >> 1, 0, max_uv_x);
            const int uvy = std::clamp(iy0 >> 1, 0, max_uv_y);
            const size_t uv_off =
                static_cast<size_t>(uvy) * static_cast<size_t>(suv_stride) + static_cast<size_t>(uvx) * 2U;
            const float u = static_cast<float>(uv_plane[uv_off + 0U]) - 128.0f;
            const float v = static_cast<float>(uv_plane[uv_off + 1U]) - 128.0f;

            // BT.601 studio-swing YUV -> normalized RGB [0, 1] -> target norm range
            const float c = std::max(luma - 16.0f, 0.0f) * 1.1643835f;
            const float r01 = std::clamp((c + 1.5960268f * v) * (1.0f / 255.0f), 0.0f, 1.0f);
            const float g01 = std::clamp((c - 0.3917623f * u - 0.8129676f * v) * (1.0f / 255.0f), 0.0f, 1.0f);
            const float b01 = std::clamp((c + 2.0172321f * u) * (1.0f / 255.0f), 0.0f, 1.0f);

            dst_r[row_off + dx] = r01 * scale_mul + offset_add;
            dst_g[row_off + dx] = g01 * scale_mul + offset_add;
            dst_b[row_off + dx] = b01 * scale_mul + offset_add;
        }
    }
}

void warp_nv12_avx2(const CapturedFrameBuffer& src, const AffineMap2D& map, uint32_t dst_w, uint32_t dst_h,
                    float scale_mul, float offset_add, float* dst_nchw) noexcept {
    const uint8_t* y_plane = src.bytes.data();
    const uint8_t* uv_plane = src.bytes.data() + static_cast<size_t>(src.stride_y) * src.height;
    const size_t plane_stride = static_cast<size_t>(dst_w) * dst_h;
    float* dst_r = dst_nchw;
    float* dst_g = dst_nchw + plane_stride;
    float* dst_b = dst_nchw + plane_stride * 2U;

    const int max_x = static_cast<int>(src.width) - 1;
    const int max_y = static_cast<int>(src.height) - 1;
    const int max_uv_x = std::max(0, static_cast<int>(src.width / 2U) - 1);
    const int max_uv_y = std::max(0, static_cast<int>(src.height / 2U) - 1);
    const int sy_stride = static_cast<int>(src.stride_y);
    const int suv_stride = static_cast<int>(src.stride_uv);

    const __m256 v_step = _mm256_setr_ps(0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f);
    const __m256 v_zero = _mm256_setzero_ps();
    const __m256 v_one = _mm256_set1_ps(1.0f);
    const __m256 v_max_x = _mm256_set1_ps(static_cast<float>(max_x));
    const __m256 v_max_y = _mm256_set1_ps(static_cast<float>(max_y));
    const __m256i vi_max_x = _mm256_set1_epi32(max_x);
    const __m256i vi_max_y = _mm256_set1_epi32(max_y);
    const __m256i vi_max_uv_x = _mm256_set1_epi32(max_uv_x);
    const __m256i vi_max_uv_y = _mm256_set1_epi32(max_uv_y);
    const __m256i vi_zero = _mm256_setzero_si256();
    const __m256i vi_one = _mm256_set1_epi32(1);
    const __m256i vi_sy_stride = _mm256_set1_epi32(sy_stride);
    const __m256i vi_suv_stride = _mm256_set1_epi32(suv_stride);
    const __m256i vi_byte_mask = _mm256_set1_epi32(0xFF);

    const __m256 v_16 = _mm256_set1_ps(16.0f);
    const __m256 v_128 = _mm256_set1_ps(128.0f);
    const __m256 v_y_scale = _mm256_set1_ps(1.1643835f);
    const __m256 v_cr_r = _mm256_set1_ps(1.5960268f);
    const __m256 v_cb_g = _mm256_set1_ps(-0.3917623f);
    const __m256 v_cr_g = _mm256_set1_ps(-0.8129676f);
    const __m256 v_cb_b = _mm256_set1_ps(2.0172321f);
    const __m256 v_inv255 = _mm256_set1_ps(1.0f / 255.0f);
    const __m256 v_scale_mul = _mm256_set1_ps(scale_mul);
    const __m256 v_off_add = _mm256_set1_ps(offset_add);

    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        const float fy_base = map.orig_y + map.vy * (static_cast<float>(dy) + 0.5f);
        const float fx_base = map.orig_x + map.vx * (static_cast<float>(dy) + 0.5f);
        const size_t row_off = static_cast<size_t>(dy) * dst_w;

        uint32_t dx = 0;
        for (; dx + 8U <= dst_w; dx += 8U) {
            const __m256 fdx = _mm256_add_ps(_mm256_set1_ps(static_cast<float>(dx)), v_step);
            const __m256 sx = _mm256_fmadd_ps(_mm256_set1_ps(map.ux), fdx, _mm256_set1_ps(fx_base));
            const __m256 sy = _mm256_fmadd_ps(_mm256_set1_ps(map.uy), fdx, _mm256_set1_ps(fy_base));

            const __m256 in_bounds = _mm256_and_ps(
                _mm256_and_ps(_mm256_cmp_ps(sx, v_zero, _CMP_GE_OQ), _mm256_cmp_ps(sx, v_max_x, _CMP_LE_OQ)),
                _mm256_and_ps(_mm256_cmp_ps(sy, v_zero, _CMP_GE_OQ), _mm256_cmp_ps(sy, v_max_y, _CMP_LE_OQ)));

            const __m256 sx_floor = _mm256_floor_ps(_mm256_max_ps(v_zero, _mm256_min_ps(v_max_x, sx)));
            const __m256 sy_floor = _mm256_floor_ps(_mm256_max_ps(v_zero, _mm256_min_ps(v_max_y, sy)));
            const __m256 wx = _mm256_sub_ps(sx, sx_floor);
            const __m256 wy = _mm256_sub_ps(sy, sy_floor);
            const __m256 one_m_wx = _mm256_sub_ps(v_one, wx);
            const __m256 one_m_wy = _mm256_sub_ps(v_one, wy);

            const __m256i ix0 = _mm256_min_epi32(vi_max_x, _mm256_max_epi32(vi_zero, _mm256_cvttps_epi32(sx_floor)));
            const __m256i iy0 = _mm256_min_epi32(vi_max_y, _mm256_max_epi32(vi_zero, _mm256_cvttps_epi32(sy_floor)));
            const __m256i ix1 = _mm256_min_epi32(vi_max_x, _mm256_add_epi32(ix0, vi_one));
            const __m256i iy1 = _mm256_min_epi32(vi_max_y, _mm256_add_epi32(iy0, vi_one));

            const __m256i row0 = _mm256_mullo_epi32(iy0, vi_sy_stride);
            const __m256i row1 = _mm256_mullo_epi32(iy1, vi_sy_stride);
            const __m256i off00 = _mm256_add_epi32(row0, ix0);
            const __m256i off10 = _mm256_add_epi32(row0, ix1);
            const __m256i off01 = _mm256_add_epi32(row1, ix0);
            const __m256i off11 = _mm256_add_epi32(row1, ix1);

            const auto gather_y = [&](__m256i offsets) noexcept -> __m256 {
                const __m256i raw = _mm256_i32gather_epi32(reinterpret_cast<const int*>(y_plane), offsets, 1);
                return _mm256_cvtepi32_ps(_mm256_and_si256(raw, vi_byte_mask));
            };
            const __m256 y00 = gather_y(off00);
            const __m256 y10 = gather_y(off10);
            const __m256 y01 = gather_y(off01);
            const __m256 y11 = gather_y(off11);

            const __m256 top = _mm256_fmadd_ps(y00, one_m_wx, _mm256_mul_ps(y10, wx));
            const __m256 bot = _mm256_fmadd_ps(y01, one_m_wx, _mm256_mul_ps(y11, wx));
            const __m256 luma = _mm256_fmadd_ps(top, one_m_wy, _mm256_mul_ps(bot, wy));

            const __m256i uvx = _mm256_min_epi32(vi_max_uv_x, _mm256_srli_epi32(ix0, 1));
            const __m256i uvy = _mm256_min_epi32(vi_max_uv_y, _mm256_srli_epi32(iy0, 1));
            const __m256i uv_off = _mm256_add_epi32(_mm256_mullo_epi32(uvy, vi_suv_stride), _mm256_slli_epi32(uvx, 1));
            const __m256i uv_raw = _mm256_i32gather_epi32(reinterpret_cast<const int*>(uv_plane), uv_off, 1);
            const __m256 u = _mm256_sub_ps(_mm256_cvtepi32_ps(_mm256_and_si256(uv_raw, vi_byte_mask)), v_128);
            const __m256 v =
                _mm256_sub_ps(_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(uv_raw, 8), vi_byte_mask)), v_128);

            const __m256 c = _mm256_mul_ps(_mm256_max_ps(v_zero, _mm256_sub_ps(luma, v_16)), v_y_scale);
            __m256 r01 = _mm256_mul_ps(_mm256_fmadd_ps(v_cr_r, v, c), v_inv255);
            __m256 g01 = _mm256_mul_ps(_mm256_fmadd_ps(v_cr_g, v, _mm256_fmadd_ps(v_cb_g, u, c)), v_inv255);
            __m256 b01 = _mm256_mul_ps(_mm256_fmadd_ps(v_cb_b, u, c), v_inv255);

            r01 = _mm256_min_ps(v_one, _mm256_max_ps(v_zero, r01));
            g01 = _mm256_min_ps(v_one, _mm256_max_ps(v_zero, g01));
            b01 = _mm256_min_ps(v_one, _mm256_max_ps(v_zero, b01));

            const __m256 rf = _mm256_blendv_ps(v_off_add, _mm256_fmadd_ps(r01, v_scale_mul, v_off_add), in_bounds);
            const __m256 gf = _mm256_blendv_ps(v_off_add, _mm256_fmadd_ps(g01, v_scale_mul, v_off_add), in_bounds);
            const __m256 bf = _mm256_blendv_ps(v_off_add, _mm256_fmadd_ps(b01, v_scale_mul, v_off_add), in_bounds);

            _mm256_storeu_ps(&dst_r[row_off + dx], rf);
            _mm256_storeu_ps(&dst_g[row_off + dx], gf);
            _mm256_storeu_ps(&dst_b[row_off + dx], bf);
        }

        // Handle non-multiple-of-8 tail cleanly
        for (; dx < dst_w; ++dx) {
            const float fdx = static_cast<float>(dx) + 0.5f;
            const float sx = fx_base + map.ux * fdx;
            const float sy = fy_base + map.uy * fdx;
            if (sx < 0.0f || sy < 0.0f || sx > static_cast<float>(max_x) || sy > static_cast<float>(max_y)) {
                dst_r[row_off + dx] = offset_add;
                dst_g[row_off + dx] = offset_add;
                dst_b[row_off + dx] = offset_add;
                continue;
            }
            const int ix0 = std::clamp(static_cast<int>(sx), 0, max_x);
            const int iy0 = std::clamp(static_cast<int>(sy), 0, max_y);
            const int ix1 = std::min(ix0 + 1, max_x);
            const int iy1 = std::min(iy0 + 1, max_y);
            const float wx = sx - static_cast<float>(ix0);
            const float wy = sy - static_cast<float>(iy0);
            const float y00 = static_cast<float>(y_plane[iy0 * sy_stride + ix0]);
            const float y10 = static_cast<float>(y_plane[iy0 * sy_stride + ix1]);
            const float y01 = static_cast<float>(y_plane[iy1 * sy_stride + ix0]);
            const float y11 = static_cast<float>(y_plane[iy1 * sy_stride + ix1]);
            const float luma = ((y00 * (1.0f - wx) + y10 * wx) * (1.0f - wy) + (y01 * (1.0f - wx) + y11 * wx) * wy);
            const int uvx = std::clamp(ix0 >> 1, 0, max_uv_x);
            const int uvy = std::clamp(iy0 >> 1, 0, max_uv_y);
            const size_t uv_off =
                static_cast<size_t>(uvy) * static_cast<size_t>(suv_stride) + static_cast<size_t>(uvx) * 2U;
            const float u = static_cast<float>(uv_plane[uv_off + 0U]) - 128.0f;
            const float v = static_cast<float>(uv_plane[uv_off + 1U]) - 128.0f;
            const float c = std::max(luma - 16.0f, 0.0f) * 1.1643835f;
            const float r01 = std::clamp((c + 1.5960268f * v) * (1.0f / 255.0f), 0.0f, 1.0f);
            const float g01 = std::clamp((c - 0.3917623f * u - 0.8129676f * v) * (1.0f / 255.0f), 0.0f, 1.0f);
            const float b01 = std::clamp((c + 2.0172321f * u) * (1.0f / 255.0f), 0.0f, 1.0f);
            dst_r[row_off + dx] = r01 * scale_mul + offset_add;
            dst_g[row_off + dx] = g01 * scale_mul + offset_add;
            dst_b[row_off + dx] = b01 * scale_mul + offset_add;
        }
    }
}

void warp_bgra_scalar(const CapturedFrameBuffer& src, const AffineMap2D& map, uint32_t dst_w, uint32_t dst_h,
                      float scale_mul, float offset_add, float* dst_nchw) noexcept {
    const uint8_t* px = src.bytes.data();
    const size_t plane_stride = static_cast<size_t>(dst_w) * dst_h;
    float* dst_r = dst_nchw;
    float* dst_g = dst_nchw + plane_stride;
    float* dst_b = dst_nchw + plane_stride * 2U;
    const int max_x = static_cast<int>(src.width) - 1;
    const int max_y = static_cast<int>(src.height) - 1;

    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        const float fy_base = map.orig_y + map.vy * (static_cast<float>(dy) + 0.5f);
        const float fx_base = map.orig_x + map.vx * (static_cast<float>(dy) + 0.5f);
        const size_t row_off = static_cast<size_t>(dy) * dst_w;
        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            const float fdx = static_cast<float>(dx) + 0.5f;
            const float sx = fx_base + map.ux * fdx;
            const float sy = fy_base + map.uy * fdx;
            if (sx < 0.0f || sy < 0.0f || sx > static_cast<float>(max_x) || sy > static_cast<float>(max_y)) {
                dst_r[row_off + dx] = offset_add;
                dst_g[row_off + dx] = offset_add;
                dst_b[row_off + dx] = offset_add;
                continue;
            }
            const int ix = std::clamp(static_cast<int>(std::lround(sx)), 0, max_x);
            const int iy = std::clamp(static_cast<int>(std::lround(sy)), 0, max_y);
            const size_t off = static_cast<size_t>(iy) * src.stride_y + static_cast<size_t>(ix) * 4U;
            const float b01 = static_cast<float>(px[off + 0U]) * (1.0f / 255.0f);
            const float g01 = static_cast<float>(px[off + 1U]) * (1.0f / 255.0f);
            const float r01 = static_cast<float>(px[off + 2U]) * (1.0f / 255.0f);
            dst_r[row_off + dx] = r01 * scale_mul + offset_add;
            dst_g[row_off + dx] = g01 * scale_mul + offset_add;
            dst_b[row_off + dx] = b01 * scale_mul + offset_add;
        }
    }
}

} // namespace

void warp_frame_to_nchw(const CapturedFrameBuffer& src, const RoiRectNorm& roi, uint32_t dst_w, uint32_t dst_h,
                        RoiNormRange range, float* dst_nchw, bool force_scalar) noexcept {
    if (dst_nchw == nullptr || src.width == 0U || src.height == 0U || dst_w == 0U || dst_h == 0U) {
        return;
    }
    const AffineMap2D map = make_affine_map(roi, src.width, src.height, dst_w, dst_h);
    const float scale_mul = (range == RoiNormRange::NegOneToPosOne) ? 2.0f : 1.0f;
    const float offset_add = (range == RoiNormRange::NegOneToPosOne) ? -1.0f : 0.0f;

    if (src.format == CameraPixelFormat::BGRA32) {
        warp_bgra_scalar(src, map, dst_w, dst_h, scale_mul, offset_add, dst_nchw);
        return;
    }

    if (force_scalar) {
        warp_nv12_scalar(src, map, dst_w, dst_h, scale_mul, offset_add, dst_nchw);
    } else {
        warp_nv12_avx2(src, map, dst_w, dst_h, scale_mul, offset_add, dst_nchw);
    }
}

void extract_m3_subset_146x2(const float* m2_landmarks_478x3, float m2_norm_scale, float* dst_146x2) noexcept {
    if (m2_landmarks_478x3 == nullptr || dst_146x2 == nullptr) {
        return;
    }
    // M3 MLP expects `[0..256]` pixel coordinates inside the 256x256 face ROI
    const float scale = (m2_norm_scale > 1e-4f) ? (256.0f / m2_norm_scale) : 1.0f;
    for (uint32_t i = 0; i < kM3SubsetCount; ++i) {
        const uint32_t lm_idx = kM3LandmarkSubsetIndices[i];
        dst_146x2[i * 2U + 0U] = m2_landmarks_478x3[lm_idx * 3U + 0U] * scale;
        dst_146x2[i * 2U + 1U] = m2_landmarks_478x3[lm_idx * 3U + 1U] * scale;
    }
}

} // namespace aether::tracking
