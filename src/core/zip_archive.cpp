#include "core/zip_archive.hpp"

#include "core/paths.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <Windows.h>
#include <bcrypt.h>
#include <Objbase.h>
#include <wincodec.h>
// clang-format on
#endif

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>

namespace aether::core {

namespace {

void append_u16_le(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
}

void append_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16u) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24u) & 0xFFu));
}

[[nodiscard]] uint16_t read_u16_le(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8u);
}

[[nodiscard]] uint32_t read_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) | (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

[[nodiscard]] bool is_safe_archive_entry_path(std::string_view p) noexcept {
    if (p.empty() || p.size() > 1024u) {
        return false;
    }
    if (p.front() == '/' || p.front() == '\\') {
        return false;
    }
    if (p.find(':') != std::string_view::npos || p.find('\\') != std::string_view::npos) {
        return false;
    }
    if (p.find("..") != std::string_view::npos) {
        return false;
    }
    return true;
}

[[nodiscard]] std::string normalize_entry_path(std::string_view raw) {
    std::string s(raw);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

[[nodiscard]] Expected<std::vector<uint8_t>> deflate_raw_rfc1951(std::span<const uint8_t> input) {
    if (input.size() > 0xFFFFFFFFULL) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "ZIP entry exceeds 4 GiB limit"));
    }
    z_stream zs{};
    if (::deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return make_unexpected(Error::make(ErrorCode::Internal, "zlib deflateInit2 failed"));
    }
    const uLong bound = ::deflateBound(&zs, static_cast<uLong>(input.size()));
    std::vector<uint8_t> out(static_cast<size_t>(bound));
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = ::deflate(&zs, Z_FINISH);
    const size_t total_out = static_cast<size_t>(zs.total_out);
    ::deflateEnd(&zs);
    if (rc != Z_STREAM_END) {
        return make_unexpected(Error::make(ErrorCode::Internal, "zlib deflate failed"));
    }
    out.resize(total_out);
    return out;
}

[[nodiscard]] Expected<std::vector<uint8_t>> inflate_raw_rfc1951(std::span<const uint8_t> input,
                                                                 uint32_t expected_size) {
    if (expected_size > ZipReader::kMaxEntryUncompressedBytes) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "ZIP entry exceeds safety cap"));
    }
    std::vector<uint8_t> out(expected_size);
    if (expected_size == 0u) {
        return out;
    }
    z_stream zs{};
    if (::inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        return make_unexpected(Error::make(ErrorCode::Internal, "zlib inflateInit2 failed"));
    }
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = ::inflate(&zs, Z_FINISH);
    const size_t got = static_cast<size_t>(zs.total_out);
    ::inflateEnd(&zs);
    if (rc != Z_STREAM_END || got != expected_size) {
        return make_unexpected(Error::make(ErrorCode::IoError, "ZIP entry inflate size/stream mismatch"));
    }
    return out;
}

#if defined(_WIN32)
struct ComPtrDeleter {
    template <class T> void operator()(T* p) const noexcept {
        if (p != nullptr) {
            p->Release();
        }
    }
};
template <class T> using ComUnique = std::unique_ptr<T, ComPtrDeleter>;

class ScopedBCryptAlg {
  public:
    BCRYPT_ALG_HANDLE h{nullptr};
    ~ScopedBCryptAlg() {
        if (h != nullptr) {
            ::BCryptCloseAlgorithmProvider(h, 0);
        }
    }
};

class ScopedBCryptHash {
  public:
    BCRYPT_HASH_HANDLE h{nullptr};
    ~ScopedBCryptHash() {
        if (h != nullptr) {
            ::BCryptDestroyHash(h);
        }
    }
};
#endif

} // namespace

