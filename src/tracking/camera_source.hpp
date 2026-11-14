#pragma once

#include "aether_ffi_contract.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace aether::tracking {

enum class CameraPixelFormat : uint8_t {
    NV12 = 0,
    BGRA32 = 1,
};

struct CameraControls {
    bool manual_exposure{false};
    int32_t exposure_log2{-5};
    bool manual_focus{false};
    int32_t focus_value{0};
    bool manual_white_balance{false};
    int32_t white_balance_kelvin{5200};
    int32_t gain_percent{50};
};

struct CameraDeviceEntry {
    std::string friendly_name{};
    std::string symbolic_link{};
    uint32_t preferred_width{1280};
    uint32_t preferred_height{720};
    uint32_t preferred_fps{60};
};

struct PersistedCameraConfig {
    std::string selected_symlink{};
    uint32_t width{1280};
    uint32_t height{720};
    uint32_t fps{60};
    CameraControls controls{};
};

inline constexpr uint32_t kMaxCamWidth = 1920;
inline constexpr uint32_t kMaxCamHeight = 1080;
inline constexpr uint32_t kMaxNv12Bytes = kMaxCamWidth * kMaxCamHeight * 4U; // covers NV12 1080p and BGRA 1080p

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
/** Pre-allocated frame slot so capture callbacks & T1 never heap-allocate (`HR-01`, `HR-12`). */
struct alignas(64) CapturedFrameBuffer {
    uint64_t capture_timestamp_us{0};
    uint64_t sequence{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride_y{0};
    uint32_t stride_uv{0};
    CameraPixelFormat format{CameraPixelFormat::NV12};
    std::array<uint8_t, kMaxNv12Bytes> bytes{};
};

/**
 * Wait-free single-slot drop-oldest mailbox (`§8.2`, `HR-02`).
 * Producer overwrites whichever of the 3 static slots is neither `published` nor `consumed`,
 * then atomically publishes its index and signals `frame_event`.
 */
class LatestFrameSlot {
  public:
    LatestFrameSlot() noexcept;
    ~LatestFrameSlot();

    LatestFrameSlot(const LatestFrameSlot&) = delete;
    LatestFrameSlot& operator=(const LatestFrameSlot&) = delete;

    /** Producer borrows write buffer (zero lock, zero alloc). */
    [[nodiscard]] CapturedFrameBuffer& begin_write() noexcept;
    void end_write_and_publish(uint8_t written_idx) noexcept;
    [[nodiscard]] uint8_t current_write_index() const noexcept { return write_idx_; }

    /** Consumer blocks up to `timeout_ms` then steals newest published frame if available. */
    [[nodiscard]] bool wait_and_acquire_latest(uint32_t timeout_ms, const CapturedFrameBuffer*& out_frame) noexcept;

    [[nodiscard]] uint64_t dropped_count() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  private:
    std::array<CapturedFrameBuffer, 3> slots_{};
    uint8_t write_idx_{0};
    uint8_t read_idx_{1};
    std::atomic<uint8_t> ready_idx_{2};
    std::atomic<bool> has_new_{false};
    std::atomic<uint64_t> dropped_{0};
    void* frame_event_{nullptr};
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

class ICameraSource {
  public:
    virtual ~ICameraSource() = default;

    [[nodiscard]] virtual AetherStatus open() = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool read_latest_blocking(uint32_t timeout_ms,
                                                    const CapturedFrameBuffer*& out_frame) noexcept = 0;
    virtual AetherStatus apply_controls(const CameraControls& controls) noexcept = 0;
    [[nodiscard]] virtual uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual uint32_t height() const noexcept = 0;
    [[nodiscard]] virtual uint32_t fps() const noexcept = 0;
    [[nodiscard]] virtual bool is_synthetic() const noexcept = 0;
    [[nodiscard]] virtual std::string source_label() const = 0;
};

/** Enumerates active video capture devices without opening a video stream (`HR-12`). */
[[nodiscard]] std::vector<CameraDeviceEntry> enumerate_camera_devices();

/** Loads/saves `selected_symlink` + manual controls into `%LOCALAPPDATA%/Aether/camera_config.toml` (never pixels). */
[[nodiscard]] std::filesystem::path default_camera_config_path();
[[nodiscard]] AetherStatus load_persisted_camera_config(const std::filesystem::path& path,
                                                        PersistedCameraConfig& out_cfg);
[[nodiscard]] AetherStatus save_persisted_camera_config(const std::filesystem::path& path,
                                                        const PersistedCameraConfig& cfg);

[[nodiscard]] std::unique_ptr<ICameraSource> create_mf_camera_source(std::string symbolic_link, uint32_t width = 1280,
                                                                     uint32_t height = 720, uint32_t fps = 60);

[[nodiscard]] std::unique_ptr<ICameraSource>
create_file_camera_source(std::filesystem::path media_path, bool force_synthetic_pattern = false, uint32_t width = 1280,
                          uint32_t height = 720, uint32_t fps = 60, uint32_t synthetic_total_frames = 0);

} // namespace aether::tracking
