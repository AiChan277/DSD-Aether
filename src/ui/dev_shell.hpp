#pragma once

#include "aether_ffi_contract.h"
#include "animation/live2d_params.hpp"
#include "core/hw_enum.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/telemetry.hpp"
#include "core/viewport_aspect.hpp"
#include "renderer/lighting_rig.hpp"
#include "renderer/preview_pass.hpp"
#include "rhi/rhi_device.hpp"
#include "ui/theme/precision_dark.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace aether::core {
class SessionManager;
}

namespace aether::ui {

class StudioWorkspace;
struct StudioActions;

enum class StudioMode : uint8_t {
    LiveStudio = 0,
    Rigging = 1,
    Lighting = 2,
    Nodes = 3,
};

struct DevShellInputEcho {
    float cursor_x{0.0f};
    float cursor_y{0.0f};
    uint32_t last_key_vk{0};
    uint64_t total_events{0};
    uint64_t dropped_moves{0};
};

struct DevShellAvatarView {
    VkDescriptorSet viewport_texture{VK_NULL_HANDLE};
    uint32_t viewport_w{0};
    uint32_t viewport_h{0};
    bool has_avatar{false};
    bool is_loading{false};
    float load_progress{0.0f};
    const char* source_path{""};
    const char* format_name{"None"};
    const char* load_error{""};
    uint32_t vertex_count{0};
    uint32_t index_count{0};
    uint32_t submesh_count{0};
    uint32_t material_count{0};
    uint32_t joint_count{0};
    uint32_t texture_count{0};
    uint32_t spring_count{0};
    uint32_t morph_count{0};
    uint32_t mapped_humanoid_bones{0};
    uint32_t warnings_mask{0};
    float avatar_height_m{0.0f};
    double open_query_ms{0.0};
    double fill_ms{0.0};
    double gpu_upload_ms{0.0};
    bool wireframe_supported{false};
    bool has_camera{false};
    std::array<float, 3> camera_eye{0.716f, 1.110f, 1.961f};
    std::array<float, 3> camera_target{0.0f, 0.90f, 0.0f};
    float camera_fovy_rad{0.62f};
    float camera_yaw_rad{0.28f};
    float camera_pitch_rad{0.06f};
    float camera_distance{2.0f};
    float gizmo_pivot_y{0.90f};
    float bone_hips_y{0.85f};
    float bone_chest_y{1.20f};
    float bone_head_y{1.46f};
    uint32_t broadcast_width{1920};
    uint32_t broadcast_height{1080};

    // Dual-viewport Stage textures & dimensions (Lighting Tab only; broadcast stays on `viewport_texture`)
    VkDescriptorSet stage_viewport_texture{VK_NULL_HANDLE};
    VkDescriptorSet quad_top_texture{VK_NULL_HANDLE};
    VkDescriptorSet quad_right_texture{VK_NULL_HANDLE};
    VkDescriptorSet stage_broadcast_pip_texture{VK_NULL_HANDLE};
    uint32_t stage_viewport_w{0};
    uint32_t stage_viewport_h{0};
    uint32_t stage_viewport_rt_w{0};
    uint32_t stage_viewport_rt_h{0};
    uint32_t quad_top_w{0};
    uint32_t quad_top_h{0};
    uint32_t quad_top_rt_w{0};
    uint32_t quad_top_rt_h{0};
    uint32_t quad_right_w{0};
    uint32_t quad_right_h{0};
    uint32_t quad_right_rt_w{0};
    uint32_t quad_right_rt_h{0};
    uint32_t stage_broadcast_pip_w{0};
    uint32_t stage_broadcast_pip_h{0};
};

struct DevShellTrackingView {
    bool enabled{false};
    bool ready{false};
    bool using_synthetic_camera{true};
    bool camera_active{false};
    bool roi_locked{false};
    uint32_t active_ep{AETHER_EP_CPU};
    int32_t selected_ep_override{-1}; // -1 = Auto
    uint32_t camera_width{0};
    uint32_t camera_height{0};
    uint32_t camera_fps{0};
    uint64_t frames_processed{0};
    uint64_t demotion_count{0};
    float preprocess_ms{0.0f};
    float inference_ms{0.0f};
    float rolling_infer_ms{0.0f};
    float spring_sim_ms{0.0f};
    float face_confidence{0.0f};
    float upper_body_confidence{0.0f};
    float left_hand_confidence{0.0f};
    float right_hand_confidence{0.0f};
    float jaw_open_preview{0.0f};
    float blink_l_preview{0.0f};
    float blink_r_preview{0.0f};
    uint32_t valid_flags{0};
    const char* active_ep_label{"Initializing..."};
    const char* camera_label{"Synthetic 60Hz"};
    const char* camera_symlink{""};
    bool audio_mic_available{false};
    bool audio_assist_enabled{true};
    bool audio_vad{false};
    float audio_rms_db{-96.0f};
    float audio_open_y{0.0f};
    const char* audio_device_label{"System Default"};
    const char* audio_device_id{""};

