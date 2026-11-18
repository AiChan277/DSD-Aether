#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace aether::ui {

enum class ParamType : uint8_t {
    Float = 0,
    Int = 1,
    Bool = 2,
    Enum = 3,
};

enum class ParamOwner : uint8_t {
    T1 = 0,
    T2 = 1,
    T3 = 2,
    T4 = 3,
    App = 4,
};

enum ParamFlags : uint32_t {
    PARAM_FLAG_NONE = 0u,
    PARAM_FLAG_PERSIST = 1u << 0,
    PARAM_FLAG_PROJECT_SCOPE = 1u << 1,
    PARAM_FLAG_APP_SCOPE = 1u << 2,
    PARAM_FLAG_OSC_EXPOSED = 1u << 3,
    PARAM_FLAG_GRAPH_EXPOSABLE = 1u << 4,
    PARAM_FLAG_REQUIRES_RESTART = 1u << 5,
    PARAM_FLAG_UNDOABLE = 1u << 6,
};

enum class ParamId : uint16_t {
    AvatarPosX = 0,
    AvatarPosY,
    AvatarPosZ,
    AvatarRotX,
    AvatarRotY,
    AvatarRotZ,
    AvatarScale,

    SimSpringStiffness,
    SimSpringDamping,
    SimGain,
    SimIkWeight,

    RenderFlatness,
    RenderLightScale,
    RenderKeyAzimuthDeg,
    RenderKeyElevationDeg,
    RenderKeyIntensity,
    RenderFillIntensity,
    RenderRimIntensity,
    RenderOutlineScale,
    RenderBloomIntensity,
    RenderRtEnabled,
    RenderDenoiser,
    RenderDebugView,
    RenderBackend,
    RenderTargetFps,
    RenderVsync,
    RenderBackgroundEco,
    RenderResolutionScalePct,
    RenderAaMode,
    RenderFaceNormalFlatten,
    RenderCelBandSharpness,
    RenderShadowWarmCool,
    RenderIroTsukeStrength,
    RenderCreaseStrength,
    RenderHairHighlight,
    RenderRimStepStrength,
    RndDebugForceDeviceLost,

    TrackingEpMode,
    TrackingSmoothingCutoffHz,
    TrackingCameraFps,
    TrackingAntiFlicker,
    TrackingExposureLock,
    TrackingSyntheticCamera,
    TrackingAudioAssist,
    TrackingMicGain,
    TrackingMicNoiseGate,
    TrackingHandEnabled,

    OutputResolution,
    OutputFps,
    OutputPremulAlpha,
    OutputNonAlphaBg,
    OutputSpoutEnabled,
    OutputSpoutCpuFallback,
    OutputVcamEnabled,
    OutputNdiEnabled,
    OutputNdiFormat,
    OutputOscEnabled,
    OutputWsEnabled,
    DspUserOffsetMs,

    AppUiScalePct,
    AppReduceMotion,
    AppHighContrast,
    AppTelemetryPaused,
    AppCheckerboardBg,
    AppOrbitOrbVisible,
    AppSkeletonHeatmap,
    AppViewportAspect,
    AppTrackingHudVisible,
    AppTrackingHudCollapsed,
    AppCameraTransitionSpeed,

    Count,
};

struct ParamSpec {
    ParamId pid;
    const char* id;
    const char* group;
    const char* label;
    const char* tooltip;
    const char* unit;
    ParamType type;
    ParamOwner owner;
    double default_val;
    double min_val;
    double max_val;
    double step;
    uint8_t precision;
    const char* enum_labels;
    uint32_t flags;
};

inline constexpr size_t kParamCount = static_cast<size_t>(ParamId::Count);

