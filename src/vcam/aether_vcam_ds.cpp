#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <dshow.h>
#include <olectl.h>
#include <initguid.h>
// clang-format on

#ifndef E_PROP_SET_UNSUPPORTED
#define E_PROP_SET_UNSUPPORTED HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND)
#endif

#include "vcam/guids.h"
#include "vcam/vcam_shm.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

namespace {

HMODULE g_hmodule = nullptr;
std::atomic<long> g_dll_refs{0};

inline uint64_t qpc_now_us() noexcept {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER cnt{};
    QueryPerformanceCounter(&cnt);
    return static_cast<uint64_t>((cnt.QuadPart * 1'000'000LL) / freq.QuadPart);
}

inline long interlocked_read_long(const volatile long* p) noexcept {
    const long v = InterlockedCompareExchange(const_cast<volatile long*>(p), 0L, 0L);
    MemoryBarrier();
    return v;
}

struct VcamCapDesc {
    uint32_t width;
    uint32_t height;
    const GUID* subtype;
    WORD bit_count;
    DWORD compression;
    uint32_t fps;
};

constexpr VcamCapDesc kCapabilities[] = {
    {1920u, 1080u, &MEDIASUBTYPE_ARGB32, 32u, BI_RGB, 60u},
    {1920u, 1080u, &MEDIASUBTYPE_RGB32, 32u, BI_RGB, 60u},
    {1920u, 1080u, &MEDIASUBTYPE_NV12, 12u, MAKEFOURCC('N', 'V', '1', '2'), 60u},
    {1280u, 720u, &MEDIASUBTYPE_ARGB32, 32u, BI_RGB, 60u},
    {1280u, 720u, &MEDIASUBTYPE_RGB32, 32u, BI_RGB, 60u},
    {1280u, 720u, &MEDIASUBTYPE_NV12, 12u, MAKEFOURCC('N', 'V', '1', '2'), 60u},
};

constexpr int kCapCount = static_cast<int>(sizeof(kCapabilities) / sizeof(kCapabilities[0]));

// Minimal embedded 5x7 ASCII glyph table for offline slate ("Aether is not running")
struct Glyph5x7 {
    char ch;
    uint8_t cols[5];
};

constexpr Glyph5x7 kFont5x7[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}}, {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, {'a', {0x20, 0x54, 0x54, 0x54, 0x78}},
    {'e', {0x38, 0x54, 0x54, 0x54, 0x18}}, {'g', {0x18, 0xA4, 0xA4, 0xA4, 0x7C}}, {'h', {0x7F, 0x08, 0x04, 0x04, 0x78}},
    {'i', {0x00, 0x44, 0x7D, 0x40, 0x00}}, {'n', {0x7C, 0x08, 0x04, 0x04, 0x78}}, {'o', {0x38, 0x44, 0x44, 0x44, 0x38}},
    {'r', {0x7C, 0x08, 0x04, 0x04, 0x08}}, {'s', {0x48, 0x54, 0x54, 0x54, 0x20}}, {'t', {0x04, 0x3F, 0x44, 0x40, 0x20}},
    {'u', {0x3C, 0x40, 0x40, 0x20, 0x7C}},
};

const uint8_t* find_glyph_cols(char ch) noexcept {
    for (const auto& g : kFont5x7) {
        if (g.ch == ch) {
            return g.cols;
        }
    }
    return kFont5x7[0].cols;
}

void draw_offline_slate_bgra(uint8_t* dst, uint32_t w, uint32_t h, bool bottom_up) noexcept {
    // Precision Dark surface #131417 in BGRA
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = dst + static_cast<size_t>(y) * w * 4u;
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 4u + 0u] = 0x17u;
            row[x * 4u + 1u] = 0x14u;
            row[x * 4u + 2u] = 0x13u;
            row[x * 4u + 3u] = 0xFFu;
        }
    }

    const char* msg = "Aether is not running";
    const uint32_t len = static_cast<uint32_t>(std::strlen(msg));
    const uint32_t scale = std::max(w / 320u, 2u);
    const uint32_t char_adv = 6u * scale;
    const uint32_t text_w = len * char_adv;
    const uint32_t text_h = 7u * scale;
    const uint32_t start_x = (w > text_w) ? ((w - text_w) / 2u) : 0u;
    const uint32_t start_y = (h > text_h) ? ((h - text_h) / 2u) : 0u;

    for (uint32_t ci = 0; ci < len; ++ci) {
        const uint8_t* cols = find_glyph_cols(msg[ci]);
        const uint32_t gx = start_x + ci * char_adv;
        for (uint32_t col = 0; col < 5u; ++col) {
            const uint8_t bits = cols[col];
            for (uint32_t r = 0; r < 7u; ++r) {
                if ((bits & (1u << r)) == 0u) {
                    continue;
                }
                for (uint32_t sy = 0; sy < scale; ++sy) {
                    const uint32_t py = start_y + r * scale + sy;
                    if (py >= h) {
                        continue;
                    }
                    const uint32_t dst_y = bottom_up ? (h - 1u - py) : py;
                    uint8_t* row = dst + static_cast<size_t>(dst_y) * w * 4u;
                    for (uint32_t sx = 0; sx < scale; ++sx) {
                        const uint32_t px = gx + col * scale + sx;
                        if (px < w) {
                            row[px * 4u + 0u] = 0xEEu;
                            row[px * 4u + 1u] = 0xEBu;
                            row[px * 4u + 2u] = 0xEAu;
                            row[px * 4u + 3u] = 0xFFu;
                        }
                    }
                }
            }
        }
    }
}

void draw_offline_slate_nv12(uint8_t* dst, uint32_t w, uint32_t h) noexcept {
    // Precision Dark luminance Y ≈ 24, neutral Chroma UV = 128
    const size_t y_bytes = static_cast<size_t>(w) * h;
    const size_t uv_bytes = y_bytes / 2u;
    std::memset(dst, 24, y_bytes);
    std::memset(dst + y_bytes, 128, uv_bytes);

    const char* msg = "Aether is not running";
    const uint32_t len = static_cast<uint32_t>(std::strlen(msg));
    const uint32_t scale = std::max(w / 320u, 2u);
    const uint32_t char_adv = 6u * scale;
    const uint32_t text_w = len * char_adv;
    const uint32_t text_h = 7u * scale;
    const uint32_t start_x = (w > text_w) ? ((w - text_w) / 2u) : 0u;
    const uint32_t start_y = (h > text_h) ? ((h - text_h) / 2u) : 0u;

    for (uint32_t ci = 0; ci < len; ++ci) {
        const uint8_t* cols = find_glyph_cols(msg[ci]);
        const uint32_t gx = start_x + ci * char_adv;
        for (uint32_t col = 0; col < 5u; ++col) {
            const uint8_t bits = cols[col];
            for (uint32_t r = 0; r < 7u; ++r) {
                if ((bits & (1u << r)) == 0u) {
                    continue;
                }
                for (uint32_t sy = 0; sy < scale; ++sy) {
                    const uint32_t py = start_y + r * scale + sy;
                    if (py >= h) {
                        continue;
                    }
                    uint8_t* row = dst + static_cast<size_t>(py) * w;
                    for (uint32_t sx = 0; sx < scale; ++sx) {
                        const uint32_t px = gx + col * scale + sx;
                        if (px < w) {
                            row[px] = 225u;
                        }
                    }
                }
            }
        }
    }
}

