#pragma once

#include "animation/expression_mixer.hpp"
#include "core/hw_enum.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace aether::tracking {

/**
 * Pure deterministic vocal-band & 3-formant audio DSP analyzer (zero allocations, zero OS locks).
 * Runs directly on the WASAPI capture callback thread or deterministically in unit tests via
 * `process_pcm_block()`.
 */
class AudioLipSyncProcessor {
  public:
    void reset() noexcept;

    /**
     * Processes `sample_count` mono PCM samples in `[-1, 1]` at `sample_rate_hz` with linear `mic_gain`
     * and `noise_gate_db` threshold, returning the updated `AudioLipSyncFrame` (`device_available` /
     * `assist_active` reflect caller runtime flags, defaulted `true` by this processor so tests can pass it directly).
     */
    [[nodiscard]] animation::AudioLipSyncFrame process_pcm_block(const float* mono_pcm, size_t sample_count,
                                                                 uint32_t sample_rate_hz, float mic_gain,
                                                                 float noise_gate_db, uint64_t timestamp_us,
                                                                 bool assist_active = true) noexcept;

  private:
    struct Biquad {
        float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};
        float z1{0.0f}, z2{0.0f};

        void reset_state() noexcept { z1 = 0.0f; z2 = 0.0f; }
        [[nodiscard]] float step(float x) noexcept {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    void ensure_sample_rate(uint32_t sample_rate_hz) noexcept;

    uint32_t sample_rate_hz_{0};
    Biquad vocal_hp_{};
    Biquad vocal_lp_{};
    std::array<std::array<Biquad, 2>, 3> formant_bp_{};

    float vocal_ms_env_{0.0f};
    std::array<float, 3> band_ms_env_{};
    float vad_env_{0.0f};
    float open_y_env_{0.0f};
    std::array<float, 5> smooth_vowels_{};
    bool vad_gate_{false};
    uint64_t seq_{0};
};

/**
 * Event-driven low-latency WASAPI Shared Mode microphone capture engine (`HR-01` lock-free atomics).
 * Publishes vocal activity, RMS speech envelope, and normalized `[A, I, U, E, O]` formant weights into
 * a wait-free 4-slot atomic mailbox readable concurrently by T2 `SimEngine` and T3 UI.
 */
class WasapiAudioLipsync {
  public:
    WasapiAudioLipsync() = default;
    ~WasapiAudioLipsync();

    WasapiAudioLipsync(const WasapiAudioLipsync&) = delete;
    WasapiAudioLipsync& operator=(const WasapiAudioLipsync&) = delete;

    /** Starts event-driven WASAPI capture (`device_id` empty = System Default console capture endpoint). */
    bool start(std::string_view device_id = {}, float mic_gain = 1.25f, float noise_gate_db = -48.0f,
               bool assist_enabled = true);
    void stop() noexcept;

    /** Switches capture endpoint cleanly outside any hot-path guard. */
    void select_device(std::string_view device_id);

    void set_gain(float gain) noexcept;
    void set_noise_gate_db(float threshold_db) noexcept;
    void set_assist_enabled(bool enabled) noexcept;

    [[nodiscard]] float gain() const noexcept { return mic_gain_.load(std::memory_order_acquire); }
    [[nodiscard]] float noise_gate_db() const noexcept { return noise_gate_db_.load(std::memory_order_acquire); }
    [[nodiscard]] bool assist_enabled() const noexcept { return assist_enabled_.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }

    [[nodiscard]] animation::AudioLipSyncFrame latest() const noexcept;
    [[nodiscard]] std::string active_device_id() const;
    [[nodiscard]] std::string active_device_label() const;

    [[nodiscard]] static std::vector<AudioInputDeviceInfo> enumerate_devices() {
        return enumerate_audio_input_devices();
    }

  private:
    void thread_entry();
    void publish(const animation::AudioLipSyncFrame& frame) noexcept;

    std::atomic<bool> running_{false};
    std::atomic<float> mic_gain_{1.25f};
    std::atomic<float> noise_gate_db_{-48.0f};
    std::atomic<bool> assist_enabled_{true};
    std::atomic<bool> info_ready_{false};
    std::atomic<uint64_t> published_seq_{0};

    std::array<animation::AudioLipSyncFrame, 4> mailbox_{};
    void* audio_event_{nullptr};
    void* stop_event_{nullptr};
    std::string target_device_id_{};
    std::array<char, 192> active_device_id_buf_{};
    std::array<char, 96> active_device_label_buf_{"No Microphone"};
    std::thread worker_;
};

} // namespace aether::tracking