Expected<std::vector<uint8_t>> ZipWriter::build_bytes(std::span<const ZipEntry> entries) {
    if (entries.size() > 65535u) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "ZIP entry count exceeds 65535"));
    }

    struct CdRecord {
        std::string path;
        uint32_t crc{0};
        uint32_t comp_size{0};
        uint32_t uncomp_size{0};
        uint16_t method{0};
        uint32_t lfh_offset{0};
    };

    std::vector<uint8_t> archive;
    archive.reserve(4096u);
    std::vector<CdRecord> cd;
    cd.reserve(entries.size());

    constexpr uint16_t kUtf8Flag = (1u << 11u);
    constexpr uint16_t kDosTime = 0x0000u;
    constexpr uint16_t kDosDate = 0x5A21u; // deterministic 2025-01-01

    for (const ZipEntry& e : entries) {
        const std::string norm_path = normalize_entry_path(e.path);
        if (!is_safe_archive_entry_path(norm_path) || norm_path.size() > 65535u) {
            return make_unexpected(Error::make(ErrorCode::InvalidArg, "Unsafe or invalid ZIP entry path"));
        }
        if (e.data.size() > ZipReader::kMaxEntryUncompressedBytes) {
            return make_unexpected(Error::make(ErrorCode::InvalidArg, "ZIP entry uncompressed size exceeds cap"));
        }

        const uint32_t crc = static_cast<uint32_t>(
            ::crc32(0L, e.data.empty() ? Z_NULL : e.data.data(), static_cast<uInt>(e.data.size())));
        uint16_t method = 0u; // STORE
        std::vector<uint8_t> compressed_buf;
        std::span<const uint8_t> payload(e.data);
        if (e.compress && !e.data.empty()) {
            auto def = deflate_raw_rfc1951(e.data);
            if (!def) {
                return make_unexpected(def.error());
            }
            if (def.value().size() < e.data.size()) {
                method = 8u; // DEFLATE
                compressed_buf = std::move(def.value());
                payload = compressed_buf;
            }
        }

        if (archive.size() + 30u + norm_path.size() + payload.size() > kMaxArchiveBytes) {
            return make_unexpected(Error::make(ErrorCode::InvalidArg, "ZIP archive size exceeds limit"));
        }

        const uint32_t lfh_offset = static_cast<uint32_t>(archive.size());
        const uint32_t comp_sz = static_cast<uint32_t>(payload.size());
        const uint32_t uncomp_sz = static_cast<uint32_t>(e.data.size());
        const uint16_t name_len = static_cast<uint16_t>(norm_path.size());

        // Local File Header (30 bytes + filename)
        append_u32_le(archive, 0x04034B50u);
        append_u16_le(archive, 20u); // version needed 2.0
        append_u16_le(archive, kUtf8Flag);
        append_u16_le(archive, method);
        append_u16_le(archive, kDosTime);
        append_u16_le(archive, kDosDate);
        append_u32_le(archive, crc);
        append_u32_le(archive, comp_sz);
        append_u32_le(archive, uncomp_sz);
        append_u16_le(archive, name_len);
        append_u16_le(archive, 0u); // extra len
        archive.insert(archive.end(), norm_path.begin(), norm_path.end());
        archive.insert(archive.end(), payload.begin(), payload.end());

        cd.push_back(CdRecord{norm_path, crc, comp_sz, uncomp_sz, method, lfh_offset});
    }

    const uint32_t cd_offset = static_cast<uint32_t>(archive.size());
    for (const CdRecord& r : cd) {
        append_u32_le(archive, 0x02014B50u);
        append_u16_le(archive, 20u); // version made by
        append_u16_le(archive, 20u); // version needed
        append_u16_le(archive, kUtf8Flag);
        append_u16_le(archive, r.method);
        append_u16_le(archive, kDosTime);
        append_u16_le(archive, kDosDate);
        append_u32_le(archive, r.crc);
        append_u32_le(archive, r.comp_size);
        append_u32_le(archive, r.uncomp_size);
        append_u16_le(archive, static_cast<uint16_t>(r.path.size()));
        append_u16_le(archive, 0u);
        append_u16_le(archive, 0u);
        append_u16_le(archive, 0u);
        append_u16_le(archive, 0u);
        append_u32_le(archive, 0u);
        append_u32_le(archive, r.lfh_offset);
        archive.insert(archive.end(), r.path.begin(), r.path.end());
    }
    const uint32_t cd_size = static_cast<uint32_t>(archive.size()) - cd_offset;

    // End of Central Directory (22 bytes)
    append_u32_le(archive, 0x06054B50u);
    append_u16_le(archive, 0u);
    append_u16_le(archive, 0u);
    append_u16_le(archive, static_cast<uint16_t>(cd.size()));
    append_u16_le(archive, static_cast<uint16_t>(cd.size()));
    append_u32_le(archive, cd_size);
    append_u32_le(archive, cd_offset);
    append_u16_le(archive, 0u);

    return archive;
}