void bgra_to_nv12_cpu(const uint8_t* bgra, uint32_t src_w, uint32_t src_h, bool src_bottom_up, uint8_t* out_nv12,
                      uint32_t dst_w, uint32_t dst_h) noexcept {
    uint8_t* y_plane = out_nv12;
    uint8_t* uv_plane = out_nv12 + static_cast<size_t>(dst_w) * dst_h;

    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        const uint32_t sy_raw = static_cast<uint32_t>((static_cast<uint64_t>(dy) * src_h) / dst_h);
        const uint32_t sy = src_bottom_up ? (src_h - 1u - sy_raw) : sy_raw;
        const uint8_t* srow = bgra + static_cast<size_t>(sy) * src_w * 4u;
        uint8_t* yrow = y_plane + static_cast<size_t>(dy) * dst_w;
        uint8_t* uvrow = ((dy & 1u) == 0u) ? (uv_plane + static_cast<size_t>(dy / 2u) * dst_w) : nullptr;

        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(dx) * src_w) / dst_w);
            const uint8_t* px = srow + static_cast<size_t>(sx) * 4u;
            const int b = px[0];
            const int g = px[1];
            const int r = px[2];
            yrow[dx] = static_cast<uint8_t>(std::clamp(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16, 16, 235));
            if (uvrow != nullptr && (dx & 1u) == 0u) {
                const int cb = ((-25 * r - 86 * g + 112 * b + 128) >> 8) + 128;
                const int cr = ((112 * r - 102 * g - 10 * b + 128) >> 8) + 128;
                uvrow[dx + 0u] = static_cast<uint8_t>(std::clamp(cb, 16, 240));
                uvrow[dx + 1u] = static_cast<uint8_t>(std::clamp(cr, 16, 240));
            }
        }
    }
}

uint32_t calc_image_size(const VcamCapDesc& cap) noexcept {
    if (cap.bit_count == 12u) {
        return cap.width * cap.height * 3u / 2u;
    }
    return cap.width * cap.height * 4u;
}

HRESULT copy_media_type(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src) noexcept {
    if (dst == nullptr || src == nullptr) {
        return E_POINTER;
    }
    *dst = *src;
    if (src->cbFormat > 0u && src->pbFormat != nullptr) {
        dst->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src->cbFormat));
        if (dst->pbFormat == nullptr) {
            return E_OUTOFMEMORY;
        }
        std::memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
    } else {
        dst->pbFormat = nullptr;
        dst->cbFormat = 0u;
    }
    if (dst->pUnk != nullptr) {
        dst->pUnk->AddRef();
    }
    return S_OK;
}

void free_media_type_contents(AM_MEDIA_TYPE& mt) noexcept {
    if (mt.cbFormat > 0u && mt.pbFormat != nullptr) {
        CoTaskMemFree(mt.pbFormat);
        mt.pbFormat = nullptr;
        mt.cbFormat = 0u;
    }
    if (mt.pUnk != nullptr) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

void init_media_type_from_cap(const VcamCapDesc& cap, AM_MEDIA_TYPE& mt, VIDEOINFOHEADER& vih) noexcept {
    std::memset(&mt, 0, sizeof(mt));
    std::memset(&vih, 0, sizeof(vih));

    const uint32_t img_bytes = calc_image_size(cap);
    vih.AvgTimePerFrame = static_cast<REFERENCE_TIME>(10'000'000LL / cap.fps);
    vih.dwBitRate = img_bytes * 8u * cap.fps;
    vih.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih.bmiHeader.biWidth = static_cast<LONG>(cap.width);
    vih.bmiHeader.biHeight = static_cast<LONG>(cap.height);
    vih.bmiHeader.biPlanes = 1u;
    vih.bmiHeader.biBitCount = cap.bit_count;
    vih.bmiHeader.biCompression = cap.compression;
    vih.bmiHeader.biSizeImage = img_bytes;

    mt.majortype = MEDIATYPE_Video;
    mt.subtype = *cap.subtype;
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = img_bytes;
    mt.formattype = FORMAT_VideoInfo;
    mt.cbFormat = sizeof(VIDEOINFOHEADER);
    mt.pbFormat = reinterpret_cast<BYTE*>(&vih);
}

bool validate_media_type(const AM_MEDIA_TYPE* pmt) noexcept {
    if (pmt == nullptr || !IsEqualGUID(pmt->majortype, MEDIATYPE_Video) ||
        !IsEqualGUID(pmt->formattype, FORMAT_VideoInfo) || pmt->pbFormat == nullptr ||
        pmt->cbFormat < sizeof(VIDEOINFOHEADER)) {
        return false;
    }
    const bool valid_sub = IsEqualGUID(pmt->subtype, MEDIASUBTYPE_ARGB32) ||
                           IsEqualGUID(pmt->subtype, MEDIASUBTYPE_RGB32) ||
                           IsEqualGUID(pmt->subtype, MEDIASUBTYPE_NV12);
    if (!valid_sub) {
        return false;
    }
    const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(pmt->pbFormat);
    const LONG w = vih->bmiHeader.biWidth;
    const LONG h = (vih->bmiHeader.biHeight < 0) ? -vih->bmiHeader.biHeight : vih->bmiHeader.biHeight;
    if (w <= 0 || h <= 0 || static_cast<uint32_t>(w) > AETHER_VCAM_MAX_WIDTH ||
        static_cast<uint32_t>(h) > AETHER_VCAM_MAX_HEIGHT) {
        return false;
    }
    return true;
}

class CAetherVCamFilter;

class CEnumMediaTypes final : public IEnumMediaTypes {
  public:
    explicit CEnumMediaTypes(int index = 0, const AM_MEDIA_TYPE* preferred = nullptr) noexcept : index_(index) {
        g_dll_refs.fetch_add(1L, std::memory_order_relaxed);
        if (preferred != nullptr && validate_media_type(preferred)) {
            if (SUCCEEDED(copy_media_type(&preferred_, preferred))) {
                has_preferred_ = true;
            }
        }
    }

    ~CEnumMediaTypes() noexcept {
        if (has_preferred_) {
            free_media_type_contents(preferred_);
        }
        g_dll_refs.fetch_sub(1L, std::memory_order_relaxed);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumMediaTypes)) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG c = static_cast<ULONG>(InterlockedDecrement(&ref_));
        if (c == 0u) {
            delete this;
        }
        return c;
    }

    IFACEMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) override {
        if (ppMediaTypes == nullptr) {
            return E_POINTER;
        }
        const int total = kCapCount + (has_preferred_ ? 1 : 0);
        ULONG fetched = 0u;
        while (fetched < cMediaTypes && index_ < total) {
            auto* pmt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
            if (pmt == nullptr) {
                return E_OUTOFMEMORY;
            }
            if (has_preferred_ && index_ == 0) {
                if (FAILED(copy_media_type(pmt, &preferred_))) {
                    CoTaskMemFree(pmt);
                    return E_OUTOFMEMORY;
                }
            } else {
                const int cap_idx = index_ - (has_preferred_ ? 1 : 0);
                AM_MEDIA_TYPE tmp{};
                VIDEOINFOHEADER vih{};
                init_media_type_from_cap(kCapabilities[cap_idx], tmp, vih);
                if (FAILED(copy_media_type(pmt, &tmp))) {
                    CoTaskMemFree(pmt);
                    return E_OUTOFMEMORY;
                }
            }
            ppMediaTypes[fetched++] = pmt;
            ++index_;
        }
        if (pcFetched != nullptr) {
            *pcFetched = fetched;
        }
        return (fetched == cMediaTypes) ? S_OK : S_FALSE;
    }

    IFACEMETHODIMP Skip(ULONG cMediaTypes) override {
        const int total = kCapCount + (has_preferred_ ? 1 : 0);
        index_ = std::min(total, index_ + static_cast<int>(cMediaTypes));
        return (index_ < total) ? S_OK : S_FALSE;
    }

    IFACEMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }

    IFACEMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
        if (ppEnum == nullptr) {
            return E_POINTER;
        }
        auto* e = new (std::nothrow) CEnumMediaTypes(index_, has_preferred_ ? &preferred_ : nullptr);
        if (e == nullptr) {
            return E_OUTOFMEMORY;
        }
        *ppEnum = e;
        return S_OK;
    }

  private:
    volatile long ref_{1};
    int index_{0};
    bool has_preferred_{false};
    AM_MEDIA_TYPE preferred_{};
};

