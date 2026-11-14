#include "tracking/manifest.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace aether::tracking {

namespace {

[[nodiscard]] std::string trim_copy(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && (std::isspace(static_cast<unsigned char>(s[b])) != 0)) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && (std::isspace(static_cast<unsigned char>(s[e - 1])) != 0)) {
        --e;
    }
    return std::string{s.substr(b, e - b)};
}

[[nodiscard]] std::string unquote_copy(std::string_view s) {
    std::string t = trim_copy(s);
    if (t.size() >= 2U && t.front() == '"' && t.back() == '"') {
        return t.substr(1U, t.size() - 2U);
    }
    return t;
}

[[nodiscard]] std::vector<std::string> parse_string_array(std::string_view v) {
    std::vector<std::string> out;
    const size_t lb = v.find('[');
    const size_t rb = v.rfind(']');
    if (lb == std::string_view::npos || rb == std::string_view::npos || rb <= lb) {
        return out;
    }
    std::string_view inner = v.substr(lb + 1U, rb - lb - 1U);
    size_t start = 0;
    while (start < inner.size()) {
        const size_t comma = inner.find(',', start);
        const std::string_view part =
            (comma == std::string_view::npos) ? inner.substr(start) : inner.substr(start, comma - start);
        std::string item = unquote_copy(part);
        if (!item.empty()) {
            out.push_back(std::move(item));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1U;
    }
    return out;
}

[[nodiscard]] std::vector<double> parse_number_array(std::string_view v) {
    std::vector<double> out;
    const size_t lb = v.find('[');
    const size_t rb = v.rfind(']');
    if (lb == std::string_view::npos || rb == std::string_view::npos || rb <= lb) {
        return out;
    }
    std::string_view inner = v.substr(lb + 1U, rb - lb - 1U);
    size_t start = 0;
    while (start < inner.size()) {
        const size_t comma = inner.find(',', start);
        const std::string_view part =
            (comma == std::string_view::npos) ? inner.substr(start) : inner.substr(start, comma - start);
        const std::string t = trim_copy(part);
        if (!t.empty()) {
            char* endp = nullptr;
            const double val = std::strtod(t.c_str(), &endp);
            if (endp != t.c_str()) {
                out.push_back(val);
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1U;
    }
    return out;
}

[[nodiscard]] bool section_to_model_id(std::string_view section, ModelId& out_id) noexcept {
    if (section == "models.M0_face_detector") {
        out_id = ModelId::M0FaceDetector;
        return true;
    }
    if (section == "models.M1_body_landmarks") {
        out_id = ModelId::M1BodyLandmarks;
        return true;
    }
    if (section == "models.M1_hands_landmarks") {
        out_id = ModelId::M1HandsLandmarks;
        return true;
    }
    if (section == "models.M2_face_landmarks") {
        out_id = ModelId::M2FaceLandmarks;
        return true;
    }
    if (section == "models.M3_arkit52_blendshapes") {
        out_id = ModelId::M3ArkitBlendshapes;
        return true;
    }
    return false;
}

[[nodiscard]] std::string to_upper_ascii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return s;
}

} // namespace

bool TrackingModelManifest::all_present() const noexcept {
    for (const auto& m : models) {
        if (!m.present) {
            return false;
        }
    }
    return manifest_version > 0;
}

AetherStatus load_model_manifest(const std::filesystem::path& manifest_path, TrackingModelManifest& out_manifest,
                                 std::string* out_err) {
    out_manifest = TrackingModelManifest{};
    out_manifest.models_dir = manifest_path.parent_path();

    std::ifstream ifs(manifest_path, std::ios::binary);
    if (!ifs.is_open()) {
        if (out_err != nullptr) {
            *out_err = "Failed to open manifest: " + manifest_path.string();
        }
        return AETHER_E_IO;
    }

    bool in_model_section = false;
    ModelId cur_id = ModelId::M0FaceDetector;
    std::string line;
    while (std::getline(ifs, line)) {
        const size_t hash_pos = line.find('#');
        if (hash_pos != std::string::npos) {
            line.erase(hash_pos);
        }
        const std::string t = trim_copy(line);
        if (t.empty()) {
            continue;
        }
        if (t.front() == '[' && t.back() == ']') {
            const std::string sec = trim_copy(std::string_view(t).substr(1U, t.size() - 2U));
            in_model_section = section_to_model_id(sec, cur_id);
            if (in_model_section) {
                auto& m = out_manifest.models[static_cast<size_t>(cur_id)];
                m.id = cur_id;
                m.section_name = sec;
                m.present = true;
            }
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = trim_copy(std::string_view(t).substr(0U, eq));
        const std::string val = trim_copy(std::string_view(t).substr(eq + 1U));

        if (!in_model_section) {
            if (key == "manifest_version") {
                out_manifest.manifest_version = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
            } else if (key == "licence") {
                out_manifest.licence = unquote_copy(val);
            }
            continue;
        }

        auto& m = out_manifest.models[static_cast<size_t>(cur_id)];
        if (key == "role") {
            m.role = unquote_copy(val);
        } else if (key == "cadence") {
            m.cadence = unquote_copy(val);
        } else if (key == "input_name") {
            m.input_name = unquote_copy(val);
        } else if (key == "input_shape") {
            const auto nums = parse_number_array(val);
            m.input_rank = static_cast<uint32_t>(std::min<size_t>(nums.size(), 4U));
            for (size_t i = 0; i < m.input_rank; ++i) {
                m.input_shape[i] = static_cast<int64_t>(std::llround(nums[i]));
            }
        } else if (key == "output_names") {
            m.output_names = parse_string_array(val);
        } else if (key == "norm_mode") {
            m.norm_mode = unquote_copy(val);
        } else if (key == "norm_mean") {
            const auto nums = parse_number_array(val);
            for (size_t i = 0; i < std::min<size_t>(nums.size(), 3U); ++i) {
                m.norm_mean[i] = static_cast<float>(nums[i]);
            }
        } else if (key == "norm_std") {
            const auto nums = parse_number_array(val);
            for (size_t i = 0; i < std::min<size_t>(nums.size(), 3U); ++i) {
                m.norm_std[i] = static_cast<float>(nums[i]);
            }
        } else if (key == "file_fp32") {
            m.fp32.filename = unquote_copy(val);
        } else if (key == "sha256_fp32") {
            m.fp32.sha256_upper = to_upper_ascii(unquote_copy(val));
        } else if (key == "file_fp16") {
            m.fp16.filename = unquote_copy(val);
        } else if (key == "sha256_fp16") {
            m.fp16.sha256_upper = to_upper_ascii(unquote_copy(val));
        } else if (key == "file_int8") {
            m.int8.filename = unquote_copy(val);
        } else if (key == "sha256_int8") {
            m.int8.sha256_upper = to_upper_ascii(unquote_copy(val));
        } else if (key == "int8_nme_delta_pct") {
            m.int8_nme_delta_pct = std::strtod(val.c_str(), nullptr);
        } else if (key == "tier1_variant") {
            m.tier1_variant = unquote_copy(val);
        } else if (key == "tier2_variant") {
            m.tier2_variant = unquote_copy(val);
        } else if (key == "tier3_variant") {
            m.tier3_variant = unquote_copy(val);
        }
    }

    if (!out_manifest.all_present()) {
        if (out_err != nullptr) {
            *out_err = "Manifest incomplete: missing one or more M0..M3 entries";
        }
        return AETHER_E_PARSE;
    }
    return AETHER_OK;
}

AetherStatus compute_file_sha256(const std::filesystem::path& path, std::string& out_hex_upper, std::string* out_err) {
    out_hex_upper.clear();
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        if (out_err != nullptr) {
            *out_err = "Cannot open model file for SHA-256 check: " + path.string();
        }
        return AETHER_E_IO;
    }

    BCRYPT_ALG_HANDLE h_alg = nullptr;
    BCRYPT_HASH_HANDLE h_hash = nullptr;
    if (::BCryptOpenAlgorithmProvider(&h_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return AETHER_E_INTERNAL;
    }
    DWORD obj_len = 0;
    DWORD cb_data = 0;
    if (::BCryptGetProperty(h_alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len), &cb_data,
                            0) < 0) {
        ::BCryptCloseAlgorithmProvider(h_alg, 0);
        return AETHER_E_INTERNAL;
    }
    std::vector<uint8_t> hash_obj(obj_len, 0U);
    if (::BCryptCreateHash(h_alg, &h_hash, hash_obj.data(), obj_len, nullptr, 0, 0) < 0) {
        ::BCryptCloseAlgorithmProvider(h_alg, 0);
        return AETHER_E_INTERNAL;
    }

    std::array<char, 65536> buf{};
    while (ifs.good()) {
        ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = ifs.gcount();
        if (got > 0) {
            if (::BCryptHashData(h_hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(got), 0) < 0) {
                ::BCryptDestroyHash(h_hash);
                ::BCryptCloseAlgorithmProvider(h_alg, 0);
                return AETHER_E_INTERNAL;
            }
        }
    }

    std::array<uint8_t, 32> digest{};
    const NTSTATUS st = ::BCryptFinishHash(h_hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    ::BCryptDestroyHash(h_hash);
    ::BCryptCloseAlgorithmProvider(h_alg, 0);
    if (st < 0) {
        return AETHER_E_INTERNAL;
    }

    static constexpr char kHex[] = "0123456789ABCDEF";
    out_hex_upper.reserve(64U);
    for (const uint8_t b : digest) {
        out_hex_upper.push_back(kHex[(b >> 4U) & 0xFU]);
        out_hex_upper.push_back(kHex[b & 0xFU]);
    }
    return AETHER_OK;
}

AetherStatus verify_and_resolve_model_path(const TrackingModelManifest& manifest, ModelId id, EpTier ep,
                                           std::filesystem::path& out_onnx_path, std::string* out_err) {
    const size_t idx = static_cast<size_t>(id);
    if (idx >= manifest.models.size() || !manifest.models[idx].present) {
        return AETHER_E_INVALID_ARG;
    }
    const auto& m = manifest.models[idx];
    const std::string& variant_tag =
        (ep == EpTier::NpuOpenVino) ? m.tier1_variant : ((ep == EpTier::Cpu) ? m.tier3_variant : m.tier2_variant);

    const ModelVariantInfo* vi = &m.fp16;
    if ((ep == EpTier::DirectMlIgpu || ep == EpTier::DirectMlDgpu) && !m.fp32.filename.empty()) {
        // ORT DirectML EP aligns tensor upload copies to 4 B (e.g., 390 B FP16 M1 -> 392 B memcpy); use 4-byte-aligned
        // FP32.
        vi = &m.fp32;
    } else if (id == ModelId::M3ArkitBlendshapes && ep == EpTier::Cpu && !m.fp32.filename.empty()) {
        // Tiny [1,146,2] MLP executes in < 0.05 ms in native x86 FP32 vs emulated FP16 kernels on CPU EP.
        vi = &m.fp32;
    } else if (variant_tag == "int8") {
        vi = &m.int8;
    } else if (variant_tag == "fp32") {
        vi = &m.fp32;
    }
    if (vi->filename.empty() || vi->sha256_upper.size() != 64U) {
        if (out_err != nullptr) {
            *out_err = "Manifest entry missing filename/hash for model " + m.section_name;
        }
        return AETHER_E_PARSE;
    }

    const std::filesystem::path full_path = manifest.models_dir / vi->filename;
    std::string actual_hex;
    const AetherStatus h_st = compute_file_sha256(full_path, actual_hex, out_err);
    if (h_st != AETHER_OK) {
        return h_st;
    }
    if (actual_hex != vi->sha256_upper) {
        if (out_err != nullptr) {
            *out_err = "SHA-256 mismatch on model " + full_path.string() + ": expected " + vi->sha256_upper + ", got " +
                       actual_hex;
        }
        return AETHER_E_PARSE;
    }
    out_onnx_path = full_path;
    return AETHER_OK;
}

} // namespace aether::tracking
