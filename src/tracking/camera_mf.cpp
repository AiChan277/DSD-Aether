#include "core/clock.hpp"
#include "core/log.hpp"
#include "tracking/camera_source.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dshow.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aether::tracking {

namespace {

using Microsoft::WRL::ComPtr;

[[nodiscard]] std::string wide_to_utf8(const wchar_t* ws) {
    if (ws == nullptr || ws[0] == L'\0') {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

[[nodiscard]] std::string to_lower_ascii(std::string_view sv) {
    std::string out(sv);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

[[nodiscard]] bool equals_ignore_case(std::string_view a, std::string_view b) {
    return !a.empty() && a.size() == b.size() && to_lower_ascii(a) == to_lower_ascii(b);
}

[[nodiscard]] bool is_likely_virtual_camera(std::string_view friendly_name, std::string_view symbolic_link) {
    const std::string fn = to_lower_ascii(friendly_name);
    const std::string sl = to_lower_ascii(symbolic_link);
    static constexpr const char* kVirtualHints[] = {
        "smart connect", "obs virtual", "obs-camera", "broadcast", "snap camera",
        "virtual",       "root\\camera", "manycam",   "iriun",     "droidcam",
        "epoccam",       "youcam",      "vcam",
    };
    for (const char* hint : kVirtualHints) {
        if (fn.find(hint) != std::string::npos || sl.find(hint) != std::string::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] uint32_t row_bytes_for_format(CameraPixelFormat fmt, uint32_t width) noexcept {
    switch (fmt) {
    case CameraPixelFormat::NV12:
        return width;
    case CameraPixelFormat::YUY2:
        return width * 2U;
    case CameraPixelFormat::BGRA32:
    default:
        return width * 4U;
    }
}

[[nodiscard]] const char* pixel_format_name(CameraPixelFormat fmt) noexcept {
    switch (fmt) {
    case CameraPixelFormat::NV12:
        return "NV12";
    case CameraPixelFormat::YUY2:
        return "YUY2";
    case CameraPixelFormat::BGRA32:
    default:
        return "BGRA32";
    }
}

class MfSessionGuard {
  public:
    MfSessionGuard() noexcept {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        co_owned_ = SUCCEEDED(hr);
        mf_started_ = SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    ~MfSessionGuard() noexcept {
        if (mf_started_) {
            ::MFShutdown();
        }
        if (co_owned_) {
            ::CoUninitialize();
        }
    }

    MfSessionGuard(const MfSessionGuard&) = delete;
    MfSessionGuard& operator=(const MfSessionGuard&) = delete;

    [[nodiscard]] bool ready() const noexcept { return mf_started_; }

  private:
    bool co_owned_{false};
    bool mf_started_{false};
};

class MfReaderCallback final : public IMFSourceReaderCallback {
  public:
    MfReaderCallback(LatestFrameSlot& slot, uint32_t width, uint32_t height, CameraPixelFormat fmt) noexcept
        : slot_(slot), width_(width), height_(height), format_(fmt) {}

    void set_reader(IMFSourceReader* reader) noexcept { reader_.store(reader, std::memory_order_release); }

    void set_active(bool active) noexcept { active_.store(active, std::memory_order_release); }

    void update_format(uint32_t w, uint32_t h, CameraPixelFormat fmt, bool bottom_up = false) noexcept {
        width_ = w;
        height_ = h;
        format_ = fmt;
        bottom_up_ = bottom_up;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == __uuidof(IMFSourceReaderCallback)) {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return ref_count_.fetch_add(1UL, std::memory_order_relaxed) + 1UL; }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = ref_count_.fetch_sub(1UL, std::memory_order_acq_rel) - 1UL;
        if (r == 0UL) {
            delete this;
        }
        return r;
    }

    // IMFSourceReaderCallback
    STDMETHODIMP OnReadSample(HRESULT hr_status, DWORD /*stream_idx*/, DWORD stream_flags, LONGLONG /*timestamp*/,
                              IMFSample* sample) override {
        if (!active_.load(std::memory_order_acquire)) {
            return S_OK;
        }
        if (FAILED(hr_status) || (stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            return S_OK;
        }

        if (sample != nullptr) {
            ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
                CapturedFrameBuffer& dst = slot_.begin_write();
                const uint8_t write_idx = slot_.current_write_index();
                dst.capture_timestamp_us = aether::now_us();
                dst.sequence = ++seq_;
                dst.width = std::min(width_, kMaxCamWidth);
                dst.height = std::min(height_, kMaxCamHeight);
                dst.format = format_;

                bool copied = false;
                ComPtr<IMF2DBuffer2> buf2d;
                if (SUCCEEDED(buf.As(&buf2d)) && buf2d) {
                    BYTE* scan0 = nullptr;
                    LONG pitch = 0;
                    BYTE* buf_start = nullptr;
                    DWORD buf_len = 0;
                    if (SUCCEEDED(buf2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &buf_start, &buf_len))) {
                        copy_locked_planes(scan0, buf_start, pitch, buf_len, dst);
                        buf2d->Unlock2D();
                        copied = true;
                    }
                }
                if (!copied) {
                    BYTE* raw = nullptr;
                    DWORD max_len = 0;
                    DWORD cur_len = 0;
                    if (SUCCEEDED(buf->Lock(&raw, &max_len, &cur_len))) {
                        const LONG abs_pitch = static_cast<LONG>(row_bytes_for_format(format_, dst.width));
                        if (bottom_up_ && format_ == CameraPixelFormat::BGRA32 && dst.height > 0U) {
                            const BYTE* scan0 = raw + static_cast<size_t>(dst.height - 1U) * static_cast<size_t>(abs_pitch);
                            copy_locked_planes(scan0, raw, -abs_pitch, cur_len, dst);
                        } else {
                            copy_locked_planes(raw, raw, abs_pitch, cur_len, dst);
                        }
                        buf->Unlock();
                        copied = true;
                    }
                }
                if (copied) {
                    slot_.end_write_and_publish(write_idx);
                }
            }
        }

        IMFSourceReader* reader = reader_.load(std::memory_order_acquire);
        if (reader != nullptr && active_.load(std::memory_order_acquire)) {
            (void)reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr,
                                     nullptr, nullptr);
        }
        return S_OK;
    }

    STDMETHODIMP OnFlush(DWORD /*stream_idx*/) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD /*stream_idx*/, IMFMediaEvent* /*event*/) override { return S_OK; }

  private:
    void copy_locked_planes(const uint8_t* scan0, const uint8_t* buf_start, LONG pitch, DWORD total_bytes,
                            CapturedFrameBuffer& dst) const noexcept {
        if (scan0 == nullptr || buf_start == nullptr) {
            return;
        }
        const uint8_t* buf_end = buf_start + static_cast<size_t>(total_bytes);
        if (format_ == CameraPixelFormat::NV12) {
            dst.stride_y = dst.width;
            dst.stride_uv = dst.width;
            const uint32_t y_rows = dst.height;
            const uint32_t uv_rows = dst.height / 2U;
            uint8_t* dst_y = dst.bytes.data();
            uint8_t* dst_uv = dst.bytes.data() + static_cast<size_t>(dst.stride_y) * y_rows;
            for (uint32_t r = 0; r < y_rows; ++r) {
                const uint8_t* row_ptr = scan0 + static_cast<ptrdiff_t>(r) * pitch;
                if (row_ptr >= buf_start && row_ptr + dst.width <= buf_end) {
                    std::memcpy(dst_y + static_cast<size_t>(r) * dst.stride_y, row_ptr, dst.width);
                }
            }
            const uint8_t* src_uv_base = scan0 + static_cast<ptrdiff_t>(y_rows) * pitch;
            for (uint32_t r = 0; r < uv_rows; ++r) {
                const uint8_t* row_ptr = src_uv_base + static_cast<ptrdiff_t>(r) * pitch;
                if (row_ptr >= buf_start && row_ptr + dst.width <= buf_end) {
                    std::memcpy(dst_uv + static_cast<size_t>(r) * dst.stride_uv, row_ptr, dst.width);
                }
            }
        } else {
            const uint32_t row_bytes = row_bytes_for_format(format_, dst.width);
            dst.stride_y = row_bytes;
            dst.stride_uv = 0;
            for (uint32_t r = 0; r < dst.height; ++r) {
                const uint8_t* row_ptr = scan0 + static_cast<ptrdiff_t>(r) * pitch;
                if (row_ptr >= buf_start && row_ptr + row_bytes <= buf_end) {
                    std::memcpy(dst.bytes.data() + static_cast<size_t>(r) * row_bytes, row_ptr, row_bytes);
                }
            }
        }
    }

    LatestFrameSlot& slot_;
    std::atomic<IMFSourceReader*> reader_{nullptr};
    std::atomic<bool> active_{false};
    std::atomic<ULONG> ref_count_{1UL};
    uint32_t width_{1280};
    uint32_t height_{720};
    CameraPixelFormat format_{CameraPixelFormat::NV12};
    bool bottom_up_{false};
    uint64_t seq_{0};
};

struct NativeCameraMode {
    UINT32 width{0};
    UINT32 height{0};
    UINT32 fps_num{30};
    UINT32 fps_den{1};
    UINT32 fps_rounded{30};
    GUID subtype{};
    int score{0};
};

[[nodiscard]] bool subtype_to_pixel_format(const GUID& sub, CameraPixelFormat& out_fmt) noexcept {
    if (sub == MFVideoFormat_NV12) {
        out_fmt = CameraPixelFormat::NV12;
        return true;
    }
    if (sub == MFVideoFormat_YUY2) {
        out_fmt = CameraPixelFormat::YUY2;
        return true;
    }
    if (sub == MFVideoFormat_RGB32 || sub == MFVideoFormat_ARGB32) {
        out_fmt = CameraPixelFormat::BGRA32;
        return true;
    }
    return false;
}

[[nodiscard]] bool try_set_output_format(IMFSourceReader* reader, UINT32 width, UINT32 height, UINT32 fps_num,
                                         UINT32 fps_den, const GUID& subtype, bool include_fps) noexcept {
    if (reader == nullptr) {
        return false;
    }
    ComPtr<IMFMediaType> target_type;
    if (FAILED(::MFCreateMediaType(&target_type)) || !target_type) {
        return false;
    }
    if (FAILED(target_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(target_type->SetGUID(MF_MT_SUBTYPE, subtype)) ||
        FAILED(::MFSetAttributeSize(target_type.Get(), MF_MT_FRAME_SIZE, width, height))) {
        return false;
    }
    if (include_fps && fps_num > 0U && fps_den > 0U) {
        (void)::MFSetAttributeRatio(target_type.Get(), MF_MT_FRAME_RATE, fps_num, fps_den);
    }
    return SUCCEEDED(
        reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, target_type.Get()));
}

[[nodiscard]] bool negotiate_stream_format(IMFSourceReader* reader, uint32_t req_width, uint32_t req_height,
                                           uint32_t req_fps, uint32_t& out_width, uint32_t& out_height,
                                           uint32_t& out_fps, CameraPixelFormat& out_fmt, bool& out_bottom_up) {
    if (reader == nullptr) {
        return false;
    }

    std::vector<NativeCameraMode> native_modes;
    for (DWORD idx = 0; idx < 128U; ++idx) {
        ComPtr<IMFMediaType> nt;
        const HRESULT hr =
            reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), idx, &nt);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr) || !nt) {
            break;
        }
        GUID major{};
        GUID sub{};
        UINT32 w = 0;
        UINT32 h = 0;
        UINT32 fps_num = 30;
        UINT32 fps_den = 1;
        if (FAILED(nt->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video) {
            continue;
        }
        if (FAILED(nt->GetGUID(MF_MT_SUBTYPE, &sub))) {
            continue;
        }
        if (FAILED(::MFGetAttributeSize(nt.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0U || h == 0U ||
            w > kMaxCamWidth || h > kMaxCamHeight) {
            continue;
        }
        if (FAILED(::MFGetAttributeRatio(nt.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den)) || fps_den == 0U ||
            fps_num == 0U) {
            fps_num = 30U;
            fps_den = 1U;
        }
        const UINT32 fps_rounded = std::max(1U, (fps_num + fps_den / 2U) / fps_den);
        if (fps_rounded < 15U) {
            continue;
        }

        int fmt_score = -1;
        if (sub == MFVideoFormat_NV12) {
            fmt_score = 40;
        } else if (sub == MFVideoFormat_MJPG) {
            fmt_score = 32;
        } else if (sub == MFVideoFormat_YUY2) {
            fmt_score = 28;
        } else if (sub == MFVideoFormat_RGB32 || sub == MFVideoFormat_ARGB32) {
            fmt_score = 20;
        } else {
            fmt_score = 8;
        }

        int res_score = 0;
        if (w == req_width && h == req_height) {
            res_score = 1000;
        } else if (w == 1280U && h == 720U) {
            res_score = 900;
        } else if (w == 640U && h == 480U) {
            res_score = 700;
        } else if (w >= 640U && h >= 480U && w <= 1920U && h <= 1080U) {
            res_score = 400 - static_cast<int>(std::abs(static_cast<int>(w) - 1280) / 16);
        } else {
            res_score = 100;
        }

        int fps_score = 0;
        if (fps_rounded >= req_fps) {
            fps_score = 200;
        } else if (fps_rounded >= 55U) {
            fps_score = 180;
        } else if (fps_rounded >= 28U) {
            fps_score = 140;
        } else {
            fps_score = static_cast<int>(fps_rounded);
        }

        NativeCameraMode mode{};
        mode.width = w;
        mode.height = h;
        mode.fps_num = fps_num;
        mode.fps_den = fps_den;
        mode.fps_rounded = fps_rounded;
        mode.subtype = sub;
        mode.score = res_score + fps_score + fmt_score;
        native_modes.push_back(mode);
    }

    std::stable_sort(native_modes.begin(), native_modes.end(), [](const NativeCameraMode& a, const NativeCameraMode& b) {
        return a.score > b.score;
    });

    bool negotiated = false;
    for (const NativeCameraMode& m : native_modes) {
        const GUID* candidates_nv12_first[3] = {&MFVideoFormat_NV12, &MFVideoFormat_YUY2, &MFVideoFormat_RGB32};
        const GUID* candidates_yuy2_first[3] = {&MFVideoFormat_YUY2, &MFVideoFormat_NV12, &MFVideoFormat_RGB32};
        const GUID* candidates_rgb_first[3] = {&MFVideoFormat_RGB32, &MFVideoFormat_NV12, &MFVideoFormat_YUY2};
        const GUID* const* order = candidates_nv12_first;
        if (m.subtype == MFVideoFormat_YUY2) {
            order = candidates_yuy2_first;
        } else if (m.subtype == MFVideoFormat_RGB32 || m.subtype == MFVideoFormat_ARGB32) {
            order = candidates_rgb_first;
        }
        for (int k = 0; k < 3; ++k) {
            if (try_set_output_format(reader, m.width, m.height, m.fps_num, m.fps_den, *order[k], true)) {
                negotiated = true;
                break;
            }
        }
        if (negotiated) {
            break;
        }
    }

    if (!negotiated) {
        struct FallbackDim {
            UINT32 w;
            UINT32 h;
            UINT32 fps;
        };
        const FallbackDim dims[] = {
            {req_width, req_height, req_fps},
            {1280U, 720U, 60U},
            {1280U, 720U, 30U},
            {640U, 480U, 60U},
            {640U, 480U, 30U},
        };
        const GUID* fmts[] = {&MFVideoFormat_NV12, &MFVideoFormat_YUY2, &MFVideoFormat_RGB32};
        for (const FallbackDim& d : dims) {
            for (const GUID* fmt : fmts) {
                if (try_set_output_format(reader, d.w, d.h, d.fps, 1U, *fmt, true) ||
                    try_set_output_format(reader, d.w, d.h, 0U, 1U, *fmt, false)) {
                    negotiated = true;
                    break;
                }
            }
            if (negotiated) {
                break;
            }
        }
    }

    ComPtr<IMFMediaType> final_type;
    if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &final_type)) ||
        !final_type) {
        return false;
    }
    GUID final_sub{};
    if (FAILED(final_type->GetGUID(MF_MT_SUBTYPE, &final_sub)) || !subtype_to_pixel_format(final_sub, out_fmt)) {
        return false;
    }
    UINT32 aw = 0;
    UINT32 ah = 0;
    if (SUCCEEDED(::MFGetAttributeSize(final_type.Get(), MF_MT_FRAME_SIZE, &aw, &ah)) && aw > 0U && ah > 0U &&
        aw <= kMaxCamWidth && ah <= kMaxCamHeight) {
        out_width = aw;
        out_height = ah;
    } else {
        out_width = req_width;
        out_height = req_height;
    }
    UINT32 num = req_fps;
    UINT32 den = 1U;
    if (SUCCEEDED(::MFGetAttributeRatio(final_type.Get(), MF_MT_FRAME_RATE, &num, &den)) && den > 0U && num > 0U) {
        out_fps = std::max(1U, (num + den / 2U) / den);
    } else {
        out_fps = req_fps;
    }
    out_bottom_up = false;
    UINT32 raw_stride = 0U;
    if (SUCCEEDED(final_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))) {
        const auto signed_stride = static_cast<INT32>(raw_stride);
        out_bottom_up = (signed_stride < 0);
    }
    return true;
}

