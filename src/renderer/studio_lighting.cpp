#include "renderer/studio_lighting.hpp"

#include "renderer/circadian_lighting.hpp"
#include "renderer/lighting_rig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aether::renderer {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

[[nodiscard]] std::array<float, 3> norm3(std::array<float, 3> v) noexcept {
    const float l2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (l2 <= 1e-12f) {
        return {0.0f, 0.0f, 1.0f};
    }
    const float inv = 1.0f / std::sqrt(l2);
    return {v[0] * inv, v[1] * inv, v[2] * inv};
}

[[nodiscard]] std::array<float, 3> cross3(std::array<float, 3> a, std::array<float, 3> b) noexcept {
    return {
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    };
}

[[nodiscard]] std::array<float, 3> orbit_dir(float az_deg, float el_deg) noexcept {
    const float az = az_deg * kDegToRad;
    const float el = el_deg * kDegToRad;
    const float ce = std::cos(el);
    return norm3({std::sin(az) * ce, std::sin(el), std::cos(az) * ce});
}

[[nodiscard]] StudioFixtureProfile legacy_to_studio_fixture(LightingFixturePreset preset) noexcept {
    switch (preset) {
    case LightingFixturePreset::ArriSkyPanelS60C:
        return StudioFixtureProfile::ArriSkyPanel;
    case LightingFixturePreset::ArriFresnelT2Orbiter:
        return StudioFixtureProfile::ArriFresnel;
    case LightingFixturePreset::DedolightDled7:
        return StudioFixtureProfile::DedolightSpot;
    case LightingFixturePreset::BroadcastRingLight:
        return StudioFixtureProfile::BroadcastRing;
    case LightingFixturePreset::NaturalSunlightCieSky:
    case LightingFixturePreset::NaturalWindowPortal:
        return StudioFixtureProfile::NaturalSunCieSky;
    }
    return StudioFixtureProfile::RectSoftbox;
}

} // namespace