// Single authoritative parameter registry (`REQ-UI-010`, §12.12, Appendix D).
// `tools/gen_params_doc.py` parses this exact table to emit and verify `docs/PARAMS.md`.
inline constexpr std::array<ParamSpec, kParamCount> kParamSpecs = {{
    ParamSpec{ParamId::AvatarPosX, "avatar.transform.pos_x", "Avatar", "Position X",
              "Avatar world-space lateral X translation.", "m", ParamType::Float, ParamOwner::T2, 0.0, -5.0, 5.0, 0.01,
              3, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::AvatarPosY, "avatar.transform.pos_y", "Avatar", "Position Y",
              "Avatar world-space vertical Y translation.", "m", ParamType::Float, ParamOwner::T2, 0.0, -5.0, 5.0, 0.01,
              3, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::AvatarPosZ, "avatar.transform.pos_z", "Avatar", "Position Z",
              "Avatar world-space depth Z translation.", "m", ParamType::Float, ParamOwner::T2, 0.0, -5.0, 5.0, 0.01, 3,
              nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::AvatarRotX, "avatar.transform.rot_x", "Avatar", "Rotation X",
              "Avatar root pitch rotation around X.", "deg", ParamType::Float, ParamOwner::T2, 0.0, -180.0, 180.0, 0.5,
              1, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::AvatarRotY, "avatar.transform.rot_y", "Avatar", "Rotation Y",
              "Avatar root yaw rotation around Y.", "deg", ParamType::Float, ParamOwner::T2, 0.0, -180.0, 180.0, 0.5, 1,
              nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::AvatarRotZ, "avatar.transform.rot_z", "Avatar", "Rotation Z",
              "Avatar root roll rotation around Z.", "deg", ParamType::Float, ParamOwner::T2, 0.0, -180.0, 180.0, 0.5,
              1, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::AvatarScale, "avatar.transform.scale", "Avatar", "Uniform Scale",
              "Uniform scale multiplier applied to the avatar root.", "x", ParamType::Float, ParamOwner::T2, 1.0, 0.25,
              4.0, 0.01, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},

    ParamSpec{ParamId::SimSpringStiffness, "sim.spring_stiffness", "Physics", "Spring Stiffness",
              "Global multiplier on SpringBone stiffness for hair and cloth.", "x", ParamType::Float, ParamOwner::T2,
              1.0, 0.1, 3.0, 0.05, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::SimSpringDamping, "sim.spring_damping", "Physics", "Spring Damping",
              "Global multiplier on SpringBone velocity damping.", "x", ParamType::Float, ParamOwner::T2, 1.0, 0.1, 3.0,
              0.05, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::SimGain, "sim.gain", "Physics", "Expression Gain",
              "Master multiplier on driven tracking expressions.", "x", ParamType::Float, ParamOwner::T2, 1.0, 0.0, 2.0,
              0.02, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::SimIkWeight, "sim.ik_weight", "Physics", "IK Blend Weight",
              "Blend weight between solved IK posture and rest pose.", "", ParamType::Float, ParamOwner::T2, 1.0, 0.0,
              1.0, 0.02, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},

    ParamSpec{ParamId::RenderFlatness, "render.flatness", "Render", "Live2D Flatness",
              "View-space Z compression factor for the 2.5D NPR look.", "", ParamType::Float, ParamOwner::T3, 0.75, 0.0,
              1.0, 0.01, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderLightScale, "render.light_scale", "Render", "Light Scale",
              "Global intensity multiplier applied to scene lights.", "x", ParamType::Float, ParamOwner::T3, 1.0, 0.1,
              4.0, 0.05, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderKeyAzimuthDeg, "render.key_azimuth_deg", "Render", "Key Azimuth",
              "Horizontal azimuth angle of the primary key light.", "deg", ParamType::Float, ParamOwner::T3, 32.0,
              -180.0, 180.0, 1.0, 1, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderKeyElevationDeg, "render.key_elevation_deg", "Render", "Key Elevation",
              "Vertical elevation angle of the primary key light.", "deg", ParamType::Float, ParamOwner::T3, 26.0,
              -85.0, 85.0, 1.0, 1, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderKeyIntensity, "render.key_intensity", "Render", "Key Intensity",
              "Key light illuminance contribution.", "EV", ParamType::Float, ParamOwner::T3, 1.00, 0.0, 6.0, 0.05, 2,
              nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderFillIntensity, "render.fill_intensity", "Render", "Fill Intensity",
              "Soft fill light contribution.", "EV", ParamType::Float, ParamOwner::T3, 0.28, 0.0, 4.0, 0.05, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderRimIntensity, "render.rim_intensity", "Render", "Rim Intensity",
              "Backlight rim accent intensity.", "EV", ParamType::Float, ParamOwner::T3, 0.35, 0.0, 6.0, 0.05, 2,
              nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderOutlineScale, "render.outline_scale", "Render", "Outline Scale",
              "Multiplier on screen-space constant NPR ink outline thickness.", "x", ParamType::Float, ParamOwner::T3,
              1.0, 0.0, 3.0, 0.05, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderBloomIntensity, "render.bloom_intensity", "Render", "Premul Bloom",
              "Premultiplied alpha-preserving bloom strength.", "", ParamType::Float, ParamOwner::T3, 0.06, 0.0, 1.5,
              0.02, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_GRAPH_EXPOSABLE |
                  PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderRtEnabled, "render.rt_enabled", "Render", "Ray Tracing",
              "Enable inline ray-query shadows, hair occlusion, and bounce.", "", ParamType::Bool, ParamOwner::T3, 1.0,
              0.0, 1.0, 1.0, 0, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderDenoiser, "render.denoiser", "Render", "Denoiser Backend",
              "Ray-traced shadow denoiser backend selection.", "", ParamType::Enum, ParamOwner::T3, 0.0, 0.0, 5.0, 1.0,
              0, "Auto|Built-in SVGF-Lite|NVIDIA NRD SIGMA|Intel OIDN XMX|AMD FidelityFX|NVIDIA OptiX",
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderDebugView, "render.debug_view", "Render", "Debug View",
              "Viewport shader diagnostic visualization pass.", "", ParamType::Enum, ParamOwner::T3, 0.0, 0.0, 13.0,
              1.0, 0,
              "Final Composite|Albedo|True Normals|Proxy Normals|Z-Flatten Weight|SDF Face Shadow|Hair Stencil|Ray "
              "Query Shadow Mask|RT Shadow Denoised|Rim Occlusion|Bounce|Inverted Hull Outline|Motion Vectors|Overdraw",
              PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderBackend, "render.backend", "Render", "Render Backend",
              "Avatar renderer. DirectX 11 is a compatibility path without ray tracing; UI and outputs stay on Vulkan.",
              "", ParamType::Enum, ParamOwner::T3, 0.0, 0.0, 1.0, 1.0, 0, "Vulkan|DirectX 11",
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderTargetFps, "render.target_fps", "Render", "Target FPS",
              "Studio display frame rate cap (30/60/120/Uncapped) to control laptop GPU load and thermals.", "",
              ParamType::Enum, ParamOwner::T3, 1.0, 0.0, 3.0, 1.0, 0, "30 FPS|60 FPS|120 FPS|Uncapped",
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderVsync, "render.vsync", "Render", "VSync",
              "Lock presentation to display vertical blanking (FIFO present mode).", "", ParamType::Bool,
              ParamOwner::T3, 0.0, 0.0, 1.0, 1.0, 0, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderBackgroundEco, "render.background_eco", "Render", "Background Eco",
              "Throttle Studio rendering to 30 FPS when unfocused and not On Air.", "", ParamType::Bool,
              ParamOwner::T3, 1.0, 0.0, 1.0, 1.0, 0, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderResolutionScalePct, "render.resolution_scale_pct", "Render", "Render Scale",
              "Internal viewport rendering resolution scale relative to viewport panel size.", "%", ParamType::Int,
              ParamOwner::T3, 100.0, 50.0, 200.0, 5.0, 0, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderAaMode, "render.aa_mode", "Render", "Anti-Aliasing",
              "Post-composite silhouette and inner ink anti-aliasing mode.", "", ParamType::Enum, ParamOwner::T3, 1.0,
              0.0, 2.0, 1.0, 0, "Off|FXAA|TAA-Lite",
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderFaceNormalFlatten, "render.face_normal_flatten", "Render", "Face Flatness",
              "Blends face and skin shading normals toward the camera plane for crisp 2D Live2D shading.", "",
              ParamType::Float, ParamOwner::T3, 0.85, 0.0, 1.0, 0.02, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderCelBandSharpness, "render.cel_band_sharpness", "Render", "Cel Feather",
              "Shadow transition feather width (smaller produces harder hand-inked cel bands).", "", ParamType::Float,
              ParamOwner::T3, 0.025, 0.005, 0.25, 0.005, 3, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderShadowWarmCool, "render.shadow_warm_cool", "Render", "Shadow Hue",
              "Chromatic shadow hue shifting (warm coral skin shadows and cool periwinkle hair/cloth shadows).", "",
              ParamType::Float, ParamOwner::T3, 0.75, 0.0, 1.0, 0.02, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderIroTsukeStrength, "render.iro_tsuke_strength", "Render", "Iro-Tsuke Ink",
              "Chromatic ink tinting on silhouette outlines matched to local base colors.", "", ParamType::Float,
              ParamOwner::T3, 0.85, 0.0, 1.0, 0.02, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderCreaseStrength, "render.crease_strength", "Render", "Crease Ink",
              "3x3 Sobel inner silhouette and garment crease ink line strength.", "", ParamType::Float, ParamOwner::T3,
              0.75, 0.0, 1.0, 0.02, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderHairHighlight, "render.hair_highlight", "Render", "Hair Highlight",
              "Stepped 2D anime hair highlight ribbon intensity.", "", ParamType::Float, ParamOwner::T3, 0.85, 0.0,
              1.5, 0.02, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RenderRimStepStrength, "render.rim_step_strength", "Render", "Rim Cutout",
              "Hard-cut 2D anime rim-light cutout intensity.", "", ParamType::Float, ParamOwner::T3, 0.65, 0.0, 1.5,
              0.02, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::RndDebugForceDeviceLost, "rnd.debug.force_device_lost", "Diagnostics", "Force Device Lost",
              "Injects a synthetic VK_ERROR_DEVICE_LOST recovery test.", "", ParamType::Bool, ParamOwner::T3, 0.0, 0.0,
              1.0, 1.0, 0, nullptr, PARAM_FLAG_NONE},

    ParamSpec{ParamId::TrackingEpMode, "tracking.ep_mode", "Tracking", "Execution Provider",
              "Preferred ORT inference tier (changes are app settings).", "", ParamType::Enum, ParamOwner::T1, 0.0, 0.0,
              4.0, 1.0, 0, "Auto|NPU (OpenVINO)|dGPU (DirectML)|iGPU (DirectML)|CPU",
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::TrackingSmoothingCutoffHz, "tracking.smoothing_cutoff_hz", "Tracking", "Filter Cutoff",
              "1-Euro minimum cutoff frequency balancing latency and jitter.", "Hz", ParamType::Float, ParamOwner::T2,
              1.2, 0.2, 10.0, 0.1, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::TrackingCameraFps, "tracking.camera_fps", "Tracking", "Camera Target FPS",
              "Preferred camera capture frame rate.", "fps", ParamType::Int, ParamOwner::T1, 60.0, 30.0, 60.0, 30.0, 0,
              nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::TrackingAntiFlicker, "tracking.anti_flicker", "Tracking", "Anti-Flicker",
              "Powerline lighting anti-flicker frequency.", "", ParamType::Enum, ParamOwner::T1, 2.0, 0.0, 2.0, 1.0, 0,
              "Off|50 Hz|60 Hz", PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::TrackingExposureLock, "tracking.exposure_lock", "Tracking", "Lock Exposure",
              "Locks webcam auto-exposure for stable tracking FPS.", "", ParamType::Bool, ParamOwner::T1, 0.0, 0.0, 1.0,
              1.0, 0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::TrackingSyntheticCamera, "tracking.synthetic_camera", "Tracking", "Synthetic Feed",
              "Use deterministic synthetic camera when testing or offline.", "", ParamType::Bool, ParamOwner::T1, 0.0,
              0.0, 1.0, 1.0, 0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::TrackingAudioAssist, "tracking.audio_assist", "Tracking", "Audio Lip-Sync",
              "Fuses low-latency WASAPI microphone vocal envelope and formants with webcam lip tracking.", "",
              ParamType::Bool, ParamOwner::T2, 1.0, 0.0, 1.0, 1.0, 0, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::TrackingMicGain, "tracking.mic_gain", "Tracking", "Mic Gain",
              "Input gain multiplier applied to WASAPI microphone speech envelope.", "x", ParamType::Float,
              ParamOwner::T2, 1.25, 0.10, 4.00, 0.05, 2, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::TrackingMicNoiseGate, "tracking.mic_noise_gate", "Tracking", "Mic Noise Gate",
              "Minimum vocal loudness threshold before microphone triggers mouth opening.", "dB", ParamType::Float,
              ParamOwner::T2, -48.0, -72.0, -18.0, 1.0, 1, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::TrackingHandEnabled, "tracking.hand_enabled", "Tracking", "Hand Tracking",
              "Enables M1 detailed 21-keypoint hand and finger tracking with body collision avoidance.", "",
              ParamType::Bool, ParamOwner::T1, 0.0, 0.0, 1.0, 1.0, 0, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE | PARAM_FLAG_UNDOABLE},

    ParamSpec{ParamId::OutputResolution, "output.resolution", "Output", "Resolution Preset",
              "Alpha output stream pixel dimensions.", "", ParamType::Enum, ParamOwner::T4, 1.0, 0.0, 3.0, 1.0, 0,
              "1280x720|1920x1080|2560x1440|3840x2160",
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::OutputFps, "output.fps", "Output", "Output Frame Rate",
              "Target broadcast output frame rate (30, 60, or 120).", "fps", ParamType::Int, ParamOwner::T4, 60.0, 30.0,
              120.0, 30.0, 0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::OutputPremulAlpha, "output.premul_alpha", "Output", "Premultiplied Alpha",
              "Outputs straight vs premultiplied alpha matte.", "", ParamType::Bool, ParamOwner::T4, 1.0, 0.0, 1.0, 1.0,
              0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::OutputNonAlphaBg, "output.non_alpha_bg", "Output", "Opaque Backdrop",
              "Background fill used for non-alpha virtual camera pins.", "", ParamType::Enum, ParamOwner::T4, 0.0, 0.0,
              2.0, 1.0, 0, "Chroma Green|Black|Custom",
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::OutputSpoutEnabled, "output.spout_enabled", "Output", "Spout2 Sender",
              "Enables zero-copy GPU texture sharing via Spout2.", "", ParamType::Bool, ParamOwner::T4, 1.0, 0.0, 1.0,
              1.0, 0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::OutputSpoutCpuFallback, "output.spout_cpu_fallback", "Output", "Spout CPU Sharing",
              "Forces Spout2 CPU staging when cross-GPU sharing fails.", "", ParamType::Bool, ParamOwner::T4, 0.0, 0.0,
              1.0, 1.0, 0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::OutputVcamEnabled, "output.vcam_enabled", "Output", "Virtual Camera",
              "Publishes frames to Aether DirectShow Virtual Cam.", "", ParamType::Bool, ParamOwner::T4, 1.0, 0.0, 1.0,
              1.0, 0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::OutputNdiEnabled, "output.ndi_enabled", "Output", "NDI Stream",
              "Publishes alpha video over local network via NDI 6.", "", ParamType::Bool, ParamOwner::T4, 0.0, 0.0, 1.0,
              1.0, 0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::OutputNdiFormat, "output.ndi_format", "Output", "NDI Pixel Format",
              "NDI frame pixel format (BGRA or UYVA).", "", ParamType::Enum, ParamOwner::T4, 0.0, 0.0, 1.0, 1.0, 0,
              "BGRA Straight|UYVA Straight", PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::OutputOscEnabled, "output.osc_enabled", "Output", "OSC Telemetry",
              "Sends 127.0.0.1:9001 UDP latency, FPS, and EP metrics.", "", ParamType::Bool, ParamOwner::T4, 1.0, 0.0,
              1.0, 1.0, 0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::OutputWsEnabled, "output.ws_enabled", "Output", "WebSocket Control",
              "Enables localhost ws://127.0.0.1:19190/v1 automation API.", "", ParamType::Bool, ParamOwner::App, 0.0,
              0.0, 1.0, 1.0, 0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::DspUserOffsetMs, "dsp.user_offset_ms", "DSP", "DSP Audio Offset",
              "Manual fine offset relative to measured M2P lip-sync delay.", "ms", ParamType::Float, ParamOwner::T4,
              0.0, -150.0, 150.0, 0.5, 1, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_OSC_EXPOSED | PARAM_FLAG_UNDOABLE},

    ParamSpec{ParamId::AppUiScalePct, "app.ui_scale_pct", "App", "UI Scale",
              "Global Studio UI scale percentage (75-200%).", "%", ParamType::Int, ParamOwner::App, 100.0, 75.0, 200.0,
              25.0, 0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::AppReduceMotion, "app.reduce_motion", "App", "Reduce Motion",
              "Disables animated UI transitions and easing.", "", ParamType::Bool, ParamOwner::App, 0.0, 0.0, 1.0, 1.0,
              0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::AppHighContrast, "app.high_contrast", "App", "High Contrast",
              "Increases text and border contrast ratios per §12.14.", "", ParamType::Bool, ParamOwner::App, 0.0, 0.0,
              1.0, 1.0, 0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE},
    ParamSpec{ParamId::AppTelemetryPaused, "app.telemetry_paused", "App", "Pause Telemetry",
              "Freezes live telemetry sparkline plots for inspection.", "", ParamType::Bool, ParamOwner::T3, 0.0, 0.0,
              1.0, 1.0, 0, nullptr, PARAM_FLAG_NONE},
    ParamSpec{ParamId::AppCheckerboardBg, "app.checkerboard_bg", "App", "Alpha Checkerboard",
              "Shows transparent checkerboard backdrop behind viewport.", "", ParamType::Bool, ParamOwner::T3, 1.0, 0.0,
              1.0, 1.0, 0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::AppOrbitOrbVisible, "app.orbit_orb_visible", "App", "Light Orbit Orb",
              "Shows the 112 px interactive lighting preview sphere.", "", ParamType::Bool, ParamOwner::T3, 1.0, 0.0,
              1.0, 1.0, 0, nullptr, PARAM_FLAG_PERSIST | PARAM_FLAG_PROJECT_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::AppSkeletonHeatmap, "app.skeleton_heatmap", "App", "Confidence Heatmap",
              "Renders per-joint solver weight heatmap overlay (Alt+C).", "", ParamType::Bool, ParamOwner::T3, 0.0, 0.0,
              1.0, 1.0, 0, nullptr, PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::AppViewportAspect, "app.viewport_aspect", "App", "Viewport Aspect",
              "Studio Viewport framing aspect mode without affecting 16:9 OBS broadcast output.", "", ParamType::Enum,
              ParamOwner::T3, 0.0, 0.0, 3.0, 1.0, 0, "Fit (Auto)|16:9|9:16|1:1",
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::AppTrackingHudVisible, "app.tracking_hud_visible", "App", "Tracking HUD",
              "Shows or hides the Viewport tracking preview overlay card while keeping tracking active.", "",
              ParamType::Bool, ParamOwner::T3, 1.0, 0.0, 1.0, 1.0, 0, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::AppTrackingHudCollapsed, "app.tracking_hud_collapsed", "App", "HUD Pill Mode",
              "Collapses the Tracking HUD webcam preview to a compact single-line status pill.", "", ParamType::Bool,
              ParamOwner::T3, 0.0, 0.0, 1.0, 1.0, 0, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE | PARAM_FLAG_UNDOABLE},
    ParamSpec{ParamId::AppCameraTransitionSpeed, "app.camera_transition_speed", "App", "Preset Ease",
              "Critically-damped transition duration when switching framing presets (0 = instant cut).", "s",
              ParamType::Float, ParamOwner::T3, 0.22, 0.00, 1.00, 0.02, 2, nullptr,
              PARAM_FLAG_PERSIST | PARAM_FLAG_APP_SCOPE | PARAM_FLAG_UNDOABLE},
}};