class CAetherVCamPin final : public IPin, public IAMStreamConfig, public IKsPropertySet {
  public:
    explicit CAetherVCamPin(CAetherVCamFilter* filter) noexcept;
    ~CAetherVCamPin() noexcept;

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IPin
    IFACEMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    IFACEMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override {
        (void)pConnector;
        (void)pmt;
        return VFW_E_NOT_ALLOWED_TO_SAVE;
    }
    IFACEMETHODIMP Disconnect() override;
    IFACEMETHODIMP ConnectedTo(IPin** pPin) override;
    IFACEMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    IFACEMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    IFACEMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override;
    IFACEMETHODIMP QueryId(LPWSTR* Id) override;
    IFACEMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    IFACEMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    IFACEMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin) override {
        (void)apPin;
        if (nPin != nullptr) {
            *nPin = 0u;
        }
        return E_NOTIMPL;
    }
    IFACEMETHODIMP EndOfStream() override { return S_OK; }
    IFACEMETHODIMP BeginFlush() override { return S_OK; }
    IFACEMETHODIMP EndFlush() override { return S_OK; }
    IFACEMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override {
        (void)tStart;
        (void)tStop;
        (void)dRate;
        return S_OK;
    }

    // IAMStreamConfig
    IFACEMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    IFACEMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    IFACEMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override;
    IFACEMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override;

    // IKsPropertySet
    IFACEMETHODIMP Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData,
                       LPVOID pPropData, DWORD cbPropData) override {
        (void)guidPropSet;
        (void)dwPropID;
        (void)pInstanceData;
        (void)cbInstanceData;
        (void)pPropData;
        (void)cbPropData;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData,
                       LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) override;
    IFACEMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override;

    void on_state_changed(FILTER_STATE next_state) noexcept;
    void open_shm_if_available() noexcept;
    void close_shm() noexcept;
    void update_shm_pin_flags() noexcept;

  private:
    static DWORD WINAPI pump_thread_entry(LPVOID param) noexcept;
    void pump_loop() noexcept;
    void start_pump() noexcept;
    void stop_pump() noexcept;

    CAetherVCamFilter* filter_{nullptr};
    CRITICAL_SECTION cs_{};

    IPin* connected_pin_{nullptr};
    IMemInputPin* mem_input_pin_{nullptr};
    IMemAllocator* allocator_{nullptr};
    bool has_preferred_mt_{false};
    AM_MEDIA_TYPE preferred_mt_{};
    AM_MEDIA_TYPE active_mt_{};

    HANDLE stop_event_{nullptr};
    HANDLE pump_thread_{nullptr};
    FILTER_STATE state_{State_Stopped};

    HANDLE shm_map_{nullptr};
    uint8_t* shm_base_{nullptr};
    uint64_t last_seen_seq_{0};
};

class CEnumPins final : public IEnumPins {
  public:
    explicit CEnumPins(CAetherVCamPin* pin, int index = 0) noexcept : pin_(pin), index_(index) {
        g_dll_refs.fetch_add(1L, std::memory_order_relaxed);
    }

    ~CEnumPins() noexcept { g_dll_refs.fetch_sub(1L, std::memory_order_relaxed); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumPins)) {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG c = static_cast<ULONG>(InterlockedDecrement(&ref_));
        if (c == 0u) {
            delete this;
        }
        return c;
    }

    IFACEMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override;
    IFACEMETHODIMP Skip(ULONG cPins) override {
        index_ = std::min(1, index_ + static_cast<int>(cPins));
        return (index_ < 1) ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }
    IFACEMETHODIMP Clone(IEnumPins** ppEnum) override {
        if (ppEnum == nullptr) {
            return E_POINTER;
        }
        auto* e = new (std::nothrow) CEnumPins(pin_, index_);
        if (e == nullptr) {
            return E_OUTOFMEMORY;
        }
        *ppEnum = e;
        return S_OK;
    }

  private:
    volatile long ref_{1};
    CAetherVCamPin* pin_{nullptr};
    int index_{0};
};

