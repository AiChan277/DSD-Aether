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

void warp_yuy2_scalar(const CapturedFrameBuffer& src, const AffineMap2D& map, uint32_t dst_w, uint32_t dst_h,
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
            const int pair_x = (ix & ~1);
            const size_t pair_off = static_cast<size_t>(iy) * src.stride_y + static_cast<size_t>(pair_x) * 2U;
            const float luma = static_cast<float>(px[pair_off + ((ix & 1) != 0 ? 2U : 0U)]);
            const float u = static_cast<float>(px[pair_off + 1U]) - 128.0f;
            const float v = static_cast<float>(px[pair_off + 3U]) - 128.0f;
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
    if (src.format == CameraPixelFormat::YUY2) {
        warp_yuy2_scalar(src, map, dst_w, dst_h, scale_mul, offset_add, dst_nchw);
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

RoiRectNorm make_letterbox_square_roi(uint32_t src_w, uint32_t src_h) noexcept {
    const float w = static_cast<float>(src_w > 0U ? src_w : 1U);
    const float h = static_cast<float>(src_h > 0U ? src_h : 1U);
    const float aspect = w / h;
    if (aspect >= 1.0f) {
        return {0.5f, 0.5f, 1.0f, aspect, 0.0f, true};
    }
    return {0.5f, 0.5f, 1.0f / aspect, 1.0f, 0.0f, true};
}

RoiRectNorm enforce_square_pixel_roi(RoiRectNorm roi, uint32_t src_w, uint32_t src_h, float min_size_frac,
                                     float max_size_frac) noexcept {
    if (!roi.valid || src_w == 0U || src_h == 0U) {
        return roi;
    }
    const float w_f = static_cast<float>(src_w);
    const float h_f = static_cast<float>(src_h);
    const float px_w = roi.width * w_f;
    const float px_h = roi.height * h_f;
    const float min_dim = std::min(w_f, h_f);
    const float min_px = min_dim * min_size_frac;
    const float max_px = min_dim * max_size_frac;
    const float side_px = std::clamp(std::max(px_w, px_h), min_px, std::max(min_px, max_px));
    roi.width = side_px / w_f;
    roi.height = side_px / h_f;
    roi.cx = std::clamp(roi.cx, 0.10f, 0.90f);
    roi.cy = std::clamp(roi.cy, 0.10f, 0.90f);
    return roi;
}

void fill_letterbox_bands_nchw(float* dst_nchw, uint32_t dst_w, uint32_t dst_h, uint32_t src_w, uint32_t src_h,
                               RoiNormRange range) noexcept {
    if (dst_nchw == nullptr || dst_w == 0U || dst_h == 0U || src_w == 0U || src_h == 0U) {
        return;
    }
    const float pad_val = (range == RoiNormRange::NegOneToPosOne) ? -1.0f : 0.0f;
    const float aspect = static_cast<float>(src_w) / static_cast<float>(src_h);
    const size_t plane = static_cast<size_t>(dst_w) * dst_h;
    if (aspect > 1.01f) {
        const uint32_t valid_h = std::max(1U, static_cast<uint32_t>(std::lround(static_cast<float>(dst_h) / aspect)));
        const uint32_t top = (dst_h - std::min(dst_h, valid_h)) / 2U;
        const uint32_t bot = std::min(dst_h, top + valid_h);
        for (uint32_t c = 0; c < 3U; ++c) {
            float* ch = dst_nchw + static_cast<size_t>(c) * plane;
            std::fill_n(ch, static_cast<size_t>(top) * dst_w, pad_val);
            if (bot < dst_h) {
                std::fill_n(ch + static_cast<size_t>(bot) * dst_w, static_cast<size_t>(dst_h - bot) * dst_w, pad_val);
            }
        }
    } else if (aspect < 0.99f) {
        const uint32_t valid_w = std::max(1U, static_cast<uint32_t>(std::lround(static_cast<float>(dst_w) * aspect)));
        const uint32_t left = (dst_w - std::min(dst_w, valid_w)) / 2U;
        const uint32_t right = std::min(dst_w, left + valid_w);
        for (uint32_t c = 0; c < 3U; ++c) {
            float* ch = dst_nchw + static_cast<size_t>(c) * plane;
            for (uint32_t y = 0; y < dst_h; ++y) {
                float* row = ch + static_cast<size_t>(y) * dst_w;
                std::fill_n(row, left, pad_val);
                if (right < dst_w) {
                    std::fill_n(row + right, dst_w - right, pad_val);
                }
            }
        }
    }
}

RoiRectNorm detect_skin_centroid_roi(const CapturedFrameBuffer& src) noexcept {
    if (src.width < 32U || src.height < 32U) {
        return {};
    }
    constexpr uint32_t kStep = 8U;
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_x2 = 0.0;
    double sum_y2 = 0.0;
    uint32_t count = 0U;

    const uint8_t* px = src.bytes.data();
    for (uint32_t y = src.height / 12U; y < (src.height * 11U) / 12U; y += kStep) {
        for (uint32_t x = src.width / 10U; x < (src.width * 9U) / 10U; x += kStep) {
            uint8_t yv = 0;
            uint8_t cb = 128;
            uint8_t cr = 128;
            if (src.format == CameraPixelFormat::NV12) {
                const uint8_t* y_plane = px;
                const uint8_t* uv_plane = px + static_cast<size_t>(src.stride_y) * src.height;
                yv = y_plane[static_cast<size_t>(y) * src.stride_y + x];
                const size_t uv_off =
                    static_cast<size_t>(y >> 1U) * src.stride_uv + static_cast<size_t>(x & ~1U);
                cb = uv_plane[uv_off + 0U];
                cr = uv_plane[uv_off + 1U];
            } else if (src.format == CameraPixelFormat::YUY2) {
                const size_t pair_off = static_cast<size_t>(y) * src.stride_y + static_cast<size_t>(x & ~1U) * 2U;
                yv = px[pair_off + ((x & 1U) != 0U ? 2U : 0U)];
                cb = px[pair_off + 1U];
                cr = px[pair_off + 3U];
            } else {
                const size_t off = static_cast<size_t>(y) * src.stride_y + static_cast<size_t>(x) * 4U;
                const float b = static_cast<float>(px[off + 0U]);
                const float g = static_cast<float>(px[off + 1U]);
                const float r = static_cast<float>(px[off + 2U]);
                yv = static_cast<uint8_t>(std::clamp(0.299f * r + 0.587f * g + 0.114f * b, 0.0f, 255.0f));
                cb = static_cast<uint8_t>(std::clamp(128.0f - 0.168736f * r - 0.331264f * g + 0.5f * b, 0.0f, 255.0f));
                cr = static_cast<uint8_t>(std::clamp(128.0f + 0.5f * r - 0.418688f * g - 0.081312f * b, 0.0f, 255.0f));
            }
            if (yv >= 50U && yv <= 240U && cb >= 77U && cb <= 127U && cr >= 133U && cr <= 173U) {
                const float nx = static_cast<float>(x) / static_cast<float>(src.width);
                const float ny = static_cast<float>(y) / static_cast<float>(src.height);
                sum_x += nx;
                sum_y += ny;
                sum_x2 += static_cast<double>(nx) * nx;
                sum_y2 += static_cast<double>(ny) * ny;
                ++count;
            }
        }
    }

    if (count < 16U) {
        return {};
    }
    const float mean_x = static_cast<float>(sum_x / count);
    const float mean_y = static_cast<float>(sum_y / count);
    const float var_x = std::max(0.0f, static_cast<float>(sum_x2 / count) - mean_x * mean_x);
    const float var_y = std::max(0.0f, static_cast<float>(sum_y2 / count) - mean_y * mean_y);
    const float span = std::max(std::sqrt(var_x) * 3.8f, std::sqrt(var_y) * 3.8f);
    RoiRectNorm roi{};
    roi.cx = mean_x;
    roi.cy = mean_y;
    roi.width = std::clamp(span, 0.22f, 0.72f);
    roi.height = roi.width;
    roi.rotation_rad = 0.0f;
    roi.valid = true;
    return enforce_square_pixel_roi(roi, src.width, src.height, 0.22f, 0.85f);
}

void downscale_camera_to_rgba8(const CapturedFrameBuffer& src, uint32_t dst_w, uint32_t dst_h,
                                 uint8_t* dst_rgba) noexcept {
    if (dst_rgba == nullptr || src.width == 0U || src.height == 0U || dst_w == 0U || dst_h == 0U) {
        return;
    }
    const uint8_t* px = src.bytes.data();
    const uint32_t max_uv_y = std::max(1U, src.height >> 1U) - 1U;
    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        const uint32_t sy = std::min(src.height - 1U, (dy * src.height + (dst_h >> 1U)) / dst_h);
        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            const uint32_t sx = std::min(src.width - 1U, (dx * src.width + (dst_w >> 1U)) / dst_w);
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            if (src.format == CameraPixelFormat::NV12) {
                const uint8_t* y_plane = px;
                const uint8_t* uv_plane = px + static_cast<size_t>(src.stride_y) * src.height;
                const float luma = static_cast<float>(y_plane[static_cast<size_t>(sy) * src.stride_y + sx]);
                const uint32_t uvy = std::min(sy >> 1U, max_uv_y);
                const size_t uv_off =
                    static_cast<size_t>(uvy) * src.stride_uv + static_cast<size_t>(sx & ~1U);
                const float u = static_cast<float>(uv_plane[uv_off + 0U]) - 128.0f;
                const float v = static_cast<float>(uv_plane[uv_off + 1U]) - 128.0f;
                const float c = std::max(luma - 16.0f, 0.0f) * 1.1643835f;
                r = static_cast<uint8_t>(std::clamp(c + 1.5960268f * v, 0.0f, 255.0f));
                g = static_cast<uint8_t>(std::clamp(c - 0.3917623f * u - 0.8129676f * v, 0.0f, 255.0f));
                b = static_cast<uint8_t>(std::clamp(c + 2.0172321f * u, 0.0f, 255.0f));
            } else if (src.format == CameraPixelFormat::YUY2) {
                const size_t pair_off = static_cast<size_t>(sy) * src.stride_y + static_cast<size_t>(sx & ~1U) * 2U;
                const float luma = static_cast<float>(px[pair_off + ((sx & 1U) != 0U ? 2U : 0U)]);
                const float u = static_cast<float>(px[pair_off + 1U]) - 128.0f;
                const float v = static_cast<float>(px[pair_off + 3U]) - 128.0f;
                const float c = std::max(luma - 16.0f, 0.0f) * 1.1643835f;
                r = static_cast<uint8_t>(std::clamp(c + 1.5960268f * v, 0.0f, 255.0f));
                g = static_cast<uint8_t>(std::clamp(c - 0.3917623f * u - 0.8129676f * v, 0.0f, 255.0f));
                b = static_cast<uint8_t>(std::clamp(c + 2.0172321f * u, 0.0f, 255.0f));
            } else {
                const size_t off = static_cast<size_t>(sy) * src.stride_y + static_cast<size_t>(sx) * 4U;
                b = px[off + 0U];
                g = px[off + 1U];
                r = px[off + 2U];
            }
            const size_t d_off = (static_cast<size_t>(dy) * dst_w + dx) * 4U;
            dst_rgba[d_off + 0U] = r;
            dst_rgba[d_off + 1U] = g;
            dst_rgba[d_off + 2U] = b;
            dst_rgba[d_off + 3U] = 255U;
        }
    }
}

} // namespace aether::tracking
