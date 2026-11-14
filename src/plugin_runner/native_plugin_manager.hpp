#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "native_plugin_abi.h"

namespace aether::native_plugin {

enum class PluginState : uint8_t {
    Stopped = 0,
    Starting = 1,
    Running = 2,
    Restarting = 3,
    Disabled = 4,
};

struct NativePluginConfig {
    std::filesystem::path dll_path{};
    std::filesystem::path runner_exe_path{};
    std::string config_json{};
    uint64_t memory_limit_bytes{512ULL * 1024ULL * 1024ULL};
    uint32_t heartbeat_timeout_ms{500};
    uint32_t max_restarts_in_window{3};
    uint32_t restart_window_sec{300};
    std::array<uint32_t, 3> backoff_ms{50, 150, 450};
};

struct NativePluginHealth {
    PluginState state{PluginState::Stopped};
    bool permanently_disabled{false};
    bool restricted_token_active{false};
    bool job_object_active{false};
    uint32_t restart_count{0};
    uint32_t crash_count{0};
    uint32_t hang_kill_count{0};
    uint32_t last_exit_code{0};
    uint64_t last_heartbeat_age_ms{0};
    AetherNativePluginDesc desc{};
    std::string last_error{};
};

class NativePluginManager {
  public:
    static constexpr size_t kMaxSlots = 16;

    NativePluginManager();
    ~NativePluginManager();

    NativePluginManager(const NativePluginManager&) = delete;
    NativePluginManager& operator=(const NativePluginManager&) = delete;
    NativePluginManager(NativePluginManager&&) = delete;
    NativePluginManager& operator=(NativePluginManager&&) = delete;

    /** Launches `aether-plugin-runner.exe` for `cfg.dll_path`; returns slot index `[0..kMaxSlots)` or `< 0`. */
    [[nodiscard]] int32_t load_plugin(const NativePluginConfig& cfg);

    /** Stops the runner cleanly and releases slot resources. Safe from non-realtime threads. */
    void unload_plugin(uint32_t slot_id);

    /** Waits until slot enters `Running` or `Disabled` (for tests/startup). */
    [[nodiscard]] bool wait_until_ready(uint32_t slot_id, uint32_t timeout_ms);

    /** Wait-free, allocation-free request enqueue into SHM SPSC ring (`HR-01`, `HR-02`). */
    [[nodiscard]] bool try_submit_process(uint32_t slot_id, uint64_t frame_index, double time_sec, const float* inputs,
                                          uint32_t input_count, uint32_t output_count,
                                          uint32_t* out_seq = nullptr) noexcept;

    /** Wait-free, allocation-free response dequeue from SHM SPSC ring (`HR-01`, `HR-02`). */
    [[nodiscard]] bool try_pop_response(uint32_t slot_id, ResponseSlot& out) noexcept;

    /** Synchronous helper for tests / control paths (never call on T1-T4). */
    [[nodiscard]] bool process_sync(uint32_t slot_id, uint64_t frame_index, double time_sec, const float* inputs,
                                    uint32_t input_count, float* outputs, uint32_t output_count, uint32_t timeout_ms,
                                    int32_t* out_plugin_status = nullptr, std::string* out_error = nullptr);

    [[nodiscard]] NativePluginHealth health(uint32_t slot_id) const;

    /** Clears permanently_disabled state and attempts a fresh launch (UI retry action). */
    [[nodiscard]] bool reset_and_restart(uint32_t slot_id);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::native_plugin