class CAetherVCamFilter final : public IBaseFilter, public IAMFilterMiscFlags {
  public:
    CAetherVCamFilter() noexcept : pin_(this) {
        g_dll_refs.fetch_add(1L, std::memory_order_relaxed);
        InitializeCriticalSection(&cs_);
    }

    ~CAetherVCamFilter() noexcept {
        if (clock_ != nullptr) {
            clock_->Release();
            clock_ = nullptr;
        }
        DeleteCriticalSection(&cs_);
        g_dll_refs.fetch_sub(1L, std::memory_order_relaxed);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IPersist) || IsEqualIID(riid, IID_IMediaFilter) ||
            IsEqualIID(riid, IID_IBaseFilter)) {
            *ppv = static_cast<IBaseFilter*>(this);
            AddRef();
            return S_OK;
        }
        if (IsEqualIID(riid, IID_IAMFilterMiscFlags)) {
            *ppv = static_cast<IAMFilterMiscFlags*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG c = static_cast<ULONG>(InterlockedDecrement(&ref_));
        if (c == 0u) {
            delete this;
        }
        return c;
    }

    // IPersist
    IFACEMETHODIMP GetClassID(CLSID* pClassID) override {
        if (pClassID == nullptr) {
            return E_POINTER;
        }
        *pClassID = CLSID_AetherVirtualCam;
        return S_OK;
    }

    // IMediaFilter
    IFACEMETHODIMP Stop() override {
        EnterCriticalSection(&cs_);
        state_ = State_Stopped;
        pin_.on_state_changed(State_Stopped);
        LeaveCriticalSection(&cs_);
        return S_OK;
    }
    IFACEMETHODIMP Pause() override {
        EnterCriticalSection(&cs_);
        state_ = State_Paused;
        pin_.on_state_changed(State_Paused);
        LeaveCriticalSection(&cs_);
        return S_OK;
    }
    IFACEMETHODIMP Run(REFERENCE_TIME tStart) override {
        (void)tStart;
        EnterCriticalSection(&cs_);
        state_ = State_Running;
        pin_.on_state_changed(State_Running);
        LeaveCriticalSection(&cs_);
        return S_OK;
    }
    IFACEMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override {
        (void)dwMilliSecsTimeout;
        if (State == nullptr) {
            return E_POINTER;
        }
        EnterCriticalSection(&cs_);
        *State = state_;
        LeaveCriticalSection(&cs_);
        return S_OK;
    }
    IFACEMETHODIMP SetSyncSource(IReferenceClock* pClock) override {
        EnterCriticalSection(&cs_);
        if (clock_ != nullptr) {
            clock_->Release();
        }
        clock_ = pClock;
        if (clock_ != nullptr) {
            clock_->AddRef();
        }
        LeaveCriticalSection(&cs_);
        return S_OK;
    }
    IFACEMETHODIMP GetSyncSource(IReferenceClock** pClock) override {
        if (pClock == nullptr) {
            return E_POINTER;
        }
        EnterCriticalSection(&cs_);
        *pClock = clock_;
        if (clock_ != nullptr) {
            clock_->AddRef();
        }
        LeaveCriticalSection(&cs_);
        return S_OK;
    }

    // IBaseFilter
    IFACEMETHODIMP EnumPins(IEnumPins** ppEnum) override {
        if (ppEnum == nullptr) {
            return E_POINTER;
        }
        auto* e = new (std::nothrow) CEnumPins(&pin_);
        if (e == nullptr) {
            return E_OUTOFMEMORY;
        }
        *ppEnum = e;
        return S_OK;
    }
    IFACEMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override {
        if (ppPin == nullptr) {
            return E_POINTER;
        }
        if (Id != nullptr && wcscmp(Id, L"Capture") == 0) {
            *ppPin = &pin_;
            pin_.AddRef();
            return S_OK;
        }
        *ppPin = nullptr;
        return VFW_E_NOT_FOUND;
    }
    IFACEMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override {
        if (pInfo == nullptr) {
            return E_POINTER;
        }
        std::memset(pInfo, 0, sizeof(*pInfo));
        wcsncpy_s(pInfo->achName, 128, filter_name_, _TRUNCATE);
        pInfo->pGraph = graph_;
        if (graph_ != nullptr) {
            graph_->AddRef();
        }
        return S_OK;
    }
    IFACEMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override {
        EnterCriticalSection(&cs_);
        graph_ = pGraph;
        if (pName != nullptr) {
            wcsncpy_s(filter_name_, 128, pName, _TRUNCATE);
        } else {
            wcsncpy_s(filter_name_, 128, AETHER_VCAM_FRIENDLY_NAME_W, _TRUNCATE);
        }
        LeaveCriticalSection(&cs_);
        return S_OK;
    }
    IFACEMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override {
        if (pVendorInfo == nullptr) {
            return E_POINTER;
        }
        static constexpr const wchar_t kVendor[] = L"AETHER-2.5D";
        *pVendorInfo = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(kVendor)));
        if (*pVendorInfo == nullptr) {
            return E_OUTOFMEMORY;
        }
        std::memcpy(*pVendorInfo, kVendor, sizeof(kVendor));
        return S_OK;
    }

    // IAMFilterMiscFlags
    IFACEMETHODIMP_(ULONG) GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

  private:
    volatile long ref_{1};
    CRITICAL_SECTION cs_{};
    FILTER_STATE state_{State_Stopped};
    IReferenceClock* clock_{nullptr};
    IFilterGraph* graph_{nullptr};
    wchar_t filter_name_[128]{L"Aether 2.5D Virtual Cam (Alpha)"};
    CAetherVCamPin pin_;
};

IFACEMETHODIMP CEnumPins::Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) {
    if (ppPins == nullptr) {
        return E_POINTER;
    }
    ULONG fetched = 0u;
    if (cPins > 0u && index_ == 0 && pin_ != nullptr) {
        ppPins[0] = pin_;
        pin_->AddRef();
        fetched = 1u;
        index_ = 1;
    }
    if (pcFetched != nullptr) {
        *pcFetched = fetched;
    }
    return (fetched == cPins) ? S_OK : S_FALSE;
}

CAetherVCamPin::CAetherVCamPin(CAetherVCamFilter* filter) noexcept : filter_(filter) {
    InitializeCriticalSection(&cs_);
    std::memset(&preferred_mt_, 0, sizeof(preferred_mt_));
    std::memset(&active_mt_, 0, sizeof(active_mt_));

    AM_MEDIA_TYPE tmp{};
    VIDEOINFOHEADER vih{};
    init_media_type_from_cap(kCapabilities[0], tmp, vih);
    (void)copy_media_type(&preferred_mt_, &tmp);
    has_preferred_mt_ = true;
    (void)copy_media_type(&active_mt_, &tmp);
}

