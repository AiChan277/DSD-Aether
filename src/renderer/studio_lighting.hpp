#pragma once

#include "aether_ffi_contract.h"
#include "renderer/gpu_structs.hpp"
#include "renderer/preview_pass.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace aether::renderer {

using StudioLightGPU = AetherStudioLightGpu;
static_assert(sizeof(StudioLightGPU) == 128, "StudioLightGPU must be 128 bytes");
static_assert(alignof(StudioLightGPU) == 64, "StudioLightGPU must be 64-byte aligned");

inline constexpr uint32_t kMaxStudioLights = AETHER_MAX_STUDIO_LIGHTS; // 16
inline constexpr uint32_t kLtcLutSize = 64u;
inline constexpr uint32_t kIesLutWidth = 256u;
inline constexpr uint32_t kIesLutRows = 64u;

/** 12 real-world photometric studio & natural light fixture profiles. */
enum class StudioFixtureProfile : uint8_t {
    RectSoftbox = AETHER_FIXTURE_RECT_SOFTBOX,             // 0: 0.90 x 0.60 m 4-vertex LTC
    Octabox = AETHER_FIXTURE_OCTABOX,                      // 1: 1.10 m 8-vertex LTC
    Stripbox = AETHER_FIXTURE_STRIPBOX,                    // 2: 0.30 x 1.20 m vertical rim LTC
    ParabolicUmbrella = AETHER_FIXTURE_PARABOLIC_UMBRELLA, // 3: 1.05 m focused disk/parabolic
    LanternChinaBall = AETHER_FIXTURE_LANTERN_BALL,        // 4: 0.45 m omni sphere + 4-side skirt
    ArriSkyPanel = AETHER_FIXTURE_ARRI_SKYPANEL,           // 5: 0.645 x 0.30 m 105 deg LED panel
    ArriFresnel = AETHER_FIXTURE_ARRI_FRESNEL,             // 6: 12-55 deg focusable Fresnel spot
    DedolightSpot = AETHER_FIXTURE_DEDOLIGHT_SPOT,         // 7: 8-25 deg precision tight aspheric spot
    EllipsoidalLeko = AETHER_FIXTURE_ELLIPSOIDAL_LEKO,     // 8: 19/26/36 deg projector + Gobo mask
    BroadcastRing = AETHER_FIXTURE_BROADCAST_RING,         // 9: 0.25/0.18 m coaxial annular ring light
    LedTubeAstera = AETHER_FIXTURE_LED_TUBE,               // 10: 1.0 x 0.025 m linear capsule/tube
    NaturalSunCieSky = AETHER_FIXTURE_NATURAL_SUN_SKY,     // 11: 0.53 deg Sun + Hillaire Sky / Window Portal
};

/** Cinematographic color gel library + anime mood filters (multiplied in linear space). */
enum class StudioGelPreset : uint8_t {
    None = 0,
    QuarterCto = 1,
    HalfCto = 2,
    FullCto = 3,
    QuarterCtb = 4,
    HalfCtb = 5,
    FullCtb = 6,
    PlusGreen = 7,
    MinusGreen = 8,
    ShinkaiGoldenRose = 9,
    CyberCyan = 10,
    StageMagenta = 11,
};

/** Passive bounce / negative fill flag modes evaluated in ray/path tracing. */
enum class PassiveFlagMode : uint8_t {
    None = 0,
    NegativeFillBlack = 1,
    BounceWhite = 2,
    BounceSilver = 3,
    BounceGold = 4,
};

/** Gobo projection pattern presets for Ellipsoidal / Leko & Window Blinds. */
enum class StudioGoboPattern : uint8_t {
    None = 0,
    WindowBlinds = 1,
    AnimeClassroomSlats = 2,
    LeafCanopy = 3,
    StageCircleIris = 4,
};

struct LightingRig;

/** High-level editable studio light + modular optical modifier configuration (no heap allocs). */
struct StudioLightSpec {
    std::array<char, 40> name{"Key Light"};
    StudioFixtureProfile fixture{StudioFixtureProfile::RectSoftbox};
    bool enabled{true};
    bool solo{false};
    bool cast_shadow{true};
    bool aim_lock{true};
    bool use_orbit_angles{true};