    bool neutral_calibrated{false};
    uint32_t stable_lock_frames{0};
    std::array<float, 3> head_euler_deg{0.0f, 0.0f, 0.0f};
    uint64_t live_preview_texture_id{0};
    uint32_t live_preview_width{0};
    uint32_t live_preview_height{0};
    bool live_preview_valid{false};
    bool live_preview_face_locked{false};
    std::array<float, 4> live_preview_face_roi{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<std::array<float, 2>, 68> live_preview_keypoints{};
    std::array<uint8_t, 68> live_preview_keypoint_valid{};
    uint32_t live_preview_keypoint_count{0};
    std::array<std::array<float, 2>, 2> live_preview_iris_centers{};
    bool live_preview_iris_valid{false};
    bool live_preview_axes_valid{false};
    std::array<std::array<float, 2>, 4> live_preview_head_axes{};
    std::array<std::array<float, 2>, 8> live_preview_left_eye_poly{};
    std::array<std::array<float, 2>, 8> live_preview_right_eye_poly{};
    bool live_preview_eye_poly_valid{false};
    std::array<std::array<float, 2>, 5> live_preview_left_brow_pts{};
    std::array<std::array<float, 2>, 5> live_preview_right_brow_pts{};
    bool live_preview_brow_pts_valid{false};
    std::array<float, 2> live_preview_gaze_vec{0.0f, 0.0f};
    bool hand_tracking_enabled{false};
    bool left_hand_detected{false};
    bool right_hand_detected{false};
    std::array<float, 4> left_hand_bbox_obb{0.0f, 0.0f, 0.0f, 0.0f};
    float left_hand_angle_rad{0.0f};
    std::array<float, 2> left_palm_center{0.0f, 0.0f};
    std::array<float, 2> left_palm_normal_2d{0.0f, 0.0f};
    std::array<std::array<float, 2>, 21> left_hand_keypoints{};
    std::array<float, 4> right_hand_bbox_obb{0.0f, 0.0f, 0.0f, 0.0f};
    float right_hand_angle_rad{0.0f};
    std::array<float, 2> right_palm_center{0.0f, 0.0f};
    std::array<float, 2> right_palm_normal_2d{0.0f, 0.0f};
    std::array<std::array<float, 2>, 21> right_hand_keypoints{};
    animation::Live2DParameters live2d{};
};

struct DevShellRenderView {
    bool rt_supported{false};
    bool rt_enabled{true};
    uint8_t debug_view{0};
    uint8_t denoiser_requested{0};
    const char* denoiser_active_name{"SVGF-Lite"};
    const char* denoiser_fallback_reason{""};
    bool denoiser_fallback_triggered{false};
    bool show_skeleton_heatmap{false};
    bool show_checkerboard{true};
    bool show_orbit_orb{false};
    bool baked_face_sdf_applied{false};
    float dominant_key_azimuth_deg{32.0f};
    float dominant_key_elevation_deg{26.0f};
    float dominant_key_intensity{1.00f};
    float live2d_flatness{0.75f};
    float bloom_intensity{0.06f};
    float outline_scale{1.0f};
    float total_gpu_ms{0.0f};
    float skin_prepass_gpu_ms{0.0f};
    float rt_gpu_ms{0.0f};
    float denoise_gpu_ms{0.0f};
    float denoise_p95_120f_ms{0.0f};
    float npr_gpu_ms{0.0f};
    float post_gpu_ms{0.0f};
    float encode_gpu_ms{0.0f};
    bool timing_valid{false};
    uint32_t device_recovery_count{0};
    const char* last_recovery_msg{""};

