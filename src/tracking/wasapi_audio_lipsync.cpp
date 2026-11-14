#include "tracking/wasapi_audio_lipsync.hpp"

#include "core/clock.hpp"
#include "core/log.hpp"

#if defined(_WIN32)
// clang-format off
#include <Windows.h>
// clang-format on
#include <Audioclient.h>
#include <Mmreg.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace aether::tracking {

namespace {

constexpr float kPiF = 3.14159265358979323846f;

void make_rbj_lowpass(float fc, float q, float fs, float& b0, float& b1, float& b2, float& a1, float& a2) noexcept {
    const float w0 = 2.0f * kPiF * std::clamp(fc, 10.0f, 0.45f * fs) / fs;
    const float cw = std::cos(w0);
    const float sw = std::sin(w0);
    const float alpha = sw / (2.0f * std::max(q, 0.1f));
    const float a0 = 1.0f + alpha;
    const float inv_a0 = 1.0f / a0;
    b0 = ((1.0f - cw) * 0.5f) * inv_a0;
    b1 = (1.0f - cw) * inv_a0;
    b2 = ((1.0f - cw) * 0.5f) * inv_a0;
    a1 = (-2.0f * cw) * inv_a0;
    a2 = (1.0f - alpha) * inv_a0;
}

void make_rbj_highpass(float fc, float q, float fs, float& b0, float& b1, float& b2, float& a1, float& a2) noexcept {
    const float w0 = 2.0f * kPiF * std::clamp(fc, 10.0f, 0.45f * fs) / fs;
    const float cw = std::cos(w0);
    const float sw = std::sin(w0);
    const float alpha = sw / (2.0f * std::max(q, 0.1f));
    const float a0 = 1.0f + alpha;
    const float inv_a0 = 1.0f / a0;
    b0 = ((1.0f + cw) * 0.5f) * inv_a0;
    b1 = -(1.0f + cw) * inv_a0;
    b2 = ((1.0f + cw) * 0.5f) * inv_a0;
    a1 = (-2.0f * cw) * inv_a0;
    a2 = (1.0f - alpha) * inv_a0;
}

void make_rbj_bandpass_peak(float fc, float q, float fs, float& b0, float& b1, float& b2, float& a1,
                            float& a2) noexcept {
    const float w0 = 2.0f * kPiF * std::clamp(fc, 20.0f, 0.42f * fs) / fs;
    const float cw = std::cos(w0);
    const float sw = std::sin(w0);
    const float alpha = sw / (2.0f * std::max(q, 0.15f));
    const float a0 = 1.0f + alpha;
    const float inv_a0 = 1.0f / a0;
    b0 = alpha * inv_a0;
    b1 = 0.0f;
    b2 = -alpha * inv_a0;
    a1 = (-2.0f * cw) * inv_a0;
    a2 = (1.0f - alpha) * inv_a0;
}

#if defined(_WIN32)
std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) {
        return {};
    }
    const int needed =
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

enum class PcmSampleKind : uint8_t {
    Unsupported = 0,
    Float32 = 1,
    Int16 = 2,
    Int32 = 3,
    Int24 = 4,
};

PcmSampleKind classify_wave_format(const WAVEFORMATEX* wf) noexcept {
    if (wf == nullptr) {
        return PcmSampleKind::Unsupported;
    }
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wf->wBitsPerSample == 32) {
        return PcmSampleKind::Float32;
    }
    if (wf->wFormatTag == WAVE_FORMAT_PCM) {
        if (wf->wBitsPerSample == 16) {
            return PcmSampleKind::Int16;
        }
        if (wf->wBitsPerSample == 32) {
            return PcmSampleKind::Int32;
        }
        if (wf->wBitsPerSample == 24) {
            return PcmSampleKind::Int24;
        }
    }
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && wf->wBitsPerSample == 32) {
            return PcmSampleKind::Float32;
        }
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            if (wf->wBitsPerSample == 16) {
                return PcmSampleKind::Int16;
            }
            if (wf->wBitsPerSample == 32) {
                return PcmSampleKind::Int32;
            }
            if (wf->wBitsPerSample == 24) {
                return PcmSampleKind::Int24;
            }
        }
    }
    return PcmSampleKind::Unsupported;
}

