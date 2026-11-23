#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace aether {

/** Canonical change-tracking bitmask domains (Rev 3.1 §1.1). */
enum class DirtyDomain : uint32_t {
    None = 0u,
    ProjectMeta = 1u << 0, // Project name, UUID, timestamps, render tier
    AvatarState = 1u << 1, // VRM/PMX path, transform, spring/rigidbody physics
    LightingRig = 1u << 2, // StudioLightingStore (16 lights + modifiers + circadian)
    EffectsFx = 1u << 3,   // Satsuei grading, bloom, halation, 2D filter, VRM parity
    CameraState = 1u << 4, // Active camera + saved presets + lighting stage cam
    NodeGraph = 1u << 5,   // Rust graph topology + material/texture overrides
    TrackingCfg = 1u << 6, // NPU bounds, eye/brow/hand tuning, audio lip-sync
    OutputsCfg = 1u << 7,  // Spout2, Virtual Cam, NDI, OSC
};

[[nodiscard]] constexpr uint32_t dirty_bit(DirtyDomain d) noexcept {
    return static_cast<uint32_t>(d);
}

[[nodiscard]] constexpr DirtyDomain operator|(DirtyDomain a, DirtyDomain b) noexcept {
    return static_cast<DirtyDomain>(dirty_bit(a) | dirty_bit(b));
}

struct AvatarProjectState {
    std::string format{};
    std::string path_absolute{};
    std::string path_relative{};
    std::string file_sha256{};
    std::array<float, 3> root_pos{0.0f, 0.0f, 0.0f};
    std::array<float, 3> root_rot_deg{0.0f, 0.0f, 0.0f};
    float uniform_scale{1.0f};
    bool physics_enabled{true};
    float spring_stiffness{1.0f};
    float spring_damping{1.0f};
    float pmx_rigidbody_damping{0.92f};
    float pmx_velocity_clamp{6.5f};
    uint32_t active_expression_slot{0u};
    float expression_gain{1.0f};
};

struct LightingLinkState {
    std::string mode{"Embedded"};
    std::string external_alfx_path{};
    std::string last_synced_utc{};
};

struct LiveCameraProjectState {
    std::array<float, 3> target{0.0f, 1.38f, 0.0f};
    float distance{1.45f};
    float yaw_deg{0.0f};
    float pitch_deg{-2.0f};
    float fov_y_deg{24.0f};
};

struct ProjectCustomCameraPreset {
    std::string id{};
    std::string name{};
    std::array<float, 3> target{0.0f, 1.38f, 0.0f};
    float distance{1.45f};
    float yaw_deg{0.0f};
    float pitch_deg{0.0f};
    float fov_y_deg{24.0f};
    std::array<float, 3> avatar_offset{0.0f, 0.0f, 0.0f};
    float avatar_rot_y_deg{0.0f};
    float avatar_scale{1.0f};
    bool has_avatar_xform{false};
};

struct CameraProjectState {
    std::string active_preset_id{"bust_default"};
    std::string aspect_mode{"16:9"};
    float transition_smooth_sec{0.22f};
    LiveCameraProjectState live_camera_state{};
    std::vector<ProjectCustomCameraPreset> custom_presets{};
};

struct LightingStageCameraState {
    std::string view_mode{"Persp"};
    bool quad_view_enabled{false};
    bool pip_visible{true};
    std::string diagnostic_mode{"Lit"};
    bool show_schematics{true};
    bool show_grid{true};
    bool preview_25d_flatten{false};
    std::array<float, 3> target{0.0f, 1.15f, 0.0f};
    float distance{3.1f};
    float yaw_deg{38.0f};
    float pitch_deg{24.0f};
    float ortho_half_height{1.35f};
};

struct TrackingAudioProjectState {
    std::string camera_device_id{"Integrated Camera"};
    std::string npu_tier{"Tier1_OpenVINO_NPU"};
    bool hand_tracking_enabled{false};
    bool show_tracking_hud{true};
    bool tracking_hud_collapsed{true};
    std::array<float, 3> npu_movement_bounds_min{-0.35f, -0.25f, -0.30f};
    std::array<float, 3> npu_movement_bounds_max{0.35f, 0.25f, 0.30f};
    float eye_blink_gain{1.0f};
    float brow_gain{1.0f};
    float mouth_gain{1.0f};
    float audio_lipsync_gain{1.0f};
    std::string audio_device_id{};
};

struct OutputsProjectState {
    bool spout_enabled{true};
    std::string spout_sender_name{"Aether2.5D"};
    bool spout_premul_alpha{true};
    uint8_t non_alpha_bg{0u};
    std::array<float, 3> custom_bg_rgb{0.0f, 177.0f / 255.0f, 64.0f / 255.0f};
    bool vcam_enabled{true};
    bool ndi_enabled{false};
    bool osc_enabled{true};
    std::string osc_host{"127.0.0.1"};
    uint16_t osc_port{9001u};
    uint32_t resolution_width{1920u};
    uint32_t resolution_height{1080u};
    uint32_t fps{60u};
    float dsp_user_offset_ms{0.0f};
};

struct StudioProjectMeta {
    std::string project_uuid{};
    std::string project_name{"Untitled Project"};
    std::string app_version{"1.0.4"};
    std::string created_utc{};
    std::string modified_utc{};
    std::string avatar_display_name{};
    std::string render_tier{"HybridRT"};
};

struct PackedAssetEntry {
    std::string archive_path{};
    std::vector<uint8_t> bytes{};
};

/**
 * Canonical single-source-of-truth project and workspace state (Rev 3.1 §1.1).
 * Every parameter has exactly one memory address; panels dispatch edits here
 * and never cache shadow copies.
 */
struct StudioStateStore {
    StudioProjectMeta meta{};
    AvatarProjectState avatar{};
    LightingLinkState lighting_link{};
    CameraProjectState camera{};
    LightingStageCameraState lighting_stage_camera{};
    TrackingAudioProjectState tracking_and_audio{};
    OutputsProjectState outputs{};
    std::string node_graph_json{"{\"schema_version\":1,\"nodes\":[],\"edges\":[],\"groups\":[],\"comments\":[]}"};
    std::vector<PackedAssetEntry> packed_assets{};

    uint32_t dirty_mask{0u};
    uint64_t dirty_generation{0u};

    void mark_dirty(DirtyDomain d) noexcept {
        const uint32_t bits = dirty_bit(d);
        if (bits != 0u) {
            dirty_mask |= bits;
            ++dirty_generation;
        }
    }

    void clear_dirty() noexcept { dirty_mask = 0u; }

    [[nodiscard]] bool is_dirty() const noexcept { return dirty_mask != 0u; }

    [[nodiscard]] bool is_domain_dirty(DirtyDomain d) const noexcept { return (dirty_mask & dirty_bit(d)) != 0u; }

    void reset_to_defaults() {
        *this = StudioStateStore{};
    }
};

} // namespace aether