    // Render backend & engine garbage collector (ADR-0016 / Task 1-7)
    uint8_t render_backend{0}; // 0 = Vulkan, 1 = DirectX 11, 2 = DirectX 12
    uint8_t lighting_mode{1};  // 0 = Traditional, 1 = Hybrid RT, 2 = 2.5D Path Tracing
    bool is_uma{false};
    bool is_intel_igpu{false};
    uint8_t circadian_phase{2}; // CircadianPhase enum
    float circadian_hour_24{12.0f};
    float circadian_key_kelvin{5800.0f};
    const char* backend_detail{""};
    float dx11_render_ms{0.0f};
    float dx12_render_ms{0.0f};
    uint64_t gc_retired_pending{0};
    uint64_t gc_reclaimed_total{0};
    uint64_t gc_bytes_reclaimed{0};

    float present_fps{0.0f};
    uint64_t vram_usage_mb{0};
    uint64_t vram_budget_mb{0};
    const std::vector<rhi::GpuAdapterDescriptor>* gpu_adapters{nullptr};
};

struct DevShellOutputView {
    uint32_t width{1920};
    uint32_t height{1080};
    uint32_t fps{60};
    bool premul_alpha{true};
    uint8_t non_alpha_bg{0}; // 0=ChromaGreen, 1=Black, 2=Custom

    bool spout_enabled{true};
    bool spout_active{false};
    bool spout_using_cpu_fallback{false};
    bool spout_adapter_mismatch{false};
    uint64_t spout_frames_published{0};
    uint64_t spout_skipped_mutex{0};
    double spout_publish_ms{0.0};

    bool vcam_enabled{true};
    bool vcam_active{false};
    bool vcam_using_ext_host_mem{false};
    bool vcam_rgb_pin_connected{false};
    uint64_t vcam_frames_published{0};
    double vcam_dma_ms{0.0};

    bool ndi_enabled{false};
    bool ndi_active{false};
    bool ndi_runtime_available{false};
    uint8_t ndi_format{0}; // 0=BGRA, 1=UYVA
    uint64_t ndi_frames_sent{0};
    double ndi_send_ms{0.0};

    bool osc_enabled{true};
    bool osc_active{false};
    uint64_t osc_packets_sent{0};