class MfCameraSource final : public ICameraSource {
  public:
    MfCameraSource(std::string symlink, uint32_t w, uint32_t h, uint32_t fps)
        : symlink_(std::move(symlink)), req_width_(w), req_height_(h), req_fps_(fps), width_(w), height_(h),
          fps_(fps) {}

    ~MfCameraSource() override { close(); }

    AetherStatus open() override {
        close();
        if (!mf_.ready()) {
            return AETHER_E_INTERNAL;
        }
        width_ = req_width_;
        height_ = req_height_;
        fps_ = req_fps_;

        ComPtr<IMFAttributes> enum_attr;
        if (FAILED(::MFCreateAttributes(&enum_attr, 1))) {
            return AETHER_E_INTERNAL;
        }
        enum_attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        if (FAILED(::MFEnumDeviceSources(enum_attr.Get(), &devices, &count)) || count == 0U) {
            return AETHER_E_IO;
        }

        struct DeviceCandidate {
            ComPtr<IMFActivate> activate;
            std::string friendly_name;
            std::string symbolic_link;
            bool is_virtual{false};
        };
        std::vector<DeviceCandidate> candidates;
        candidates.reserve(count);
        for (UINT32 i = 0; i < count; ++i) {
            DeviceCandidate dc{};
            dc.activate = devices[i];
            WCHAR* fname = nullptr;
            UINT32 flen = 0;
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &fname, &flen))) {
                dc.friendly_name = wide_to_utf8(fname);
                ::CoTaskMemFree(fname);
            }
            WCHAR* slink = nullptr;
            UINT32 slen = 0;
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                         &slink, &slen))) {
                dc.symbolic_link = wide_to_utf8(slink);
                ::CoTaskMemFree(slink);
            }
            dc.is_virtual = is_likely_virtual_camera(dc.friendly_name, dc.symbolic_link);
            candidates.push_back(std::move(dc));
            devices[i]->Release();
        }
        ::CoTaskMemFree(static_cast<void*>(devices));

        std::vector<size_t> ordered;
        ordered.reserve(candidates.size());
        std::vector<bool> used(candidates.size(), false);
        if (!symlink_.empty()) {
            for (size_t i = 0; i < candidates.size(); ++i) {
                if (candidates[i].symbolic_link == symlink_ || equals_ignore_case(candidates[i].friendly_name, symlink_)) {
                    ordered.push_back(i);
                    used[i] = true;
                }
            }
        }
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (!used[i] && !candidates[i].is_virtual) {
                ordered.push_back(i);
                used[i] = true;
            }
        }
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (!used[i]) {
                ordered.push_back(i);
                used[i] = true;
            }
        }

        for (size_t idx : ordered) {
            const DeviceCandidate& dc = candidates[idx];
            if (!dc.activate) {
                continue;
            }
            if (FAILED(dc.activate->ActivateObject(IID_PPV_ARGS(&media_source_))) || !media_source_) {
                AETHER_LOG_WARN("Tracking", "Skipping camera '%s' (ActivateObject failed)", dc.friendly_name.c_str());
                media_source_.Reset();
                continue;
            }

            callback_.Attach(new MfReaderCallback(slot_, req_width_, req_height_, CameraPixelFormat::NV12));
            ComPtr<IMFAttributes> reader_attr;
            if (SUCCEEDED(::MFCreateAttributes(&reader_attr, 3)) && reader_attr) {
                reader_attr->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get());
                reader_attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
                reader_attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            }

            if (FAILED(::MFCreateSourceReaderFromMediaSource(media_source_.Get(), reader_attr.Get(), &reader_)) ||
                !reader_) {
                reader_.Reset();
                ComPtr<IMFAttributes> fallback_attr;
                if (SUCCEEDED(::MFCreateAttributes(&fallback_attr, 2)) && fallback_attr) {
                    fallback_attr->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get());
                    fallback_attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
                }
                if (FAILED(::MFCreateSourceReaderFromMediaSource(media_source_.Get(), fallback_attr.Get(), &reader_)) ||
                    !reader_) {
                    AETHER_LOG_WARN("Tracking", "Skipping camera '%s' (CreateSourceReader failed)",
                                    dc.friendly_name.c_str());
                    close();
                    continue;
                }
            }

            uint32_t neg_w = req_width_;
            uint32_t neg_h = req_height_;
            uint32_t neg_fps = req_fps_;
            CameraPixelFormat neg_fmt = CameraPixelFormat::NV12;
            bool neg_bottom_up = false;
            if (!negotiate_stream_format(reader_.Get(), req_width_, req_height_, req_fps_, neg_w, neg_h, neg_fps,
                                         neg_fmt, neg_bottom_up)) {
                AETHER_LOG_WARN("Tracking", "Skipping camera '%s' (format negotiation failed)",
                                dc.friendly_name.c_str());
                close();
                continue;
            }

            width_ = neg_w;
            height_ = neg_h;
            fps_ = neg_fps;
            resolved_symlink_ = dc.symbolic_link;
            friendly_name_ = dc.friendly_name;

            callback_->update_format(width_, height_, neg_fmt, neg_bottom_up);
            callback_->set_reader(reader_.Get());
            callback_->set_active(true);

            if (FAILED(reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr,
                                           nullptr, nullptr))) {
                AETHER_LOG_WARN("Tracking", "Skipping camera '%s' (initial ReadSample failed)",
                                dc.friendly_name.c_str());
                close();
                continue;
            }

            opened_ = true;
            AETHER_LOG_INFO("Tracking", "Opened camera '%s' (%ux%u @ %u fps, %s)", friendly_name_.c_str(), width_,
                            height_, fps_, pixel_format_name(neg_fmt));
            return AETHER_OK;
        }

        return AETHER_E_IO;
    }

    void close() noexcept override {
        opened_ = false;
        if (callback_) {
            callback_->set_active(false);
            callback_->set_reader(nullptr);
        }
        if (reader_) {
            (void)reader_->Flush(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS));
            reader_.Reset();
        }
        if (media_source_) {
            (void)media_source_->Shutdown();
            media_source_.Reset();
        }
        callback_.Reset();
    }

    bool read_latest_blocking(uint32_t timeout_ms, const CapturedFrameBuffer*& out_frame) noexcept override {
        return slot_.wait_and_acquire_latest(timeout_ms, out_frame);
    }

    AetherStatus apply_controls(const CameraControls& controls) noexcept override {
        if (!media_source_) {
            return AETHER_E_INVALID_ARG;
        }
        ComPtr<IAMCameraControl> cam_ctrl;
        if (SUCCEEDED(media_source_.As(&cam_ctrl)) && cam_ctrl) {
            if (controls.manual_exposure) {
                (void)cam_ctrl->Set(CameraControl_Exposure, controls.exposure_log2, CameraControl_Flags_Manual);
            } else {
                (void)cam_ctrl->Set(CameraControl_Exposure, 0, CameraControl_Flags_Auto);
            }
            if (controls.manual_focus) {
                (void)cam_ctrl->Set(CameraControl_Focus, controls.focus_value, CameraControl_Flags_Manual);
            } else {
                (void)cam_ctrl->Set(CameraControl_Focus, 0, CameraControl_Flags_Auto);
            }
        }
        ComPtr<IAMVideoProcAmp> amp;
        if (SUCCEEDED(media_source_.As(&amp)) && amp) {
            if (controls.manual_white_balance) {
                (void)amp->Set(VideoProcAmp_WhiteBalance, controls.white_balance_kelvin, VideoProcAmp_Flags_Manual);
            } else {
                (void)amp->Set(VideoProcAmp_WhiteBalance, 0, VideoProcAmp_Flags_Auto);
            }
            (void)amp->Set(VideoProcAmp_Gain, controls.gain_percent, VideoProcAmp_Flags_Manual);
        }
        return AETHER_OK;
    }

    uint32_t width() const noexcept override { return width_; }
    uint32_t height() const noexcept override { return height_; }
    uint32_t fps() const noexcept override { return fps_; }
    bool is_synthetic() const noexcept override { return false; }
    bool is_open() const noexcept override { return opened_; }
    std::string device_symlink() const override { return resolved_symlink_; }
    std::string source_label() const override {
        return friendly_name_.empty() ? std::string("MediaFoundation Camera") : friendly_name_;
    }

  private:
    MfSessionGuard mf_{};
    std::string symlink_{};
    std::string resolved_symlink_{};
    std::string friendly_name_{};
    uint32_t req_width_{1280};
    uint32_t req_height_{720};
    uint32_t req_fps_{60};
    uint32_t width_{1280};
    uint32_t height_{720};
    uint32_t fps_{60};
    bool opened_{false};
    LatestFrameSlot slot_{};
    ComPtr<IMFMediaSource> media_source_;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<MfReaderCallback> callback_;
};

} // namespace