float read_sample_mono(const uint8_t* frame_ptr, uint16_t channels, uint16_t block_align, PcmSampleKind kind) noexcept {
    if (channels == 0U) {
        return 0.0f;
    }
    double sum = 0.0;
    const uint32_t stride = std::max<uint32_t>(1U, block_align / channels);
    for (uint16_t c = 0; c < channels; ++c) {
        const uint8_t* p = frame_ptr + static_cast<size_t>(c) * stride;
        float v = 0.0f;
        switch (kind) {
        case PcmSampleKind::Float32: {
            std::memcpy(&v, p, sizeof(float));
            break;
        }
        case PcmSampleKind::Int16: {
            int16_t s = 0;
            std::memcpy(&s, p, sizeof(int16_t));
            v = static_cast<float>(s) * (1.0f / 32768.0f);
            break;
        }
        case PcmSampleKind::Int32: {
            int32_t s = 0;
            std::memcpy(&s, p, sizeof(int32_t));
            v = static_cast<float>(static_cast<double>(s) / 2147483648.0);
            break;
        }
        case PcmSampleKind::Int24: {
            int32_t s = (static_cast<int32_t>(p[0]) << 8) | (static_cast<int32_t>(p[1]) << 16) |
                        (static_cast<int32_t>(p[2]) << 24);
            s >>= 8;
            v = static_cast<float>(static_cast<double>(s) / 8388608.0);
            break;
        }
        default:
            break;
        }
        sum += static_cast<double>(std::isfinite(v) ? v : 0.0f);
    }
    return static_cast<float>(sum / static_cast<double>(channels));
}
#endif

} // namespace

void AudioLipSyncProcessor::reset() noexcept {
    vocal_hp_.reset_state();
    vocal_lp_.reset_state();
    for (auto& band : formant_bp_) {
        for (auto& st : band) {
            st.reset_state();
        }
    }
    vocal_ms_env_ = 0.0f;
    band_ms_env_.fill(0.0f);
    vad_env_ = 0.0f;
    open_y_env_ = 0.0f;
    smooth_vowels_.fill(0.0f);
    vad_gate_ = false;
    seq_ = 0;
}

void AudioLipSyncProcessor::ensure_sample_rate(uint32_t sample_rate_hz) noexcept {
    const uint32_t fs_i = std::clamp(sample_rate_hz, 8000U, 192000U);
    if (fs_i == sample_rate_hz_) {
        return;
    }
    sample_rate_hz_ = fs_i;
    const float fs = static_cast<float>(fs_i);
    constexpr float kButterworthQ = 0.70710678f;

    // Human vocal envelope band (120 Hz - 3800 Hz)
    make_rbj_highpass(120.0f, kButterworthQ, fs, vocal_hp_.b0, vocal_hp_.b1, vocal_hp_.b2, vocal_hp_.a1, vocal_hp_.a2);
    make_rbj_lowpass(3800.0f, kButterworthQ, fs, vocal_lp_.b0, vocal_lp_.b1, vocal_lp_.b2, vocal_lp_.a1, vocal_lp_.a2);
    vocal_hp_.reset_state();
    vocal_lp_.reset_state();

    // 3 resonant formant bandpass pairs (4th-order cascaded per band for sharp formant separation):
    //   B1: 250-650 Hz (f0 = 403.1 Hz, Q = 1.008) -> F1 / U, O resonance
    //   B2: 700-1600 Hz (f0 = 1058.3 Hz, Q = 1.176) -> central A / O resonance
    //   B3: 1700-3200 Hz (f0 = 2332.4 Hz, Q = 1.555) -> front F2 I / E brightness
    struct BandSpec {
        float fc;
        float q;
    };
    const BandSpec kBands[3] = {
        {403.11f, 1.0078f},
        {1058.30f, 1.1759f},
        {2332.38f, 1.5549f},
    };
    for (size_t b = 0; b < 3U; ++b) {
        for (size_t s = 0; s < 2U; ++s) {
            auto& f = formant_bp_[b][s];
            make_rbj_bandpass_peak(kBands[b].fc, kBands[b].q, fs, f.b0, f.b1, f.b2, f.a1, f.a2);
            f.reset_state();
        }
    }
}

