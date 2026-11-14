#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace aether::test {

struct FlipSummary {
    double mean{0.0};
    double p95{0.0};
    double max{0.0};
    std::vector<float> map{};
};

namespace detail {

[[nodiscard]] inline float srgb_to_linear(uint8_t c) noexcept {
    const float u = static_cast<float>(c) / 255.0f;
    return (u <= 0.04045f) ? (u / 12.92f) : std::pow((u + 0.055f) / 1.055f, 2.4f);
}

struct Lab {
    float L{0.0f};
    float a{0.0f};
    float b{0.0f};
};

struct YCxCz {
    float Y{0.0f};
    float Cx{0.0f};
    float Cz{0.0f};
};

[[nodiscard]] inline float lab_f(float t) noexcept {
    constexpr float kDelta = 6.0f / 29.0f;
    constexpr float kDelta3 = kDelta * kDelta * kDelta;
    return (t > kDelta3) ? std::cbrt(t) : (t / (3.0f * kDelta * kDelta) + 4.0f / 29.0f);
}

[[nodiscard]] inline void linear_rgb_to_xyz_d65(float r, float g, float b, float& X, float& Y, float& Z) noexcept {
    X = 0.4124564f * r + 0.3575761f * g + 0.1804375f * b;
    Y = 0.2126729f * r + 0.7151522f * g + 0.0721750f * b;
    Z = 0.0193339f * r + 0.1191920f * g + 0.9503041f * b;
}

[[nodiscard]] inline Lab xyz_to_cielab(float X, float Y, float Z) noexcept {
    constexpr float Xn = 0.95047f;
    constexpr float Yn = 1.00000f;
    constexpr float Zn = 1.08883f;
    const float fx = lab_f(X / Xn);
    const float fy = lab_f(Y / Yn);
    const float fz = lab_f(Z / Zn);
    return Lab{116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}

[[nodiscard]] inline YCxCz xyz_to_ycxcz(float X, float Y, float Z) noexcept {
    constexpr float Xn = 0.95047f;
    constexpr float Yn = 1.00000f;
    constexpr float Zn = 1.08883f;
    const float xn = X / Xn;
    const float yn = Y / Yn;
    const float zn = Z / Zn;
    return YCxCz{
        116.0f * yn - 16.0f,
        500.0f * (xn - yn),
        200.0f * (yn - zn),
    };
}

inline void separable_gaussian(std::vector<float>& src, uint32_t w, uint32_t h, float sigma, std::vector<float>& tmp) {
    if (sigma <= 0.1f || w == 0 || h == 0) {
        return;
    }
    const int radius = std::clamp(static_cast<int>(std::ceil(3.0f * sigma)), 1, 12);
    float weights[25]{};
    float wsum = 0.0f;
    for (int k = -radius; k <= radius; ++k) {
        const float val = std::exp(-0.5f * (static_cast<float>(k * k)) / (sigma * sigma));
        weights[k + radius] = val;
        wsum += val;
    }
    for (int i = 0; i <= 2 * radius; ++i) {
        weights[i] /= wsum;
    }

    tmp.resize(src.size());
    for (uint32_t y = 0; y < h; ++y) {
        const size_t row = static_cast<size_t>(y) * w;
        for (uint32_t x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                const int sx = std::clamp(static_cast<int>(x) + k, 0, static_cast<int>(w) - 1);
                acc += src[row + static_cast<size_t>(sx)] * weights[k + radius];
            }
            tmp[row + x] = acc;
        }
    }
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                const int sy = std::clamp(static_cast<int>(y) + k, 0, static_cast<int>(h) - 1);
                acc += tmp[static_cast<size_t>(sy) * w + x] * weights[k + radius];
            }
            src[static_cast<size_t>(y) * w + x] = acc;
        }
    }
}

} // namespace detail