    // Spatial placement (orbit around target_ws OR explicit position_ws)
    float orbit_azimuth_deg{32.0f};
    float orbit_elevation_deg{26.0f};
    float orbit_distance_m{2.10f};
    float roll_deg{0.0f};
    std::array<float, 3> position_ws{1.05f, 1.95f, 1.50f};
    std::array<float, 3> target_ws{0.0f, 1.32f, 0.0f};

    // Photometry & Light Channels
    float intensity_ev100{1.00f}; // EV100-calibrated studio scale (0 = base 1.0x reference, additive/linear normalized)
    float exposure_stops{0.0f};   // Relative photographic stops (-6..+6)
    float kelvin_cct{5600.0f};    // 1500K..20000K
    std::array<float, 3> tint_rgb{1.0f, 1.0f, 1.0f};
    float green_magenta{0.0f}; // -1.0 (+Magenta) .. +1.0 (+Green)
    uint32_t channel_mask{AETHER_LIGHT_CHANNEL_ALL};
    float catchlight_weight{1.0f};
    float shadow_bias_scale{1.0f};

    // Emitter dimensions & optics
    float width_or_outer_m{0.90f};
    float height_or_inner_m{0.60f};
    float spot_inner_deg{24.0f};
    float spot_outer_deg{42.0f};

    // Independently toggleable Optical Modifiers
    bool softbox_enabled{true};
    bool grid_enabled{false};
    float grid_angle_deg{40.0f}; // 30 / 40 / 60 deg Honeycomb Egg-Crate
    bool barn_doors_enabled{false};
    std::array<float, 4> barn_door_cuts_deg{38.0f, 38.0f, 42.0f, 42.0f}; // Top, Bottom, Left, Right
    bool diffusion_enabled{true};
    float diffusion_density{0.65f}; // 0.35 (1/4), 0.65 (1/2), 1.0 (Full Grid)
    bool gel_enabled{false};
    StudioGelPreset gel{StudioGelPreset::None};
    bool coaxial_lock{false};
    bool window_portal{false};
    StudioGoboPattern gobo{StudioGoboPattern::None};
    int8_t ies_profile{-1}; // -1 = procedural, 0..7 = ANSI/IES LUT row
    PassiveFlagMode passive_flag{PassiveFlagMode::None};
};

/** Split-Layer "holo-grading" parameters isolating avatar vs background. */
struct SplitLayerGradingParams {
    float avatar_contrast{1.04f};
    float avatar_saturation{1.05f};
    float avatar_warmth{0.0f};
    float avatar_ev_bias{0.0f};
    float bg_contrast{1.02f};
    float bg_saturation{0.96f};
    float bg_exposure{1.00f};
    float master_grade_strength{1.0f};
};

/** Makoto Shinkai Satsuei optical composite controls. */
struct SatsueiCompositeParams {
    float light_wrap_strength{0.22f};
    float halation_strength{0.18f};
    float halation_threshold{0.95f};
    float backscatter_strength{0.55f};
};

/** Applies factory optical dimensions/cone defaults for one of the 12 fixtures cleanly. */
void configure_fixture_defaults(StudioLightSpec& spec, StudioFixtureProfile profile) noexcept;

/** Linear RGB spectral transmittance for cinematographic color gels + green/magenta trim. */
[[nodiscard]] std::array<float, 3> gel_transmittance_rgb(StudioGelPreset gel, float green_magenta) noexcept;

/** Computes Frostbite EV100 photometric normalization scalar for photographic stop adjustments. */
[[nodiscard]] float frostbite_ev100_exposure_scale(float ev100_offset, float luminance_scale = 1.0f) noexcept;

/** Packs up to `kMaxStudioLights` (16) studio lights into aligned `StudioLightGPU` buffer without heap allocs. */
uint32_t pack_studio_lights_gpu(const LightingRig& rig, const std::array<float, 3>& pivot_ws,
                                const std::array<float, 3>& eye_pos_ws,
                                std::array<StudioLightGPU, kMaxStudioLights>& out_gpu) noexcept;