animation::AudioLipSyncFrame AudioLipSyncProcessor::process_pcm_block(const float* mono_pcm, size_t sample_count,
                                                                      uint32_t sample_rate_hz, float mic_gain,
                                                                      float noise_gate_db, uint64_t timestamp_us,
                                                                      bool assist_active) noexcept {
    ensure_sample_rate(sample_rate_hz);
    const float fs = static_cast<float>(sample_rate_hz_);
    const float gain = std::clamp(std::isfinite(mic_gain) ? mic_gain : 1.0f, 0.0f, 8.0f);
    const float gate_open_db = std::clamp(std::isfinite(noise_gate_db) ? noise_gate_db : -48.0f, -80.0f, -6.0f);
    const float gate_close_db = gate_open_db - 4.0f;

    // Mean-square time constants correspond to 8 ms amplitude attack / 90 ms amplitude release
    const float dt_samp = 1.0f / fs;
    const float a_ms_atk = 1.0f - std::exp(-dt_samp / 0.004f);
    const float a_ms_rel = 1.0f - std::exp(-dt_samp / 0.045f);
    const float a_band = 1.0f - std::exp(-dt_samp / 0.008f);

    if (mono_pcm != nullptr && sample_count > 0U) {
        for (size_t i = 0; i < sample_count; ++i) {
            const float x_raw = mono_pcm[i];
            const float x = (std::isfinite(x_raw) ? std::clamp(x_raw, -4.0f, 4.0f) : 0.0f) * gain;

            const float yv = vocal_lp_.step(vocal_hp_.step(x));
            const float ev = yv * yv;
            const float av = (ev > vocal_ms_env_) ? a_ms_atk : a_ms_rel;
            vocal_ms_env_ += av * (ev - vocal_ms_env_);

            for (size_t b = 0; b < 3U; ++b) {
                const float yb = formant_bp_[b][1].step(formant_bp_[b][0].step(x));
                const float eb = yb * yb;
                band_ms_env_[b] += a_band * (eb - band_ms_env_[b]);
            }
        }
    }

    const float block_dt =
        (sample_count > 0U) ? std::clamp(static_cast<float>(sample_count) * dt_samp, 1e-4f, 0.25f) : (1.0f / 60.0f);
    const float rms_lin = std::sqrt(std::max(vocal_ms_env_, 0.0f));
    const float rms_db = 20.0f * std::log10(std::max(rms_lin, 1e-5f));

    if (!vad_gate_ && rms_db >= gate_open_db) {
        vad_gate_ = true;
    } else if (vad_gate_ && rms_db < gate_close_db) {
        vad_gate_ = false;
    }

    const float a_vad = 1.0f - std::exp(-block_dt / (vad_gate_ ? 0.008f : 0.090f));
    const float target_vad = vad_gate_ ? 1.0f : 0.0f;
    vad_env_ += a_vad * (target_vad - vad_env_);
    if (!vad_gate_ && vad_env_ < 0.01f) {
        vad_env_ = 0.0f;
    }

    float target_open = 0.0f;
    if (vad_env_ > 0.0f && vad_gate_) {
        const float over_db = std::clamp((rms_db - gate_open_db) / 26.0f, 0.0f, 1.0f);
        target_open = std::pow(over_db, 0.85f);
    }
    const float a_open = 1.0f - std::exp(-block_dt / ((target_open > open_y_env_) ? 0.008f : 0.090f));
    open_y_env_ += a_open * (target_open - open_y_env_);
    if (vad_env_ <= 0.0f && open_y_env_ < 1e-4f) {
        open_y_env_ = 0.0f;
    }

    std::array<float, 3> band_rms{};
    float band_sum = 0.0f;
    for (size_t b = 0; b < 3U; ++b) {
        band_rms[b] = std::sqrt(std::max(band_ms_env_[b], 0.0f));
        band_sum += band_rms[b];
    }

    std::array<float, 5> target_vowels{};
    if (vad_env_ > 0.0f && band_sum > 1e-6f) {
        const float r1 = band_rms[0] / band_sum;
        const float r2 = band_rms[1] / band_sum;
        const float r3 = band_rms[2] / band_sum;
        // Prototype formant centroids in normalized [B1(250-650), B2(700-1600), B3(1700-3200)] space:
        //   A (aa): strong mid F1/F2 B2 resonance
        //   I (ih): high front F2 B3 brightness
        //   U (ou): low rounded F1 B1 dominance
        //   E (ee): split front B1+B3 brightness without pure-B3 extreme
        //   O (oh): balanced low-mid B1+B2 warmth with low B3
        static constexpr float kCentroids[5][3] = {
            {0.10f, 0.85f, 0.05f}, // A
            {0.08f, 0.08f, 0.84f}, // I
            {0.84f, 0.12f, 0.04f}, // U
            {0.30f, 0.25f, 0.45f}, // E
            {0.52f, 0.43f, 0.05f}, // O
        };
        float exp_sum = 0.0f;
        for (size_t v = 0; v < 5U; ++v) {
            const float d1 = r1 - kCentroids[v][0];
            const float d2 = r2 - kCentroids[v][1];
            const float d3 = r3 - kCentroids[v][2];
            const float d2_sq = d1 * d1 + d2 * d2 + d3 * d3;
            target_vowels[v] = std::exp(-d2_sq / 0.11f);
            exp_sum += target_vowels[v];
        }
        if (exp_sum > 1e-9f) {
            const float inv = 1.0f / exp_sum;
            for (float& tv : target_vowels) {
                tv *= inv;
            }
        }
    }

    const float a_vwl = 1.0f - std::exp(-block_dt / 0.018f);
    float smooth_sum = 0.0f;
    for (size_t v = 0; v < 5U; ++v) {
        smooth_vowels_[v] += a_vwl * (target_vowels[v] - smooth_vowels_[v]);
        if (vad_env_ <= 0.0f && smooth_vowels_[v] < 1e-4f) {
            smooth_vowels_[v] = 0.0f;
        }
        smooth_sum += smooth_vowels_[v];
    }

    animation::AudioLipSyncFrame out{};
    out.sequence = ++seq_;
    out.timestamp_us = timestamp_us;
    out.vad = vad_env_;
    out.audio_open_y = std::clamp(open_y_env_, 0.0f, 1.0f);
    if (smooth_sum > 1e-6f && vad_env_ > 0.0f) {
        const float inv = 1.0f / smooth_sum;
        for (size_t v = 0; v < 5U; ++v) {
            out.vowel_probs[v] = std::clamp(smooth_vowels_[v] * inv, 0.0f, 1.0f);
        }
    }
    out.band_energy = band_rms;
    out.rms_db = rms_db;
    out.device_available = true;
    out.assist_active = assist_active;
    return out;
}

