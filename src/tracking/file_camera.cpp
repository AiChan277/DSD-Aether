#include "core/clock.hpp"
#include "tracking/camera_source.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace aether::tracking {

namespace {

using Microsoft::WRL::ComPtr;

class FileOrSyntheticCameraSource final : public ICameraSource {
  public:
    FileOrSyntheticCameraSource(std::filesystem::path path, bool force_synth, uint32_t w, uint32_t h, uint32_t fps,
                                uint32_t synth_limit)
        : media_path_(std::move(path)), force_synth_(force_synth), width_(std::clamp(w, 64U, kMaxCamWidth)),
          height_(std::clamp(h, 64U, kMaxCamHeight)), fps_(std::max(1U, fps)), synth_limit_(synth_limit) {}

    ~FileOrSyntheticCameraSource() override { close(); }

    AetherStatus open() override {
        close();
        frame_ = std::make_unique<CapturedFrameBuffer>();
        seq_ = 0;

        if (!force_synth_ && !media_path_.empty() && std::filesystem::exists(media_path_)) {
            const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            co_owned_ = SUCCEEDED(hr);
            mf_started_ = SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_LITE));
            if (mf_started_) {
                ComPtr<IMFAttributes> attr;
                ::MFCreateAttributes(&attr, 1);
                attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
                if (SUCCEEDED(::MFCreateSourceReaderFromURL(media_path_.wstring().c_str(), attr.Get(), &reader_))) {
                    ComPtr<IMFMediaType> out_t;
                    ::MFCreateMediaType(&out_t);
                    out_t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                    out_t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                    (void)reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
                                                       out_t.Get());
                    ComPtr<IMFMediaType> cur;
                    if (SUCCEEDED(reader_->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                                               &cur)) &&
                        cur) {
                        UINT32 aw = 0, ah = 0;
                        if (SUCCEEDED(MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &aw, &ah)) && aw > 0 && ah > 0) {
                            width_ = std::min(aw, kMaxCamWidth);
                            height_ = std::min(ah, kMaxCamHeight);
                        }
                    }
                    use_mf_file_ = true;
                    opened_ = true;
                    return AETHER_OK;
                }
            }
            if (!force_synth_) {
                close();
                return AETHER_E_PARSE;
            }
        }
        use_mf_file_ = false;
        opened_ = true;
        return AETHER_OK;
    }

    void close() noexcept override {
        opened_ = false;
        if (reader_) {
            reader_.Reset();
        }
        if (mf_started_) {
            ::MFShutdown();
            mf_started_ = false;
        }
        if (co_owned_) {
            ::CoUninitialize();
            co_owned_ = false;
        }
        frame_.reset();
    }

    bool read_latest_blocking(uint32_t /*timeout_ms*/, const CapturedFrameBuffer*& out_frame) noexcept override {
        if (!frame_) {
            return false;
        }
        if (use_mf_file_ && reader_) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            HRESULT hr = reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr,
                                             &flags, &ts, &sample);
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                PROPVARIANT var{};
                var.vt = VT_I8;
                var.hVal.QuadPart = 0;
                (void)reader_->SetCurrentPosition(GUID_NULL, var);
                hr = reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags,
                                         &ts, &sample);
            }
            if (SUCCEEDED(hr) && sample) {
                ComPtr<IMFMediaBuffer> buf;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
                    BYTE* raw = nullptr;
                    DWORD max_l = 0, cur_l = 0;
                    if (SUCCEEDED(buf->Lock(&raw, &max_l, &cur_l))) {
                        const size_t nv12_need = static_cast<size_t>(width_) * height_ * 3U / 2U;
                        const size_t cp = std::min<size_t>(cur_l, std::min(nv12_need, frame_->bytes.size()));
                        std::memcpy(frame_->bytes.data(), raw, cp);
                        buf->Unlock();
                        frame_->width = width_;
                        frame_->height = height_;
                        frame_->stride_y = width_;
                        frame_->stride_uv = width_;
                        frame_->format = CameraPixelFormat::NV12;
                        frame_->sequence = ++seq_;
                        const uint64_t now = aether::now_us();
                        frame_->capture_timestamp_us = std::max(now, last_capture_ts_us_ + 1ULL);
                        last_capture_ts_us_ = frame_->capture_timestamp_us;
                        out_frame = frame_.get();
                        return true;
                    }
                }
            }
            return false;
        }

        if (synth_limit_ > 0U && seq_ >= synth_limit_) {
            return false;
        }

        // Deterministic synthetic animated face+torso+hand NV12 generator (`HR-12` clean)
        const uint64_t f_idx = ++seq_;
        const uint64_t now = aether::now_us();
        frame_->width = width_;
        frame_->height = height_;
        frame_->stride_y = width_;
        frame_->stride_uv = width_;
        frame_->format = CameraPixelFormat::NV12;
        frame_->sequence = f_idx;
        frame_->capture_timestamp_us = std::max(now, last_capture_ts_us_ + 1ULL);
        last_capture_ts_us_ = frame_->capture_timestamp_us;

        const float t = static_cast<float>(f_idx) / static_cast<float>(fps_);
        const float cx = 0.50f + 0.08f * std::sin(t * 2.0f);
        const float cy = 0.36f + 0.04f * std::cos(t * 1.5f);
        const float hx = cx * static_cast<float>(width_);
        const float hy = cy * static_cast<float>(height_);
        const float r = 0.16f * static_cast<float>(std::min(width_, height_));

        uint8_t* y_plane = frame_->bytes.data();
        uint8_t* uv_plane = frame_->bytes.data() + static_cast<size_t>(width_) * height_;

        for (uint32_t y = 0; y < height_; ++y) {
            const float dy = static_cast<float>(y) - hy;
            const float dy2 = dy * dy;
            for (uint32_t x = 0; x < width_; ++x) {
                const float dx = static_cast<float>(x) - hx;
                const float d2 = (dx * dx + dy2) / (r * r);
                uint8_t luma = 42U;
                if (d2 < 1.0f) {
                    luma = static_cast<uint8_t>(185.0f - 45.0f * d2);
                } else if (dy > r * 0.8f && std::abs(dx) < r * 1.5f) {
                    luma = 120U;
                }
                y_plane[static_cast<size_t>(y) * width_ + x] = luma;
            }
        }
        const size_t uv_bytes = static_cast<size_t>(width_) * (height_ / 2U);
        std::memset(uv_plane, 128, uv_bytes);

        out_frame = frame_.get();
        return true;
    }

    AetherStatus apply_controls(const CameraControls& /*controls*/) noexcept override { return AETHER_OK; }
    uint32_t width() const noexcept override { return width_; }
    uint32_t height() const noexcept override { return height_; }
    uint32_t fps() const noexcept override { return fps_; }
    bool is_synthetic() const noexcept override { return !use_mf_file_; }
    bool is_open() const noexcept override { return opened_; }
    std::string source_label() const override {
        if (use_mf_file_) {
            return "File: " + media_path_.filename().string();
        }
        return "Synthetic FileCamera (60Hz)";
    }

  private:
    std::filesystem::path media_path_{};
    bool force_synth_{false};
    bool use_mf_file_{false};
    bool opened_{false};
    bool co_owned_{false};
    bool mf_started_{false};
    uint32_t width_{1280};
    uint32_t height_{720};
    uint32_t fps_{60};
    uint32_t synth_limit_{0};
    uint64_t seq_{0};
    uint64_t last_capture_ts_us_{0};
    std::unique_ptr<CapturedFrameBuffer> frame_{};
    ComPtr<IMFSourceReader> reader_;
};

} // namespace

std::unique_ptr<ICameraSource> create_file_camera_source(std::filesystem::path media_path, bool force_synthetic_pattern,
                                                         uint32_t width, uint32_t height, uint32_t fps,
                                                         uint32_t synthetic_total_frames) {
    return std::make_unique<FileOrSyntheticCameraSource>(std::move(media_path), force_synthetic_pattern, width, height,
                                                         fps, synthetic_total_frames);
}

} // namespace aether::tracking