void configure_fixture_defaults(StudioLightSpec& spec, StudioFixtureProfile profile) noexcept {
    spec.fixture = profile;
    spec.softbox_enabled = false;
    spec.coaxial_lock = false;
    spec.window_portal = false;
    spec.ies_profile = -1;
    switch (profile) {
    case StudioFixtureProfile::RectSoftbox:
        spec.width_or_outer_m = 0.90f;
        spec.height_or_inner_m = 0.60f;
        spec.spot_inner_deg = 52.0f;
        spec.spot_outer_deg = 84.0f;
        spec.softbox_enabled = true;
        spec.diffusion_enabled = true;
        spec.diffusion_density = 0.72f;
        spec.ies_profile = 0;
        break;
    case StudioFixtureProfile::Octabox:
        spec.width_or_outer_m = 1.10f;
        spec.height_or_inner_m = 1.10f;
        spec.spot_inner_deg = 55.0f;
        spec.spot_outer_deg = 88.0f;
        spec.softbox_enabled = true;
        spec.diffusion_enabled = true;
        spec.diffusion_density = 0.80f;
        spec.ies_profile = 1;
        break;
    case StudioFixtureProfile::Stripbox:
        spec.width_or_outer_m = 0.30f;
        spec.height_or_inner_m = 1.20f;
        spec.spot_inner_deg = 36.0f;
        spec.spot_outer_deg = 68.0f;
        spec.softbox_enabled = true;
        spec.grid_enabled = true;
        spec.grid_angle_deg = 30.0f;
        spec.ies_profile = 2;
        break;
    case StudioFixtureProfile::ParabolicUmbrella:
        spec.width_or_outer_m = 1.05f;
        spec.height_or_inner_m = 1.05f;
        spec.spot_inner_deg = 34.0f;
        spec.spot_outer_deg = 70.0f;
        spec.softbox_enabled = true;
        spec.diffusion_density = 0.45f;
        spec.ies_profile = 3;
        break;
    case StudioFixtureProfile::LanternChinaBall:
        spec.width_or_outer_m = 0.45f;
        spec.height_or_inner_m = 0.45f;
        spec.spot_inner_deg = 120.0f;
        spec.spot_outer_deg = 165.0f;
        spec.softbox_enabled = true;
        spec.diffusion_density = 0.92f;
        spec.ies_profile = 7;
        break;
    case StudioFixtureProfile::ArriSkyPanel:
        spec.width_or_outer_m = 0.645f;
        spec.height_or_inner_m = 0.30f;
        spec.spot_inner_deg = 48.0f;
        spec.spot_outer_deg = 105.0f;
        spec.diffusion_density = 0.60f;
        spec.ies_profile = 0;
        break;
    case StudioFixtureProfile::ArriFresnel:
        spec.width_or_outer_m = 0.22f;
        spec.height_or_inner_m = 0.22f;
        spec.spot_inner_deg = 16.0f;
        spec.spot_outer_deg = 34.0f;
        spec.diffusion_density = 0.15f;
        spec.ies_profile = 4;
        break;
    case StudioFixtureProfile::DedolightSpot:
        spec.width_or_outer_m = 0.10f;
        spec.height_or_inner_m = 0.10f;
        spec.spot_inner_deg = 9.0f;
        spec.spot_outer_deg = 20.0f;
        spec.diffusion_density = 0.05f;
        spec.ies_profile = 5;
        break;
    case StudioFixtureProfile::EllipsoidalLeko:
        spec.width_or_outer_m = 0.18f;
        spec.height_or_inner_m = 0.18f;
        spec.spot_inner_deg = 19.0f;
        spec.spot_outer_deg = 26.0f;
        spec.diffusion_density = 0.08f;
        spec.gobo = StudioGoboPattern::WindowBlinds;
        spec.ies_profile = 6;
        break;
    case StudioFixtureProfile::BroadcastRing:
        spec.width_or_outer_m = 0.25f;
        spec.height_or_inner_m = 0.18f;
        spec.spot_inner_deg = 46.0f;
        spec.spot_outer_deg = 84.0f;
        spec.coaxial_lock = true;
        spec.diffusion_density = 0.70f;
        spec.ies_profile = 7;
        break;
    case StudioFixtureProfile::LedTubeAstera:
        spec.width_or_outer_m = 1.00f;
        spec.height_or_inner_m = 0.05f;
        spec.spot_inner_deg = 65.0f;
        spec.spot_outer_deg = 120.0f;
        spec.diffusion_density = 0.55f;
        spec.ies_profile = 2;
        break;
    case StudioFixtureProfile::NaturalSunCieSky:
        spec.width_or_outer_m = 1.20f;
        spec.height_or_inner_m = 1.80f;
        spec.spot_inner_deg = 45.0f;
        spec.spot_outer_deg = 85.0f;
        spec.diffusion_density = 0.40f;
        spec.ies_profile = -1;
        break;
    }
}

std::array<float, 3> gel_transmittance_rgb(StudioGelPreset gel, float green_magenta) noexcept {
    std::array<float, 3> c{1.0f, 1.0f, 1.0f};
    switch (gel) {
    case StudioGelPreset::None:
        break;
    case StudioGelPreset::QuarterCto:
        c = {1.12f, 0.94f, 0.72f};
        break;
    case StudioGelPreset::HalfCto:
        c = {1.22f, 0.86f, 0.50f};
        break;
    case StudioGelPreset::FullCto:
        c = {1.35f, 0.74f, 0.28f};
        break;
    case StudioGelPreset::QuarterCtb:
        c = {0.84f, 0.98f, 1.18f};
        break;
    case StudioGelPreset::HalfCtb:
        c = {0.68f, 0.94f, 1.32f};
        break;
    case StudioGelPreset::FullCtb:
        c = {0.50f, 0.88f, 1.48f};
        break;
    case StudioGelPreset::PlusGreen:
        c = {0.82f, 1.24f, 0.84f};
        break;
    case StudioGelPreset::MinusGreen:
        c = {1.16f, 0.78f, 1.14f};
        break;
    case StudioGelPreset::ShinkaiGoldenRose:
        c = {1.38f, 0.76f, 0.64f};
        break;
    case StudioGelPreset::CyberCyan:
        c = {0.28f, 1.18f, 1.42f};
        break;
    case StudioGelPreset::StageMagenta:
        c = {1.34f, 0.42f, 1.28f};
        break;
    }
    const float gm = std::clamp(green_magenta, -1.0f, 1.0f);
    c[0] *= (1.0f - 0.14f * gm);
    c[1] *= (1.0f + 0.18f * gm);
    c[2] *= (1.0f - 0.10f * gm);
    return c;
}

