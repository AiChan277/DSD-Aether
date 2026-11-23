#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace aether::core {

enum class ViewportAspectMode : uint8_t {
    FitAuto = 0,    ///< Unconstrained viewport fill with 1:1 world-to-pixel aspect lock
    Aspect16x9 = 1, ///< Centered 16:9 broadcast safe-frame with #0B0D12 matte
    Aspect9x16 = 2, ///< Centered 9:16 vertical short frame with #0B0D12 matte
    Aspect1x1 = 3,  ///< Centered 1:1 square frame with #0B0D12 matte
};

[[nodiscard]] constexpr const char* viewport_aspect_label(ViewportAspectMode mode) noexcept {
    switch (mode) {
    case ViewportAspectMode::FitAuto:
        return "Fit (Auto)";
    case ViewportAspectMode::Aspect16x9:
        return "16:9";
    case ViewportAspectMode::Aspect9x16:
        return "9:16";
    case ViewportAspectMode::Aspect1x1:
        return "1:1";
    }
    return "Fit (Auto)";
}

[[nodiscard]] constexpr float viewport_aspect_target_ratio(ViewportAspectMode mode) noexcept {
    switch (mode) {
    case ViewportAspectMode::FitAuto:
        return 0.0f;
    case ViewportAspectMode::Aspect16x9:
        return 16.0f / 9.0f;
    case ViewportAspectMode::Aspect9x16:
        return 9.0f / 16.0f;
    case ViewportAspectMode::Aspect1x1:
        return 1.0f;
    }
    return 0.0f;
}

[[nodiscard]] constexpr ViewportAspectMode clamp_viewport_aspect_mode(int value) noexcept {
    if (value <= 0) {
        return ViewportAspectMode::FitAuto;
    }
    if (value == 1) {
        return ViewportAspectMode::Aspect16x9;
    }
    if (value == 2) {
        return ViewportAspectMode::Aspect9x16;
    }
    return ViewportAspectMode::Aspect1x1;
}

struct ViewportAspectLayout {
    float vp_w{1.0f};
    float vp_h{1.0f};
    float frame_x0{0.0f}; ///< Pixel-local X start inside viewport [0, vp_w]
    float frame_y0{0.0f}; ///< Pixel-local Y start inside viewport [0, vp_h]
    float frame_x1{1.0f}; ///< Pixel-local X end inside viewport [0, vp_w]
    float frame_y1{1.0f}; ///< Pixel-local Y end inside viewport [0, vp_h]
    float norm_x0{0.0f};  ///< Normalized [0, 1] X start inside viewport
    float norm_y0{0.0f};  ///< Normalized [0, 1] Y start inside viewport
    float norm_x1{1.0f};  ///< Normalized [0, 1] X end inside viewport
    float norm_y1{1.0f};  ///< Normalized [0, 1] Y end inside viewport
    float uv_du{1.0f};    ///< Sampled UV width from broadcast source texture (<= 1.0)
    float uv_dv{1.0f};    ///< Sampled UV height from broadcast source texture (<= 1.0)
    float frame_aspect{16.0f / 9.0f};
    bool explicit_matte{false};

    [[nodiscard]] float frame_width() const noexcept { return std::max(1.0f, frame_x1 - frame_x0); }

    [[nodiscard]] float frame_height() const noexcept { return std::max(1.0f, frame_y1 - frame_y0); }

    /** Computes effective vertical FOV (radians) matching the sampled vertical UV span `uv_dv`. */
    [[nodiscard]] float effective_fovy_rad(float source_fovy_rad) const noexcept {
        const float half = std::max(0.01f, source_fovy_rad * 0.5f);
        return 2.0f * std::atan(uv_dv * std::tan(half));
    }

    /** Returns horizontal/vertical world-unit pixel ratio (exact 1.0 means zero stretch/squish). */
    [[nodiscard]] float pixel_aspect_ratio(float source_aspect) const noexcept {
        const float safe_src = std::max(1e-4f, source_aspect);
        const float src_aspect_uv = std::max(1e-5f, (uv_du * safe_src) / std::max(1e-5f, uv_dv));
        return frame_aspect / src_aspect_uv;
    }
};

/**
 * Computes lossless 1:1 pixel-aspect layout between a rendered broadcast source (`out_w x out_h`, e.g. 1920x1080)
 * and a dynamically sized Studio viewport (`vp_w x vp_h`).
 */
