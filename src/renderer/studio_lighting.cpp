#include "renderer/studio_lighting.hpp"

#include "renderer/circadian_lighting.hpp"
#include "renderer/lighting_rig.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace aether::renderer {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

[[nodiscard]] std::array<float, 3> add3(std::array<float, 3> a, std::array<float, 3> b) noexcept {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

[[nodiscard]] std::array<float, 3> sub3(std::array<float, 3> a, std::array<float, 3> b) noexcept {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

[[nodiscard]] std::array<float, 3> mul3(std::array<float, 3> a, float s) noexcept {
    return {a[0] * s, a[1] * s, a[2] * s};
}

[[nodiscard]] float dot3(std::array<float, 3> a, std::array<float, 3> b) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

[[nodiscard]] float len3(std::array<float, 3> a) noexcept {
    return std::sqrt(std::max(0.0f, dot3(a, a)));
}

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

void set_name_buf(std::array<char, 40>& dst, const char* src) noexcept {
    dst.fill('\0');
    if (src != nullptr) {
        std::snprintf(dst.data(), dst.size(), "%s", src);
    }
}

void set_preset_name_buf(std::array<char, 64>& dst, const char* src) noexcept {
    dst.fill('\0');
    if (src != nullptr) {
        std::snprintf(dst.data(), dst.size(), "%s", src);
    }
}

inline void set4(float (&dst)[4], float a, float b, float c, float d) noexcept {
    dst[0] = a;
    dst[1] = b;
    dst[2] = c;
    dst[3] = d;
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

StudioLightWorldPose resolve_studio_light_world_pose(const StudioLightSpec& spec, const std::array<float, 3>& pivot_ws,
                                                     const std::array<float, 3>& eye_pos_ws) noexcept {
    StudioLightWorldPose pose{};
    pose.target_ws = spec.aim_lock ? pivot_ws : spec.target_ws;

    if (spec.coaxial_lock && spec.fixture == StudioFixtureProfile::BroadcastRing) {
        pose.position_ws = {eye_pos_ws[0], eye_pos_ws[1] + 0.04f, eye_pos_ws[2]};
        const std::array<float, 3> delta = sub3(pose.target_ws, pose.position_ws);
        pose.distance_m = std::max(len3(delta), 0.10f);
        pose.forward_ws = norm3(delta);
    } else if (spec.use_orbit_angles) {
        const std::array<float, 3> to_light = orbit_dir(spec.orbit_azimuth_deg, spec.orbit_elevation_deg);
        pose.distance_m = std::clamp(spec.orbit_distance_m, 0.25f, 20.0f);
        pose.position_ws = add3(pose.target_ws, mul3(to_light, pose.distance_m));
        pose.forward_ws = norm3({-to_light[0], -to_light[1], -to_light[2]});
    } else {
        pose.position_ws = spec.position_ws;
        const std::array<float, 3> delta = sub3(pose.target_ws, pose.position_ws);
        pose.distance_m = std::max(len3(delta), 0.10f);
        pose.forward_ws = norm3(delta);
    }

    const std::array<float, 3> world_up = (std::fabs(pose.forward_ws[1]) > 0.96f)
                                              ? std::array<float, 3>{0.0f, 0.0f, 1.0f}
                                              : std::array<float, 3>{0.0f, 1.0f, 0.0f};
    const std::array<float, 3> base_u = norm3(cross3(world_up, pose.forward_ws));
    const std::array<float, 3> base_v = norm3(cross3(pose.forward_ws, base_u));
    const float r_rad = spec.roll_deg * kDegToRad;
    const float cr = std::cos(r_rad);
    const float sr = std::sin(r_rad);
    pose.right_ws =
        norm3({base_u[0] * cr + base_v[0] * sr, base_u[1] * cr + base_v[1] * sr, base_u[2] * cr + base_v[2] * sr});
    pose.up_ws = norm3(cross3(pose.forward_ws, pose.right_ws));
    return pose;
}

void bake_studio_light_world_pose(StudioLightSpec& spec, const std::array<float, 3>& pivot_ws,
                                  const std::array<float, 3>& eye_pos_ws) noexcept {
    const StudioLightWorldPose pose = resolve_studio_light_world_pose(spec, pivot_ws, eye_pos_ws);
    spec.position_ws = pose.position_ws;
    spec.target_ws = pose.target_ws;
    spec.use_orbit_angles = false;
}

namespace {

uint32_t synthesize_legacy_studio_lights(const LightingRig& rig, const std::array<float, 3>& pivot_ws,
                                         std::array<StudioLightSpec, kMaxStudioLights>& out_specs) noexcept {
    const uint32_t n = std::min<uint32_t>(rig.active_count, LightingRig::kMaxLights);
    const StudioFixtureProfile key_profile =
        (rig.key_studio_profile_override >= 0 && rig.key_studio_profile_override <= 11)
            ? static_cast<StudioFixtureProfile>(rig.key_studio_profile_override)
            : legacy_to_studio_fixture(rig.fixture_preset);
    uint32_t count = 0;
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
            set_name_buf(spec.name, "Key Light");
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
            set_name_buf(spec.name, "Soft Fill");
            configure_fixture_defaults(spec, StudioFixtureProfile::LanternChinaBall);
            spec.catchlight_weight = 0.25f;
        } else if (sl.role == LightRole::Rim) {
            set_name_buf(spec.name, "Rim Light");
            configure_fixture_defaults(spec, StudioFixtureProfile::Stripbox);
            spec.catchlight_weight = 0.40f;
        } else if (sl.role == LightRole::HairOrbit) {
            set_name_buf(spec.name, "Hair Orbit");
            configure_fixture_defaults(spec, StudioFixtureProfile::Stripbox);
            spec.catchlight_weight = 0.35f;
        } else {
            char label[40]{};
            std::snprintf(label, sizeof(label), "Light %u", i + 1u);
            set_name_buf(spec.name, label);
            configure_fixture_defaults(spec, StudioFixtureProfile::RectSoftbox);
        }
        out_specs[count++] = spec;
    }
    return count;
}

} // namespace

uint32_t pack_studio_lights_gpu(const LightingRig& rig, const std::array<float, 3>& pivot_ws,
                                const std::array<float, 3>& eye_pos_ws,
                                std::array<StudioLightGPU, kMaxStudioLights>& out_gpu) noexcept {
    for (auto& g : out_gpu) {
        g = StudioLightGPU{};
    }

    auto pack_one = [&](const StudioLightSpec& s, uint32_t dst_idx) noexcept {
        StudioLightGPU& g = out_gpu[dst_idx];
        const StudioLightWorldPose pose = resolve_studio_light_world_pose(s, pivot_ws, eye_pos_ws);
        const std::array<float, 3>& pos = pose.position_ws;
        const std::array<float, 3>& light_to_scene_dir = pose.forward_ws;
        const std::array<float, 3>& tangent_u = pose.right_ws;

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
    std::array<StudioLightSpec, kMaxStudioLights> synthesized{};
    const uint32_t synth_count = synthesize_legacy_studio_lights(rig, pivot_ws, synthesized);
    for (uint32_t i = 0; i < synth_count && count < kMaxStudioLights; ++i) {
        pack_one(synthesized[i], count++);
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

const char* passive_flag_name(PassiveFlagMode mode) noexcept {
    switch (mode) {
    case PassiveFlagMode::None:
        return "None";
    case PassiveFlagMode::NegativeFillBlack:
        return "Negative Fill Flag (Black)";
    case PassiveFlagMode::BounceWhite:
        return "Bounce Card (White)";
    case PassiveFlagMode::BounceSilver:
        return "Bounce Card (Silver)";
    case PassiveFlagMode::BounceGold:
        return "Bounce Card (Gold)";
    }
    return "None";
}

const char* studio_gobo_name(StudioGoboPattern gobo) noexcept {
    switch (gobo) {
    case StudioGoboPattern::None:
        return "None";
    case StudioGoboPattern::WindowBlinds:
        return "Window Blinds";
    case StudioGoboPattern::AnimeClassroomSlats:
        return "Anime Classroom Slats";
    case StudioGoboPattern::LeafCanopy:
        return "Leaf Canopy";
    case StudioGoboPattern::StageCircleIris:
        return "Stage Circle Iris";
    }
    return "None";
}

void ensure_studio_lights_initialized(LightingRig& rig, const std::array<float, 3>& pivot_ws) noexcept {
    if (rig.use_studio_lights && rig.studio_light_count > 0u) {
        rig.studio_light_count = std::min(rig.studio_light_count, kMaxStudioLights);
        return;
    }
    rig.studio_light_count = synthesize_legacy_studio_lights(rig, pivot_ws, rig.studio_lights);
    rig.use_studio_lights = true;
}

int add_studio_light(LightingRig& rig, StudioFixtureProfile profile, const std::array<float, 3>& pivot_ws) noexcept {
    ensure_studio_lights_initialized(rig, pivot_ws);
    if (rig.studio_light_count >= kMaxStudioLights) {
        return -1;
    }
    const uint32_t idx = rig.studio_light_count++;
    StudioLightSpec& spec = rig.studio_lights[idx];
    spec = StudioLightSpec{};
    configure_fixture_defaults(spec, profile);
    spec.target_ws = pivot_ws;
    spec.aim_lock = true;
    spec.use_orbit_angles = true;
    spec.orbit_azimuth_deg = -40.0f + 35.0f * static_cast<float>((idx + 1u) % 6u);
    spec.orbit_elevation_deg = 22.0f + 6.0f * static_cast<float>(idx % 3u);
    spec.orbit_distance_m = (profile == StudioFixtureProfile::NaturalSunCieSky) ? 3.20f : 2.15f;
    spec.intensity_ev100 = 0.75f;
    if (profile == StudioFixtureProfile::BroadcastRing) {
        spec.coaxial_lock = true;
    }
    char label[40]{};
    std::snprintf(label, sizeof(label), "%s %u", studio_fixture_name(profile), idx + 1u);
    set_name_buf(spec.name, label);
    return static_cast<int>(idx);
}

int add_passive_flag(LightingRig& rig, PassiveFlagMode mode, const std::array<float, 3>& pivot_ws) noexcept {
    ensure_studio_lights_initialized(rig, pivot_ws);
    if (rig.studio_light_count >= kMaxStudioLights) {
        return -1;
    }
    const PassiveFlagMode safe_mode = (mode == PassiveFlagMode::None) ? PassiveFlagMode::NegativeFillBlack : mode;
    const uint32_t idx = rig.studio_light_count++;
    StudioLightSpec& spec = rig.studio_lights[idx];
    spec = StudioLightSpec{};
    spec.fixture = StudioFixtureProfile::RectSoftbox;
    spec.passive_flag = safe_mode;
    spec.enabled = true;
    spec.cast_shadow = false;
    spec.aim_lock = true;
    spec.use_orbit_angles = true;
    spec.orbit_azimuth_deg = (safe_mode == PassiveFlagMode::NegativeFillBlack) ? -72.0f : 58.0f;
    spec.orbit_elevation_deg = (safe_mode == PassiveFlagMode::NegativeFillBlack) ? 8.0f : -14.0f;
    spec.orbit_distance_m = 1.45f;
    spec.target_ws = pivot_ws;
    spec.width_or_outer_m = 0.85f;
    spec.height_or_inner_m = 1.25f;
    spec.intensity_ev100 = 0.0f;
    spec.catchlight_weight = 0.0f;
    spec.softbox_enabled = true;
    set_name_buf(spec.name, passive_flag_name(safe_mode));
    return static_cast<int>(idx);
}

int duplicate_studio_light(LightingRig& rig, uint32_t index) noexcept {
    if (!rig.use_studio_lights) {
        ensure_studio_lights_initialized(rig);
    }
    if (index >= rig.studio_light_count || rig.studio_light_count >= kMaxStudioLights) {
        return -1;
    }
    const uint32_t dst = rig.studio_light_count++;
    rig.studio_lights[dst] = rig.studio_lights[index];
    rig.studio_lights[dst].solo = false;
    rig.studio_lights[dst].orbit_azimuth_deg += 18.0f;
    rig.studio_lights[dst].position_ws[0] += 0.25f;
    char label[40]{};
    std::snprintf(label, sizeof(label), "%.*s Copy", 32, rig.studio_lights[index].name.data());
    set_name_buf(rig.studio_lights[dst].name, label);
    return static_cast<int>(dst);
}

bool remove_studio_light(LightingRig& rig, uint32_t index) noexcept {
    if (!rig.use_studio_lights) {
        ensure_studio_lights_initialized(rig);
    }
    if (index >= rig.studio_light_count || rig.studio_light_count <= 1u) {
        return false;
    }
    for (uint32_t i = index; i + 1u < rig.studio_light_count; ++i) {
        rig.studio_lights[i] = rig.studio_lights[i + 1u];
    }
    rig.studio_lights[rig.studio_light_count - 1u] = StudioLightSpec{};
    --rig.studio_light_count;
    return true;
}

const char* studio_rig_preset_name(StudioRigPresetId id) noexcept {
    switch (id) {
    case StudioRigPresetId::ThreePointBroadcast:
        return "Three-Point Broadcast";
    case StudioRigPresetId::DramaticRimSplit:
        return "Dramatic Rim Split";
    case StudioRigPresetId::ShinkaiGoldenHour:
        return "Shinkai Golden Hour";
    case StudioRigPresetId::HighKeySoftBeauty:
        return "High-Key Soft Beauty";
    case StudioRigPresetId::NeonCyberpunkDualColor:
        return "Neon Cyberpunk Dual-Color";
    }
    return "Three-Point Broadcast";
}

void apply_studio_rig_preset(LightingRig& rig, StudioRigPresetId id, const std::array<float, 3>& pivot_ws) noexcept {
    rig.studio_lights = {};
    rig.use_studio_lights = true;

    auto init_slot = [&](uint32_t i, const char* name, StudioFixtureProfile prof, float az, float el, float dist,
                         float ev, float cct, bool shadow) -> StudioLightSpec& {
        StudioLightSpec& s = rig.studio_lights[i];
        s = StudioLightSpec{};
        configure_fixture_defaults(s, prof);
        set_name_buf(s.name, name);
        s.target_ws = pivot_ws;
        s.aim_lock = true;
        s.use_orbit_angles = true;
        s.orbit_azimuth_deg = az;
        s.orbit_elevation_deg = el;
        s.orbit_distance_m = dist;
        s.intensity_ev100 = ev;
        s.kelvin_cct = cct;
        s.cast_shadow = shadow;
        return s;
    };

    switch (id) {
    case StudioRigPresetId::ThreePointBroadcast: {
        rig.studio_light_count = 4;
        rig.lighting_mode = 1;
        rig.global_live2d_flatness = 0.75f;
        rig.split_grading = SplitLayerGradingParams{};
        rig.satsuei = SatsueiCompositeParams{};
        init_slot(0, "Key SkyPanel", StudioFixtureProfile::ArriSkyPanel, 32.0f, 26.0f, 2.15f, 1.00f, 5600.0f, true);
        auto& fill = init_slot(1, "Soft Fill Ball", StudioFixtureProfile::LanternChinaBall, -52.0f, 12.0f, 2.10f, 0.26f,
                               6200.0f, false);
        fill.catchlight_weight = 0.25f;
        auto& rim =
            init_slot(2, "Edge Rim Strip", StudioFixtureProfile::Stripbox, 148.0f, 22.0f, 2.05f, 0.34f, 6500.0f, false);
        rim.catchlight_weight = 0.40f;
        auto& hair = init_slot(3, "Hair Angel Strip", StudioFixtureProfile::Stripbox, 10.0f, 52.0f, 1.95f, 0.85f,
                               5800.0f, false);
        hair.roll_deg = 90.0f;
        hair.catchlight_weight = 0.35f;
        break;
    }
    case StudioRigPresetId::DramaticRimSplit: {
        rig.studio_light_count = 3;
        rig.lighting_mode = 2;
        rig.global_live2d_flatness = 0.62f;
        rig.split_grading.avatar_contrast = 1.12f;
        rig.satsuei.light_wrap_strength = 0.16f;
        auto& key = init_slot(0, "Fresnel Split Key", StudioFixtureProfile::ArriFresnel, 68.0f, 30.0f, 2.30f, 1.18f,
                              4800.0f, true);
        key.grid_enabled = true;
        key.grid_angle_deg = 30.0f;
        key.barn_doors_enabled = true;
        key.barn_door_cuts_deg = {28.0f, 28.0f, 32.0f, 32.0f};
        auto& nfill = init_slot(1, "Negative Fill Black", StudioFixtureProfile::RectSoftbox, -78.0f, 10.0f, 1.40f, 0.0f,
                                5600.0f, false);
        nfill.passive_flag = PassiveFlagMode::NegativeFillBlack;
        nfill.width_or_outer_m = 0.90f;
        nfill.height_or_inner_m = 1.40f;
        nfill.catchlight_weight = 0.0f;
        auto& rim = init_slot(2, "Dedolight Rim", StudioFixtureProfile::DedolightSpot, -145.0f, 32.0f, 2.40f, 0.96f,
                              5600.0f, true);
        rim.spot_outer_deg = 16.0f;
        rim.spot_inner_deg = 9.0f;
        break;
    }
    case StudioRigPresetId::ShinkaiGoldenHour: {
        rig.studio_light_count = 4;
        rig.lighting_mode = 2;
        rig.global_live2d_flatness = 0.72f;
        rig.satsuei.light_wrap_strength = 0.32f;
        rig.satsuei.halation_strength = 0.28f;
        rig.satsuei.backscatter_strength = 0.72f;
        auto& sun = init_slot(0, "Golden Sun CIE", StudioFixtureProfile::NaturalSunCieSky, 48.0f, 16.0f, 3.30f, 1.12f,
                              3800.0f, true);
        sun.gel_enabled = true;
        sun.gel = StudioGelPreset::ShinkaiGoldenRose;
        auto& portal = init_slot(1, "Classroom Window", StudioFixtureProfile::RectSoftbox, -68.0f, 22.0f, 2.20f, 0.65f,
                                 4300.0f, true);
        portal.window_portal = true;
        portal.gobo = StudioGoboPattern::AnimeClassroomSlats;
        portal.width_or_outer_m = 1.20f;
        portal.height_or_inner_m = 1.80f;
        auto& skyfill = init_slot(2, "Cool Sky Bounce", StudioFixtureProfile::ArriSkyPanel, -25.0f, 10.0f, 2.20f, 0.24f,
                                  7800.0f, false);
        skyfill.catchlight_weight = 0.25f;
        auto& rim = init_slot(3, "Sunset Warm Rim", StudioFixtureProfile::Stripbox, 152.0f, 20.0f, 2.00f, 0.58f,
                              4200.0f, false);
        rim.gel_enabled = true;
        rim.gel = StudioGelPreset::HalfCto;
        break;
    }
    case StudioRigPresetId::HighKeySoftBeauty: {
        rig.studio_light_count = 4;
        rig.lighting_mode = 1;
        rig.global_live2d_flatness = 0.84f;
        rig.split_grading.avatar_contrast = 0.98f;
        rig.satsuei.light_wrap_strength = 0.24f;
        auto& ring = init_slot(0, "Coaxial Beauty Ring", StudioFixtureProfile::BroadcastRing, 0.0f, 6.0f, 1.60f, 0.74f,
                               5600.0f, false);
        ring.coaxial_lock = true;
        ring.catchlight_weight = 1.20f;
        auto& octa =
            init_slot(1, "Overhead Octabox", StudioFixtureProfile::Octabox, 12.0f, 42.0f, 2.05f, 0.86f, 5600.0f, true);
        octa.diffusion_enabled = true;
        octa.diffusion_density = 1.0f;
        auto& bounce = init_slot(2, "White Chin Bounce", StudioFixtureProfile::RectSoftbox, 0.0f, -22.0f, 1.25f, 0.0f,
                                 5600.0f, false);
        bounce.passive_flag = PassiveFlagMode::BounceWhite;
        bounce.width_or_outer_m = 0.80f;
        bounce.height_or_inner_m = 0.55f;
        bounce.catchlight_weight = 0.0f;
        init_slot(3, "Soft Hair Halo", StudioFixtureProfile::Stripbox, 165.0f, 38.0f, 1.95f, 0.45f, 6000.0f, false);
        break;
    }
    case StudioRigPresetId::NeonCyberpunkDualColor: {
        rig.studio_light_count = 4;
        rig.lighting_mode = 2;
        rig.global_live2d_flatness = 0.68f;
        rig.split_grading.avatar_saturation = 1.12f;
        rig.satsuei.halation_strength = 0.30f;
        auto& cyan = init_slot(0, "Cyber Cyan Tube", StudioFixtureProfile::LedTubeAstera, -58.0f, 18.0f, 1.85f, 1.05f,
                               6800.0f, true);
        cyan.gel_enabled = true;
        cyan.gel = StudioGelPreset::CyberCyan;
        cyan.roll_deg = 15.0f;
        auto& mag = init_slot(1, "Stage Magenta Rim", StudioFixtureProfile::Stripbox, 122.0f, 20.0f, 1.95f, 0.96f,
                              5600.0f, true);
        mag.gel_enabled = true;
        mag.gel = StudioGelPreset::StageMagenta;
        auto& key = init_slot(2, "Face Anchor Spot", StudioFixtureProfile::ArriFresnel, 18.0f, 22.0f, 2.25f, 0.42f,
                              5200.0f, false);
        key.grid_enabled = true;
        key.grid_angle_deg = 30.0f;
        auto& top = init_slot(3, "Overhead Cyan Tube", StudioFixtureProfile::LedTubeAstera, 168.0f, 48.0f, 1.90f, 0.55f,
                              6500.0f, false);
        top.gel_enabled = true;
        top.gel = StudioGelPreset::CyberCyan;
        top.roll_deg = 90.0f;
        break;
    }
    }
}

void builtin_studio_rig_presets(std::array<StudioRigPresetRecord, 5>& out_presets,
                                const std::array<float, 3>& pivot_ws) noexcept {
    constexpr std::array<StudioRigPresetId, 5> kIds{
        StudioRigPresetId::ThreePointBroadcast,    StudioRigPresetId::DramaticRimSplit,
        StudioRigPresetId::ShinkaiGoldenHour,      StudioRigPresetId::HighKeySoftBeauty,
        StudioRigPresetId::NeonCyberpunkDualColor,
    };
    for (size_t i = 0; i < kIds.size(); ++i) {
        out_presets[i].rig = LightingRig::default_three_point();
        set_preset_name_buf(out_presets[i].name, studio_rig_preset_name(kIds[i]));
        apply_studio_rig_preset(out_presets[i].rig, kIds[i], pivot_ws);
    }
}

namespace {

[[nodiscard]] nlohmann::json serialize_studio_light(const StudioLightSpec& s) {
    return nlohmann::json{
        {"name", std::string(s.name.data())},
        {"fixture", static_cast<int>(s.fixture)},
        {"enabled", s.enabled},
        {"solo", s.solo},
        {"cast_shadow", s.cast_shadow},
        {"aim_lock", s.aim_lock},
        {"use_orbit_angles", s.use_orbit_angles},
        {"orbit_azimuth_deg", s.orbit_azimuth_deg},
        {"orbit_elevation_deg", s.orbit_elevation_deg},
        {"orbit_distance_m", s.orbit_distance_m},
        {"roll_deg", s.roll_deg},
        {"position_ws", {s.position_ws[0], s.position_ws[1], s.position_ws[2]}},
        {"target_ws", {s.target_ws[0], s.target_ws[1], s.target_ws[2]}},
        {"intensity_ev100", s.intensity_ev100},
        {"exposure_stops", s.exposure_stops},
        {"kelvin_cct", s.kelvin_cct},
        {"tint_rgb", {s.tint_rgb[0], s.tint_rgb[1], s.tint_rgb[2]}},
        {"green_magenta", s.green_magenta},
        {"channel_mask", s.channel_mask},
        {"catchlight_weight", s.catchlight_weight},
        {"shadow_bias_scale", s.shadow_bias_scale},
        {"width_or_outer_m", s.width_or_outer_m},
        {"height_or_inner_m", s.height_or_inner_m},
        {"spot_inner_deg", s.spot_inner_deg},
        {"spot_outer_deg", s.spot_outer_deg},
        {"softbox_enabled", s.softbox_enabled},
        {"grid_enabled", s.grid_enabled},
        {"grid_angle_deg", s.grid_angle_deg},
        {"barn_doors_enabled", s.barn_doors_enabled},
        {"barn_door_cuts_deg",
         {s.barn_door_cuts_deg[0], s.barn_door_cuts_deg[1], s.barn_door_cuts_deg[2], s.barn_door_cuts_deg[3]}},
        {"diffusion_enabled", s.diffusion_enabled},
        {"diffusion_density", s.diffusion_density},
        {"gel_enabled", s.gel_enabled},
        {"gel", static_cast<int>(s.gel)},
        {"coaxial_lock", s.coaxial_lock},
        {"window_portal", s.window_portal},
        {"gobo", static_cast<int>(s.gobo)},
        {"ies_profile", static_cast<int>(s.ies_profile)},
        {"passive_flag", static_cast<int>(s.passive_flag)},
    };
}

void deserialize_studio_light(const nlohmann::json& j, StudioLightSpec& s) {
    s = StudioLightSpec{};
    if (const auto it = j.find("name"); it != j.end() && it->is_string()) {
        set_name_buf(s.name, it->get<std::string>().c_str());
    }
    const int fix_raw = std::clamp(j.value("fixture", 0), 0, 11);
    s.fixture = static_cast<StudioFixtureProfile>(fix_raw);
    configure_fixture_defaults(s, s.fixture);
    s.enabled = j.value("enabled", s.enabled);
    s.solo = j.value("solo", s.solo);
    s.cast_shadow = j.value("cast_shadow", s.cast_shadow);
    s.aim_lock = j.value("aim_lock", s.aim_lock);
    s.use_orbit_angles = j.value("use_orbit_angles", s.use_orbit_angles);
    s.orbit_azimuth_deg = j.value("orbit_azimuth_deg", s.orbit_azimuth_deg);
    s.orbit_elevation_deg = j.value("orbit_elevation_deg", s.orbit_elevation_deg);
    s.orbit_distance_m = j.value("orbit_distance_m", s.orbit_distance_m);
    s.roll_deg = j.value("roll_deg", s.roll_deg);
    if (const auto it = j.find("position_ws"); it != j.end() && it->is_array() && it->size() == 3u) {
        for (size_t k = 0; k < 3u; ++k) {
            s.position_ws[k] = (*it)[k].get<float>();
        }
    }
    if (const auto it = j.find("target_ws"); it != j.end() && it->is_array() && it->size() == 3u) {
        for (size_t k = 0; k < 3u; ++k) {
            s.target_ws[k] = (*it)[k].get<float>();
        }
    }
    s.intensity_ev100 = j.value("intensity_ev100", s.intensity_ev100);
    s.exposure_stops = j.value("exposure_stops", s.exposure_stops);
    s.kelvin_cct = j.value("kelvin_cct", s.kelvin_cct);
    if (const auto it = j.find("tint_rgb"); it != j.end() && it->is_array() && it->size() == 3u) {
        for (size_t k = 0; k < 3u; ++k) {
            s.tint_rgb[k] = (*it)[k].get<float>();
        }
    }
    s.green_magenta = j.value("green_magenta", s.green_magenta);
    s.channel_mask = j.value("channel_mask", s.channel_mask);
    s.catchlight_weight = j.value("catchlight_weight", s.catchlight_weight);
    s.shadow_bias_scale = j.value("shadow_bias_scale", s.shadow_bias_scale);
    s.width_or_outer_m = j.value("width_or_outer_m", s.width_or_outer_m);
    s.height_or_inner_m = j.value("height_or_inner_m", s.height_or_inner_m);
    s.spot_inner_deg = j.value("spot_inner_deg", s.spot_inner_deg);
    s.spot_outer_deg = j.value("spot_outer_deg", s.spot_outer_deg);
    s.softbox_enabled = j.value("softbox_enabled", s.softbox_enabled);
    s.grid_enabled = j.value("grid_enabled", s.grid_enabled);
    s.grid_angle_deg = j.value("grid_angle_deg", s.grid_angle_deg);
    s.barn_doors_enabled = j.value("barn_doors_enabled", s.barn_doors_enabled);
    if (const auto it = j.find("barn_door_cuts_deg"); it != j.end() && it->is_array() && it->size() == 4u) {
        for (size_t k = 0; k < 4u; ++k) {
            s.barn_door_cuts_deg[k] = (*it)[k].get<float>();
        }
    }
    s.diffusion_enabled = j.value("diffusion_enabled", s.diffusion_enabled);
    s.diffusion_density = j.value("diffusion_density", s.diffusion_density);
    s.gel_enabled = j.value("gel_enabled", s.gel_enabled);
    s.gel = static_cast<StudioGelPreset>(std::clamp(j.value("gel", 0), 0, 11));
    s.coaxial_lock = j.value("coaxial_lock", s.coaxial_lock);
    s.window_portal = j.value("window_portal", s.window_portal);
    s.gobo = static_cast<StudioGoboPattern>(std::clamp(j.value("gobo", 0), 0, 4));
    s.ies_profile = static_cast<int8_t>(std::clamp(j.value("ies_profile", -1), -1, 7));
    s.passive_flag = static_cast<PassiveFlagMode>(std::clamp(j.value("passive_flag", 0), 0, 4));
}

} // namespace

bool load_lighting_rigs_json(const std::filesystem::path& path,
                             std::vector<StudioRigPresetRecord>& out_presets) noexcept {
    try {
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            return false;
        }
        const nlohmann::json root = nlohmann::json::parse(ifs);
        const auto it_presets = root.find("presets");
        if (it_presets == root.end() || !it_presets->is_array()) {
            return false;
        }
        std::vector<StudioRigPresetRecord> parsed;
        parsed.reserve(it_presets->size());
        for (const auto& pj : *it_presets) {
            if (!pj.is_object()) {
                continue;
            }
            StudioRigPresetRecord rec{};
            rec.rig = LightingRig::default_three_point();
            rec.rig.use_studio_lights = true;
            set_preset_name_buf(rec.name, pj.value("name", std::string("Untitled Rig")).c_str());
            rec.rig.lighting_mode = static_cast<uint8_t>(std::clamp(pj.value("lighting_mode", 1), 0, 2));
            rec.rig.global_live2d_flatness = pj.value("global_live2d_flatness", rec.rig.global_live2d_flatness);
            rec.rig.eye_catchlight_intensity = pj.value("eye_catchlight_intensity", rec.rig.eye_catchlight_intensity);
            rec.rig.eye_catchlight_size = pj.value("eye_catchlight_size", rec.rig.eye_catchlight_size);
            if (const auto sg = pj.find("split_grading"); sg != pj.end() && sg->is_object()) {
                rec.rig.split_grading.avatar_contrast = sg->value("avatar_contrast", 1.04f);
                rec.rig.split_grading.avatar_saturation = sg->value("avatar_saturation", 1.05f);
                rec.rig.split_grading.avatar_warmth = sg->value("avatar_warmth", 0.0f);
                rec.rig.split_grading.avatar_ev_bias = sg->value("avatar_ev_bias", 0.0f);
                rec.rig.split_grading.bg_contrast = sg->value("bg_contrast", 1.02f);
                rec.rig.split_grading.bg_saturation = sg->value("bg_saturation", 0.96f);
                rec.rig.split_grading.bg_exposure = sg->value("bg_exposure", 1.00f);
                rec.rig.split_grading.master_grade_strength = sg->value("master_grade_strength", 1.0f);
            }
            if (const auto st = pj.find("satsuei"); st != pj.end() && st->is_object()) {
                rec.rig.satsuei.light_wrap_strength = st->value("light_wrap_strength", 0.22f);
                rec.rig.satsuei.halation_strength = st->value("halation_strength", 0.18f);
                rec.rig.satsuei.halation_threshold = st->value("halation_threshold", 0.95f);
                rec.rig.satsuei.backscatter_strength = st->value("backscatter_strength", 0.55f);
            }
            rec.rig.studio_light_count = 0u;
            if (const auto lj = pj.find("lights"); lj != pj.end() && lj->is_array()) {
                for (const auto& item : *lj) {
                    if (rec.rig.studio_light_count >= kMaxStudioLights) {
                        break;
                    }
                    if (item.is_object()) {
                        deserialize_studio_light(item, rec.rig.studio_lights[rec.rig.studio_light_count++]);
                    }
                }
            }
            parsed.push_back(rec);
        }
        if (parsed.empty()) {
            return false;
        }
        out_presets = std::move(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool save_lighting_rigs_json(const std::filesystem::path& path,
                             std::span<const StudioRigPresetRecord> presets) noexcept {
    try {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& rec : presets) {
            LightingRig copy = rec.rig;
            ensure_studio_lights_initialized(copy);
            nlohmann::json lights = nlohmann::json::array();
            const uint32_t count = std::min(copy.studio_light_count, kMaxStudioLights);
            for (uint32_t i = 0; i < count; ++i) {
                lights.push_back(serialize_studio_light(copy.studio_lights[i]));
            }
            arr.push_back(nlohmann::json{
                {"name", std::string(rec.name.data())},
                {"lighting_mode", static_cast<int>(copy.lighting_mode)},
                {"global_live2d_flatness", copy.global_live2d_flatness},
                {"eye_catchlight_intensity", copy.eye_catchlight_intensity},
                {"eye_catchlight_size", copy.eye_catchlight_size},
                {"split_grading",
                 {{"avatar_contrast", copy.split_grading.avatar_contrast},
                  {"avatar_saturation", copy.split_grading.avatar_saturation},
                  {"avatar_warmth", copy.split_grading.avatar_warmth},
                  {"avatar_ev_bias", copy.split_grading.avatar_ev_bias},
                  {"bg_contrast", copy.split_grading.bg_contrast},
                  {"bg_saturation", copy.split_grading.bg_saturation},
                  {"bg_exposure", copy.split_grading.bg_exposure},
                  {"master_grade_strength", copy.split_grading.master_grade_strength}}},
                {"satsuei",
                 {{"light_wrap_strength", copy.satsuei.light_wrap_strength},
                  {"halation_strength", copy.satsuei.halation_strength},
                  {"halation_threshold", copy.satsuei.halation_threshold},
                  {"backscatter_strength", copy.satsuei.backscatter_strength}}},
                {"lights", lights},
            });
        }
        std::error_code ec;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        const nlohmann::json root{{"version", 1}, {"presets", arr}};
        std::ofstream ofs(path);
        if (!ofs.is_open()) {
            return false;
        }
        ofs << root.dump(2) << '\n';
        return ofs.good();
    } catch (...) {
        return false;
    }
}

bool save_or_update_lighting_rig_preset(const std::filesystem::path& path, const char* preset_name,
                                        const LightingRig& rig) noexcept {
    try {
        std::vector<StudioRigPresetRecord> list;
        if (!load_lighting_rigs_json(path, list)) {
            std::array<StudioRigPresetRecord, 5> builtins{};
            builtin_studio_rig_presets(builtins);
            list.assign(builtins.begin(), builtins.end());
        }
        const char* safe_name = (preset_name != nullptr && preset_name[0] != '\0') ? preset_name : "Custom Rig";
        bool updated = false;
        for (auto& rec : list) {
            if (std::strncmp(rec.name.data(), safe_name, rec.name.size()) == 0) {
                rec.rig = rig;
                updated = true;
                break;
            }
        }
        if (!updated) {
            StudioRigPresetRecord entry{};
            set_preset_name_buf(entry.name, safe_name);
            entry.rig = rig;
            list.push_back(entry);
        }
        return save_lighting_rigs_json(path, list);
    } catch (...) {
        return false;
    }
}

PreviewCamera make_stage_camera_preset(StageCameraView view, const std::array<float, 3>& pivot_ws,
                                       float current_ortho_height) noexcept {
    PreviewCamera cam{};
    cam.target_x = pivot_ws[0];
    cam.target_y = pivot_ws[1];
    cam.target_z = pivot_ws[2];
    cam.fovy_rad = 42.0f * kDegToRad;
    cam.ortho_height = std::clamp(current_ortho_height, 0.35f, 12.0f);
    switch (view) {
    case StageCameraView::FreePerspective:
        cam.is_orthographic = false;
        cam.yaw_rad = 32.0f * kDegToRad;
        cam.pitch_rad = 20.0f * kDegToRad;
        cam.distance = 3.20f;
        break;
    case StageCameraView::OrthoFront:
        cam.is_orthographic = true;
        cam.yaw_rad = 0.0f;
        cam.pitch_rad = 0.0f;
        cam.distance = 4.50f;
        break;
    case StageCameraView::OrthoBack:
        cam.is_orthographic = true;
        cam.yaw_rad = kPi;
        cam.pitch_rad = 0.0f;
        cam.distance = 4.50f;
        break;
    case StageCameraView::OrthoRight:
        cam.is_orthographic = true;
        cam.yaw_rad = 0.5f * kPi;
        cam.pitch_rad = 0.0f;
        cam.distance = 4.50f;
        break;
    case StageCameraView::OrthoLeft:
        cam.is_orthographic = true;
        cam.yaw_rad = -0.5f * kPi;
        cam.pitch_rad = 0.0f;
        cam.distance = 4.50f;
        break;
    case StageCameraView::OrthoTop:
        cam.is_orthographic = true;
        cam.yaw_rad = 0.0f;
        cam.pitch_rad = 0.5f * kPi;
        cam.distance = 5.50f;
        break;
    case StageCameraView::OrthoBottom:
        cam.is_orthographic = true;
        cam.yaw_rad = 0.0f;
        cam.pitch_rad = -0.5f * kPi;
        cam.distance = 5.50f;
        break;
    }
    return cam;
}

namespace {

struct StageCamBasis {
    std::array<float, 3> eye{};
    std::array<float, 3> fwd{};
    std::array<float, 3> right{};
    std::array<float, 3> up{};
};

[[nodiscard]] StageCamBasis eval_stage_cam_basis(const PreviewCamera& cam) noexcept {
    const float cp = std::cos(cam.pitch_rad);
    const float sp = std::sin(cam.pitch_rad);
    const float cy = std::cos(cam.yaw_rad);
    const float sy = std::sin(cam.yaw_rad);
    const std::array<float, 3> target{cam.target_x, cam.target_y, cam.target_z};
    const std::array<float, 3> eye{
        target[0] + cam.distance * cp * sy,
        target[1] + cam.distance * sp,
        target[2] + cam.distance * cp * cy,
    };
    const std::array<float, 3> fwd = norm3(sub3(target, eye));
    std::array<float, 3> up_ref{0.0f, 1.0f, 0.0f};
    if (std::fabs(fwd[1]) > 0.985f) {
        const float sign_y = (fwd[1] < 0.0f) ? -1.0f : 1.0f;
        up_ref = {sign_y * sy, 0.0f, sign_y * cy};
    }
    const std::array<float, 3> right = norm3(cross3(fwd, up_ref));
    const std::array<float, 3> up = cross3(right, fwd);
    return {eye, fwd, right, up};
}

} // namespace

StagePickRay compute_stage_pick_ray(const PreviewCamera& cam, float u_01, float v_01, float aspect) noexcept {
    const StageCamBasis b = eval_stage_cam_basis(cam);
    const float ndc_x = u_01 * 2.0f - 1.0f;
    const float ndc_y_up = 1.0f - 2.0f * v_01;
    const float safe_aspect = std::max(aspect, 0.1f);

    StagePickRay ray{};
    ray.is_orthographic = cam.is_orthographic;
    ray.ortho_height = cam.ortho_height;
    if (cam.is_orthographic) {
        const float half_h = std::max(0.05f, cam.ortho_height * 0.5f);
        const float half_w = half_h * safe_aspect;
        ray.origin_ws = add3(b.eye, add3(mul3(b.right, ndc_x * half_w), mul3(b.up, ndc_y_up * half_h)));
        ray.dir_ws = b.fwd;
    } else {
        const float fovy = std::clamp(cam.fovy_rad, 0.1f, 2.5f);
        const float tan_half = std::tan(fovy * 0.5f);
        const std::array<float, 3> dir =
            add3(b.fwd, add3(mul3(b.right, ndc_x * tan_half * safe_aspect), mul3(b.up, ndc_y_up * tan_half)));
        ray.origin_ws = b.eye;
        ray.dir_ws = norm3(dir);
    }
    return ray;
}

int pick_stage_studio_light(const LightingRig& rig, const std::array<float, 3>& pivot_ws,
                            const std::array<float, 3>& eye_pos_ws, const PreviewCamera& stage_cam, float u_01,
                            float v_01, float aspect, float* out_hit_t) noexcept {
    const StagePickRay ray = compute_stage_pick_ray(stage_cam, u_01, v_01, aspect);
    uint32_t count = rig.studio_light_count;
    std::array<StudioLightSpec, kMaxStudioLights> scratch{};
    const StudioLightSpec* list = rig.studio_lights.data();
    if (!rig.use_studio_lights || count == 0u) {
        count = synthesize_legacy_studio_lights(rig, pivot_ws, scratch);
        list = scratch.data();
    } else {
        count = std::min(count, kMaxStudioLights);
    }

    int best_idx = -1;
    float best_t = 1e9f;
    for (uint32_t i = 0; i < count; ++i) {
        const StudioLightSpec& s = list[i];
        const StudioLightWorldPose pose = resolve_studio_light_world_pose(s, pivot_ws, eye_pos_ws);
        const float radius = std::clamp(std::max(s.width_or_outer_m, s.height_or_inner_m) * 0.55f, 0.16f, 0.90f);
        const std::array<float, 3> oc = sub3(pose.position_ws, ray.origin_ws);
        const float t = dot3(oc, ray.dir_ws);
        if (t <= 0.05f) {
            continue;
        }
        const std::array<float, 3> closest = add3(ray.origin_ws, mul3(ray.dir_ws, t));
        const float dist_to_center = len3(sub3(pose.position_ws, closest));
        if (dist_to_center <= radius && t < best_t) {
            best_t = t;
            best_idx = static_cast<int>(i);
        }
    }
    if (out_hit_t != nullptr) {
        *out_hit_t = best_t;
    }
    return best_idx;
}

float stage_gizmo_world_scale(const PreviewCamera& cam, const std::array<float, 3>& world_pos, float target_px,
                              float viewport_h_px) noexcept {
    const float vh = std::max(viewport_h_px, 64.0f);
    if (cam.is_orthographic) {
        return std::max(0.05f, (target_px / vh) * std::max(cam.ortho_height, 0.1f));
    }
    const StageCamBasis b = eval_stage_cam_basis(cam);
    const float depth = std::max(0.25f, dot3(sub3(world_pos, b.eye), b.fwd));
    const float world_screen_h = 2.0f * depth * std::tan(std::clamp(cam.fovy_rad, 0.1f, 2.5f) * 0.5f);
    return std::max(0.05f, (target_px / vh) * world_screen_h);
}

uint32_t build_stage_schematic_overlay(const LightingRig& rig, const StageSchematicBuildParams& params,
                                       std::span<GpuStageOverlayElem> out_elems) noexcept {
    uint32_t count = 0u;
    auto push_line = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& col,
                         float width_px, float dash_freq = 0.0f) noexcept {
        if (count >= out_elems.size()) {
            return;
        }
        GpuStageOverlayElem& e = out_elems[count++];
        set4(e.p0_ws, a[0], a[1], a[2], width_px);
        set4(e.p1_ws, b[0], b[1], b[2], dash_freq);
        set4(e.p2_ws, 0.0f, 0.0f, 0.0f, 0.92f);
        set4(e.color_and_mode, col[0], col[1], col[2], (dash_freq > 0.01f) ? 1.0f : 0.0f);
    };

    auto push_tri = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c,
                        const std::array<float, 3>& col, float alpha) noexcept {
        if (count >= out_elems.size()) {
            return;
        }
        GpuStageOverlayElem& e = out_elems[count++];
        set4(e.p0_ws, a[0], a[1], a[2], 1.0f);
        set4(e.p1_ws, b[0], b[1], b[2], 1.0f);
        set4(e.p2_ws, c[0], c[1], c[2], alpha);
        set4(e.color_and_mode, col[0], col[1], col[2], 2.0f);
    };

    auto push_quad_frame = [&](const std::array<float, 3>& c00, const std::array<float, 3>& c10,
                               const std::array<float, 3>& c11, const std::array<float, 3>& c01,
                               const std::array<float, 3>& col, float line_px, float fill_alpha) noexcept {
        push_line(c00, c10, col, line_px);
        push_line(c10, c11, col, line_px);
        push_line(c11, c01, col, line_px);
        push_line(c01, c00, col, line_px);
        if (fill_alpha > 0.005f) {
            push_tri(c00, c10, c11, col, fill_alpha);
            push_tri(c00, c11, c01, col, fill_alpha);
        }
    };

    // 1. Metric 0.5m / 1.0m Studio Floor Grid (Y = 0)
    if (params.show_grid) {
        constexpr int kHalfSteps = 6; // -3.0m .. +3.0m at 0.5m spacing
        constexpr float kExtent = 3.0f;
        for (int s = -kHalfSteps; s <= kHalfSteps; ++s) {
            const float coord = static_cast<float>(s) * 0.5f;
            const bool is_major = (s % 2) == 0;
            const std::array<float, 3> grid_col =
                is_major ? std::array<float, 3>{0.36f, 0.40f, 0.46f} : std::array<float, 3>{0.22f, 0.24f, 0.28f};
            const float lw = is_major ? 1.4f : 1.0f;
            if (s == 0) {
                push_line({-kExtent, 0.0f, 0.0f}, {kExtent, 0.0f, 0.0f}, {0.82f, 0.28f, 0.28f}, 1.8f);
                push_line({0.0f, 0.0f, -kExtent}, {0.0f, 0.0f, kExtent}, {0.28f, 0.56f, 0.90f}, 1.8f);
            } else {
                push_line({-kExtent, 0.0f, coord}, {kExtent, 0.0f, coord}, grid_col, lw);
                push_line({coord, 0.0f, -kExtent}, {coord, 0.0f, kExtent}, grid_col, lw);
            }
        }
        // Subtle Y elevation axis pole at avatar origin X=0, Z=0 (ticks every 0.5m)
        push_line({0.0f, 0.0f, 0.0f}, {0.0f, 2.5f, 0.0f}, {0.32f, 0.72f, 0.36f}, 1.2f, 10.0f);
    }

    // 2. Studio Broadcast Camera FOV Pyramid Frustum
    if (params.show_frustum && params.has_studio_frustum) {
        const StageCamBasis cb = eval_stage_cam_basis(params.studio_cam);
        const float near_d = 0.22f;
        const float far_d = std::clamp(len3(sub3(params.pivot_ws, cb.eye)), 0.8f, 4.0f);
        const float tan_h = std::tan(std::clamp(params.studio_cam.fovy_rad, 0.1f, 2.4f) * 0.5f);
        const float asp = std::max(params.studio_aspect, 0.2f);
        auto corner = [&](float d, float sx, float sy) {
            const float hh = d * tan_h;
            const float hw = hh * asp;
            return add3(cb.eye, add3(mul3(cb.fwd, d), add3(mul3(cb.right, sx * hw), mul3(cb.up, sy * hh))));
        };
        const std::array<float, 3> cam_col{0.34f, 0.86f, 0.96f};
        const auto n00 = corner(near_d, -1.0f, -1.0f);
        const auto n10 = corner(near_d, 1.0f, -1.0f);
        const auto n11 = corner(near_d, 1.0f, 1.0f);
        const auto n01 = corner(near_d, -1.0f, 1.0f);
        const auto f00 = corner(far_d, -1.0f, -1.0f);
        const auto f10 = corner(far_d, 1.0f, -1.0f);
        const auto f11 = corner(far_d, 1.0f, 1.0f);
        const auto f01 = corner(far_d, -1.0f, 1.0f);
        push_quad_frame(n00, n10, n11, n01, cam_col, 1.8f, 0.06f);
        push_quad_frame(f00, f10, f11, f01, cam_col, 1.4f, 0.0f);
        push_line(cb.eye, f00, cam_col, 1.2f);
        push_line(cb.eye, f10, cam_col, 1.2f);
        push_line(cb.eye, f11, cam_col, 1.2f);
        push_line(cb.eye, f01, cam_col, 1.2f);
        // Sightline dashed vector + miniature camera body prism
        push_line(cb.eye, add3(cb.eye, mul3(cb.fwd, far_d)), {0.55f, 0.92f, 1.0f}, 1.2f, 8.0f);
        push_line(cb.eye, {cb.eye[0], 0.0f, cb.eye[2]}, {0.28f, 0.60f, 0.72f}, 1.0f, 6.0f);
    }

    // 3. Pure Schematic Studio Light & Modifier Geometry
    uint32_t light_count = rig.studio_light_count;
    std::array<StudioLightSpec, kMaxStudioLights> scratch{};
    const StudioLightSpec* list = rig.studio_lights.data();
    if (!rig.use_studio_lights || light_count == 0u) {
        light_count = synthesize_legacy_studio_lights(rig, params.pivot_ws, scratch);
        list = scratch.data();
    } else {
        light_count = std::min(light_count, kMaxStudioLights);
    }

    bool any_solo = false;
    for (uint32_t i = 0; i < light_count; ++i) {
        if (list[i].solo && list[i].enabled) {
            any_solo = true;
            break;
        }
    }

    for (uint32_t i = 0; i < light_count && count < out_elems.size(); ++i) {
        const StudioLightSpec& s = list[i];
        const StudioLightWorldPose pose = resolve_studio_light_world_pose(s, params.pivot_ws, params.eye_pos_ws);
        const bool is_selected = (params.selected_light_idx == static_cast<int>(i));
        const bool is_active = s.enabled && (!any_solo || s.solo);
        const bool is_passive = (s.passive_flag != PassiveFlagMode::None);

        std::array<float, 3> wire_col{0.86f, 0.72f, 0.42f};
        if (!is_active) {
            wire_col = {0.32f, 0.34f, 0.38f};
        } else if (is_passive) {
            wire_col = (s.passive_flag == PassiveFlagMode::NegativeFillBlack)
                           ? std::array<float, 3>{0.58f, 0.34f, 0.78f}
                           : std::array<float, 3>{0.90f, 0.88f, 0.65f};
        } else if (is_selected) {
            wire_col = {1.00f, 0.72f, 0.16f};
        } else if (s.fixture == StudioFixtureProfile::LedTubeAstera ||
                   s.fixture == StudioFixtureProfile::BroadcastRing) {
            wire_col = {0.26f, 0.86f, 0.96f};
        }

        const float line_px = is_selected ? 2.6f : 1.7f;
        const float fill_alpha = is_active ? (is_selected ? 0.18f : 0.14f) : 0.05f;
        const float hw = std::clamp(s.width_or_outer_m * 0.5f, 0.08f, 1.25f);
        const float hh = std::clamp(s.height_or_inner_m * 0.5f, 0.08f, 1.25f);

        auto pt = [&](float u, float v, float d) {
            return add3(pose.position_ws,
                        add3(mul3(pose.right_ws, u), add3(mul3(pose.up_ws, v), mul3(pose.forward_ws, d))));
        };

        auto draw_poly_ring = [&](uint32_t segs, float rad_x, float rad_y, float depth_z, bool fill_disc) {
            const std::array<float, 3> center = pt(0.0f, 0.0f, depth_z);
            std::array<float, 3> prev{};
            for (uint32_t k = 0; k <= segs; ++k) {
                const float ang = (2.0f * kPi * static_cast<float>(k % segs)) / static_cast<float>(segs);
                const auto cur = pt(rad_x * std::cos(ang), rad_y * std::sin(ang), depth_z);
                if (k > 0u) {
                    push_line(prev, cur, wire_col, line_px);
                    if (fill_disc) {
                        push_tri(center, prev, cur, wire_col, fill_alpha);
                    }
                }
                prev = cur;
            }
        };

        if (is_passive) {
            const auto c00 = pt(-hw, -hh, 0.0f);
            const auto c10 = pt(hw, -hh, 0.0f);
            const auto c11 = pt(hw, hh, 0.0f);
            const auto c01 = pt(-hw, hh, 0.0f);
            push_quad_frame(c00, c10, c11, c01, wire_col, line_px, fill_alpha * 1.15f);
            if (s.passive_flag == PassiveFlagMode::NegativeFillBlack) {
                push_line(c00, c11, wire_col, 1.2f);
                push_line(c10, c01, wire_col, 1.2f);
            }
            push_line(pose.position_ws, {pose.position_ws[0], 0.0f, pose.position_ws[2]}, {0.35f, 0.35f, 0.40f}, 1.0f,
                      6.0f);
            continue;
        }

        // Draw light stand floor drop + dashed Aim Line + target reticle
        push_line(pose.position_ws, {pose.position_ws[0], 0.0f, pose.position_ws[2]}, {0.30f, 0.32f, 0.36f}, 1.0f,
                  5.0f);
        if (is_active) {
            push_line(pose.position_ws, pose.target_ws, wire_col, is_selected ? 1.5f : 1.0f, 8.0f);
            if (is_selected) {
                constexpr float kCross = 0.06f;
                push_line({pose.target_ws[0] - kCross, pose.target_ws[1], pose.target_ws[2]},
                          {pose.target_ws[0] + kCross, pose.target_ws[1], pose.target_ws[2]}, wire_col, 1.6f);
                push_line({pose.target_ws[0], pose.target_ws[1] - kCross, pose.target_ws[2]},
                          {pose.target_ws[0], pose.target_ws[1] + kCross, pose.target_ws[2]}, wire_col, 1.6f);
            }
        }

        // Fixture-specific geometric schematic
        switch (s.fixture) {
        case StudioFixtureProfile::Octabox: {
            draw_poly_ring(8u, hw, hw, 0.0f, true);
            const auto apex = pt(0.0f, 0.0f, -std::max(0.22f, hw * 0.65f));
            for (uint32_t k = 0; k < 8u; ++k) {
                const float ang = (2.0f * kPi * static_cast<float>(k)) / 8.0f;
                push_line(apex, pt(hw * std::cos(ang), hw * std::sin(ang), 0.0f), wire_col, 1.2f);
            }
            break;
        }
        case StudioFixtureProfile::ParabolicUmbrella: {
            draw_poly_ring(12u, hw, hw, 0.0f, true);
            const auto apex = pt(0.0f, 0.0f, -hw * 0.72f);
            for (uint32_t k = 0; k < 6u; ++k) {
                const float ang = (2.0f * kPi * static_cast<float>(k)) / 6.0f;
                push_line(apex, pt(hw * std::cos(ang), hw * std::sin(ang), 0.0f), wire_col, 1.2f);
            }
            push_line(apex, pt(0.0f, 0.0f, hw * 0.35f), wire_col, 1.6f);
            break;
        }
        case StudioFixtureProfile::LanternChinaBall: {
            draw_poly_ring(12u, hw, hw, 0.0f, false);
            for (uint32_t k = 0; k < 12u; ++k) {
                const float a0 = (2.0f * kPi * static_cast<float>(k)) / 12.0f;
                const float a1 = (2.0f * kPi * static_cast<float>(k + 1u)) / 12.0f;
                push_line(pt(hw * std::cos(a0), 0.0f, hw * std::sin(a0)),
                          pt(hw * std::cos(a1), 0.0f, hw * std::sin(a1)), wire_col, line_px);
                push_line(pt(0.0f, hw * std::cos(a0), hw * std::sin(a0)),
                          pt(0.0f, hw * std::cos(a1), hw * std::sin(a1)), wire_col, 1.2f);
            }
            draw_poly_ring(8u, hw * 0.85f, hw * 0.85f, 0.0f, true);
            break;
        }
        case StudioFixtureProfile::ArriFresnel:
        case StudioFixtureProfile::DedolightSpot:
        case StudioFixtureProfile::EllipsoidalLeko: {
            const float lens_r = std::clamp(hw, 0.09f, 0.28f);
            const float back_d = (s.fixture == StudioFixtureProfile::EllipsoidalLeko) ? -0.44f : -0.28f;
            draw_poly_ring(12u, lens_r, lens_r, 0.0f, true);
            draw_poly_ring(10u, lens_r * 0.82f, lens_r * 0.82f, back_d, false);
            for (uint32_t k = 0; k < 4u; ++k) {
                const float a = (0.5f * kPi * static_cast<float>(k)) + 0.25f * kPi;
                push_line(pt(lens_r * 0.82f * std::cos(a), lens_r * 0.82f * std::sin(a), back_d),
                          pt(lens_r * std::cos(a), lens_r * std::sin(a), 0.0f), wire_col, 1.3f);
            }
            // Projected light cone frustum preview
            const float cone_len = 0.70f;
            const float cone_r =
                lens_r + cone_len * std::tan(std::clamp(s.spot_outer_deg, 6.0f, 80.0f) * 0.5f * kDegToRad);
            draw_poly_ring(12u, cone_r, cone_r, cone_len, false);
            for (uint32_t k = 0; k < 4u; ++k) {
                const float a = 0.5f * kPi * static_cast<float>(k);
                push_line(pt(lens_r * std::cos(a), lens_r * std::sin(a), 0.0f),
                          pt(cone_r * std::cos(a), cone_r * std::sin(a), cone_len), wire_col, 1.0f, 6.0f);
            }
            break;
        }
        case StudioFixtureProfile::BroadcastRing: {
            const float r_out = std::clamp(hw, 0.18f, 0.45f);
            const float r_in = std::clamp(hh, 0.10f, r_out * 0.78f);
            draw_poly_ring(16u, r_out, r_out, 0.0f, false);
            draw_poly_ring(16u, r_in, r_in, 0.0f, false);
            for (uint32_t k = 0; k < 16u; ++k) {
                const float a0 = (2.0f * kPi * static_cast<float>(k)) / 16.0f;
                const float a1 = (2.0f * kPi * static_cast<float>(k + 1u)) / 16.0f;
                const auto o0 = pt(r_out * std::cos(a0), r_out * std::sin(a0), 0.0f);
                const auto o1 = pt(r_out * std::cos(a1), r_out * std::sin(a1), 0.0f);
                const auto i0 = pt(r_in * std::cos(a0), r_in * std::sin(a0), 0.0f);
                const auto i1 = pt(r_in * std::cos(a1), r_in * std::sin(a1), 0.0f);
                push_tri(i0, o0, o1, wire_col, fill_alpha);
                push_tri(i0, o1, i1, wire_col, fill_alpha);
            }
            break;
        }
        case StudioFixtureProfile::NaturalSunCieSky: {
            draw_poly_ring(12u, 0.22f, 0.22f, 0.0f, true);
            for (uint32_t k = 0; k < 12u; ++k) {
                const float a = (2.0f * kPi * static_cast<float>(k)) / 12.0f;
                push_line(pt(0.25f * std::cos(a), 0.25f * std::sin(a), 0.0f),
                          pt(0.38f * std::cos(a), 0.38f * std::sin(a), 0.0f), wire_col, 1.6f);
            }
            break;
        }
        case StudioFixtureProfile::RectSoftbox:
        case StudioFixtureProfile::Stripbox:
        case StudioFixtureProfile::ArriSkyPanel:
        case StudioFixtureProfile::LedTubeAstera: {
            const float eff_hw = s.softbox_enabled ? hw : std::min(hw, 0.14f);
            const float eff_hh = s.softbox_enabled ? hh : std::min(hh, 0.14f);
            const auto c00 = pt(-eff_hw, -eff_hh, 0.0f);
            const auto c10 = pt(eff_hw, -eff_hh, 0.0f);
            const auto c11 = pt(eff_hw, eff_hh, 0.0f);
            const auto c01 = pt(-eff_hw, eff_hh, 0.0f);
            push_quad_frame(c00, c10, c11, c01, wire_col, line_px, fill_alpha);
            // Tapered softbox rear hood
            const float back_d = (s.fixture == StudioFixtureProfile::LedTubeAstera) ? -0.05f : -0.24f;
            const auto b00 = pt(-eff_hw * 0.45f, -eff_hh * 0.45f, back_d);
            const auto b10 = pt(eff_hw * 0.45f, -eff_hh * 0.45f, back_d);
            const auto b11 = pt(eff_hw * 0.45f, eff_hh * 0.45f, back_d);
            const auto b01 = pt(-eff_hw * 0.45f, eff_hh * 0.45f, back_d);
            push_quad_frame(b00, b10, b11, b01, wire_col, 1.2f, 0.0f);
            push_line(b00, c00, wire_col, 1.1f);
            push_line(b10, c10, wire_col, 1.1f);
            push_line(b11, c11, wire_col, 1.1f);
            push_line(b01, c01, wire_col, 1.1f);

            // Honeycomb Egg-Crate Grid Hatch (30/40/60 deg density)
            if (s.grid_enabled) {
                const int cells = (s.grid_angle_deg <= 32.0f) ? 6 : 4;
                for (int g = 1; g < cells; ++g) {
                    const float t = -1.0f + 2.0f * (static_cast<float>(g) / static_cast<float>(cells));
                    push_line(pt(t * eff_hw, -eff_hh, 0.005f), pt(t * eff_hw, eff_hh, 0.005f), wire_col, 1.0f);
                    push_line(pt(-eff_hw, t * eff_hh, 0.005f), pt(eff_hw, t * eff_hh, 0.005f), wire_col, 1.0f);
                }
            }

            // 4-leaf Barn Door Flaps hinged along aperture edges
            if (s.barn_doors_enabled) {
                const float flap_d = 0.18f;
                auto draw_flap = [&](const std::array<float, 3>& h0, const std::array<float, 3>& h1,
                                     const std::array<float, 3>& out_dir, float cut_deg) {
                    const float flare = std::cos(std::clamp(cut_deg, 10.0f, 80.0f) * kDegToRad) * flap_d;
                    const float fwd_ext = std::sin(std::clamp(cut_deg, 10.0f, 80.0f) * kDegToRad) * flap_d;
                    const auto off = add3(mul3(out_dir, flare), mul3(pose.forward_ws, fwd_ext));
                    push_quad_frame(h0, h1, add3(h1, off), add3(h0, off), wire_col, 1.3f, fill_alpha * 0.5f);
                };
                draw_flap(c01, c11, pose.up_ws, s.barn_door_cuts_deg[0]);
                draw_flap(c00, c10, mul3(pose.up_ws, -1.0f), s.barn_door_cuts_deg[1]);
                draw_flap(c00, c01, mul3(pose.right_ws, -1.0f), s.barn_door_cuts_deg[2]);
                draw_flap(c10, c11, pose.right_ws, s.barn_door_cuts_deg[3]);
            }
            break;
        }
        }
    }

    return count;
}

} // namespace aether::renderer