WasapiAudioLipsync::~WasapiAudioLipsync() {
    stop();
}

bool WasapiAudioLipsync::start(std::string_view device_id, float mic_gain, float noise_gate_db, bool assist_enabled) {
    stop();

    info_ready_.store(false, std::memory_order_release);
    active_device_id_buf_.fill('\0');
    std::snprintf(active_device_label_buf_.data(), active_device_label_buf_.size(), "Initializing...");
    mic_gain_.store(std::clamp(mic_gain, 0.1f, 8.0f), std::memory_order_release);
    noise_gate_db_.store(std::clamp(noise_gate_db, -80.0f, -6.0f), std::memory_order_release);
    assist_enabled_.store(assist_enabled, std::memory_order_release);
    target_device_id_.assign(device_id);

#if defined(_WIN32)
    audio_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (audio_event_ == nullptr || stop_event_ == nullptr) {
        stop();
        return false;
    }
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&WasapiAudioLipsync::thread_entry, this);
    return true;
#else
    publish(animation::AudioLipSyncFrame{});
    return false;
#endif
}

void WasapiAudioLipsync::stop() noexcept {
    running_.store(false, std::memory_order_release);
#if defined(_WIN32)
    if (stop_event_ != nullptr) {
        ::SetEvent(static_cast<HANDLE>(stop_event_));
    }
#endif
    if (worker_.joinable()) {
        worker_.join();
    }
#if defined(_WIN32)
    if (audio_event_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(audio_event_));
        audio_event_ = nullptr;
    }
    if (stop_event_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(stop_event_));
        stop_event_ = nullptr;
    }
#endif
}

void WasapiAudioLipsync::select_device(std::string_view device_id) {
    if (device_id == target_device_id_ && is_running()) {
        return;
    }
    const float g = gain();
    const float ng = noise_gate_db();
    const bool en = assist_enabled();
    (void)start(device_id, g, ng, en);
}

void WasapiAudioLipsync::set_gain(float gain) noexcept {
    mic_gain_.store(std::clamp(std::isfinite(gain) ? gain : 1.0f, 0.1f, 8.0f), std::memory_order_release);
}