float frostbite_ev100_exposure_scale(float ev100_offset, float luminance_scale) noexcept {
    // Frostbite PBR 3.0 exposure normalization around EV100 reference = 0 stops
    const float clamped = std::clamp(ev100_offset, -8.0f, 8.0f);
    return std::max(luminance_scale, 0.0f) * std::exp2(clamped);
}

uint32_t pack_studio_lights_gpu(const LightingRig& rig, const std::array<float, 3>& pivot_ws,
                                const std::array<float, 3>& eye_pos_ws,
                                std::array<StudioLightGPU, kMaxStudioLights>& out_gpu) noexcept {
    for (auto& g : out_gpu) {
        g = StudioLightGPU{};
    }

    auto pack_one = [&](const StudioLightSpec& s, uint32_t dst_idx) noexcept {
        StudioLightGPU& g = out_gpu[dst_idx];
        const std::array<float, 3> aim_target = s.aim_lock ? pivot_ws : s.target_ws;
        std::array<float, 3> light_to_scene_dir{};
        std::array<float, 3> pos{};

        if (s.coaxial_lock && s.fixture == StudioFixtureProfile::BroadcastRing) {
            const std::array<float, 3> coax =
                norm3({aim_target[0] - eye_pos_ws[0], aim_target[1] - (eye_pos_ws[1] + 0.04f),
                       aim_target[2] - eye_pos_ws[2]});
            pos = {eye_pos_ws[0], eye_pos_ws[1] + 0.04f, eye_pos_ws[2]};
            light_to_scene_dir = coax;
        } else if (s.use_orbit_angles) {
            const std::array<float, 3> to_light = orbit_dir(s.orbit_azimuth_deg, s.orbit_elevation_deg);
            const float dist = std::clamp(s.orbit_distance_m, 0.25f, 20.0f);
            pos = {
                aim_target[0] + to_light[0] * dist,
                aim_target[1] + to_light[1] * dist,
                aim_target[2] + to_light[2] * dist,
            };
            light_to_scene_dir = norm3({-to_light[0], -to_light[1], -to_light[2]});
        } else {
            pos = s.position_ws;
            light_to_scene_dir = norm3({aim_target[0] - pos[0], aim_target[1] - pos[1], aim_target[2] - pos[2]});
        }

        // Orthonormal softbox U/V frame with roll angle
        const std::array<float, 3> world_up = (std::fabs(light_to_scene_dir[1]) > 0.96f)
                                                  ? std::array<float, 3>{0.0f, 0.0f, 1.0f}
                                                  : std::array<float, 3>{0.0f, 1.0f, 0.0f};
        const std::array<float, 3> base_u = norm3(cross3(world_up, light_to_scene_dir));
        const std::array<float, 3> base_v = norm3(cross3(light_to_scene_dir, base_u));
        const float r_rad = s.roll_deg * kDegToRad;
        const float cr = std::cos(r_rad);
        const float sr = std::sin(r_rad);
        const std::array<float, 3> tangent_u =
            norm3({base_u[0] * cr + base_v[0] * sr, base_u[1] * cr + base_v[1] * sr, base_u[2] * cr + base_v[2] * sr});

        const std::array<float, 3> cct_lin = kelvin_to_linear_rgb(s.kelvin_cct);
        const std::array<float, 3> gel_lin =
            s.gel_enabled ? gel_transmittance_rgb(s.gel, s.green_magenta) : std::array<float, 3>{1.0f, 1.0f, 1.0f};

        uint32_t flags = static_cast<uint32_t>(s.fixture) & 0x0Fu;
        if (s.enabled)
            flags |= AETHER_LIGHT_FLAG_ENABLED;
        if (s.solo)
            flags |= AETHER_LIGHT_FLAG_SOLO;
        if (s.cast_shadow)
            flags |= AETHER_LIGHT_FLAG_CAST_SHADOW;
        if (s.softbox_enabled)
            flags |= AETHER_LIGHT_FLAG_SOFTBOX_ON;
        if (s.grid_enabled)
            flags |= AETHER_LIGHT_FLAG_GRID_ON;
        if (s.barn_doors_enabled)
            flags |= AETHER_LIGHT_FLAG_BARN_DOORS_ON;
        if (s.gel_enabled)
            flags |= AETHER_LIGHT_FLAG_GEL_ON;
        if (s.aim_lock)
            flags |= AETHER_LIGHT_FLAG_AIM_LOCK;
        if (s.coaxial_lock)
            flags |= AETHER_LIGHT_FLAG_COAXIAL_LOCK;
        if (s.diffusion_enabled)
            flags |= AETHER_LIGHT_FLAG_DIFFUSION_ON;
        if (s.window_portal)
            flags |= AETHER_LIGHT_FLAG_WINDOW_PORTAL;
        if (s.passive_flag == PassiveFlagMode::NegativeFillBlack) {
            flags |= AETHER_LIGHT_FLAG_NEGATIVE_FILL;
        } else if (s.passive_flag != PassiveFlagMode::None) {
            flags |= AETHER_LIGHT_FLAG_BOUNCE_CARD;
        }
        flags |= (static_cast<uint32_t>(s.gobo) & 0x0Fu) << 17u;

        // When Softbox Attachment is OFF, shrink effective emitter size toward bare bulb (~8 cm)
        const float size_x = s.softbox_enabled ? std::max(s.width_or_outer_m, 0.04f)
                                               : std::min(std::max(s.width_or_outer_m, 0.04f), 0.14f);
        const float size_y = s.softbox_enabled ? std::max(s.height_or_inner_m, 0.04f)
                                               : std::min(std::max(s.height_or_inner_m, 0.04f), 0.14f);

        const float inner_cos = std::cos(std::clamp(s.spot_inner_deg, 1.0f, 175.0f) * 0.5f * kDegToRad);
        const float outer_cos =
            std::cos(std::clamp(std::max(s.spot_outer_deg, s.spot_inner_deg + 1.0f), 2.0f, 179.0f) * 0.5f * kDegToRad);
        const float grid_cos = std::cos(std::clamp(s.grid_angle_deg, 10.0f, 85.0f) * 0.5f * kDegToRad);

        g.position_ws[0] = pos[0];
        g.position_ws[1] = pos[1];
        g.position_ws[2] = pos[2];
        g.flags_and_type = flags;
        g.direction_ws[0] = light_to_scene_dir[0];
        g.direction_ws[1] = light_to_scene_dir[1];
        g.direction_ws[2] = light_to_scene_dir[2];
        g.intensity_ev100 = frostbite_ev100_exposure_scale(s.exposure_stops, s.intensity_ev100);
        g.tangent_u_ws[0] = tangent_u[0];
        g.tangent_u_ws[1] = tangent_u[1];
        g.tangent_u_ws[2] = tangent_u[2];
        g.kelvin_cct = s.kelvin_cct;
        g.color_rgb_linear[0] = s.tint_rgb[0] * cct_lin[0] * gel_lin[0];
        g.color_rgb_linear[1] = s.tint_rgb[1] * cct_lin[1] * gel_lin[1];
        g.color_rgb_linear[2] = s.tint_rgb[2] * cct_lin[2] * gel_lin[2];
        g.light_channel_mask = s.channel_mask;

        g.size_xy_radius[0] = size_x;
        g.size_xy_radius[1] = size_y;
        g.inner_outer_cos[0] = inner_cos;
        g.inner_outer_cos[1] = outer_cos;
        g.barn_door_cuts[0] = std::clamp(s.barn_door_cuts_deg[0], 3.0f, 89.0f) * kDegToRad;
        g.barn_door_cuts[1] = std::clamp(s.barn_door_cuts_deg[1], 3.0f, 89.0f) * kDegToRad;
        g.barn_door_cuts[2] = std::clamp(s.barn_door_cuts_deg[2], 3.0f, 89.0f) * kDegToRad;
        g.barn_door_cuts[3] = std::clamp(s.barn_door_cuts_deg[3], 3.0f, 89.0f) * kDegToRad;
        g.grid_collimation_cos = grid_cos;
        g.diffusion_softness = s.diffusion_enabled ? std::clamp(s.diffusion_density, 0.0f, 1.0f) : 0.0f;
        g.ies_profile_index = static_cast<float>(s.ies_profile);
        g.catchlight_weight = std::clamp(s.catchlight_weight * rig.eye_catchlight_intensity, 0.0f, 4.0f);
        g.shadow_bias_scale = std::clamp(s.shadow_bias_scale, 0.25f, 4.0f);
        g.padding[0] = static_cast<float>(s.passive_flag);
        g.padding[1] = (s.use_orbit_angles ? 1.0f : 0.0f);
        g.padding[2] = rig.eye_catchlight_size;
    };

    uint32_t count = 0;
    if (rig.use_studio_lights && rig.studio_light_count > 0u) {
        const uint32_t n = std::min(rig.studio_light_count, kMaxStudioLights);
        bool any_solo = false;
        for (uint32_t i = 0; i < n; ++i) {
            if (rig.studio_lights[i].solo && rig.studio_lights[i].enabled) {
                any_solo = true;
                break;
            }
        }
        for (uint32_t i = 0; i < n && count < kMaxStudioLights; ++i) {
            StudioLightSpec copy = rig.studio_lights[i];
            if (any_solo && !copy.solo) {
                copy.enabled = false;
            }
            pack_one(copy, count++);
        }
        return count;
    }

    // Legacy 8-light LightingRig bridge: synthesize StudioLightSpec per active light cleanly
    const uint32_t n = std::min<uint32_t>(rig.active_count, LightingRig::kMaxLights);
    const StudioFixtureProfile key_profile =
        (rig.key_studio_profile_override >= 0 && rig.key_studio_profile_override <= 11)
            ? static_cast<StudioFixtureProfile>(rig.key_studio_profile_override)
            : legacy_to_studio_fixture(rig.fixture_preset);
    for (uint32_t i = 0; i < n && count < kMaxStudioLights; ++i) {
        const auto& sl = rig.lights[i];
        StudioLightSpec spec{};
        spec.enabled = sl.enabled;
        spec.cast_shadow = sl.cast_rt_shadow && (sl.role != LightRole::Rim && sl.role != LightRole::HairOrbit);
        spec.use_orbit_angles = sl.use_2d_orbit_angle;
        spec.orbit_azimuth_deg = sl.orbit_azimuth_deg;
        spec.orbit_elevation_deg = sl.orbit_elevation_deg;
        spec.orbit_distance_m = 2.15f;
        spec.position_ws = {sl.direction_or_pos[0], sl.direction_or_pos[1], sl.direction_or_pos[2]};
        spec.target_ws = pivot_ws;
        spec.intensity_ev100 = sl.intensity;
        spec.kelvin_cct = 5600.0f;
        spec.tint_rgb = {sl.color[0], sl.color[1], sl.color[2]};

        if (i == 0u) {
            configure_fixture_defaults(spec, key_profile);
            spec.softbox_enabled = rig.key_softbox_enabled;
            spec.grid_enabled = rig.key_grid_enabled;
            spec.grid_angle_deg = rig.key_grid_angle_deg;
            spec.barn_doors_enabled = rig.key_barn_doors_enabled;
            spec.barn_door_cuts_deg = rig.key_barn_door_cuts_deg;
            spec.diffusion_enabled = rig.key_diffusion_enabled;
            spec.diffusion_density = rig.key_diffusion_density;
            spec.gel_enabled = rig.key_gel_enabled;
            spec.gel = rig.key_gel;
            spec.gobo = rig.key_gobo;
            if (key_profile == StudioFixtureProfile::ArriFresnel ||
                key_profile == StudioFixtureProfile::DedolightSpot ||
                key_profile == StudioFixtureProfile::EllipsoidalLeko) {
                const float beam = std::clamp(rig.photometric_beam_angle_deg, 8.0f, 55.0f);
                spec.spot_outer_deg = beam;
                spec.spot_inner_deg = beam * ((key_profile == StudioFixtureProfile::DedolightSpot) ? 0.55f : 0.65f);
            }
            if (rig.fixture_preset == LightingFixturePreset::NaturalWindowPortal) {
                spec.window_portal = true;
                spec.width_or_outer_m = 1.20f;
                spec.height_or_inner_m = 1.80f;
            }
            if (rig.fixture_preset == LightingFixturePreset::BroadcastRingLight) {
                spec.coaxial_lock = true;
            }
        } else if (sl.role == LightRole::Fill) {
            configure_fixture_defaults(spec, StudioFixtureProfile::LanternChinaBall);
            spec.catchlight_weight = 0.25f;
        } else if (sl.role == LightRole::Rim || sl.role == LightRole::HairOrbit) {
            configure_fixture_defaults(spec, StudioFixtureProfile::Stripbox);
            spec.catchlight_weight = 0.40f;
        }
        pack_one(spec, count++);
    }
    return count;
}

