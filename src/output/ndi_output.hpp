#pragma once

#include "core/expected.hpp"
#include "output/output_config.hpp"
#include "rhi/rhi_device.hpp"

#include <array>
#include <cstdint>

namespace aether::output {

// Binary-compatible NDI 6 C ABI declarations (`§11.4`, ADR-0011; zero external headers bundled).
inline constexpr uint32_t kNdiFourCcBgra = 0x41524742u; // 'BGRA' (straight alpha)
inline constexpr uint32_t kNdiFourCcUyva = 0x41565955u; // 'UYVA' (UYVY 4:2:2 + 8-bit alpha plane)

struct NdiSendCreateAbi {
    const char* ndi_name{nullptr};
    const char* groups{nullptr};
    bool clock_video{false};
    bool clock_audio{false};
};

struct NdiVideoFrameV2Abi {
    int32_t xres{0};
    int32_t yres{0};
    uint32_t fourcc{kNdiFourCcBgra};
    int32_t frame_rate_n{60000};
    int32_t frame_rate_d{1000};
    float picture_aspect_ratio{16.0f / 9.0f};
    int32_t frame_format_type{1}; // NDIlib_frame_format_type_progressive
    int64_t timecode{0};
    uint8_t* p_data{nullptr};
    int32_t line_stride_in_bytes{0};
    const char* p_metadata{nullptr};
    int64_t timestamp{0};
};

struct NdiDispatchTable {
    bool (*initialize)(){nullptr};
    void (*destroy)(){nullptr};
    void* (*send_create)(const NdiSendCreateAbi* create_settings){nullptr};
    void (*send_destroy)(void* instance){nullptr};
    void (*send_video_async_v2)(void* instance, const NdiVideoFrameV2Abi* video_data){nullptr};
};

/**
 * NDI 6 Network Stream Sender (`§11.4`).
 * Dynamically probes `Processing.NDI.Lib.x64.dll` via `NDI_RUNTIME_DIR_V6` (or accepts an injectable
 * `NdiDispatchTable` for CI testing). Maintains double-buffered host frame allocations so
 * `NDIlib_send_send_video_async_v2` keeps the previous frame alive until the next submission.
 * Always produces straight alpha (`BGRA` / `UYVA`) with `clock_video = false`.
 */
class NdiOutput {
  public:
    NdiOutput() = default;
    ~NdiOutput() { shutdown(); }

    NdiOutput(const NdiOutput&) = delete;
    NdiOutput& operator=(const NdiOutput&) = delete;

    [[nodiscard]] Expected<void> init(rhi::RhiDevice& device, uint32_t width, uint32_t height,
                                      NdiVideoFormat format = NdiVideoFormat::BgraStraight,
                                      const NdiDispatchTable* test_override = nullptr);
    [[nodiscard]] Expected<void> resize(uint32_t width, uint32_t height);
    void shutdown() noexcept;

    /** Encodes/submits `src_bgra_straight` asynchronously (double-buffered host memory, zero per-frame allocs). */
    [[nodiscard]] Expected<void> send_frame(VkImage src_bgra_straight, uint32_t width, uint32_t height, uint32_t fps,
                                            uint64_t present_qpc_us, uint64_t wait_timeline_value) noexcept;

    /** Utility converting straight BGRA rows into 2-plane `UYVA` (`UYVY` 4:2:2 + 8-bit Alpha plane, BT.709). */
    static void pack_bgra_straight_to_uyva(const uint8_t* bgra, uint32_t width, uint32_t height,
                                           uint8_t* out_uyva) noexcept;

    [[nodiscard]] static bool is_runtime_dll_present();

    [[nodiscard]] bool is_active() const noexcept { return sender_ != nullptr; }
    [[nodiscard]] uint64_t frames_sent() const noexcept { return frames_sent_; }
    [[nodiscard]] double last_send_ms() const noexcept { return last_send_ms_; }
    [[nodiscard]] const char* sender_name() const noexcept { return sender_name_.data(); }

  private:
    Expected<void> allocate_buffers(uint32_t width, uint32_t height);
    void free_buffers() noexcept;

    rhi::RhiDevice* device_{nullptr};
    void* dll_module_{nullptr};
    bool initialized_ndi_lib_{false};
    NdiDispatchTable api_{};
    void* sender_{nullptr};
    NdiVideoFormat format_{NdiVideoFormat::BgraStraight};
    uint32_t width_{0};
    uint32_t height_{0};
    std::array<char, 128> sender_name_{};

    std::array<rhi::RhiBuffer, 2> host_staging_{};
    std::array<uint8_t*, 2> uyva_buffers_{nullptr, nullptr};
    size_t uyva_bytes_{0};
    VkCommandPool cmd_pool_{VK_NULL_HANDLE};
    std::array<VkCommandBuffer, 2> cmds_{};
    VkFence fence_{VK_NULL_HANDLE};

    uint32_t buf_idx_{0};
    uint64_t frames_sent_{0};
    double last_send_ms_{0.0};
};

} // namespace aether::output