LatestFrameSlot::LatestFrameSlot() noexcept {
    frame_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

LatestFrameSlot::~LatestFrameSlot() {
    if (frame_event_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(frame_event_));
        frame_event_ = nullptr;
    }
}

CapturedFrameBuffer& LatestFrameSlot::begin_write() noexcept {
    const uint8_t ready = ready_idx_.load(std::memory_order_acquire);
    for (uint8_t i = 0; i < 3U; ++i) {
        if (i != read_idx_ && i != ready) {
            write_idx_ = i;
            break;
        }
    }
    return slots_[write_idx_];
}

void LatestFrameSlot::end_write_and_publish(uint8_t written_idx) noexcept {
    if (has_new_.exchange(true, std::memory_order_acq_rel)) {
        dropped_.fetch_add(1ULL, std::memory_order_relaxed);
    }
    ready_idx_.store(written_idx, std::memory_order_release);
    if (frame_event_ != nullptr) {
        ::SetEvent(static_cast<HANDLE>(frame_event_));
    }
}

bool LatestFrameSlot::wait_and_acquire_latest(uint32_t timeout_ms, const CapturedFrameBuffer*& out_frame) noexcept {
    if (!has_new_.load(std::memory_order_acquire) && frame_event_ != nullptr && timeout_ms > 0U) {
        (void)::WaitForSingleObject(static_cast<HANDLE>(frame_event_), timeout_ms);
    }
    if (!has_new_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    read_idx_ = ready_idx_.load(std::memory_order_acquire);
    out_frame = &slots_[read_idx_];
    return true;
}

std::vector<CameraDeviceEntry> enumerate_camera_devices() {
    std::vector<CameraDeviceEntry> list;
    MfSessionGuard mf;
    if (!mf.ready()) {
        return list;
    }
    ComPtr<IMFAttributes> attr;
    if (FAILED(::MFCreateAttributes(&attr, 1))) {
        return list;
    }
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(::MFEnumDeviceSources(attr.Get(), &devices, &count))) {
        return list;
    }
    for (UINT32 i = 0; i < count; ++i) {
        CameraDeviceEntry entry{};
        WCHAR* fname = nullptr;
        UINT32 flen = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &fname, &flen))) {
            entry.friendly_name = wide_to_utf8(fname);
            ::CoTaskMemFree(fname);
        }
        WCHAR* slink = nullptr;
        UINT32 slen = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &slink,
                                                     &slen))) {
            entry.symbolic_link = wide_to_utf8(slink);
            ::CoTaskMemFree(slink);
        }
        entry.likely_virtual = is_likely_virtual_camera(entry.friendly_name, entry.symbolic_link);
        list.push_back(std::move(entry));
        devices[i]->Release();
    }
    if (devices != nullptr) {
        ::CoTaskMemFree(static_cast<void*>(devices));
    }
    std::stable_partition(list.begin(), list.end(), [](const CameraDeviceEntry& e) { return !e.likely_virtual; });
    return list;
}