Expected<void> ZipWriter::write_atomic(const std::filesystem::path& dst_path, std::span<const ZipEntry> entries) {
    auto bytes_res = build_bytes(entries);
    if (!bytes_res) {
        return make_unexpected(bytes_res.error());
    }
    return write_bytes_atomic(dst_path, bytes_res.value(), [](const std::filesystem::path& tmp_path) -> Expected<void> {
        auto zr = ZipReader::open(tmp_path);
        if (!zr) {
            return make_unexpected(zr.error());
        }
        return zr.value().verify_all();
    });
}

Expected<ZipReader> ZipReader::open(const std::filesystem::path& src_path) {
    auto bytes_res = read_file_bytes(src_path, ZipWriter::kMaxArchiveBytes);
    if (!bytes_res) {
        return make_unexpected(bytes_res.error());
    }
    return open_memory(std::move(bytes_res.value()));
}

Expected<ZipReader> ZipReader::open_memory(std::vector<uint8_t> bytes) {
    if (bytes.size() < 22u) {
        return make_unexpected(Error::make(ErrorCode::IoError, "ZIP archive too small"));
    }
    const size_t min_off = (bytes.size() > (65535u + 22u)) ? (bytes.size() - (65535u + 22u)) : 0u;
    size_t eocd_off = bytes.size();
    for (size_t pos = bytes.size() - 22u;; --pos) {
        if (bytes[pos] == 0x50u && bytes[pos + 1u] == 0x4Bu && bytes[pos + 2u] == 0x05u && bytes[pos + 3u] == 0x06u) {
            const uint16_t comment_len = read_u16_le(&bytes[pos + 20u]);
            if (pos + 22u + comment_len == bytes.size()) {
                eocd_off = pos;
                break;
            }
        }
        if (pos == min_off) {
            break;
        }
    }
    if (eocd_off >= bytes.size()) {
        return make_unexpected(Error::make(ErrorCode::IoError, "ZIP End of Central Directory not found"));
    }

    const uint16_t entry_count = read_u16_le(&bytes[eocd_off + 10u]);
    const uint32_t cd_size = read_u32_le(&bytes[eocd_off + 12u]);
    const uint32_t cd_off = read_u32_le(&bytes[eocd_off + 16u]);
    if (static_cast<uint64_t>(cd_off) + static_cast<uint64_t>(cd_size) > eocd_off) {
        return make_unexpected(Error::make(ErrorCode::IoError, "ZIP Central Directory out of bounds"));
    }

    ZipReader reader;
    reader.archive_bytes_ = std::move(bytes);
    reader.entries_.reserve(entry_count);

    size_t cur = cd_off;
    const size_t cd_end = static_cast<size_t>(cd_off) + static_cast<size_t>(cd_size);
    for (uint16_t i = 0; i < entry_count; ++i) {
        if (cur + 46u > cd_end) {
            return make_unexpected(Error::make(ErrorCode::IoError, "Truncated ZIP Central Directory entry"));
        }
        if (read_u32_le(&reader.archive_bytes_[cur]) != 0x02014B50u) {
            return make_unexpected(Error::make(ErrorCode::IoError, "Bad ZIP Central Directory signature"));
        }
        ZipEntryInfo info{};
        info.method = read_u16_le(&reader.archive_bytes_[cur + 10u]);
        info.crc32 = read_u32_le(&reader.archive_bytes_[cur + 16u]);
        info.compressed_size = read_u32_le(&reader.archive_bytes_[cur + 20u]);
        info.uncompressed_size = read_u32_le(&reader.archive_bytes_[cur + 24u]);
        const uint16_t name_len = read_u16_le(&reader.archive_bytes_[cur + 28u]);
        const uint16_t extra_len = read_u16_le(&reader.archive_bytes_[cur + 30u]);
        const uint16_t comment_len = read_u16_le(&reader.archive_bytes_[cur + 32u]);
        info.local_header_offset = read_u32_le(&reader.archive_bytes_[cur + 42u]);
        cur += 46u;
        if (cur + name_len + extra_len + comment_len > cd_end) {
            return make_unexpected(
                Error::make(ErrorCode::IoError, "ZIP Central Directory variable fields out of bounds"));
        }
        info.path.assign(reinterpret_cast<const char*>(&reader.archive_bytes_[cur]), name_len);
        cur += static_cast<size_t>(name_len) + extra_len + comment_len;
        if (!is_safe_archive_entry_path(info.path)) {
            return make_unexpected(Error::make(ErrorCode::InvalidArg, "Unsafe path in ZIP archive"));
        }
        if (info.uncompressed_size > kMaxEntryUncompressedBytes) {
            return make_unexpected(Error::make(ErrorCode::InvalidArg, "ZIP entry exceeds uncompressed cap"));
        }
        reader.entries_.push_back(std::move(info));
    }
    return reader;
}