[[nodiscard]] constexpr const ParamSpec& param_spec(ParamId id) noexcept {
    return kParamSpecs[static_cast<size_t>(id)];
}

[[nodiscard]] std::span<const ParamSpec> all_param_specs() noexcept;
[[nodiscard]] const ParamSpec* find_param_by_id(std::string_view id) noexcept;
[[nodiscard]] double clamp_param_value(const ParamSpec& spec, double v) noexcept;
[[nodiscard]] const char* param_owner_name(ParamOwner owner) noexcept;
[[nodiscard]] const char* param_type_name(ParamType type) noexcept;

/** Maps `ParamId::TrackingEpMode` UI combo index (`0=Auto,1=NPU,2=dGPU,3=iGPU,4=CPU`) to `EpTier` override. */
[[nodiscard]] inline constexpr int32_t tracking_ep_mode_to_router_override(int mode_idx) noexcept {
    switch (mode_idx) {
    case 1:
        return 1; // EpTier::NpuOpenVino
    case 2:
        return 3; // EpTier::DirectMlDgpu
    case 3:
        return 2; // EpTier::DirectMlIgpu
    case 4:
        return 0; // EpTier::Cpu
    default:
        return -1; // Auto cascade
    }
}

class ParamStore {
  public:
    ParamStore() noexcept { reset_all_to_defaults(); }

    void reset_all_to_defaults() noexcept;
    void reset_project_to_defaults() noexcept;

    [[nodiscard]] double get(ParamId id) const noexcept { return values_[static_cast<size_t>(id)]; }
    [[nodiscard]] float get_float(ParamId id) const noexcept {
        return static_cast<float>(values_[static_cast<size_t>(id)]);
    }
    [[nodiscard]] int get_int(ParamId id) const noexcept { return static_cast<int>(values_[static_cast<size_t>(id)]); }
    [[nodiscard]] bool get_bool(ParamId id) const noexcept { return values_[static_cast<size_t>(id)] >= 0.5; }

    /** Sets parameter clamped to spec bounds; returns true if value changed. */
    bool set(ParamId id, double val) noexcept;
    bool set_by_id(std::string_view id, double val) noexcept;
    [[nodiscard]] bool is_modified(ParamId id) const noexcept;
    [[nodiscard]] uint64_t epoch() const noexcept { return epoch_; }

  private:
    std::array<double, kParamCount> values_{};
    uint64_t epoch_{1};
};

} // namespace aether::ui
