#pragma once

#include <array>
#include <cstdint>

namespace aether::output {

enum class OutputResolutionPreset : uint8_t {
    Hd720 = 0,
    Fhd1080 = 1,
    Qhd1440 = 2,
    Uhd4k = 3,
};

[[nodiscard]] constexpr uint32_t output_preset_width(OutputResolutionPreset p) noexcept {
    switch (p) {
    case OutputResolutionPreset::Hd720:
        return 1280u;
    case OutputResolutionPreset::Fhd1080:
        return 1920u;
    case OutputResolutionPreset::Qhd1440:
        return 2560u;
    case OutputResolutionPreset::Uhd4k:
        return 3840u;
    }
    return 1920u;
}

[[nodiscard]] constexpr uint32_t output_preset_height(OutputResolutionPreset p) noexcept {
    switch (p) {
    case OutputResolutionPreset::Hd720:
        return 720u;
    case OutputResolutionPreset::Fhd1080:
        return 1080u;
    case OutputResolutionPreset::Qhd1440:
        return 1440u;
    case OutputResolutionPreset::Uhd4k:
        return 2160u;
    }
    return 1080u;
}

[[nodiscard]] constexpr uint32_t clamp_output_fps_for_resolution(uint32_t w, uint32_t h,
                                                                 uint32_t requested_fps) noexcept {
    const uint32_t max_fps = (w >= 3840u || h >= 2160u) ? 60u : 120u;
    if (requested_fps < 30u) {
        return 30u;
    }
    return requested_fps > max_fps ? max_fps : requested_fps;
}

enum class SpoutSharingMode : uint8_t {
    GpuDefault = 0,
    CpuCompatibility = 1,
};

enum class NonAlphaBgPreset : uint8_t {
    ChromaGreen = 0, // #00B140
    Black = 1,
    Custom = 2,
};

enum class NdiVideoFormat : uint8_t {
    BgraStraight = 0,
    UyvaStraight = 1,
};

struct OutputConfig {
    OutputResolutionPreset resolution{OutputResolutionPreset::Fhd1080};
    uint32_t width{1920u};
    uint32_t height{1080u};
    uint32_t fps{60u};
    bool premul_alpha{true};

    NonAlphaBgPreset non_alpha_bg{NonAlphaBgPreset::ChromaGreen};
    std::array<float, 3> custom_bg_rgb{0.0f, 177.0f / 255.0f, 64.0f / 255.0f}; // #00B140

    bool spout_enabled{true};
    SpoutSharingMode spout_mode{SpoutSharingMode::GpuDefault};
    char spout_sender_name[64]{"Aether2.5D"};

    bool vcam_enabled{true};
    bool vcam_skip_when_no_reader{false};
    bool ndi_enabled{false};
    NdiVideoFormat ndi_format{NdiVideoFormat::BgraStraight};
    bool ndi_clock_video{false};

    bool osc_enabled{true};
    char osc_host[32]{"127.0.0.1"};
    uint16_t osc_port{9001};

    float dsp_user_offset_ms{0.0f};
};

struct OutputRuntimeStatus {
    bool spout_active{false};
    bool spout_using_cpu_fallback{false};
    bool spout_adapter_mismatch{false};
    uint64_t spout_frames_published{0};
    uint64_t spout_skipped_mutex{0};
    double spout_publish_ms{0.0};

    bool vcam_active{false};
    bool vcam_using_ext_host_mem{false};
    bool vcam_rgb_pin_connected{false};
    bool vcam_filter_active{false};
    uint64_t vcam_frames_published{0};
    double vcam_dma_ms{0.0};

    bool ndi_active{false};
    bool ndi_runtime_available{false};
    uint64_t ndi_frames_sent{0};
    double ndi_send_ms{0.0};

    bool osc_active{false};
    uint64_t osc_packets_sent{0};

    bool dsp_connected{false};
    bool dsp_voice_active{false};
    float dsp_input_rms_db{-96.0f};
    float dsp_reported_latency_ms{0.0f};
    float dsp_recommended_delay_ms{0.0f};
    float video_latency_ms{0.0f};
    float latency_jitter_ms{0.0f};
    uint64_t dsp_stale_count{0};
    uint64_t late_latch_us{0};
};

} // namespace aether::output