/** Computes CIE76 Delta-E between two sRGB RGBA8 pixels (ignoring alpha). */
[[nodiscard]] inline double compute_delta_e76(const uint8_t rgba_a[4], const uint8_t rgba_b[4]) noexcept {
    float xa = 0.0f, ya = 0.0f, za = 0.0f;
    float xb = 0.0f, yb = 0.0f, zb = 0.0f;
    detail::linear_rgb_to_xyz_d65(detail::srgb_to_linear(rgba_a[0]), detail::srgb_to_linear(rgba_a[1]),
                                  detail::srgb_to_linear(rgba_a[2]), xa, ya, za);
    detail::linear_rgb_to_xyz_d65(detail::srgb_to_linear(rgba_b[0]), detail::srgb_to_linear(rgba_b[1]),
                                  detail::srgb_to_linear(rgba_b[2]), xb, yb, zb);
    const auto la = detail::xyz_to_cielab(xa, ya, za);
    const auto lb = detail::xyz_to_cielab(xb, yb, zb);
    const double dl = la.L - lb.L;
    const double da = la.a - lb.a;
    const double db = la.b - lb.b;
    return std::sqrt(dl * dl + da * da + db * db);
}

/**
 * Self-contained C++20 LDR FLIP evaluator (`ADR-0010`, Andersson et al. HPG 2020)
 * assuming ~67 pixels/degree (0.7 m viewing distance, 0.7 mm pixel pitch).
 */