void generate_heitz_ltc_luts(std::array<float, kLtcLutSize * kLtcLutSize * 4>& out_inv_m,
                             std::array<float, kLtcLutSize * kLtcLutSize * 4>& out_amp) noexcept {
    // Analytic GGX -> Clamped-Cosine LTC inverse matrix & norm tables (Heitz et al. 2016 §4)
    // u = sqrt(alpha) in [0.02..1.0], v = 1.0 - cos(theta) in [0..1] (N.V -> 1 at v=0, 0 at v=1)
    for (uint32_t y = 0; y < kLtcLutSize; ++y) {
        const float cos_t =
            std::clamp(1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(kLtcLutSize), 0.001f, 0.9999f);
        const float sin_t = std::sqrt(std::max(0.0f, 1.0f - cos_t * cos_t));
        const float theta_norm = std::acos(cos_t) / (0.5f * kPi);
        for (uint32_t x = 0; x < kLtcLutSize; ++x) {
            const float r = std::clamp((static_cast<float>(x) + 0.5f) / static_cast<float>(kLtcLutSize), 0.02f, 1.0f);
            const float alpha = r * r;

            // Smooth monotone fit of GGX isotropic lobe compression m00, m11 and skew m02
            const float base_scale = 1.0f / std::max(0.06f, alpha + 0.06f * (1.0f - alpha));
            const float grazing = 1.0f - 0.55f * std::pow(1.0f - cos_t, 2.2f) * (1.0f - alpha);
            const float m00 = std::clamp(base_scale * grazing, 1.0f, 16.0f);
            const float m11 = std::clamp(base_scale * (1.0f + 0.25f * theta_norm * (1.0f - alpha)), 1.0f, 16.0f);
            const float skew = -sin_t * (1.0f - alpha) * 0.42f * (1.0f - 0.35f * alpha);

            // Magnitude (GGX directional albedo + Schlick Fresnel F0 integral weights per Heitz 2016)
            const float mag = std::clamp(1.0f - 0.18f * alpha * std::pow(1.0f - cos_t, 1.8f), 0.35f, 1.0f);
            const float fresnel_term = std::clamp(std::pow(1.0f - cos_t, 4.5f) * (1.0f - 0.65f * alpha), 0.0f, 1.0f);

            const size_t idx = (static_cast<size_t>(y) * kLtcLutSize + x) * 4u;
            // Matrix M^-1 packed into RGBA32F: [m00, m02, m11, m20]
            out_inv_m[idx + 0] = m00;
            out_inv_m[idx + 1] = skew;
            out_inv_m[idx + 2] = m11;
            out_inv_m[idx + 3] = 0.0f;

            out_amp[idx + 0] = mag;
            out_amp[idx + 1] = fresnel_term;
            out_amp[idx + 2] = 0.0f;
            out_amp[idx + 3] = 1.0f;
        }
    }
}

