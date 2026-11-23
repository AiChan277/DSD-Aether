#pragma once

#include "core/expected.hpp"
#include "lighting/lighting_store.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace aether::core {

/** Four canonical `.alfx` import application modes (Rev 3.1 §4.3). */
enum class AlfxImportMode : uint8_t {
    ReplaceAll = 0,           // Apply Full (Default): lighting + env/circadian + shading/RT/PT + Satsuei post-FX
    LightsOnly = 1,           // Lights & Modifiers Only: 16-slot lights + softbox/modifiers + passive flags
    ColorPostFxOnly = 2,      // Effects & Grading Only: shading_and_rt_pt + vrm_mmd_parity + satsuei_post_fx
    AdditiveAppendLights = 3, // Append / Merge Lights: append source lights into trailing empty slots <= 16
};

struct AlfxMetadata {
    std::string name{"Custom Rig"};
    std::string author{"AETHER Studio"};
    std::string description{};
    std::vector<std::string> tags{};
    std::string created_utc{};
    std::string app_version{"1.0.4"};
    std::array<std::string, 3> preview_palette_hex{"#FFB870", "#5B6EE1", "#1B2038"};
};

struct AlfxDocument {
    std::string schema_version{"1.0.1"};
    AlfxMetadata meta{};
    renderer::LightingRig rig{renderer::LightingRig::default_three_point()};
    std::string sha256_hex{};
    std::vector<uint8_t> optional_preview_png{};
};

struct AlfxBuiltinPresetInfo {
    const char* filename;
    const char* display_name;
    AlfxMetadata meta;
};

class AlfxSerializer {
  public:
    static constexpr const char* kSchemaVersion = "1.0.1";

    /** Serializes `store` into canonical 2-space-indented UTF-8 `.alfx` v1.0.1 JSON. */
    [[nodiscard]] static Expected<std::string> serialize_to_json_string(const lighting::StudioLightingStore& store,
                                                                        const AlfxMetadata* meta_opt = nullptr);

    /** Convenience overload serializing a raw `LightingRig` snapshot (used for background saves). */
    [[nodiscard]] static Expected<std::string> serialize_rig_to_json_string(const renderer::LightingRig& rig,
                                                                            std::string_view active_name,
                                                                            const AlfxMetadata* meta_opt = nullptr);

    /**
     * Parses `.alfx` v1.0.0 / v1.0.1 UTF-8 JSON bytes (also accepts ZIP-wrapped `.alfx` payloads).
     * Rejects mismatched `format`, unsupported `schema_version`, or checksum mismatches.
     */
    [[nodiscard]] static Expected<AlfxDocument> deserialize_from_bytes(std::span<const uint8_t> bytes);

    /** Atomically writes `.alfx` JSON (using `write_bytes_atomic` + parse-back verification). */
    [[nodiscard]] static Expected<void> save_file(const std::filesystem::path& dst_path,
                                                  const lighting::StudioLightingStore& store,
                                                  const AlfxMetadata* meta_opt = nullptr);

    /** Loads and validates `.alfx` from disk. */
    [[nodiscard]] static Expected<AlfxDocument> load_file(const std::filesystem::path& src_path);

    /** Applies an `AlfxDocument` onto `dst_store` according to `mode`. */
    static void apply_to_store(const AlfxDocument& doc, lighting::StudioLightingStore& dst_store,
                               AlfxImportMode mode) noexcept;

    /** Builds one of the 5 built-in canonical `.alfx` documents (`index` in 0..4). */
    [[nodiscard]] static AlfxDocument build_builtin_preset(size_t index,
                                                           const std::array<float, 3>& pivot_ws = {0.0f, 1.32f, 0.0f});

    /** Returns descriptors for all 5 shipped `.alfx` presets without heap allocation. */
    [[nodiscard]] static const std::array<AlfxBuiltinPresetInfo, 5>& builtin_preset_catalog();

    /** Ensures the 5 standard `.alfx` preset files exist on disk under `presets_alfx_dir`. */
    [[nodiscard]] static Expected<void> write_builtin_presets_dir(const std::filesystem::path& presets_alfx_dir,
                                                                  bool overwrite = false);
};

} // namespace aether::core
