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
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
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

    void update_format(uint32_t w, uint32_t h, CameraPixelFormat fmt) noexcept {
        width_ = w;
        height_ = h;
        format_ = fmt;
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
                        copy_locked_planes(scan0, pitch, buf_len, dst);
                        buf2d->Unlock2D();
                        copied = true;
                    }
                }
                if (!copied) {
                    BYTE* raw = nullptr;
                    DWORD max_len = 0;
                    DWORD cur_len = 0;
                    if (SUCCEEDED(buf->Lock(&raw, &max_len, &cur_len))) {
                        const LONG default_pitch = (format_ == CameraPixelFormat::NV12)
                                                       ? static_cast<LONG>(dst.width)
                                                       : static_cast<LONG>(dst.width * 4U);
                        copy_locked_planes(raw, default_pitch, cur_len, dst);
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
    void copy_locked_planes(const uint8_t* src, LONG pitch, DWORD total_bytes,
                            CapturedFrameBuffer& dst) const noexcept {
        if (src == nullptr) {
            return;
        }
        const uint32_t abs_pitch = static_cast<uint32_t>(pitch >= 0 ? pitch : -pitch);
        if (format_ == CameraPixelFormat::NV12) {
            dst.stride_y = dst.width;
            dst.stride_uv = dst.width;
            const uint32_t y_rows = dst.height;
            const uint32_t uv_rows = dst.height / 2U;
            uint8_t* dst_y = dst.bytes.data();
            uint8_t* dst_uv = dst.bytes.data() + static_cast<size_t>(dst.stride_y) * y_rows;
            for (uint32_t r = 0; r < y_rows; ++r) {
                const size_t s_off = static_cast<size_t>(r) * abs_pitch;
                if (s_off + dst.width <= total_bytes) {
                    std::memcpy(dst_y + static_cast<size_t>(r) * dst.stride_y, src + s_off, dst.width);
                }
            }
            const size_t src_uv_base = static_cast<size_t>(abs_pitch) * y_rows;
            for (uint32_t r = 0; r < uv_rows; ++r) {
                const size_t s_off = src_uv_base + static_cast<size_t>(r) * abs_pitch;
                if (s_off + dst.width <= total_bytes) {
                    std::memcpy(dst_uv + static_cast<size_t>(r) * dst.stride_uv, src + s_off, dst.width);
                }
            }
        } else {
            dst.stride_y = dst.width * 4U;
            dst.stride_uv = 0;
            for (uint32_t r = 0; r < dst.height; ++r) {
                const size_t s_off = static_cast<size_t>(r) * abs_pitch;
                if (s_off + dst.stride_y <= total_bytes) {
                    std::memcpy(dst.bytes.data() + static_cast<size_t>(r) * dst.stride_y, src + s_off, dst.stride_y);
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
    uint64_t seq_{0};
};

class MfCameraSource final : public ICameraSource {
  public:
    MfCameraSource(std::string symlink, uint32_t w, uint32_t h, uint32_t fps)
        : symlink_(std::move(symlink)), width_(w), height_(h), fps_(fps) {}

    ~MfCameraSource() override { close(); }

    AetherStatus open() override {
        close();
        if (!mf_.ready()) {
            return AETHER_E_INTERNAL;
        }

        // Enumerate devices and match symbolic link (or pick first device if empty)
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

        ComPtr<IMFActivate> chosen;
        for (UINT32 i = 0; i < count; ++i) {
            WCHAR* slink = nullptr;
            UINT32 slen = 0;
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                         &slink, &slen))) {
                const std::string cur = wide_to_utf8(slink);
                ::CoTaskMemFree(slink);
                if (!chosen || (!symlink_.empty() && cur == symlink_)) {
                    chosen = devices[i];
                    resolved_symlink_ = cur;
                    if (!symlink_.empty() && cur == symlink_) {
                        WCHAR* fname = nullptr;
                        UINT32 flen = 0;
                        if (SUCCEEDED(
                                devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &fname, &flen))) {
                            friendly_name_ = wide_to_utf8(fname);
                            ::CoTaskMemFree(fname);
                        }
                        break;
                    }
                }
            }
            if (!chosen) {
                chosen = devices[i];
            }
            if (friendly_name_.empty()) {
                WCHAR* fname = nullptr;
                UINT32 flen = 0;
                if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &fname, &flen))) {
                    friendly_name_ = wide_to_utf8(fname);
                    ::CoTaskMemFree(fname);
                }
            }
        }
        for (UINT32 i = 0; i < count; ++i) {
            devices[i]->Release();
        }
        ::CoTaskMemFree(static_cast<void*>(devices));

        if (!chosen) {
            return AETHER_E_IO;
        }
        if (FAILED(chosen->ActivateObject(IID_PPV_ARGS(&media_source_)))) {
            return AETHER_E_INTERNAL;
        }

        callback_.Attach(new MfReaderCallback(slot_, width_, height_, CameraPixelFormat::NV12));
        ComPtr<IMFAttributes> reader_attr;
        ::MFCreateAttributes(&reader_attr, 2);
        reader_attr->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get());
        reader_attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

        if (FAILED(::MFCreateSourceReaderFromMediaSource(media_source_.Get(), reader_attr.Get(), &reader_))) {
            close();
            return AETHER_E_INTERNAL;
        }

        // Request NV12 at target dimensions/FPS, else fall back cleanly to native RGB32
        ComPtr<IMFMediaType> target_type;
        ::MFCreateMediaType(&target_type);
        target_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        target_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(target_type.Get(), MF_MT_FRAME_SIZE, width_, height_);
        MFSetAttributeRatio(target_type.Get(), MF_MT_FRAME_RATE, fps_, 1);

        CameraPixelFormat fmt = CameraPixelFormat::NV12;
        if (FAILED(reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
                                                target_type.Get()))) {
            target_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            fmt = CameraPixelFormat::BGRA32;
            if (FAILED(reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
                                                    target_type.Get()))) {
                ComPtr<IMFMediaType> cur;
                if (SUCCEEDED(
                        reader_->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur)) &&
                    cur) {
                    UINT32 aw = 0, ah = 0;
                    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &aw, &ah);
                    GUID sub{};
                    cur->GetGUID(MF_MT_SUBTYPE, &sub);
                    fmt = (sub == MFVideoFormat_NV12) ? CameraPixelFormat::NV12 : CameraPixelFormat::BGRA32;
                    width_ = aw > 0 ? aw : width_;
                    height_ = ah > 0 ? ah : height_;
                }
            }
        }
        ComPtr<IMFMediaType> final_type;
        if (SUCCEEDED(
                reader_->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &final_type)) &&
            final_type) {
            UINT32 aw = 0, ah = 0;
            if (SUCCEEDED(MFGetAttributeSize(final_type.Get(), MF_MT_FRAME_SIZE, &aw, &ah)) && aw > 0 && ah > 0) {
                width_ = aw;
                height_ = ah;
            }
        }
        callback_->update_format(width_, height_, fmt);
        callback_->set_reader(reader_.Get());
        callback_->set_active(true);

        if (FAILED(reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr,
                                       nullptr, nullptr))) {
            close();
            return AETHER_E_INTERNAL;
        }
        return AETHER_OK;
    }

    void close() noexcept override {
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
    std::string source_label() const override {
        return friendly_name_.empty() ? std::string("MediaFoundation Camera") : friendly_name_;
    }

  private:
    MfSessionGuard mf_{};
    std::string symlink_{};
    std::string resolved_symlink_{};
    std::string friendly_name_{};
    uint32_t width_{1280};
    uint32_t height_{720};
    uint32_t fps_{60};
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
        list.push_back(std::move(entry));
        devices[i]->Release();
    }
    if (devices != nullptr) {
        ::CoTaskMemFree(static_cast<void*>(devices));
    }
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