bool ZipReader::contains(std::string_view entry_path) const noexcept {
    for (const auto& e : entries_) {
        if (e.path == entry_path) {
            return true;
        }
    }
    return false;
}

Expected<std::vector<uint8_t>> ZipReader::read_entry(std::string_view entry_path) const {
    const ZipEntryInfo* found = nullptr;
    for (const auto& e : entries_) {
        if (e.path == entry_path) {
            found = &e;
            break;
        }
    }
    if (found == nullptr) {
        return make_unexpected(Error::make(ErrorCode::IoError, "ZIP entry not found"));
    }
    const uint64_t lfh = found->local_header_offset;
    if (lfh + 30ULL > archive_bytes_.size()) {
        return make_unexpected(Error::make(ErrorCode::IoError, "ZIP local header out of bounds"));
    }
    if (read_u32_le(&archive_bytes_[static_cast<size_t>(lfh)]) != 0x04034B50u) {
        return make_unexpected(Error::make(ErrorCode::IoError, "Bad ZIP local header signature"));
    }
    const uint16_t name_len = read_u16_le(&archive_bytes_[static_cast<size_t>(lfh) + 26u]);
    const uint16_t extra_len = read_u16_le(&archive_bytes_[static_cast<size_t>(lfh) + 28u]);
    const uint64_t data_off = lfh + 30ULL + name_len + extra_len;
    if (data_off + found->compressed_size > archive_bytes_.size()) {
        return make_unexpected(Error::make(ErrorCode::IoError, "ZIP entry payload out of bounds"));
    }
    const std::span<const uint8_t> comp_span(archive_bytes_.data() + static_cast<size_t>(data_off),
                                             found->compressed_size);

    std::vector<uint8_t> decoded;
    if (found->method == 0u) {
        if (found->compressed_size != found->uncompressed_size) {
            return make_unexpected(Error::make(ErrorCode::IoError, "ZIP STORE size mismatch"));
        }
        decoded.assign(comp_span.begin(), comp_span.end());
    } else if (found->method == 8u) {
        auto inf = inflate_raw_rfc1951(comp_span, found->uncompressed_size);
        if (!inf) {
            return make_unexpected(inf.error());
        }
        decoded = std::move(inf.value());
    } else {
        return make_unexpected(Error::make(ErrorCode::Unsupported, "Unsupported ZIP compression method"));
    }

    const uint32_t actual_crc = static_cast<uint32_t>(
        ::crc32(0L, decoded.empty() ? Z_NULL : decoded.data(), static_cast<uInt>(decoded.size())));
    if (actual_crc != found->crc32) {
        return make_unexpected(Error::make(ErrorCode::IoError, "ZIP CRC-32 mismatch"));
    }
    return decoded;
}

Expected<std::string> ZipReader::read_text(std::string_view entry_path) const {
    auto b = read_entry(entry_path);
    if (!b) {
        return make_unexpected(b.error());
    }
    return std::string(reinterpret_cast<const char*>(b.value().data()), b.value().size());
}

Expected<void> ZipReader::verify_all() const {
    for (const auto& e : entries_) {
        auto r = read_entry(e.path);
        if (!r) {
            return make_unexpected(r.error());
        }
    }
    return {};
}

Expected<std::string> sha256_hex(std::span<const uint8_t> data) {
#if !defined(_WIN32)
    (void)data;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "Win32 BCrypt unavailable"));