std::filesystem::path default_camera_config_path() {
    WCHAR* appdata = nullptr;
    DWORD len = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (len > 1U) {
        std::wstring buf(len, L'\0');
        ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf.data(), len);
        buf.resize(len - 1U);
        return std::filesystem::path(buf) / "Aether" / "camera_config.toml";
    }
    (void)appdata;
    return std::filesystem::temp_directory_path() / "aether_camera_config.toml";
}

AetherStatus load_persisted_camera_config(const std::filesystem::path& path, PersistedCameraConfig& out_cfg) {
    out_cfg = PersistedCameraConfig{};
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return AETHER_E_IO;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
        };
        trim(key);
        trim(val);
        if (val.size() >= 2U && val.front() == '"' && val.back() == '"') {
            val = val.substr(1U, val.size() - 2U);
        }
        if (key == "selected_symlink")
            out_cfg.selected_symlink = val;
        else if (key == "width")
            out_cfg.width = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        else if (key == "height")
            out_cfg.height = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        else if (key == "fps")
            out_cfg.fps = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        else if (key == "manual_exposure")
            out_cfg.controls.manual_exposure = (val == "true" || val == "1");
        else if (key == "exposure_log2")
            out_cfg.controls.exposure_log2 = std::atoi(val.c_str());
        else if (key == "manual_focus")
            out_cfg.controls.manual_focus = (val == "true" || val == "1");
        else if (key == "focus_value")
            out_cfg.controls.focus_value = std::atoi(val.c_str());
        else if (key == "manual_white_balance")
            out_cfg.controls.manual_white_balance = (val == "true" || val == "1");
        else if (key == "white_balance_kelvin")
            out_cfg.controls.white_balance_kelvin = std::atoi(val.c_str());
        else if (key == "gain_percent")
            out_cfg.controls.gain_percent = std::atoi(val.c_str());
    }
    return AETHER_OK;
}