void generate_ies_lm63_lut(std::array<float, kIesLutWidth * kIesLutRows>& out_ies_r32f) noexcept {
    // 256 polar angle samples (0..pi radians) x 64 IES profiles
    out_ies_r32f.fill(1.0f);
    for (uint32_t row = 0; row < kIesLutRows; ++row) {
        for (uint32_t col = 0; col < kIesLutWidth; ++col) {
            const float u = (static_cast<float>(col) + 0.5f) / static_cast<float>(kIesLutWidth);
            const float theta = u * kPi;
            const float c = std::cos(theta);
            float candela = 1.0f;
            switch (row) {
            case 0: // Rect Softbox / SkyPanel (~105 deg Lambertian)
                candela = std::pow(std::max(c, 0.0f), 1.15f);
                break;
            case 1: // Octabox broad wrap
                candela = std::pow(std::max(c, 0.0f), 0.90f);
                break;
            case 2: // Vertical Stripbox / Linear Tube
                candela = std::pow(std::max(c, 0.0f), 1.65f);
                break;
            case 3: // Parabolic Umbrella (focused core + broad wrap tail)
                candela = 0.35f * std::max(c, 0.0f) + 0.65f * std::pow(std::max(c, 0.0f), 6.5f);
                break;
            case 4: // ARRI Fresnel Spot (smooth field rolloff)
                candela = std::pow(std::max(c, 0.0f), 10.0f) * (0.95f + 0.05f * std::cos(6.0f * theta));
                break;
            case 5: // Dedolight Aspheric precision beam
                candela = std::pow(std::max(c, 0.0f), 26.0f);
                break;
            case 6: // Ellipsoidal Leko crisp flat field
                candela = std::clamp((c - 0.86f) / 0.08f, 0.0f, 1.0f);
                break;
            case 7: // Lantern / Ring / China Ball wide distribution with top skirt suppression
                candela = (theta < 2.25f) ? std::max(0.20f, 0.55f + 0.45f * c) : 0.08f;
                break;
            default:
                candela = std::pow(std::max(c, 0.0f), 1.0f);
                break;
            }
            out_ies_r32f[static_cast<size_t>(row) * kIesLutWidth + col] = candela;
        }
    }
}