    bool dsp_connected{false};
    bool dsp_voice_active{false};
    float dsp_input_rms_db{-96.0f};
    float dsp_reported_latency_ms{0.0f};
    float dsp_recommended_delay_ms{0.0f};
    float video_latency_ms{0.0f};
    float latency_jitter_ms{0.0f};
    float dsp_user_offset_ms{0.0f};
    uint64_t dsp_stale_count{0};
    uint64_t late_latch_us{0};
};

struct DevShellViewportActions {
    uint32_t desired_vp_w{960};
    uint32_t desired_vp_h{640};
    float orbit_yaw_delta{0.0f};   // radians, accumulated since last T3 frame
    float orbit_pitch_delta{0.0f}; // radians
    float pan_dx{0.0f};            // world units, scaled by camera distance on T3
    float pan_dy{0.0f};
    float zoom_delta{0.0f};       // log-distance step: distance *= exp(-zoom_delta) (wheel up = closer)
    float request_focal_mm{0.0f}; // > 0: apply focal length (24 mm sensor), keeping subject framing
    bool reset_camera{false};
    bool show_bones{true};
    bool wireframe{false};
    bool show_skeleton_heatmap{false};
    bool show_rig_colliders{false};
    bool show_checkerboard{true};
    bool show_orbit_orb{false};
    bool rt_enabled{true};
    uint8_t lighting_mode{1}; // 0 = Traditional, 1 = Hybrid RT, 2 = 2.5D Path Tracing
    uint8_t debug_view{0};
    uint8_t denoiser_kind{0};
    uint8_t render_backend{0}; // 0 = Vulkan, 1 = DirectX 11, 2 = DirectX 12 (applied by T3 on change)
    float dominant_key_azimuth_deg{32.0f};
    float dominant_key_elevation_deg{26.0f};
    float dominant_key_intensity{1.00f};
    bool quick_key_orbit_dirty{false};
    float live2d_flatness{0.75f};
    float bloom_intensity{0.06f};
    float outline_scale{1.0f};
    float face_normal_flatten{0.85f};
    float neck_seam_blend{1.00f};
    float cel_band_sharpness{0.025f};
    float shadow_warm_cool_shift{0.75f};
    float iro_tsuke_strength{0.85f};
    float crease_strength{0.75f};
    float hair_highlight_intensity{0.85f};
    float rim_step_strength{0.65f};
    float indirect_bleed_strength{0.55f};
    float rim_depth_offset_px{2.8f};
    bool filter_2d_pro{true};
    float filter_kuwahara_strength{0.55f};
    float filter_dog_ink_boost{0.60f};
    bool filter_paper_warmth{true};
    float filter_paper_warmth_strength{0.25f};
    float eye_catchlight_intensity{0.85f};
    float eye_catchlight_size{1.00f};
    bool crease_disguise_enabled{false};
    float crease_disguise_strength{0.70f};
    bool circadian_enabled{false};
    int32_t circadian_timezone_idx{0};
    int32_t circadian_time_override_min{-1};
    float satsuei_light_wrap{0.22f};
    float satsuei_halation{0.18f};
    float satsuei_backscatter{0.55f};
    float holo_avatar_contrast{1.04f};
    float holo_bg_exposure{1.00f};
    bool npu_bounds_enabled{true};
    bool npu_anti_clip_enabled{true};
    std::array<float, 3> npu_bounds_half{0.35f, 0.22f, 0.30f};
    float npu_soft_elasticity{0.35f};
    float npu_max_pitch_deg{42.0f};
    float npu_anti_clip_margin{0.035f};
    bool npu_bounds_dirty{true};
    uint8_t target_fps_mode{1};
    bool vsync_enabled{false};
    bool background_eco_mode{true};
    float render_resolution_scale{1.0f};
    uint8_t aa_mode{1};
    std::array<float, 3> avatar_pos{0.0f, 0.0f, 0.0f};
    float avatar_rot_y_deg{0.0f};
    float avatar_scale{1.0f};
    bool lighting_dirty{false};
    bool request_bake_face_sdf{false};
    bool request_denoiser_apply{false};
    const char* request_load_path{nullptr};
    bool request_cancel_load{false};
    bool request_calibrate_neutral{false};
    bool request_toggle_camera_source{false};
    bool request_set_camera_active{false};
    bool target_camera_active{true};
    bool request_set_synthetic_camera{false};
    bool target_synthetic_camera{false};
    bool request_select_camera{false};
    int32_t select_camera_fps{60};
    std::array<char, 256> select_camera_link{};
    int32_t request_ep_override{-99}; // -99 = no-op, -1 = Auto, >=0 = EpTier enum

    // Phase 5 Output & Audio-DSP Sync controls
    uint8_t output_res_preset{1}; // 0=720p, 1=1080p, 2=1440p, 3=4K
    uint32_t output_fps{60};
    bool output_premul_alpha{true};
    uint8_t output_non_alpha_bg{0}; // 0=ChromaGreen, 1=Black, 2=Custom
    std::array<float, 3> output_custom_bg{0.0f, 177.0f / 255.0f, 64.0f / 255.0f};
    bool spout_enabled{true};
    bool spout_cpu_fallback{false};
    bool vcam_enabled{true};
    bool ndi_enabled{false};
    uint8_t ndi_format{0}; // 0=BGRA straight, 1=UYVA straight
    bool osc_enabled{true};
    float dsp_user_offset_ms{0.0f};
    bool output_config_dirty{false};
    bool audio_assist_enabled{true};
    float mic_gain{1.25f};
    float mic_noise_gate_db{-48.0f};
    bool audio_lipsync_dirty{false};
    bool request_select_mic{false};
    std::array<char, 192> select_mic_id{};