CAetherVCamPin::~CAetherVCamPin() noexcept {
    stop_pump();
    (void)Disconnect();
    close_shm();
    free_media_type_contents(preferred_mt_);
    free_media_type_contents(active_mt_);
    DeleteCriticalSection(&cs_);
}

IFACEMETHODIMP CAetherVCamPin::QueryInterface(REFIID riid, void** ppv) {
    if (ppv == nullptr) {
        return E_POINTER;
    }
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IPin)) {
        *ppv = static_cast<IPin*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IAMStreamConfig)) {
        *ppv = static_cast<IAMStreamConfig*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IKsPropertySet)) {
        *ppv = static_cast<IKsPropertySet*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) CAetherVCamPin::AddRef() {
    return filter_->AddRef();
}
IFACEMETHODIMP_(ULONG) CAetherVCamPin::Release() {
    return filter_->Release();
}

IFACEMETHODIMP CAetherVCamPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (pReceivePin == nullptr) {
        return E_POINTER;
    }
    EnterCriticalSection(&cs_);
    if (connected_pin_ != nullptr) {
        LeaveCriticalSection(&cs_);
        return VFW_E_ALREADY_CONNECTED;
    }

    auto try_connect = [&](const AM_MEDIA_TYPE* cand) -> HRESULT {
        if (!validate_media_type(cand)) {
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        HRESULT hr = pReceivePin->ReceiveConnection(this, cand);
        if (FAILED(hr)) {
            return hr;
        }
        IMemInputPin* mip = nullptr;
        hr = pReceivePin->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&mip));
        if (FAILED(hr) || mip == nullptr) {
            pReceivePin->Disconnect();
            return E_NOINTERFACE;
        }

        IMemAllocator* alloc = nullptr;
        hr = mip->GetAllocator(&alloc);
        if (FAILED(hr) || alloc == nullptr) {
            hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator,
                                  reinterpret_cast<void**>(&alloc));
            if (FAILED(hr) || alloc == nullptr) {
                mip->Release();
                pReceivePin->Disconnect();
                return hr;
            }
        }

        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(cand->pbFormat);
        const LONG w = vih->bmiHeader.biWidth;
        const LONG h = (vih->bmiHeader.biHeight < 0) ? -vih->bmiHeader.biHeight : vih->bmiHeader.biHeight;
        const DWORD img_sz =
            (vih->bmiHeader.biSizeImage > 0u) ? vih->bmiHeader.biSizeImage : static_cast<DWORD>(w * h * 4L);

        ALLOCATOR_PROPERTIES req{};
        req.cBuffers = 3;
        req.cbBuffer = static_cast<long>(img_sz);
        req.cbAlign = 1;
        req.cbPrefix = 0;
        ALLOCATOR_PROPERTIES actual{};
        hr = alloc->SetProperties(&req, &actual);
        if (FAILED(hr)) {
            alloc->Release();
            mip->Release();
            pReceivePin->Disconnect();
            return hr;
        }
        hr = mip->NotifyAllocator(alloc, FALSE);
        if (FAILED(hr)) {
            alloc->Release();
            mip->Release();
            pReceivePin->Disconnect();
            return hr;
        }

        free_media_type_contents(active_mt_);
        (void)copy_media_type(&active_mt_, cand);
        connected_pin_ = pReceivePin;
        connected_pin_->AddRef();
        mem_input_pin_ = mip;
        allocator_ = alloc;
        return S_OK;
    };

    HRESULT res = VFW_E_NO_ACCEPTABLE_TYPES;
    if (pmt != nullptr) {
        res = try_connect(pmt);
    } else {
        if (has_preferred_mt_) {
            res = try_connect(&preferred_mt_);
        }
        for (int i = 0; FAILED(res) && i < kCapCount; ++i) {
            AM_MEDIA_TYPE tmp{};
            VIDEOINFOHEADER vih{};
            init_media_type_from_cap(kCapabilities[i], tmp, vih);
            res = try_connect(&tmp);
        }
    }

    if (SUCCEEDED(res)) {
        open_shm_if_available();
        update_shm_pin_flags();
    }
    LeaveCriticalSection(&cs_);
    return res;
}

IFACEMETHODIMP CAetherVCamPin::Disconnect() {
    stop_pump();
    EnterCriticalSection(&cs_);
    if (allocator_ != nullptr) {
        allocator_->Release();
        allocator_ = nullptr;
    }
    if (mem_input_pin_ != nullptr) {
        mem_input_pin_->Release();
        mem_input_pin_ = nullptr;
    }
    if (connected_pin_ != nullptr) {
        connected_pin_->Release();
        connected_pin_ = nullptr;
    }
    update_shm_pin_flags();
    LeaveCriticalSection(&cs_);
    return S_OK;
}

IFACEMETHODIMP CAetherVCamPin::ConnectedTo(IPin** pPin) {
    if (pPin == nullptr) {
        return E_POINTER;
    }
    EnterCriticalSection(&cs_);
    *pPin = connected_pin_;
    if (connected_pin_ != nullptr) {
        connected_pin_->AddRef();
        LeaveCriticalSection(&cs_);
        return S_OK;
    }
    LeaveCriticalSection(&cs_);
    return VFW_E_NOT_CONNECTED;
}

IFACEMETHODIMP CAetherVCamPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (pmt == nullptr) {
        return E_POINTER;
    }
    EnterCriticalSection(&cs_);
    if (connected_pin_ == nullptr) {
        LeaveCriticalSection(&cs_);
        return VFW_E_NOT_CONNECTED;
    }
    const HRESULT hr = copy_media_type(pmt, &active_mt_);
    LeaveCriticalSection(&cs_);
    return hr;
}

IFACEMETHODIMP CAetherVCamPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (pInfo == nullptr) {
        return E_POINTER;
    }
    pInfo->pFilter = filter_;
    if (filter_ != nullptr) {
        filter_->AddRef();
    }
    pInfo->dir = PINDIR_OUTPUT;
    wcsncpy_s(pInfo->achName, 128, L"Capture", _TRUNCATE);
    return S_OK;
}

IFACEMETHODIMP CAetherVCamPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (pPinDir == nullptr) {
        return E_POINTER;
    }
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

IFACEMETHODIMP CAetherVCamPin::QueryId(LPWSTR* Id) {
    if (Id == nullptr) {
        return E_POINTER;
    }
    static constexpr const wchar_t kPinId[] = L"Capture";
    *Id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(kPinId)));
    if (*Id == nullptr) {
        return E_OUTOFMEMORY;
    }
    std::memcpy(*Id, kPinId, sizeof(kPinId));
    return S_OK;
}