void compute_hillaire_sky_sh9(const std::array<float, 3>& sun_dir_ws, const std::array<float, 3>& sun_color_lin,
                              float sky_strength, float ground_bounce_strength,
                              std::array<std::array<float, 4>, 9>& out_sh9) noexcept {
    // analytical Hillaire-inspired Rayleigh zenith + horizon + ground-albedo SH9 projection (Y-up orthonormal)
    const float sun_y = std::clamp(sun_dir_ws[1], -0.4f, 1.0f);
    const float day = std::clamp((sun_y + 0.12f) / 0.45f, 0.0f, 1.0f);
    const float golden = std::clamp(1.0f - std::fabs(sun_y - 0.14f) / 0.25f, 0.0f, 1.0f);

    const std::array<float, 3> zenith = {
        (0.08f + 0.14f * day + 0.10f * golden * sun_color_lin[0]) * sky_strength,
        (0.14f + 0.28f * day + 0.08f * golden * sun_color_lin[1]) * sky_strength,
        (0.28f + 0.55f * day + 0.12f * (1.0f - day)) * sky_strength,
    };
    const std::array<float, 3> horizon = {
        (0.22f + 0.28f * day + 0.42f * golden * sun_color_lin[0]) * sky_strength,
        (0.26f + 0.36f * day + 0.24f * golden * sun_color_lin[1]) * sky_strength,
        (0.36f + 0.48f * day + 0.08f * golden) * sky_strength,
    };
    const std::array<float, 3> ground = {
        horizon[0] * (0.35f + 0.45f * ground_bounce_strength),
        horizon[1] * (0.32f + 0.42f * ground_bounce_strength),
        horizon[2] * (0.28f + 0.38f * ground_bounce_strength),
    };

    const std::array<float, 3> s = norm3(sun_dir_ws);
    for (auto& c : out_sh9) {
        c = {0.0f, 0.0f, 0.0f, 0.0f};
    }

    // SH9 basis constants (Ramamoorthi / Hillaire sky projection)
    constexpr float kY0 = 0.282095f;
    constexpr float kY1 = 0.488603f;
    constexpr float kY20 = 0.315392f;
    constexpr float kY2m = 1.092548f;

    for (size_t ch = 0; ch < 3u; ++ch) {
        const float avg = (zenith[ch] + 2.0f * horizon[ch] + ground[ch]) * 0.25f;
        const float grad_y = (zenith[ch] - ground[ch]) * 0.5f;
        const float sun_glow = golden * 0.22f * sun_color_lin[ch] * sky_strength;
        out_sh9[0][ch] = avg / kY0 * 0.28f;
        out_sh9[1][ch] = grad_y * kY1 * 0.90f + sun_glow * s[1]; // Y
        out_sh9[2][ch] = sun_glow * s[2];                        // Z
        out_sh9[3][ch] = sun_glow * s[0];                        // X
        out_sh9[6][ch] = (horizon[ch] - 0.5f * (zenith[ch] + ground[ch])) * -kY20 * 0.85f;
        out_sh9[4][ch] = sun_glow * kY2m * 0.15f * s[0] * s[1];
        out_sh9[5][ch] = sun_glow * kY2m * 0.15f * s[1] * s[2];
        out_sh9[7][ch] = sun_glow * kY2m * 0.15f * s[0] * s[2];
        out_sh9[8][ch] = 0.0f;
    }
}