    // Viewport aspect lock, Framing Preset smooth transitions & Hand tracking toggle
    core::ViewportAspectMode viewport_aspect_mode{core::ViewportAspectMode::FitAuto};
    float camera_transition_speed_s{0.22f};
    bool request_apply_camera_preset{false};
    int32_t request_builtin_preset{-1}; // 0..5 = Ctrl+1..6 built-in, -1 = custom target below
    float target_cam_x{0.0f};
    float target_cam_y{0.85f};
    float target_cam_z{0.0f};
    float target_cam_yaw_rad{0.28f};
    float target_cam_pitch_rad{0.06f};
    float target_cam_distance{2.0f};
    float target_cam_fovy_rad{0.2805f};
    bool target_preset_has_avatar_xform{false};
    std::array<float, 3> target_preset_avatar_pos{0.0f, 0.0f, 0.0f};
    float target_preset_avatar_rot_y_deg{0.0f};
    float target_preset_avatar_scale{1.0f};
    bool request_set_hand_tracking{false};
    bool target_hand_tracking_enabled{false};

    // Phase 2 Lighting Tab & Dual-Viewport Stage synchronization
    bool request_stage_render{false};
    uint32_t desired_stage_w{960};
    uint32_t desired_stage_h{640};
    uint32_t desired_quad_top_w{480};
    uint32_t desired_quad_top_h{320};
    uint32_t desired_quad_right_w{480};
    uint32_t desired_quad_right_h{320};
    renderer::PreviewCamera stage_camera{};
    renderer::StageDiagnosticMode stage_diag_mode{renderer::StageDiagnosticMode::Lit};
    bool stage_show_schematic{true};
    bool stage_show_grid{true};
    bool stage_preview_25d_flatten{false};
    bool stage_fast_panning{false};
    int32_t stage_selected_light_idx{0};
    bool stage_quad_view{false};
    renderer::PreviewCamera quad_top_camera{};
    renderer::PreviewCamera quad_right_camera{};
    renderer::LightingRig* mutable_lighting_rig{nullptr};
};

/**
 * Precision Dark Studio UI Shell (`AETHER-TS-001` §12).
 * Delegates all 5-zone panels, single-source `ParamStore`, UndoStack, Command Palette,
 * Hardware Optimizer Wizard, Node Canvas, and Toast notifications to `StudioWorkspace`.
 */
class DevShell {
  public:
    DevShell();
    ~DevShell();
    DevShell(const DevShell&) = delete;
    DevShell& operator=(const DevShell&) = delete;
    DevShell(DevShell&&) noexcept;
    DevShell& operator=(DevShell&&) noexcept;

    void init(const RuntimePaths& paths, const HardwareInventory& hw, float dpi_scale,
              const char* initial_avatar_path = nullptr, bool allow_disk_config = true,
              core::SessionManager* session_mgr = nullptr);
    void shutdown();
    void drain_logs() noexcept;

    void render(const rhi::RhiDevice& rhi, const TelemetrySnapshot& telemetry, const DevShellInputEcho& input_echo,
                const ImGuiArenaAllocator& arena, bool ffi_layout_ok, const DevShellAvatarView& avatar,
                const DevShellTrackingView& tracking, const DevShellRenderView& render_view,
                const DevShellOutputView& output_view, DevShellViewportActions& actions);

    void render_studio(double now_s, const rhi::RhiDevice& rhi, const TelemetrySnapshot& telemetry,
                       const DevShellInputEcho& input_echo, const ImGuiArenaAllocator& arena, bool ffi_layout_ok,
                       const DevShellAvatarView& avatar, const DevShellTrackingView& tracking,
                       const DevShellRenderView& render_view, const DevShellOutputView& output_view,
                       StudioActions& actions);

    [[nodiscard]] StudioMode mode() const noexcept;
    [[nodiscard]] StudioWorkspace& workspace() noexcept { return *workspace_; }
    [[nodiscard]] const StudioWorkspace& workspace() const noexcept { return *workspace_; }

  private:
    std::unique_ptr<StudioWorkspace> workspace_;
};

} // namespace aether::ui
