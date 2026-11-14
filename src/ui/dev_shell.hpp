#pragma once

#include "aether_ffi_contract.h"
#include "core/hw_enum.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/telemetry.hpp"
#include "rhi/rhi_device.hpp"
#include "ui/theme/precision_dark.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace aether::ui {

class StudioWorkspace;
struct StudioActions;

enum class StudioMode : uint8_t {
    LiveStudio = 0,
    Rigging = 1,
    Nodes = 2,
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
    VkDescriptorSet orbit_orb_texture{VK_NULL_HANDLE};
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
};

struct DevShellTrackingView {
    bool enabled{false};
    bool ready{false};
    bool using_synthetic_camera{true};
    bool roi_locked{false};
    uint32_t active_ep{AETHER_EP_CPU};
    int32_t selected_ep_override{-1}; // -1 = Auto
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
    bool show_orbit_orb{true};
    bool baked_face_sdf_applied{false};
    float key_azimuth_deg{32.0f};
    float key_elevation_deg{26.0f};
    float key_intensity{1.85f};
    float fill_intensity{0.55f};
    float rim_intensity{1.15f};
    float live2d_flatness{0.75f};
    float bloom_intensity{0.28f};
    float outline_scale{1.0f};
    float total_gpu_ms{0.0f};
    float rt_gpu_ms{0.0f};
    float denoise_gpu_ms{0.0f};
    float denoise_p95_120f_ms{0.0f};
    float npr_gpu_ms{0.0f};
    float post_gpu_ms{0.0f};
    float encode_gpu_ms{0.0f};
    bool timing_valid{false};
    uint32_t device_recovery_count{0};
    const char* last_recovery_msg{""};

    // Render backend & engine garbage collector (ADR-0016)
    uint8_t render_backend{0}; // 0 = Vulkan, 1 = DirectX 11
    const char* backend_detail{""};
    float dx11_render_ms{0.0f};
    uint64_t gc_retired_pending{0};
    uint64_t gc_reclaimed_total{0};
    uint64_t gc_bytes_reclaimed{0};
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
    float orbit_yaw_delta{0.0f};
    float orbit_pitch_delta{0.0f};
    float pan_dx{0.0f};
    float pan_dy{0.0f};
    float zoom_delta{0.0f};
    bool reset_camera{false};
    bool show_bones{true};
    bool wireframe{false};
    bool show_skeleton_heatmap{false};
    bool show_checkerboard{true};
    bool show_orbit_orb{true};
    bool rt_enabled{true};
    uint8_t debug_view{0};
    uint8_t denoiser_kind{0};
    uint8_t render_backend{0}; // 0 = Vulkan, 1 = DirectX 11 (applied by T3 when it differs from the active one)
    float key_azimuth_deg{32.0f};
    float key_elevation_deg{26.0f};
    float key_intensity{1.85f};
    float fill_intensity{0.55f};
    float rim_intensity{1.15f};
    float live2d_flatness{0.75f};
    float bloom_intensity{0.28f};
    float outline_scale{1.0f};
    bool lighting_dirty{false};
    bool request_bake_face_sdf{false};
    bool request_denoiser_apply{false};
    const char* request_load_path{nullptr};
    bool request_cancel_load{false};
    bool request_calibrate_neutral{false};
    bool request_toggle_camera_source{false};
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
              const char* initial_avatar_path = nullptr, bool allow_disk_config = true);
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
