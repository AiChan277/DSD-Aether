#pragma once

#include "aether_ffi_contract.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aether::tracking {

enum class ModelId : uint8_t {
    M0FaceDetector = 0,
    M1BodyLandmarks = 1,
    M1HandsLandmarks = 2,
    M2FaceLandmarks = 3,
    M3ArkitBlendshapes = 4,
    Count = 5,
};

struct ModelVariantInfo {
    std::string filename{};
    std::string sha256_upper{};
};

struct ModelManifestEntry {
    ModelId id{ModelId::M0FaceDetector};
    std::string section_name{};
    std::string role{};
    std::string cadence{};
    std::string input_name{};
    std::array<int64_t, 4> input_shape{};
    uint32_t input_rank{4};
    std::vector<std::string> output_names{};
    std::string norm_mode{};
    std::array<float, 3> norm_mean{};
    std::array<float, 3> norm_std{1.0f, 1.0f, 1.0f};
    ModelVariantInfo fp32{};
    ModelVariantInfo fp16{};
    ModelVariantInfo int8{};
    double int8_nme_delta_pct{0.0};
    std::string tier1_variant{"int8"};
    std::string tier2_variant{"fp16"};
    std::string tier3_variant{"int8"};
    bool present{false};
};

struct TrackingModelManifest {
    uint32_t manifest_version{0};
    std::string licence{};
    std::filesystem::path models_dir{};
    std::array<ModelManifestEntry, static_cast<size_t>(ModelId::Count)> models{};

    [[nodiscard]] bool all_present() const noexcept;
};

/** Parses `models/manifest.toml` cleanly without throwing across C++ boundaries. */
[[nodiscard]] AetherStatus load_model_manifest(const std::filesystem::path& manifest_path,
                                               TrackingModelManifest& out_manifest, std::string* out_err = nullptr);

/** Computes uppercase hex SHA-256 using Windows BCrypt (zero third-party dependency). */
[[nodiscard]] AetherStatus compute_file_sha256(const std::filesystem::path& path, std::string& out_hex_upper,
                                               std::string* out_err = nullptr);

/** Resolves the pinned variant for the active EP tier and verifies SHA-256 before session creation (`P3-01`). */
/// Internal tracking execution provider tier, ordered monotonically by capability.
/// Mapped to frozen `AETHER_EP_*` defines when written to `AetherTrackingFrame` / `SkinningFrame`.
enum class EpTier : uint32_t {
    Cpu = 0,
    NpuOpenVino = 1,
    DirectMlIgpu = 2,
    DirectMlDgpu = 3,
    TensorRt = 4,
    GpuOpenVino = 5,
};

[[nodiscard]] constexpr uint32_t to_ffi_ep(EpTier tier) noexcept {
    switch (tier) {
    case EpTier::TensorRt:
        return AETHER_EP_NVIDIA_TRT;
    case EpTier::NpuOpenVino:
        return AETHER_EP_INTEL_NPU;
    case EpTier::GpuOpenVino:
        return AETHER_EP_OPENVINO_GPU;
    case EpTier::DirectMlIgpu:
    case EpTier::DirectMlDgpu:
        return AETHER_EP_DIRECTML_GPU;
    case EpTier::Cpu:
    default:
        return AETHER_EP_CPU;
    }
}

[[nodiscard]] AetherStatus verify_and_resolve_model_path(const TrackingModelManifest& manifest, ModelId id, EpTier ep,
                                                         std::filesystem::path& out_onnx_path,
                                                         std::string* out_err = nullptr);

} // namespace aether::tracking