IFACEMETHODIMP CAetherVCamPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    return validate_media_type(pmt) ? S_OK : S_FALSE;
}

IFACEMETHODIMP CAetherVCamPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (ppEnum == nullptr) {
        return E_POINTER;
    }
    EnterCriticalSection(&cs_);
    auto* e = new (std::nothrow) CEnumMediaTypes(0, has_preferred_mt_ ? &preferred_mt_ : nullptr);
    LeaveCriticalSection(&cs_);
    if (e == nullptr) {
        return E_OUTOFMEMORY;
    }
    *ppEnum = e;
    return S_OK;
}

IFACEMETHODIMP CAetherVCamPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (pmt == nullptr) {
        return E_POINTER;
    }
    if (!validate_media_type(pmt)) {
        return VFW_E_INVALIDMEDIATYPE;
    }
    EnterCriticalSection(&cs_);
    if (connected_pin_ != nullptr) {
        LeaveCriticalSection(&cs_);
        return VFW_E_ALREADY_CONNECTED;
    }
    free_media_type_contents(preferred_mt_);
    const HRESULT hr = copy_media_type(&preferred_mt_, pmt);
    has_preferred_mt_ = SUCCEEDED(hr);
    LeaveCriticalSection(&cs_);
    return hr;
}

IFACEMETHODIMP CAetherVCamPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (ppmt == nullptr) {
        return E_POINTER;
    }
    EnterCriticalSection(&cs_);
    auto* pmt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (pmt == nullptr) {
        LeaveCriticalSection(&cs_);
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = copy_media_type(pmt, &preferred_mt_);
    LeaveCriticalSection(&cs_);
    if (FAILED(hr)) {
        CoTaskMemFree(pmt);
        return hr;
    }
    *ppmt = pmt;
    return S_OK;
}

IFACEMETHODIMP CAetherVCamPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (piCount == nullptr || piSize == nullptr) {
        return E_POINTER;
    }
    *piCount = kCapCount;
    *piSize = static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS));
    return S_OK;
}

IFACEMETHODIMP CAetherVCamPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (ppmt == nullptr || pSCC == nullptr) {
        return E_POINTER;
    }
    if (iIndex < 0 || iIndex >= kCapCount) {
        return S_FALSE;
    }

    const auto& cap = kCapabilities[iIndex];
    auto* pmt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (pmt == nullptr) {
        return E_OUTOFMEMORY;
    }

    AM_MEDIA_TYPE tmp{};
    VIDEOINFOHEADER vih{};
    init_media_type_from_cap(cap, tmp, vih);
    const HRESULT hr = copy_media_type(pmt, &tmp);
    if (FAILED(hr)) {
        CoTaskMemFree(pmt);
        return hr;
    }
    *ppmt = pmt;

    auto* caps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(pSCC);
    std::memset(caps, 0, sizeof(*caps));
    caps->guid = FORMAT_VideoInfo;
    caps->VideoStandard = AnalogVideo_None;
    caps->InputSize = {static_cast<LONG>(cap.width), static_cast<LONG>(cap.height)};
    caps->MinCroppingSize = caps->InputSize;
    caps->MaxCroppingSize = caps->InputSize;
    caps->CropGranularityX = 1;
    caps->CropGranularityY = 1;
    caps->MinOutputSize = caps->InputSize;
    caps->MaxOutputSize = caps->InputSize;
    caps->OutputGranularityX = 1;
    caps->OutputGranularityY = 1;
    caps->MinFrameInterval = 83'333LL;  // up to 120 Hz
    caps->MaxFrameInterval = 333'333LL; // down to 30 Hz
    caps->MinBitsPerSecond = 1;
    caps->MaxBitsPerSecond = static_cast<LONG>(std::min<uint64_t>(0x7FFFFFFFULL, calc_image_size(cap) * 8ULL * 60ULL));
    return S_OK;
}

IFACEMETHODIMP CAetherVCamPin::Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData,
                                   LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) {
    (void)pInstanceData;
    (void)cbInstanceData;
    if (!IsEqualGUID(guidPropSet, AMPROPSETID_Pin) || dwPropID != AMPROPERTY_PIN_CATEGORY) {
        return E_PROP_SET_UNSUPPORTED;
    }
    if (pcbReturned != nullptr) {
        *pcbReturned = sizeof(GUID);
    }
    if (pPropData == nullptr) {
        return S_OK;
    }
    if (cbPropData < sizeof(GUID)) {
        return E_UNEXPECTED;
    }
    *static_cast<GUID*>(pPropData) = PIN_CATEGORY_CAPTURE;
    return S_OK;
}

IFACEMETHODIMP CAetherVCamPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (!IsEqualGUID(guidPropSet, AMPROPSETID_Pin) || dwPropID != AMPROPERTY_PIN_CATEGORY) {
        return E_PROP_SET_UNSUPPORTED;
    }
    if (pTypeSupport != nullptr) {
        *pTypeSupport = KSPROPERTY_SUPPORT_GET;
    }
    return S_OK;
}

void CAetherVCamPin::open_shm_if_available() noexcept {
    if (shm_base_ != nullptr) {
        return;
    }
    HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, AETHER_VCAM_SHM_NAME_W);
    if (m == nullptr) {
        return;
    }
    void* view = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0u, 0u, AETHER_VCAM_SECTION_BYTES);
    if (view == nullptr) {
        CloseHandle(m);
        return;
    }
    shm_map_ = m;
    shm_base_ = static_cast<uint8_t*>(view);
}

void CAetherVCamPin::close_shm() noexcept {
    if (shm_base_ != nullptr) {
        auto* hdr = reinterpret_cast<AetherVCamHeader*>(shm_base_);
        if (hdr->magic == AETHER_VCAM_MAGIC) {
            const uint32_t pin_bits =
                AETHER_VCAM_PIN_FILTER_ACTIVE | AETHER_VCAM_PIN_RGB_CONNECTED | AETHER_VCAM_PIN_NV12_CONNECTED;
            const long clear_mask = ~static_cast<long>(pin_bits);
            InterlockedAnd(&hdr->active_pin_flags, clear_mask);
        }
        UnmapViewOfFile(shm_base_);
        shm_base_ = nullptr;
    }
    if (shm_map_ != nullptr) {
        CloseHandle(shm_map_);
        shm_map_ = nullptr;
    }
}