[[nodiscard]] inline ViewportAspectLayout
compute_viewport_aspect_layout(float vp_w, float vp_h, float out_w, float out_h, ViewportAspectMode mode) noexcept {
    ViewportAspectLayout r{};
    const float safe_vp_w = std::max(1.0f, vp_w);
    const float safe_vp_h = std::max(1.0f, vp_h);
    const float safe_out_w = std::max(1.0f, out_w);
    const float safe_out_h = std::max(1.0f, out_h);
    const float out_aspect = safe_out_w / safe_out_h;
    const float vp_aspect = safe_vp_w / safe_vp_h;

    r.vp_w = safe_vp_w;
    r.vp_h = safe_vp_h;

    const float target = viewport_aspect_target_ratio(mode);
    if (mode == ViewportAspectMode::FitAuto || target <= 0.0f) {
        r.explicit_matte = false;
        r.frame_x0 = 0.0f;
        r.frame_y0 = 0.0f;
        r.frame_x1 = safe_vp_w;
        r.frame_y1 = safe_vp_h;
        r.norm_x0 = 0.0f;
        r.norm_y0 = 0.0f;
        r.norm_x1 = 1.0f;
        r.norm_y1 = 1.0f;
        r.frame_aspect = vp_aspect;
    } else {
        r.explicit_matte = true;
        float fw = safe_vp_w;
        float fh = safe_vp_h;
        if (vp_aspect >= target) {
            fh = safe_vp_h;
            fw = std::min(safe_vp_w, fh * target);
        } else {
            fw = safe_vp_w;
            fh = std::min(safe_vp_h, fw / target);
        }
        fw = std::max(1.0f, fw);
        fh = std::max(1.0f, fh);
        const float mx = (safe_vp_w - fw) * 0.5f;
        const float my = (safe_vp_h - fh) * 0.5f;
        r.frame_x0 = mx;
        r.frame_y0 = my;
        r.frame_x1 = mx + fw;
        r.frame_y1 = my + fh;
        r.norm_x0 = r.frame_x0 / safe_vp_w;
        r.norm_y0 = r.frame_y0 / safe_vp_h;
        r.norm_x1 = r.frame_x1 / safe_vp_w;
        r.norm_y1 = r.frame_y1 / safe_vp_h;
        r.frame_aspect = fw / fh;
    }

    // Lock 1:1 world-to-pixel aspect: (uv_du * out_aspect) / uv_dv == frame_aspect, clamped in [0.02, 1].
    if (r.frame_aspect >= out_aspect) {
        r.uv_du = 1.0f;
        r.uv_dv = std::clamp(out_aspect / std::max(1e-4f, r.frame_aspect), 0.02f, 1.0f);
    } else {
        r.uv_dv = 1.0f;
        r.uv_du = std::clamp(r.frame_aspect / std::max(1e-4f, out_aspect), 0.02f, 1.0f);
    }
    return r;
}

struct ContainedRect {
    float x0{0.0f};
    float y0{0.0f};
    float x1{1.0f};
    float y1{1.0f};

    [[nodiscard]] float width() const noexcept { return std::max(1.0f, x1 - x0); }
    [[nodiscard]] float height() const noexcept { return std::max(1.0f, y1 - y0); }
    [[nodiscard]] float aspect() const noexcept { return width() / height(); }
};

/**
 * Fits a rectangle of fixed aspect ratio `content_aspect` (e.g., 16:9 StudioBroadcastView) centered
 * inside an arbitrary container `(container_w x container_h)` using aspect-preserving letterbox/pillarbox.
 */
[[nodiscard]] inline ContainedRect compute_contained_aspect_rect(float container_w, float container_h,
                                                                 float content_aspect) noexcept {
    const float cw = std::max(1.0f, container_w);
    const float ch = std::max(1.0f, container_h);
    const float ca = std::max(1e-4f, content_aspect);
    const float box_aspect = cw / ch;
    float rw = cw;
    float rh = ch;
    if (box_aspect >= ca) {
        rh = ch;
        rw = std::min(cw, rh * ca);
    } else {
        rw = cw;
        rh = std::min(ch, rw / ca);
    }
    rw = std::max(1.0f, rw);
    rh = std::max(1.0f, rh);
    const float mx = (cw - rw) * 0.5f;
    const float my = (ch - rh) * 0.5f;
    return {mx, my, mx + rw, my + rh};
}

} // namespace aether::core