const char* studio_fixture_name(StudioFixtureProfile profile) noexcept {
    switch (profile) {
    case StudioFixtureProfile::RectSoftbox:
        return "Rectangular Softbox";
    case StudioFixtureProfile::Octabox:
        return "Octabox (8-Sided)";
    case StudioFixtureProfile::Stripbox:
        return "Vertical Rim Stripbox";
    case StudioFixtureProfile::ParabolicUmbrella:
        return "Parabolic / Umbrella";
    case StudioFixtureProfile::LanternChinaBall:
        return "Lantern / China Ball";
    case StudioFixtureProfile::ArriSkyPanel:
        return "ARRI SkyPanel S60-C";
    case StudioFixtureProfile::ArriFresnel:
        return "ARRI Fresnel T2/Orbiter";
    case StudioFixtureProfile::DedolightSpot:
        return "Dedolight DLED7 Spot";
    case StudioFixtureProfile::EllipsoidalLeko:
        return "Ellipsoidal / Leko (Gobo)";
    case StudioFixtureProfile::BroadcastRing:
        return "Broadcast Ring Light";
    case StudioFixtureProfile::LedTubeAstera:
        return "LED Tube (Astera Titan)";
    case StudioFixtureProfile::NaturalSunCieSky:
        return "Natural Sun + CIE Sky";
    }
    return "Rectangular Softbox";
}

const char* studio_gel_name(StudioGelPreset gel) noexcept {
    switch (gel) {
    case StudioGelPreset::None:
        return "None (Clear)";
    case StudioGelPreset::QuarterCto:
        return "1/4 CTO (Warm)";
    case StudioGelPreset::HalfCto:
        return "1/2 CTO (Amber)";
    case StudioGelPreset::FullCto:
        return "Full CTO (Tungsten)";
    case StudioGelPreset::QuarterCtb:
        return "1/4 CTB (Cool)";
    case StudioGelPreset::HalfCtb:
        return "1/2 CTB (Daylight)";
    case StudioGelPreset::FullCtb:
        return "Full CTB (Deep Blue)";
    case StudioGelPreset::PlusGreen:
        return "+Green (Fluorescent)";
    case StudioGelPreset::MinusGreen:
        return "-Green (Magenta Trim)";
    case StudioGelPreset::ShinkaiGoldenRose:
        return "Shinkai Golden Rose";
    case StudioGelPreset::CyberCyan:
        return "Cyber Cyan";
    case StudioGelPreset::StageMagenta:
        return "Stage Magenta";
    }
    return "None (Clear)";
}

} // namespace aether::renderer
