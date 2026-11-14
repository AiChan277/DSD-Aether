#pragma once

#include "aether_ffi_contract.h"
#include "core/expected.hpp"
#include "renderer/asset_upload/gpu_avatar.hpp"
#include "rhi/rhi_device.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace aether::renderer {

enum class AssetJobState : uint8_t {
    Idle = 0,
    Loading = 1,
    ReadyForGpu = 2,
    Uploaded = 3,
    Cancelled = 4,
    Failed = 5,
};

/**
 * Lock-free asynchronous two-phase asset loader (`§5.6`, `HR-02`, `HR-05`).
 *
 * Worker calls Rust `aether_loader_open` / `query` / `fill` directly into a VMA
 * host-visible staging buffer and hands it back without blocking T3.
 */
class AssetJob {
  public:
    AssetJob() = default;
    ~AssetJob();

    AssetJob(const AssetJob&) = delete;
    AssetJob& operator=(const AssetJob&) = delete;

    static constexpr uint32_t kDefaultLoadFlags = AETHER_LOAD_FLAG_BAKE_PROXY_NORMALS |
                                                  AETHER_LOAD_FLAG_BAKE_SMOOTH_TANGENTS | AETHER_LOAD_FLAG_MORTON_SORT |
                                                  AETHER_LOAD_FLAG_DECODE_TEXTURES | AETHER_LOAD_FLAG_TEXTURE_CACHE;

    /** Cancels any active job, joins previous worker, and begins loading `path_utf8` asynchronously. */
    void start_load(rhi::RhiDevice& rhi, std::string path_utf8, uint32_t load_flags = kDefaultLoadFlags);

    /** Cooperatively cancels the active Rust load job (wait-free safe from UI/T3). */
    void cancel() noexcept;

    /** Cancels and joins worker thread, releasing any unconsumed staged VMA buffer. */
    void reset() noexcept;

    [[nodiscard]] AssetJobState state() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] float progress() const noexcept;
    [[nodiscard]] const char* last_error() const noexcept { return error_buf_; }
    [[nodiscard]] AetherStatus last_status() const noexcept { return last_status_.load(std::memory_order_acquire); }

    /**
     * Transfers ownership of the staged VMA upload buffer when `state() == ReadyForGpu`.
     * Caller runs outside `AETHER_HOT_PATH_SCOPE` to upload to GPU and free staging.
     */
    [[nodiscard]] std::unique_ptr<StagedAvatarUpload> take_staged() noexcept;

    /**
     * Convenience wrapper calling `take_staged()` + `upload_staged_avatar()`.
     * Marks job `Uploaded` on success or `Failed` on Vulkan upload error.
     */
    [[nodiscard]] Expected<std::unique_ptr<GpuAvatar>> finalize_gpu_upload(rhi::RhiDevice& rhi);

  private:
    void worker_main(rhi::RhiDevice* rhi, std::string path_utf8, uint32_t load_flags);
    void set_error(AetherStatus status, const char* msg) noexcept;

    std::thread worker_;
    std::atomic<AssetJobState> state_{AssetJobState::Idle};
    std::atomic<AetherStatus> last_status_{AETHER_OK};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<AetherLoaderHandle> active_loader_{nullptr};
    mutable std::atomic<uint64_t> active_polls_{0};
    mutable std::atomic<float> last_progress_{0.0f};
    std::atomic<StagedAvatarUpload*> staged_ptr_{nullptr};
    char error_buf_[256]{};
};

} // namespace aether::renderer