#else
    ScopedBCryptAlg alg;
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "BCryptOpenAlgorithmProvider(SHA256) failed"));
    }
    DWORD hash_obj_size = 0;
    DWORD res_len = 0;
    if (!BCRYPT_SUCCESS(::BCryptGetProperty(alg.h, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hash_obj_size),
                                            sizeof(hash_obj_size), &res_len, 0))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "BCryptGetProperty(OBJECT_LENGTH) failed"));
    }
    std::vector<uint8_t> hash_obj(hash_obj_size);
    ScopedBCryptHash h;
    if (!BCRYPT_SUCCESS(::BCryptCreateHash(alg.h, &h.h, hash_obj.data(), hash_obj_size, nullptr, 0, 0))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "BCryptCreateHash failed"));
    }
    if (!data.empty() &&
        !BCRYPT_SUCCESS(::BCryptHashData(h.h, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "BCryptHashData failed"));
    }
    std::array<uint8_t, 32> digest{};
    if (!BCRYPT_SUCCESS(::BCryptFinishHash(h.h, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "BCryptFinishHash failed"));
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (uint8_t b : digest) {
        hex.push_back(kHex[(b >> 4u) & 0xFu]);
        hex.push_back(kHex[b & 0xFu]);
    }
    return hex;
#endif
}

Expected<std::string> sha256_file_hex(const std::filesystem::path& file_path) {
#if !defined(_WIN32)
    (void)file_path;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "Win32 BCrypt unavailable"));
#else
    std::ifstream f(file_path, std::ios::binary);
    if (!f.is_open()) {
        return make_unexpected(Error::make(ErrorCode::IoError, "Failed to open file for SHA-256"));
    }
    ScopedBCryptAlg alg;
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "BCryptOpenAlgorithmProvider(SHA256) failed"));
    }
    DWORD hash_obj_size = 0;
    DWORD res_len = 0;
    if (!BCRYPT_SUCCESS(::BCryptGetProperty(alg.h, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hash_obj_size),
                                            sizeof(hash_obj_size), &res_len, 0))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "BCryptGetProperty failed"));
    }
    std::vector<uint8_t> hash_obj(hash_obj_size);
    ScopedBCryptHash h;
    if (!BCRYPT_SUCCESS(::BCryptCreateHash(alg.h, &h.h, hash_obj.data(), hash_obj_size, nullptr, 0, 0))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "BCryptCreateHash failed"));
    }
    std::array<uint8_t, 65536> buf{};
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got > 0 && !BCRYPT_SUCCESS(::BCryptHashData(h.h, buf.data(), static_cast<ULONG>(got), 0))) {
            return make_unexpected(Error::make(ErrorCode::Internal, "BCryptHashData failed"));
        }
    }
    std::array<uint8_t, 32> digest{};
    if (!BCRYPT_SUCCESS(::BCryptFinishHash(h.h, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "BCryptFinishHash failed"));
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (uint8_t b : digest) {
        hex.push_back(kHex[(b >> 4u) & 0xFu]);
        hex.push_back(kHex[b & 0xFu]);
    }
    return hex;
#endif
}

Expected<std::vector<uint8_t>> encode_thumbnail_png_480x270(std::span<const uint8_t> pixels, uint32_t src_w,
                                                            uint32_t src_h, bool is_bgra) {
#if !defined(_WIN32)
    (void)pixels;
    (void)src_w;
    (void)src_h;
    (void)is_bgra;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "Win32 WIC unavailable"));