AetherStatus save_persisted_camera_config(const std::filesystem::path& path, const PersistedCameraConfig& cfg) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        return AETHER_E_INTERNAL;
    }
    ofs << "# AETHER-2.5D persisted camera metadata (HR-12: zero pixels ever persisted)\n";
    ofs << "selected_symlink = \"" << cfg.selected_symlink << "\"\n";
    ofs << "width = " << cfg.width << "\n";
    ofs << "height = " << cfg.height << "\n";
    ofs << "fps = " << cfg.fps << "\n";
    ofs << "manual_exposure = " << (cfg.controls.manual_exposure ? "true" : "false") << "\n";
    ofs << "exposure_log2 = " << cfg.controls.exposure_log2 << "\n";
    ofs << "manual_focus = " << (cfg.controls.manual_focus ? "true" : "false") << "\n";
    ofs << "focus_value = " << cfg.controls.focus_value << "\n";
    ofs << "manual_white_balance = " << (cfg.controls.manual_white_balance ? "true" : "false") << "\n";
    ofs << "white_balance_kelvin = " << cfg.controls.white_balance_kelvin << "\n";
    ofs << "gain_percent = " << cfg.controls.gain_percent << "\n";
    return AETHER_OK;
}

std::unique_ptr<ICameraSource> create_mf_camera_source(std::string symbolic_link, uint32_t width, uint32_t height,
                                                       uint32_t fps) {
    return std::make_unique<MfCameraSource>(std::move(symbolic_link), width, height, fps);
}

} // namespace aether::tracking
