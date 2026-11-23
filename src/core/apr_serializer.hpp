#pragma once

#include "aether/ui_state_contract.hpp"
#include "aether_ffi_contract.h"
#include "core/alfx_serializer.hpp"
#include "core/expected.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace aether::core {

struct AprManifestSummary {
    std::string avatar_display_name{};
    std::string avatar_format{};
    std::string active_alfx_name{};
    uint32_t active_light_count{0u};
    std::string render_tier{"HybridRT"};
    bool has_thumbnail{false};
};

struct AprManifest {
    std::string format{"APR"};
    std::string schema_version{"1.0.0"};
    std::string project_uuid{};
    std::string project_name{};
    std::string app_version{"1.0.4"};
    std::string created_utc{};
    std::string modified_utc{};
    AprManifestSummary summary{};
    std::string sha256_project_json{};
    std::string sha256_lighting_alfx{};
    std::string sha256_node_graph_json{};
};

struct AprLoadedBundle {
    AprManifest manifest{};
    StudioStateStore state{};
    renderer::LightingRig lighting_rig{renderer::LightingRig::default_three_point()};
    AlfxMetadata lighting_meta{};
    std::vector<uint8_t> thumbnail_png{};
};

struct AprSaveInput {
    const StudioStateStore* state{nullptr};
    const lighting::StudioLightingStore* lighting{nullptr};
    /** Optional Rust host handle; when set, `node_graph.json` is pulled live via `aether_graph_serialize_json`. */
    AetherHostHandle rust_host{nullptr};
    const AlfxMetadata* lighting_meta{nullptr};
    /** Optional raw RGBA8/BGRA8 pixels to be WIC-encoded to 480x270 PNG. */
    std::span<const uint8_t> thumbnail_pixels{};
    uint32_t thumb_width{0u};
    uint32_t thumb_height{0u};
    bool thumb_is_bgra{false};
    /** Optional pre-encoded PNG bytes (e.g. reused thumbnail from session state). */
    std::span<const uint8_t> preencoded_thumbnail_png{};
    /** Optional test hook invoked after `.tmp` write & ZIP verification, before `MoveFileExW` commit. */
    std::function<Expected<void>(const std::filesystem::path& tmp_path)> pre_commit_hook{};
};

class AprSerializer {
  public:
    static constexpr const char* kSchemaVersion = "1.0.0";

    /** Generates a lowercase RFC 4122 v4 UUID string via `BCryptGenRandom`. */
    [[nodiscard]] static std::string generate_uuid_v4();

    /** Returns current UTC timestamp formatted as ISO-8601 (`YYYY-MM-DDTHH:MM:SSZ`). */
    [[nodiscard]] static std::string current_utc_timestamp();

    /** Fast metadata read for Project Manager card scan (reads `manifest.json` + optional `thumbnail.png` only). */
    [[nodiscard]] static Expected<AprManifest> read_manifest_fast(const std::filesystem::path& apr_path,
                                                                  std::vector<uint8_t>* out_thumbnail_png = nullptr);

    /**
     * Full transactional read + SHA-256 verification of `.apr` archive.
     * Rejects corrupted ZIPs, missing entries, invalid format/version, or checksum tampering.
     */
    [[nodiscard]] static Expected<AprLoadedBundle> load_file(const std::filesystem::path& apr_path);

    /** Pushes `bundle.state.node_graph_json` onto `rust_host` via `aether_graph_deserialize_json`. */
    [[nodiscard]] static Expected<void> apply_loaded_graph_to_host(const AprLoadedBundle& bundle,
                                                                   AetherHostHandle rust_host);

    /**
     * Atomically writes `.apr` archive:
     *   1) Build ZIP (`manifest.json`, `project.json`, `lighting.alfx`, `node_graph.json`, `thumbnail.png`,
     * `assets/...`) 2) Write `<dst>.tmp` + `FlushFileBuffers` 3) Verify ZIP CRC32/DEFLATE & SHA-256 checksums on
     * `<dst>.tmp` 4) Backup existing `dst` to `<dst>.bak` and swap atomically via `MoveFileExW`
     */
    [[nodiscard]] static Expected<AprManifest> save_file(const std::filesystem::path& dst_apr_path,
                                                         const AprSaveInput& input);

    /**
     * Resolves an avatar path stored in `.apr`: checks `path_relative` (resolved against `apr_path.parent_path()`)
     * before falling back to `path_absolute` (Rev 3.1 §3.4 Phase A).
     */
    [[nodiscard]] static std::filesystem::path resolve_avatar_path(const std::filesystem::path& apr_path,
                                                                   const AvatarProjectState& avatar);
};

class LegacyAetherMigrator {
  public:
    /** Returns true if `path` looks like a legacy `*.aether` directory bundle or legacy `.aether` JSON/TOML file. */
    [[nodiscard]] static bool is_legacy_aether_path(const std::filesystem::path& path);

    /**
     * Converts a legacy `MyShow.aether/` bundle (`project.toml` v1/v2 + `graph.json` + `assets.toml`) or single-file
     * legacy `.aether` project into an unsaved `AprLoadedBundle` without modifying `src_aether_path`.
     */
    [[nodiscard]] static Expected<AprLoadedBundle> migrate_to_bundle(const std::filesystem::path& src_aether_path);

    /** Convenience helper: migrates `src_aether_path` and writes a new `.apr` at `dst_apr_path`. */
    [[nodiscard]] static Expected<AprLoadedBundle> migrate_to_apr_file(const std::filesystem::path& src_aether_path,
                                                                       const std::filesystem::path& dst_apr_path,
                                                                       AetherHostHandle rust_host = nullptr);
};

} // namespace aether::core