#else
    constexpr uint32_t kThumbW = 480u;
    constexpr uint32_t kThumbH = 270u;
    if (src_w == 0u || src_h == 0u || pixels.size() < static_cast<size_t>(src_w) * src_h * 4u) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Invalid thumbnail pixel buffer dimensions"));
    }

    const HRESULT hr_co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool did_co_init = SUCCEEDED(hr_co);
    struct CoGuard {
        bool active{false};
        ~CoGuard() {
            if (active) {
                ::CoUninitialize();
            }
        }
    } co_guard{did_co_init};

    IWICImagingFactory* raw_factory = nullptr;
    if (FAILED(
            ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw_factory)))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "Failed to create WICImagingFactory"));
    }
    ComUnique<IWICImagingFactory> factory(raw_factory);

    const WICPixelFormatGUID src_fmt = is_bgra ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat32bppRGBA;
    IWICBitmap* raw_bmp = nullptr;
    if (FAILED(factory->CreateBitmapFromMemory(src_w, src_h, src_fmt, src_w * 4u, static_cast<UINT>(pixels.size()),
                                               const_cast<BYTE*>(pixels.data()), &raw_bmp))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC CreateBitmapFromMemory failed"));
    }
    ComUnique<IWICBitmap> bmp(raw_bmp);

    IWICBitmapScaler* raw_scaler = nullptr;
    if (FAILED(factory->CreateBitmapScaler(&raw_scaler))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC CreateBitmapScaler failed"));
    }
    ComUnique<IWICBitmapScaler> scaler(raw_scaler);
    if (FAILED(scaler->Initialize(bmp.get(), kThumbW, kThumbH, WICBitmapInterpolationModeLinear))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC BitmapScaler Initialize failed"));
    }

    IWICFormatConverter* raw_conv = nullptr;
    if (FAILED(factory->CreateFormatConverter(&raw_conv))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC CreateFormatConverter failed"));
    }
    ComUnique<IWICFormatConverter> conv(raw_conv);
    if (FAILED(conv->Initialize(scaler.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC FormatConverter Initialize failed"));
    }

    IStream* raw_stream = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, &raw_stream))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "CreateStreamOnHGlobal failed"));
    }
    ComUnique<IStream> mem_stream(raw_stream);

    IWICBitmapEncoder* raw_enc = nullptr;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &raw_enc))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC CreateEncoder(PNG) failed"));
    }
    ComUnique<IWICBitmapEncoder> encoder(raw_enc);
    if (FAILED(encoder->Initialize(mem_stream.get(), WICBitmapEncoderNoCache))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC Encoder Initialize failed"));
    }

    IWICBitmapFrameEncode* raw_frame = nullptr;
    if (FAILED(encoder->CreateNewFrame(&raw_frame, nullptr))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC CreateNewFrame failed"));
    }
    ComUnique<IWICBitmapFrameEncode> frame(raw_frame);
    if (FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(kThumbW, kThumbH))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC Frame Initialize/SetSize failed"));
    }
    WICPixelFormatGUID dst_fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&dst_fmt)) || FAILED(frame->WriteSource(conv.get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC PNG encode Commit failed"));
    }

    STATSTG stat{};
    if (FAILED(mem_stream->Stat(&stat, STATFLAG_NONAME))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC IStream Stat failed"));
    }
    const size_t png_bytes = static_cast<size_t>(stat.cbSize.QuadPart);
    std::vector<uint8_t> out(png_bytes);
    LARGE_INTEGER zero_pos{};
    if (FAILED(mem_stream->Seek(zero_pos, STREAM_SEEK_SET, nullptr))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC IStream Seek failed"));
    }
    ULONG read_bytes = 0u;
    if (png_bytes > 0u &&
        (FAILED(mem_stream->Read(out.data(), static_cast<ULONG>(png_bytes), &read_bytes)) || read_bytes != png_bytes)) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC IStream Read failed"));
    }
    return out;
#endif
}

Expected<DecodedRgbaImage> decode_png_rgba8(std::span<const uint8_t> png_bytes) {
#if !defined(_WIN32)
    (void)png_bytes;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "Win32 WIC unavailable"));
#else
    if (png_bytes.empty()) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Empty PNG payload"));
    }
    const HRESULT hr_co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool did_co_init = SUCCEEDED(hr_co);
    struct CoGuard {
        bool active{false};
        ~CoGuard() {
            if (active) {
                ::CoUninitialize();
            }
        }
    } co_guard{did_co_init};

    IWICImagingFactory* raw_factory = nullptr;
    if (FAILED(
            ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw_factory)))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "Failed to create WICImagingFactory"));
    }
    ComUnique<IWICImagingFactory> factory(raw_factory);

    IWICStream* raw_wic_stream = nullptr;
    if (FAILED(factory->CreateStream(&raw_wic_stream))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC CreateStream failed"));
    }
    ComUnique<IWICStream> wic_stream(raw_wic_stream);
    if (FAILED(wic_stream->InitializeFromMemory(const_cast<BYTE*>(png_bytes.data()),
                                                static_cast<DWORD>(png_bytes.size())))) {
        return make_unexpected(Error::make(ErrorCode::ParseError, "WIC InitializeFromMemory failed"));
    }

    IWICBitmapDecoder* raw_dec = nullptr;
    if (FAILED(factory->CreateDecoderFromStream(wic_stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, &raw_dec))) {
        return make_unexpected(Error::make(ErrorCode::ParseError, "WIC CreateDecoderFromStream failed"));
    }
    ComUnique<IWICBitmapDecoder> decoder(raw_dec);

    IWICBitmapFrameDecode* raw_frame = nullptr;
    if (FAILED(decoder->GetFrame(0u, &raw_frame))) {
        return make_unexpected(Error::make(ErrorCode::ParseError, "WIC decoder GetFrame(0) failed"));
    }
    ComUnique<IWICBitmapFrameDecode> frame(raw_frame);

    UINT w = 0u;
    UINT h = 0u;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0u || h == 0u || w > 4096u || h > 4096u) {
        return make_unexpected(Error::make(ErrorCode::ParseError, "Invalid PNG frame dimensions"));
    }

    IWICFormatConverter* raw_conv = nullptr;
    if (FAILED(factory->CreateFormatConverter(&raw_conv))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC CreateFormatConverter failed"));
    }
    ComUnique<IWICFormatConverter> conv(raw_conv);
    if (FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC FormatConverter(RGBA) failed"));
    }

    DecodedRgbaImage img{};
    img.width = w;
    img.height = h;
    img.rgba8.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    if (FAILED(conv->CopyPixels(nullptr, w * 4u, static_cast<UINT>(img.rgba8.size()), img.rgba8.data()))) {
        return make_unexpected(Error::make(ErrorCode::Internal, "WIC CopyPixels failed"));
    }
    return img;