void CAetherVCamPin::update_shm_pin_flags() noexcept {
    if (shm_base_ == nullptr) {
        return;
    }
    auto* hdr = reinterpret_cast<AetherVCamHeader*>(shm_base_);
    if (hdr->magic != AETHER_VCAM_MAGIC) {
        return;
    }
    long desired = static_cast<long>(AETHER_VCAM_PIN_FILTER_ACTIVE);
    if (connected_pin_ != nullptr) {
        if (IsEqualGUID(active_mt_.subtype, MEDIASUBTYPE_NV12)) {
            desired |= static_cast<long>(AETHER_VCAM_PIN_NV12_CONNECTED);
        } else {
            desired |= static_cast<long>(AETHER_VCAM_PIN_RGB_CONNECTED);
        }
    }
    InterlockedExchange(&hdr->active_pin_flags, desired);
}

void CAetherVCamPin::on_state_changed(FILTER_STATE next_state) noexcept {
    EnterCriticalSection(&cs_);
    state_ = next_state;
    if (next_state == State_Running && connected_pin_ != nullptr) {
        LeaveCriticalSection(&cs_);
        start_pump();
        return;
    }
    if (next_state == State_Stopped) {
        LeaveCriticalSection(&cs_);
        stop_pump();
        return;
    }
    LeaveCriticalSection(&cs_);
}

void CAetherVCamPin::start_pump() noexcept {
    if (pump_thread_ != nullptr) {
        return;
    }
    if (allocator_ != nullptr) {
        (void)allocator_->Commit();
    }
    if (stop_event_ == nullptr) {
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    } else {
        ResetEvent(stop_event_);
    }
    pump_thread_ = CreateThread(nullptr, 0u, &CAetherVCamPin::pump_thread_entry, this, 0u, nullptr);
}

void CAetherVCamPin::stop_pump() noexcept {
    if (stop_event_ != nullptr) {
        SetEvent(stop_event_);
    }
    if (pump_thread_ != nullptr) {
        WaitForSingleObject(pump_thread_, 2000u);
        CloseHandle(pump_thread_);
        pump_thread_ = nullptr;
    }
    if (stop_event_ != nullptr) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
    if (allocator_ != nullptr) {
        (void)allocator_->Decommit();
    }
}

DWORD WINAPI CAetherVCamPin::pump_thread_entry(LPVOID param) noexcept {
    static_cast<CAetherVCamPin*>(param)->pump_loop();
    return 0u;
}