/**
 * Generates precomputed 64x64 Heitz (2016) GGX Linearly Transformed Cosines inverse matrix (`M^-1`)
 * and amplitude/Fresnel LUT (`RGBA32F`, 4 floats per texel).
 */
void generate_heitz_ltc_luts(std::array<float, kLtcLutSize * kLtcLutSize * 4>& out_inv_m,
                             std::array<float, kLtcLutSize * kLtcLutSize * 4>& out_amp) noexcept;

/**
 * Generates precomputed 256x64 `R32F` ANSI/IES LM-63-19 photometric goniometer beam LUT.
 * Row `0..7` hold calibrated profiles (Softbox, Octa, Strip, Parabolic, Fresnel Spot, Dedo Spot, Leko, Ring).
 */
void generate_ies_lm63_lut(std::array<float, kIesLutWidth * kIesLutRows>& out_ies_r32f) noexcept;

/** Computes analytical 3-band Hillaire (2020) Rayleigh+Mie atmosphere Spherical Harmonics (9 `float4` RGB). */
void compute_hillaire_sky_sh9(const std::array<float, 3>& sun_dir_ws, const std::array<float, 3>& sun_color_lin,
                              float sky_strength, float ground_bounce_strength,
                              std::array<std::array<float, 4>, 9>& out_sh9) noexcept;

/** Returns human-readable label for a `StudioFixtureProfile`. */
[[nodiscard]] const char* studio_fixture_name(StudioFixtureProfile profile) noexcept;

/** Returns human-readable label for a `StudioGelPreset`. */
[[nodiscard]] const char* studio_gel_name(StudioGelPreset gel) noexcept;

/** Returns human-readable label for a `PassiveFlagMode`. */
[[nodiscard]] const char* passive_flag_name(PassiveFlagMode mode) noexcept;

/** Returns human-readable label for a `StudioGoboPattern`. */
[[nodiscard]] const char* studio_gobo_name(StudioGoboPattern gobo) noexcept;

/** Resolved world-space luminaire pose (origin, target, basis axes). */
struct StudioLightWorldPose {
    std::array<float, 3> position_ws{};
    std::array<float, 3> target_ws{};
    std::array<float, 3> forward_ws{};
    std::array<float, 3> right_ws{};
    std::array<float, 3> up_ws{};
    float distance_m{2.0f};
};

/** Resolves world-space emitter position, target, forward direction, and tangent frame. */
[[nodiscard]] StudioLightWorldPose resolve_studio_light_world_pose(const StudioLightSpec& spec,
                                                                   const std::array<float, 3>& pivot_ws,
                                                                   const std::array<float, 3>& eye_pos_ws) noexcept;

/** Converts an orbit light to explicit world-space coordinates (`position_ws`) for direct gizmo manipulation. */
void bake_studio_light_world_pose(StudioLightSpec& spec, const std::array<float, 3>& pivot_ws,
                                  const std::array<float, 3>& eye_pos_ws) noexcept;

/** Ensures `rig.studio_lights` is populated (converting legacy `rig.lights` once if needed) and activates studio array mode. */
void ensure_studio_lights_initialized(LightingRig& rig, const std::array<float, 3>& pivot_ws = {0.0f, 1.32f, 0.0f}) noexcept;

/** Appends a new fixture to `rig.studio_lights` (up to `kMaxStudioLights`), returning its index or `-1` when full. */
[[nodiscard]] int add_studio_light(LightingRig& rig, StudioFixtureProfile profile,
                                     const std::array<float, 3>& pivot_ws = {0.0f, 1.32f, 0.0f}) noexcept;

/** Appends a passive bounce card or negative fill flag to `rig.studio_lights`, returning its index or `-1` when full. */
[[nodiscard]] int add_passive_flag(LightingRig& rig, PassiveFlagMode mode,
                                     const std::array<float, 3>& pivot_ws = {0.0f, 1.32f, 0.0f}) noexcept;