void WasapiAudioLipsync::set_noise_gate_db(float threshold_db) noexcept {
    noise_gate_db_.store(std::clamp(std::isfinite(threshold_db) ? threshold_db : -48.0f, -80.0f, -6.0f),
                         std::memory_order_release);
}

void WasapiAudioLipsync::set_assist_enabled(bool enabled) noexcept {
    assist_enabled_.store(enabled, std::memory_order_release);
}

void WasapiAudioLipsync::publish(const animation::AudioLipSyncFrame& frame) noexcept {
    const uint64_t next_seq = published_seq_.load(std::memory_order_relaxed) + 1ULL;
    mailbox_[next_seq & 3ULL] = frame;
    published_seq_.store(next_seq, std::memory_order_release);
}

animation::AudioLipSyncFrame WasapiAudioLipsync::latest() const noexcept {
    animation::AudioLipSyncFrame out{};
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t s0 = published_seq_.load(std::memory_order_acquire);
        if (s0 == 0ULL) {
            return out;
        }
        out = mailbox_[s0 & 3ULL];
        const uint64_t s1 = published_seq_.load(std::memory_order_acquire);
        if (s1 - s0 < 3ULL) {
            return out;
        }
    }
    return out;
}

std::string WasapiAudioLipsync::active_device_id() const {
    if (!info_ready_.load(std::memory_order_acquire)) {
        return target_device_id_;
    }
    return std::string(active_device_id_buf_.data());
}

std::string WasapiAudioLipsync::active_device_label() const {
    if (!info_ready_.load(std::memory_order_acquire)) {
        return "Initializing...";
    }
    return std::string(active_device_label_buf_.data());
}