void CAetherVCamPin::pump_loop() noexcept {
    const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(active_mt_.pbFormat);
    const uint32_t out_w = static_cast<uint32_t>(vih->bmiHeader.biWidth);
    const LONG raw_h = vih->bmiHeader.biHeight;
    const uint32_t out_h = static_cast<uint32_t>(raw_h < 0 ? -raw_h : raw_h);
    const bool want_bottom_up = (raw_h > 0);
    const bool out_nv12 = IsEqualGUID(active_mt_.subtype, MEDIASUBTYPE_NV12);
    const REFERENCE_TIME interval = (vih->AvgTimePerFrame > 0) ? vih->AvgTimePerFrame : 166'667LL;
    const DWORD wait_ms = static_cast<DWORD>(std::clamp<REFERENCE_TIME>(interval / 10'000LL, 4LL, 33LL));

    REFERENCE_TIME pts = 0;
    for (;;) {
        if (WaitForSingleObject(stop_event_, wait_ms) == WAIT_OBJECT_0) {
            break;
        }

        IMediaSample* sample = nullptr;
        if (allocator_ == nullptr || mem_input_pin_ == nullptr ||
            FAILED(allocator_->GetBuffer(&sample, nullptr, nullptr, 0u)) || sample == nullptr) {
            continue;
        }

        BYTE* dst = nullptr;
        const long buf_cap = sample->GetSize();
        if (FAILED(sample->GetPointer(&dst)) || dst == nullptr) {
            sample->Release();
            continue;
        }

        EnterCriticalSection(&cs_);
        open_shm_if_available();
        update_shm_pin_flags();
        LeaveCriticalSection(&cs_);

        bool copied_live = false;
        if (shm_base_ != nullptr) {
            const auto* hdr = reinterpret_cast<const AetherVCamHeader*>(shm_base_);
            for (int attempt = 0; attempt < 4; ++attempt) {
                const long s1 = interlocked_read_long(&hdr->write_seq);
                if ((s1 & 1L) != 0L) {
                    continue;
                }
                const uint32_t magic = hdr->magic;
                const uint32_t sw = hdr->width;
                const uint32_t sh = hdr->height;
                const uint32_t slot = hdr->latest_slot % AETHER_VCAM_SLOTS;
                const uint64_t pqpc = hdr->producer_qpc_us;
                const bool src_bottom_up = (hdr->bottom_up != 0u);
                if (magic == AETHER_VCAM_MAGIC && sw > 0u && sh > 0u && (qpc_now_us() - pqpc) < 1'500'000ULL) {
                    const uint8_t* slot_ptr = shm_base_ + AETHER_VCAM_HEADER_BYTES +
                                              static_cast<size_t>(slot) * AETHER_VCAM_SLOT_STRIDE_BYTES;
                    const uint32_t need_bytes = out_nv12 ? (out_w * out_h * 3u / 2u) : (out_w * out_h * 4u);
                    if (static_cast<long>(need_bytes) <= buf_cap) {
                        if (out_nv12) {
                            if (hdr->active_fourcc == AETHER_VCAM_FOURCC_NV12 && sw == out_w && sh == out_h) {
                                std::memcpy(dst, slot_ptr + AETHER_VCAM_NV12_OFFSET_IN_SLOT, need_bytes);
                            } else {
                                bgra_to_nv12_cpu(slot_ptr, sw, sh, src_bottom_up, dst, out_w, out_h);
                            }
                        } else {
                            const bool flip = (want_bottom_up != src_bottom_up);
                            const uint32_t row_bytes = out_w * 4u;
                            if (!flip && sw == out_w && sh == out_h) {
                                std::memcpy(dst, slot_ptr, need_bytes);
                            } else {
                                for (uint32_t dy = 0; dy < out_h; ++dy) {
                                    const uint32_t sy_raw =
                                        static_cast<uint32_t>((static_cast<uint64_t>(dy) * sh) / out_h);
                                    const uint32_t sy = flip ? (sh - 1u - sy_raw) : sy_raw;
                                    const uint8_t* srow = slot_ptr + static_cast<size_t>(sy) * sw * 4u;
                                    uint8_t* drow = dst + static_cast<size_t>(dy) * row_bytes;
                                    if (sw == out_w) {
                                        std::memcpy(drow, srow, row_bytes);
                                    } else {
                                        for (uint32_t dx = 0; dx < out_w; ++dx) {
                                            const uint32_t sx =
                                                static_cast<uint32_t>((static_cast<uint64_t>(dx) * sw) / out_w);
                                            std::memcpy(drow + static_cast<size_t>(dx) * 4u,
                                                        srow + static_cast<size_t>(sx) * 4u, 4u);
                                        }
                                    }
                                }
                            }
                        }
                        copied_live = true;
                    }
                }
                const long s2 = interlocked_read_long(&hdr->write_seq);
                if (s1 == s2) {
                    last_seen_seq_ = hdr->slot_frame_seq[slot];
                    break;
                }
                copied_live = false;
            }
        }

        const uint32_t actual_bytes = out_nv12 ? (out_w * out_h * 3u / 2u) : (out_w * out_h * 4u);
        if (!copied_live && static_cast<long>(actual_bytes) <= buf_cap) {
            if (out_nv12) {
                draw_offline_slate_nv12(dst, out_w, out_h);
            } else {
                draw_offline_slate_bgra(dst, out_w, out_h, want_bottom_up);
            }
        }

        REFERENCE_TIME t_end = pts + interval;
        (void)sample->SetTime(&pts, &t_end);
        (void)sample->SetSyncPoint(TRUE);
        (void)sample->SetActualDataLength(static_cast<long>(actual_bytes));
        pts = t_end;

        (void)mem_input_pin_->Receive(sample);
        sample->Release();
    }
}

class CClassFactory final : public IClassFactory {
  public:
    CClassFactory() noexcept { g_dll_refs.fetch_add(1L, std::memory_order_relaxed); }
    ~CClassFactory() noexcept { g_dll_refs.fetch_sub(1L, std::memory_order_relaxed); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG c = static_cast<ULONG>(InterlockedDecrement(&ref_));
        if (c == 0u) {
            delete this;
        }
        return c;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (pUnkOuter != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        auto* filter = new (std::nothrow) CAetherVCamFilter();
        if (filter == nullptr) {
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = filter->QueryInterface(riid, ppvObject);
        filter->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL fLock) override {
        if (fLock != FALSE) {
            g_dll_refs.fetch_add(1L, std::memory_order_relaxed);
        } else {
            g_dll_refs.fetch_sub(1L, std::memory_order_relaxed);
        }
        return S_OK;
    }

  private:
    volatile long ref_{1};
};

HRESULT set_reg_str(HKEY root, const wchar_t* subkey, const wchar_t* name, const wchar_t* value) noexcept {
    HKEY key = nullptr;
    const LONG rc =
        RegCreateKeyExW(root, subkey, 0u, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(static_cast<DWORD>(rc));
    }
    const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1u) * sizeof(wchar_t));
    const LONG wr = RegSetValueExW(key, name, 0u, REG_SZ, reinterpret_cast<const BYTE*>(value), bytes);
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(static_cast<DWORD>(wr));
}

} // namespace

extern "C" HRESULT __stdcall DllCanUnloadNow(void) {
    return (g_dll_refs.load(std::memory_order_acquire) == 0L) ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (ppv == nullptr) {
        return E_POINTER;
    }
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, CLSID_AetherVirtualCam)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) CClassFactory();
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllRegisterServer(void) {
    wchar_t mod_path[MAX_PATH]{};
    if (g_hmodule == nullptr || GetModuleFileNameW(g_hmodule, mod_path, MAX_PATH) == 0u) {
        return E_FAIL;
    }

    const wchar_t* clsid_key = L"SOFTWARE\\Classes\\CLSID\\" AETHER_VCAM_CLSID_STR_W;
    const wchar_t* inproc_key = L"SOFTWARE\\Classes\\CLSID\\" AETHER_VCAM_CLSID_STR_W L"\\InprocServer32";

    HRESULT hr = set_reg_str(HKEY_LOCAL_MACHINE, clsid_key, nullptr, AETHER_VCAM_FRIENDLY_NAME_W);
    if (FAILED(hr)) {
        return hr;
    }
    hr = set_reg_str(HKEY_LOCAL_MACHINE, inproc_key, nullptr, mod_path);
    if (FAILED(hr)) {
        return hr;
    }
    hr = set_reg_str(HKEY_LOCAL_MACHINE, inproc_key, L"ThreadingModel", L"Both");
    if (FAILED(hr)) {
        return hr;
    }

    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFilterMapper2* mapper = nullptr;
    hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2,
                          reinterpret_cast<void**>(&mapper));
    if (SUCCEEDED(hr) && mapper != nullptr) {
        static const REGPINTYPES kPinTypes[] = {{&MEDIATYPE_Video, &MEDIASUBTYPE_ARGB32},
                                                {&MEDIATYPE_Video, &MEDIASUBTYPE_RGB32},
                                                {&MEDIATYPE_Video, &MEDIASUBTYPE_NV12}};
        REGFILTERPINS2 pin_reg{};
        pin_reg.dwFlags = REG_PINFLAG_B_OUTPUT;
        pin_reg.cInstances = 1u;
        pin_reg.nMediaTypes = 3u;
        pin_reg.lpMediaType = kPinTypes;
        pin_reg.nMediums = 0u;
        pin_reg.lpMedium = nullptr;
        pin_reg.clsPinCategory = &PIN_CATEGORY_CAPTURE;

        REGFILTER2 rf2{};
        rf2.dwVersion = 2u;
        rf2.dwMerit = MERIT_DO_NOT_USE;
        rf2.cPins2 = 1u;
        rf2.rgPins2 = &pin_reg;

        IMoniker* moniker = nullptr;
        hr = mapper->RegisterFilter(CLSID_AetherVirtualCam, AETHER_VCAM_FRIENDLY_NAME_W, &moniker,
                                    &CLSID_VideoInputDeviceCategory, AETHER_VCAM_FRIENDLY_NAME_W, &rf2);
        if (moniker != nullptr) {
            moniker->Release();
        }
        mapper->Release();
    }
    if (SUCCEEDED(com_hr)) {
        CoUninitialize();
    }
    return hr;
}

extern "C" HRESULT __stdcall DllUnregisterServer(void) {
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFilterMapper2* mapper = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2,
                                  reinterpret_cast<void**>(&mapper));
    if (SUCCEEDED(hr) && mapper != nullptr) {
        hr = mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, AETHER_VCAM_FRIENDLY_NAME_W,
                                      CLSID_AetherVirtualCam);
        mapper->Release();
    }
    if (SUCCEEDED(com_hr)) {
        CoUninitialize();
    }
    (void)RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\" AETHER_VCAM_CLSID_STR_W);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hmodule = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}