/** Duplicates `rig.studio_lights[index]` (offset slightly in azimuth), returning the new index or `-1`. */
[[nodiscard]] int duplicate_studio_light(LightingRig& rig, uint32_t index) noexcept;

/** Removes `rig.studio_lights[index]` cleanly without leaving dangling gaps. */
[[nodiscard]] bool remove_studio_light(LightingRig& rig, uint32_t index) noexcept;

/** 5 built-in complete studio rig presets shipped in `config/lighting_rigs.json`. */
enum class StudioRigPresetId : uint8_t {
    ThreePointBroadcast = 0,
    DramaticRimSplit = 1,
    ShinkaiGoldenHour = 2,
    HighKeySoftBeauty = 3,
    NeonCyberpunkDualColor = 4,
};

/** Returns human-readable name for a `StudioRigPresetId`. */
[[nodiscard]] const char* studio_rig_preset_name(StudioRigPresetId id) noexcept;

/** Applies one of the 5 built-in complete studio rig presets to `rig` (activating all 16-slot controls). */
void apply_studio_rig_preset(LightingRig& rig, StudioRigPresetId id,
                             const std::array<float, 3>& pivot_ws = {0.0f, 1.32f, 0.0f}) noexcept;

/** Constructs a Stage Camera preset for one of the 7 stage views (Free Perspective + 6 Orthographic). */
[[nodiscard]] PreviewCamera make_stage_camera_preset(StageCameraView view, const std::array<float, 3>& pivot_ws,
                                                     float current_ortho_height = 2.20f) noexcept;

/** World-space picking ray generated from either a Perspective or Parallel Orthographic stage camera. */
struct StagePickRay {
    std::array<float, 3> origin_ws{};
    std::array<float, 3> dir_ws{};
    bool is_orthographic{false};
    float ortho_height{2.20f};
};

/** Computes a picking ray at normalized viewport coordinates `u_01, v_01` in `[0, 1]` for both Perspective & Ortho. */
[[nodiscard]] StagePickRay compute_stage_pick_ray(const PreviewCamera& cam, float u_01, float v_01,
                                                  float aspect) noexcept;

/** Picks the nearest studio light fixture or passive flag under `u_01, v_01`, returning its index or `-1`. */
[[nodiscard]] int pick_stage_studio_light(const LightingRig& rig, const std::array<float, 3>& pivot_ws,
                                          const std::array<float, 3>& eye_pos_ws, const PreviewCamera& stage_cam,
                                          float u_01, float v_01, float aspect, float* out_hit_t = nullptr) noexcept;

/** Computes constant screen-pixel world scale factor for 3D gizmos in both Perspective & Ortho cameras. */
[[nodiscard]] float stage_gizmo_world_scale(const PreviewCamera& cam, const std::array<float, 3>& world_pos,
                                            float target_px = 78.0f, float viewport_h_px = 720.0f) noexcept;

/** Parameters for generating strict schematic-only geometry into `GpuStageOverlayElem`. */
struct StageSchematicBuildParams {
    int selected_light_idx{-1};
    bool show_grid{true};
    bool show_frustum{true};
    bool has_studio_frustum{false};
    PreviewCamera studio_cam{};
    float studio_aspect{16.0f / 9.0f};
    std::array<float, 3> pivot_ws{0.0f, 1.32f, 0.0f};
    std::array<float, 3> eye_pos_ws{0.0f, 1.48f, 2.20f};
};

/**
 * Generates pure Schematic Lighting Overlay primitives (fixture wireframes, 15% translucent apertures,
 * honeycomb grid hatch, barn door flaps, dashed aim lines, StudioCamera FOV frustum, and metric floor grid)
 * strictly for `LightingStageView`. Writes up to `out_elems.size()` elements and returns count written.
 */
[[nodiscard]] uint32_t build_stage_schematic_overlay(const LightingRig& rig, const StageSchematicBuildParams& params,
                                                     std::span<GpuStageOverlayElem> out_elems) noexcept;

} // namespace aether::renderer
