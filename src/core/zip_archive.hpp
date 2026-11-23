#pragma once

#include "core/expected.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aether::core {

struct ZipEntry {
    std::string path;
    std::vector<uint8_t> data;
    bool compress{true};
};

struct ZipEntryInfo {
    std::string path;
    uint32_t crc32{0};
    uint32_t compressed_size{0};
    uint32_t uncompressed_size{0};
    uint16_t method{0};
    uint64_t local_header_offset{0};
};

class ZipWriter {
  public:
    static constexpr uint64_t kMaxArchiveBytes = 256ULL * 1024ULL * 1024ULL;

    /** Serializes `entries` into a standard RFC 1951 (DEFLATE/STORE) ZIP byte buffer. */
    [[nodiscard]] static Expected<std::vector<uint8_t>> build_bytes(std::span<const ZipEntry> entries);

    /**
     * Atomically commits a ZIP archive to `dst_path`:
     *   1) Write `<dst>.tmp` with Win32 `CreateFileW` + `FlushFileBuffers`
     *   2) Read back and verify every entry's CRC-32 + DEFLATE stream via `ZipReader`
     *   3) Preserve existing `dst_path` into `<dst>.bak` (if present)
     *   4) Commit via `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`
     */
    [[nodiscard]] static Expected<void> write_atomic(const std::filesystem::path& dst_path,
                                                     std::span<const ZipEntry> entries);
};

class ZipReader {
  public:
    static constexpr uint64_t kMaxEntryUncompressedBytes = 64ULL * 1024ULL * 1024ULL;

    [[nodiscard]] static Expected<ZipReader> open(const std::filesystem::path& src_path);
    [[nodiscard]] static Expected<ZipReader> open_memory(std::vector<uint8_t> bytes);

    [[nodiscard]] const std::vector<ZipEntryInfo>& entries() const noexcept { return entries_; }
    [[nodiscard]] bool contains(std::string_view entry_path) const noexcept;
    [[nodiscard]] Expected<std::vector<uint8_t>> read_entry(std::string_view entry_path) const;
    [[nodiscard]] Expected<std::string> read_text(std::string_view entry_path) const;
    [[nodiscard]] Expected<void> verify_all() const;

  private:
    std::vector<uint8_t> archive_bytes_{};
    std::vector<ZipEntryInfo> entries_{};
};

/** Computes lowercase 64-char hex SHA-256 of `data` via Win32 BCrypt CNG. */
[[nodiscard]] Expected<std::string> sha256_hex(std::span<const uint8_t> data);

/** Streaming lowercase 64-char hex SHA-256 of `file_path`. */
[[nodiscard]] Expected<std::string> sha256_file_hex(const std::filesystem::path& file_path);

/**
 * Encodes raw RGBA8/BGRA8 pixels into an aspect-scaled 480x270 PNG byte vector using Win32 WIC.
 */
[[nodiscard]] Expected<std::vector<uint8_t>>
encode_thumbnail_png_480x270(std::span<const uint8_t> pixels, uint32_t src_w, uint32_t src_h, bool is_bgra = false);

/**
 * Writes raw `bytes` atomically (`CreateFileW` -> `FlushFileBuffers` -> optional verify -> `.bak` -> `MoveFileExW`).
 */
[[nodiscard]] Expected<void>
write_bytes_atomic(const std::filesystem::path& dst_path, std::span<const uint8_t> bytes,
                   const std::function<Expected<void>(const std::filesystem::path&)>& verify_tmp = {});

/** Reads an entire bounded binary file (up to `max_bytes`). */
[[nodiscard]] Expected<std::vector<uint8_t>> read_file_bytes(const std::filesystem::path& src_path,
                                                             uint64_t max_bytes = ZipWriter::kMaxArchiveBytes);

struct DecodedRgbaImage {
    uint32_t width{0u};
    uint32_t height{0u};
    std::vector<uint8_t> rgba8{};
};

/** Decodes a PNG payload into tight top-down RGBA8 pixels using Win32 WIC. */
[[nodiscard]] Expected<DecodedRgbaImage> decode_png_rgba8(std::span<const uint8_t> png_bytes);

} // namespace aether::core