[[nodiscard]] inline FlipSummary evaluate_flip_rgba8(const uint8_t* ref_rgba, const uint8_t* test_rgba, uint32_t w,
                                                     uint32_t h, double pixels_per_degree = 67.0) {
    FlipSummary out{};
    const size_t n = static_cast<size_t>(w) * h;
    if (n == 0 || ref_rgba == nullptr || test_rgba == nullptr) {
        return out;
    }

    std::vector<float> ref_y(n), ref_cx(n), ref_cz(n);
    std::vector<float> tst_y(n), tst_cx(n), tst_cz(n);
    for (size_t i = 0; i < n; ++i) {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        detail::linear_rgb_to_xyz_d65(detail::srgb_to_linear(ref_rgba[i * 4 + 0]),
                                      detail::srgb_to_linear(ref_rgba[i * 4 + 1]),
                                      detail::srgb_to_linear(ref_rgba[i * 4 + 2]), x, y, z);
        const auto ry = detail::xyz_to_ycxcz(x, y, z);
        ref_y[i] = ry.Y;
        ref_cx[i] = ry.Cx;
        ref_cz[i] = ry.Cz;

        detail::linear_rgb_to_xyz_d65(detail::srgb_to_linear(test_rgba[i * 4 + 0]),
                                      detail::srgb_to_linear(test_rgba[i * 4 + 1]),
                                      detail::srgb_to_linear(test_rgba[i * 4 + 2]), x, y, z);
        const auto ty = detail::xyz_to_ycxcz(x, y, z);
        tst_y[i] = ty.Y;
        tst_cx[i] = ty.Cx;
        tst_cz[i] = ty.Cz;
    }

    // Spatial CSF approximations mapped from pixels-per-degree (~1.0 / 2.2 px sigmas at 67 ppd)
    const float ppd_scale = static_cast<float>(pixels_per_degree / 67.0);
    const float sigma_achromatic = std::max(0.45f, 0.85f * ppd_scale);
    const float sigma_chromatic = std::max(0.90f, 1.80f * ppd_scale);

    std::vector<float> ref_y_csf = ref_y;
    std::vector<float> tst_y_csf = tst_y;
    std::vector<float> scratch;
    detail::separable_gaussian(ref_y_csf, w, h, sigma_achromatic, scratch);
    detail::separable_gaussian(tst_y_csf, w, h, sigma_achromatic, scratch);
    detail::separable_gaussian(ref_cx, w, h, sigma_chromatic, scratch);
    detail::separable_gaussian(tst_cx, w, h, sigma_chromatic, scratch);
    detail::separable_gaussian(ref_cz, w, h, sigma_chromatic, scratch);
    detail::separable_gaussian(tst_cz, w, h, sigma_chromatic, scratch);

    out.map.resize(n, 0.0f);
    std::vector<float> sorted_vals;
    sorted_vals.reserve(n);

    constexpr float kHyabMax = 100.0f + 120.0f;
    double sum = 0.0;
    float mx = 0.0f;

    auto sample_y = [&](const std::vector<float>& buf, int x, int y) -> float {
        const int sx = std::clamp(x, 0, static_cast<int>(w) - 1);
        const int sy = std::clamp(y, 0, static_cast<int>(h) - 1);
        return buf[static_cast<size_t>(sy) * w + static_cast<size_t>(sx)] / 100.0f;
    };

    for (uint32_t py = 0; py < h; ++py) {
        for (uint32_t px = 0; px < w; ++px) {
            const size_t i = static_cast<size_t>(py) * w + px;
            // Fast exact equality check preserves FLIP(I, I) == 0.0
            if (ref_rgba[i * 4 + 0] == test_rgba[i * 4 + 0] && ref_rgba[i * 4 + 1] == test_rgba[i * 4 + 1] &&
                ref_rgba[i * 4 + 2] == test_rgba[i * 4 + 2] && std::abs(ref_y_csf[i] - tst_y_csf[i]) < 1e-6f &&
                std::abs(ref_cx[i] - tst_cx[i]) < 1e-6f && std::abs(ref_cz[i] - tst_cz[i]) < 1e-6f) {
                out.map[i] = 0.0f;
                sorted_vals.push_back(0.0f);
                continue;
            }

            const float dy = std::abs(ref_y_csf[i] - tst_y_csf[i]);
            const float dcx = ref_cx[i] - tst_cx[i];
            const float dcz = ref_cz[i] - tst_cz[i];
            const float dchroma = std::sqrt(dcx * dcx + dcz * dcz);
            const float hyab = std::min(dy + dchroma, kHyabMax);
            const float dc = std::clamp(std::pow(hyab / 52.0f, 0.72f), 0.0f, 1.0f);

            // Edge/point feature discrepancy via Scharr gradient difference on Y
            const int ix = static_cast<int>(px);
            const int iy = static_cast<int>(py);
            auto grad_mag = [&](const std::vector<float>& buf) -> float {
                const float gx = -3.0f * sample_y(buf, ix - 1, iy - 1) - 10.0f * sample_y(buf, ix - 1, iy) -
                                 3.0f * sample_y(buf, ix - 1, iy + 1) + 3.0f * sample_y(buf, ix + 1, iy - 1) +
                                 10.0f * sample_y(buf, ix + 1, iy) + 3.0f * sample_y(buf, ix + 1, iy + 1);
                const float gy = -3.0f * sample_y(buf, ix - 1, iy - 1) - 10.0f * sample_y(buf, ix, iy - 1) -
                                 3.0f * sample_y(buf, ix + 1, iy - 1) + 3.0f * sample_y(buf, ix - 1, iy + 1) +
                                 10.0f * sample_y(buf, ix, iy + 1) + 3.0f * sample_y(buf, ix + 1, iy + 1);
                return std::sqrt(gx * gx + gy * gy) / 16.0f;
            };
            const float g_diff = std::abs(grad_mag(ref_y) - grad_mag(tst_y));
            const float df = std::clamp(g_diff * 0.45f, 0.0f, 1.0f);

            const float flip = std::clamp(std::pow(std::max(dc, 1e-6f), 1.0f - df), 0.0f, 1.0f);
            out.map[i] = flip;
            sorted_vals.push_back(flip);
            sum += static_cast<double>(flip);
            mx = std::max(mx, flip);
        }
    }

    std::sort(sorted_vals.begin(), sorted_vals.end());
    out.mean = sum / static_cast<double>(n);
    const size_t p95_idx = std::min(n - 1, static_cast<size_t>(std::floor(0.95 * static_cast<double>(n - 1))));
    out.p95 = static_cast<double>(sorted_vals[p95_idx]);
    out.max = static_cast<double>(mx);
    return out;
}

} // namespace aether::test