#endif
}

Expected<void> write_bytes_atomic(const std::filesystem::path& dst_path, std::span<const uint8_t> bytes,
                                  const std::function<Expected<void>(const std::filesystem::path&)>& verify_tmp) {
#if !defined(_WIN32)
    (void)dst_path;
    (void)bytes;
    (void)verify_tmp;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "Win32 atomic write unavailable"));
#else
    if (dst_path.empty()) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Empty destination path"));
    }
    std::error_code ec;
    if (dst_path.has_parent_path()) {
        std::filesystem::create_directories(dst_path.parent_path(), ec);
    }

    const std::filesystem::path tmp_path(dst_path.native() + L".tmp");
    const std::filesystem::path bak_path(dst_path.native() + L".bak");

    HANDLE h =
        ::CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0u, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return make_unexpected(Error::make(ErrorCode::IoError, "CreateFileW(.tmp) failed"));
    }

    size_t offset = 0u;
    bool write_ok = true;
    while (offset < bytes.size()) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, static_cast<size_t>(64u * 1024u * 1024u)));
        DWORD written = 0u;
        if (!::WriteFile(h, bytes.data() + offset, chunk, &written, nullptr) || written == 0u) {
            write_ok = false;
            break;
        }
        offset += written;
    }
    if (write_ok && !::FlushFileBuffers(h)) {
        write_ok = false;
    }
    ::CloseHandle(h);

    if (!write_ok) {
        ::DeleteFileW(tmp_path.c_str());
        return make_unexpected(Error::make(ErrorCode::IoError, "WriteFile/FlushFileBuffers(.tmp) failed"));
    }

    if (verify_tmp) {
        if (auto vr = verify_tmp(tmp_path); !vr) {
            ::DeleteFileW(tmp_path.c_str());
            return make_unexpected(vr.error());
        }
    }

    if (std::filesystem::exists(dst_path, ec)) {
        ::CopyFileW(dst_path.c_str(), bak_path.c_str(), FALSE);
    }

    if (!::MoveFileExW(tmp_path.c_str(), dst_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tmp_path.c_str());
        return make_unexpected(Error::make(ErrorCode::IoError, "MoveFileExW atomic commit failed"));
    }
    return {};
#endif
}

Expected<std::vector<uint8_t>> read_file_bytes(const std::filesystem::path& src_path, uint64_t max_bytes) {
    std::error_code ec;
    if (!std::filesystem::exists(src_path, ec) || !std::filesystem::is_regular_file(src_path, ec)) {
        return make_unexpected(Error::make(ErrorCode::IoError, "Archive file does not exist"));
    }
    const uint64_t sz = std::filesystem::file_size(src_path, ec);
    if (ec || sz > max_bytes) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "File exceeds size cap or stat failed"));
    }
    std::ifstream f(src_path, std::ios::binary);
    if (!f.is_open()) {
        return make_unexpected(Error::make(ErrorCode::IoError, "Failed to open archive file"));
    }
    std::vector<uint8_t> out(static_cast<size_t>(sz));
    if (sz > 0ULL) {
        f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(sz));
        if (!f) {
            return make_unexpected(Error::make(ErrorCode::IoError, "Failed reading archive bytes"));
        }
    }
    return out;
}

} // namespace aether::core
