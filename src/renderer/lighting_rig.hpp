#pragma once

#include <array>
#include <cstdint>

namespace aether::renderer {

enum class LightRole : uint8_t {
    Key = 0,
    Fill = 1,
    Rim = 2,
    HairOrbit = 3,
};

enum class LightType : uint8_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
};

struct SceneLight {
    LightRole role{LightRole::Key};
    LightType type{LightType::Directional};
    bool enabled{true};
    bool cast_rt_shadow{true};
    bool use_2d_orbit_angle{true};
    float direction_or_pos[3]{0.42f, 0.56f, 0.71f};
    float color[3]{1.0f, 0.97f, 0.94f};
    float intensity{1.85f};
    float angular_radius_rad{0.06f};
    float cone_angle_rad{0.75f};
    float orbit_azimuth_deg{32.0f};
    float orbit_elevation_deg{28.0f};
};

struct LightingRig {
    static constexpr uint32_t kMaxLights = 8;

    std::array<SceneLight, kMaxLights> lights{};
    uint32_t active_count{4};

    // Spec §10 & 2D Hand-Drawn Live2D Illustration look parameters
    float global_live2d_flatness{0.75f};
    float face_normal_flatten{0.85f};
    float cel_band_sharpness{0.025f};
    float shadow_warm_cool_shift{0.75f};
    float iro_tsuke_strength{0.85f};
    float crease_strength{0.75f};
    float hair_highlight_intensity{0.85f};
    float rim_step_strength{0.65f};
    float sdf_feather{0.08f};
    float hair_shadow_offset_px{6.0f};
    float hair_shadow_strength{0.60f};
    float rim_t_max{0.45f};
    float bounce_t_max{0.25f};
    float bounce_strength{0.35f};
    float toon_ramp_shift{-0.05f};
    float toon_ramp_toony{0.85f};
    float outline_width_scale{1.0f};
    float crease_threshold_deg{35.0f};
    float bloom_threshold{1.05f};
    float bloom_intensity{0.06f};
    uint32_t tone_mode{1}; // 0 = Standard clamp, 1 = Soft highlights (AgX shoulder)
    float bg_rgba[4]{0.0f, 0.0f, 0.0f, 0.0f};

    // Tasks 3-7 extensions: 3-tier lighting, pre-quantized color bleed, dynamic rim, 2D Filter Pro, circadian
    uint8_t lighting_mode{1}; // 0 = Traditional (Raster+SDF), 1 = Hybrid RT, 2 = 2.5D Path Tracing
    bool enable_2d_filter_pro{true};
    float painterly_flatten_strength{0.55f};
    float dog_ink_boost{0.65f};
    bool enable_paper_warmth{false};
    float paper_warmth_strength{0.20f};
    float indirect_bleed_strength{0.75f};
    float rim_depth_offset_px{3.0f};
    bool circadian_enabled{false};
    int32_t circadian_timezone{0};
    int32_t circadian_time_minutes{-1}; // -1 = Live Clock, 0..1439 = Manual preview scrub

    [[nodiscard]] static LightingRig default_three_point() noexcept {
        LightingRig rig{};
        rig.active_count = 4;

        // 0: Key Light (primary RT shadow caster + Light Orbit Orb driver)
        rig.lights[0].role = LightRole::Key;
        rig.lights[0].type = LightType::Directional;
        rig.lights[0].enabled = true;
        rig.lights[0].cast_rt_shadow = true;
        rig.lights[0].use_2d_orbit_angle = true;
        rig.lights[0].orbit_azimuth_deg = 32.0f;
        rig.lights[0].orbit_elevation_deg = 26.0f;
        rig.lights[0].color[0] = 1.0f;
        rig.lights[0].color[1] = 0.98f;
        rig.lights[0].color[2] = 0.95f;
        rig.lights[0].intensity = 1.00f;
        rig.lights[0].angular_radius_rad = 0.06f;

        // 1: Soft Fill Light
        rig.lights[1].role = LightRole::Fill;
        rig.lights[1].type = LightType::Directional;
        rig.lights[1].enabled = true;
        rig.lights[1].cast_rt_shadow = false;
        rig.lights[1].orbit_azimuth_deg = -52.0f;
        rig.lights[1].orbit_elevation_deg = 12.0f;
        rig.lights[1].color[0] = 0.62f;
        rig.lights[1].color[1] = 0.76f;
        rig.lights[1].color[2] = 1.0f;
        rig.lights[1].intensity = 0.28f;
        rig.lights[1].angular_radius_rad = 0.14f;

        // 2: Occluded Rim Light
        rig.lights[2].role = LightRole::Rim;
        rig.lights[2].type = LightType::Directional;
        rig.lights[2].enabled = true;
        rig.lights[2].cast_rt_shadow = true;
        rig.lights[2].orbit_azimuth_deg = 148.0f;
        rig.lights[2].orbit_elevation_deg = 22.0f;
        rig.lights[2].color[0] = 0.78f;
        rig.lights[2].color[1] = 0.89f;
        rig.lights[2].color[2] = 1.0f;
        rig.lights[2].intensity = 0.35f;
        rig.lights[2].angular_radius_rad = 0.04f;

        // 3: Hair Orbit Specular Driver (angel-ring highlight)
        rig.lights[3].role = LightRole::HairOrbit;
        rig.lights[3].type = LightType::Directional;
        rig.lights[3].enabled = true;
        rig.lights[3].cast_rt_shadow = false;
        rig.lights[3].orbit_azimuth_deg = 10.0f;
        rig.lights[3].orbit_elevation_deg = 52.0f;
        rig.lights[3].color[0] = 1.0f;
        rig.lights[3].color[1] = 0.98f;
        rig.lights[3].color[2] = 0.96f;
        rig.lights[3].intensity = 0.95f;
        return rig;
    }
};

} // namespace aether::renderer