void WasapiAudioLipsync::thread_entry() {
#if defined(_WIN32)
    const HRESULT co_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = (co_hr == S_OK || co_hr == S_FALSE);

    auto set_buf = [](auto& dst, const char* src) noexcept {
        std::snprintf(dst.data(), dst.size(), "%s", (src != nullptr) ? src : "");
    };
    auto publish_offline = [&](const char* label) {
        if (!info_ready_.load(std::memory_order_relaxed)) {
            set_buf(active_device_label_buf_, label);
            info_ready_.store(true, std::memory_order_release);
        }
        animation::AudioLipSyncFrame off{};
        off.timestamp_us = now_us();
        off.device_available = false;
        off.assist_active = false;
        publish(off);
    };

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    Microsoft::WRL::ComPtr<IMMDevice> device;
    HRESULT hr =
        ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr) && enumerator) {
        if (!target_device_id_.empty()) {
            const std::wstring wid = utf8_to_wide(target_device_id_);
            if (!wid.empty()) {
                hr = enumerator->GetDevice(wid.c_str(), &device);
            }
        }
        if (!device) {
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        }
    }

    if (!device) {
        set_buf(active_device_id_buf_, "");
        publish_offline("No Microphone Detected");
        if (com_owned) {
            ::CoUninitialize();
        }
        return;
    }

    {
        LPWSTR wid = nullptr;
        if (SUCCEEDED(device->GetId(&wid)) && wid != nullptr) {
            set_buf(active_device_id_buf_, wide_to_utf8(wid).c_str());
            ::CoTaskMemFree(wid);
        }
        Microsoft::WRL::ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
            static constexpr PROPERTYKEY kFriendlyNameKey = {
                {0xa45c254eU, 0xdf1cU, 0x4efdU, {0x80U, 0x20U, 0x67U, 0xd1U, 0x46U, 0xa8U, 0x50U, 0xe0U}}, 14};
            PROPVARIANT var{};
            PropVariantInit(&var);
            if (SUCCEEDED(props->GetValue(kFriendlyNameKey, &var)) && var.vt == VT_LPWSTR &&
                var.pwszVal != nullptr) {
                set_buf(active_device_label_buf_, wide_to_utf8(var.pwszVal).c_str());
            }
            ::PropVariantClear(&var);
        }
        info_ready_.store(true, std::memory_order_release);
    }

    Microsoft::WRL::ComPtr<IAudioClient> audio_client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(audio_client.GetAddressOf()));
    if (FAILED(hr) || !audio_client) {
        publish_offline("Microphone Unavailable");
        if (com_owned) {
            ::CoUninitialize();
        }
        return;
    }

    WAVEFORMATEX* mix_fmt = nullptr;
    hr = audio_client->GetMixFormat(&mix_fmt);
    if (FAILED(hr) || mix_fmt == nullptr) {
        publish_offline("Unsupported Audio Format");
        if (com_owned) {
            ::CoUninitialize();
        }
        return;
    }

    const PcmSampleKind sample_kind = classify_wave_format(mix_fmt);
    const uint32_t sample_rate = mix_fmt->nSamplesPerSec;
    const uint16_t channels = mix_fmt->nChannels;
    const uint16_t block_align = mix_fmt->nBlockAlign;

    // 10 ms low-latency shared-mode event-driven capture buffer (100,000 * 100ns = 10 ms)
    constexpr REFERENCE_TIME kBufferDuration = 100000;
    hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDuration, 0,
                                  mix_fmt, nullptr);
    ::CoTaskMemFree(mix_fmt);
    mix_fmt = nullptr;

    if (FAILED(hr) || sample_kind == PcmSampleKind::Unsupported) {
        publish_offline("WASAPI Init Failed");
        if (com_owned) {
            ::CoUninitialize();
        }
        return;
    }

    hr = audio_client->SetEventHandle(static_cast<HANDLE>(audio_event_));
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_client;
    if (SUCCEEDED(hr)) {
        hr = audio_client->GetService(IID_PPV_ARGS(&capture_client));
    }
    if (FAILED(hr) || !capture_client) {
        publish_offline("WASAPI Capture Unavailable");
        if (com_owned) {
            ::CoUninitialize();
        }
        return;
    }

    (void)::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    AudioLipSyncProcessor dsp{};
    dsp.reset();
    if (FAILED(audio_client->Start())) {
        publish_offline("WASAPI Start Failed");
        if (com_owned) {
            ::CoUninitialize();
        }
        return;
    }

    AETHER_LOG_INFO("tracking", "WASAPI audio lip-sync started on '%s' (%u Hz, %u ch)",
                    active_device_label_buf_.data(), sample_rate, channels);

    // Zero heap allocations inside capture loop: process downmixed chunks on stack
    float mono_chunk[1024]{};
    HANDLE wait_handles[2] = {static_cast<HANDLE>(audio_event_), static_cast<HANDLE>(stop_event_)};

    while (running_.load(std::memory_order_acquire)) {
        const DWORD wait_res = ::WaitForMultipleObjects(2, wait_handles, FALSE, 50);
        if (wait_res == WAIT_OBJECT_0 + 1 || !running_.load(std::memory_order_acquire)) {
            break;
        }

        const float gain = mic_gain_.load(std::memory_order_acquire);
        const float gate_db = noise_gate_db_.load(std::memory_order_acquire);
        const bool assist = assist_enabled_.load(std::memory_order_acquire);

        UINT32 packet_frames = 0;
        hr = capture_client->GetNextPacketSize(&packet_frames);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            publish_offline("Microphone Disconnected");
            break;
        }
        while (SUCCEEDED(hr) && packet_frames > 0U && running_.load(std::memory_order_acquire)) {
            BYTE* data = nullptr;
            UINT32 num_frames = 0;
            DWORD flags = 0;
            hr = capture_client->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                publish_offline("Microphone Disconnected");
                running_.store(false, std::memory_order_release);
                break;
            }
            if (FAILED(hr)) {
                break;
            }

            const bool silent = ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0U) || (data == nullptr);
            uint32_t offset = 0;
            while (offset < num_frames) {
                const uint32_t chunk = std::min<uint32_t>(num_frames - offset, 1024U);
                if (silent) {
                    std::fill_n(mono_chunk, chunk, 0.0f);
                } else {
                    const uint8_t* ptr = data + static_cast<size_t>(offset) * block_align;
                    for (uint32_t i = 0; i < chunk; ++i) {
                        mono_chunk[i] = read_sample_mono(ptr + static_cast<size_t>(i) * block_align, channels,
                                                         block_align, sample_kind);
                    }
                }
                const auto f = dsp.process_pcm_block(mono_chunk, chunk, sample_rate, gain, gate_db, now_us(), assist);
                publish(f);
                offset += chunk;
            }

            (void)capture_client->ReleaseBuffer(num_frames);
            hr = capture_client->GetNextPacketSize(&packet_frames);
        }
    }

    (void)audio_client->Stop();
    if (com_owned) {
        ::CoUninitialize();
    }
#endif
}

} // namespace aether::tracking
