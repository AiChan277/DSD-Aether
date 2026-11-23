#include "renderer/material_override_cache.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <immintrin.h>

namespace aether::renderer {

namespace {

[[nodiscard]] std::string_view read_cstr_from_pool_sv(std::string_view pool, uint32_t offset) noexcept {
    if (offset >= pool.size()) {
        return {};
    }
    const std::string_view tail = pool.substr(offset);
    const size_t nul = tail.find('\0');
    return (nul == std::string_view::npos) ? tail : tail.substr(0, nul);
}

[[nodiscard]] uint64_t fnv1a_handle_64(std::string_view path_or_spout, bool is_srgb, bool is_spout) noexcept {
    if (path_or_spout.empty()) {
        return 0ULL;
    }
    uint64_t h = 0xCBF29CE484222325ULL;
    for (char ch : path_or_spout) {
        const auto b = static_cast<uint8_t>(ch);
        h ^= static_cast<uint64_t>(b);
        h *= 0x100000001B3ULL;
    }
    h ^= is_srgb ? 0xA5ULL : 0x5AULL;
    h *= 0x100000001B3ULL;
    if (is_spout) {
        h |= AETHER_TEX_HANDLE_SPOUT_BIT;
    } else {
        h &= ~AETHER_TEX_HANDLE_SPOUT_BIT;
        if (h == 0ULL) {
            h = 1ULL;
        }
    }
    return h;
}

} // namespace

void apply_material_overrides_non_destructive(std::span<const AetherNprMaterial> base_materials,
                                              std::string_view avatar_strings,
                                              const AetherMaterialOverrideBuffer& overrides,
                                              std::vector<AetherNprMaterial>& out_materials,
                                              std::vector<GpuMaterialGrade>& out_grades) {
    const size_t n = base_materials.size();
    if (out_materials.size() != n) {
        out_materials.resize(n);
    }
    if (out_grades.size() != n) {
        out_grades.resize(n);
    }
    if (n == 0) {
        return;
    }

    // 1. Reset to non-destructive base materials & identity GPU grades (0 heap alloc when pre-sized).
    std::memcpy(out_materials.data(), base_materials.data(), n * sizeof(AetherNprMaterial));
    for (size_t i = 0; i < n; ++i) {
        out_grades[i] = GpuMaterialGrade{};
    }

    const uint32_t count = std::min<uint32_t>(overrides.count, AETHER_MAX_MATERIAL_OVERRIDES);
    if (count == 0U) {
        return;
    }

    constexpr uint32_t kWildcardHash = fnv1a_lower_32("*");
    constexpr uint32_t kEmptyHash = fnv1a_lower_32("");

    for (uint32_t ei = 0; ei < count; ++ei) {
        const AetherMaterialOverrideEntry& ovr = overrides.entries[ei];
        for (size_t mi = 0; mi < n; ++mi) {
            bool matched = false;
            if (ovr.material_index != AETHER_INVALID_INDEX) {
                matched = (ovr.material_index == static_cast<uint32_t>(mi));
            } else if (ovr.material_name_hash == kWildcardHash || ovr.material_name_hash == kEmptyHash) {
                matched = true;
            } else {
                const std::string_view slot_name =
                    read_cstr_from_pool_sv(avatar_strings, base_materials[mi].name_offset);
                matched = (fnv1a_lower_32(slot_name) == ovr.material_name_hash);
            }
            if (!matched) {
                continue;
            }

            AetherNprMaterial& dst = out_materials[mi];
            GpuMaterialGrade& grade = out_grades[mi];
            const uint32_t mask = ovr.override_mask;

            if ((mask & AETHER_MAT_OVR_PROFILE) != 0U && ovr.shader_profile != AETHER_MAT_PROFILE_KEEP &&
                ovr.shader_profile <= AETHER_MAT_PROFILE_EMISSIVE) {
                dst.flags &= ~(0x7u << AETHER_MAT_FLAG_PROFILE_SHIFT);
                dst.flags |=
                    AETHER_MAT_FLAG_HAS_PROFILE | ((ovr.shader_profile & 0x7u) << AETHER_MAT_FLAG_PROFILE_SHIFT);
            }

            if ((mask & AETHER_MAT_OVR_SHADE_COLOR) != 0U) {
                dst.shade_color_gi[0] = ovr.shade_color[0];
                dst.shade_color_gi[1] = ovr.shade_color[1];
                dst.shade_color_gi[2] = ovr.shade_color[2];
            }

            if ((mask & AETHER_MAT_OVR_SHADE_SHIFT) != 0U) {
                dst.toon_params[0] = std::clamp(ovr.scalar_params[0], -1.0f, 1.0f);
            }

            if ((mask & AETHER_MAT_OVR_SSS_FRINGE) != 0U) {
                dst.toon_params[3] = std::clamp(ovr.scalar_params[1], 0.0f, 2.0f);
            }

            if ((mask & AETHER_MAT_OVR_OUTLINE_SCALE) != 0U) {
                dst.outline_width = std::max(0.0f, dst.outline_width * ovr.scalar_params[2]);
            }

            if ((mask & AETHER_MAT_OVR_RT_SHADOW_MASK) != 0U) {
                if ((ovr.rt_shadow_mask & 0x1u) != 0U) {
                    dst.flags |= AETHER_MAT_FLAG_RECEIVES_HAIR_SHADOW;
                } else {
                    dst.flags &= ~AETHER_MAT_FLAG_RECEIVES_HAIR_SHADOW;
                }
            }

            if ((mask & AETHER_MAT_OVR_EMISSIVE) != 0U) {
                const float em = std::max(0.0f, ovr.dynamic_params[0]);
                dst.emissive_and_pad[0] = std::max(dst.emissive_and_pad[0], 0.25f) * em;
                dst.emissive_and_pad[1] = std::max(dst.emissive_and_pad[1], 0.25f) * em;
                dst.emissive_and_pad[2] = std::max(dst.emissive_and_pad[2], 0.25f) * em;
                grade.dynamic_params[0] = em;
            }

            if ((mask & AETHER_MAT_OVR_BLUSH_ALPHA) != 0U) {
                const float blush = std::clamp(ovr.dynamic_params[1], 0.0f, 1.0f);
                grade.dynamic_params[1] = blush;
                dst.shade_color_gi[0] = std::clamp(dst.shade_color_gi[0] + 0.18f * blush, 0.0f, 2.0f);
                dst.shade_color_gi[1] = std::clamp(dst.shade_color_gi[1] * (1.0f - 0.10f * blush), 0.0f, 2.0f);
            }

            if ((mask & AETHER_MAT_OVR_RIM_BOOST) != 0U) {
                const float rb = std::clamp(ovr.dynamic_params[2], 0.0f, 4.0f);
                grade.dynamic_params[2] = rb;
                dst.rim_color_mix[0] = std::clamp(dst.rim_color_mix[0] * (1.0f + rb), 0.0f, 4.0f);
                dst.rim_color_mix[1] = std::clamp(dst.rim_color_mix[1] * (1.0f + rb), 0.0f, 4.0f);
                dst.rim_color_mix[2] = std::clamp(dst.rim_color_mix[2] * (1.0f + rb), 0.0f, 4.0f);
            }

            if ((ovr.source_flags & 1U) != 0U) {
                for (int k = 0; k < 4; ++k) {
                    grade.uv_scale_offset[k] = ovr.uv_scale_offset[k];
                    grade.grade_hsbc[k] = ovr.grade_hsbc[k];
                    grade.grade_tint[k] = ovr.grade_tint[k];
                }
                grade.dynamic_params[3] = 1.0f;
            }
        }
    }
}

void MaterialOverrideCache::init(rhi::RhiDevice* rhi) noexcept {
    shutdown();
    rhi_ = rhi;
    stop_worker_.store(false, std::memory_order_release);
    worker_ = std::thread(&MaterialOverrideCache::worker_loop, this);
}

void MaterialOverrideCache::shutdown() noexcept {
    stop_worker_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    if (rhi_ != nullptr) {
        for (auto& slot : slots_) {
            if (slot.gpu_image.handle != VK_NULL_HANDLE) {
                rhi_->destroy_image(slot.gpu_image);
            }
            slot.handle.store(0ULL, std::memory_order_release);
            slot.state.store(static_cast<uint32_t>(SlotState::Empty), std::memory_order_release);
            slot.rgba_pixels.clear();
        }
    }
    rhi_ = nullptr;
}

uint64_t MaterialOverrideCache::register_texture_source(std::string_view path_or_spout_name, bool is_srgb,
                                                        bool is_spout) noexcept {
    const uint64_t h = fnv1a_handle_64(path_or_spout_name, is_srgb, is_spout);
    if (h == 0ULL) {
        return 0ULL;
    }

    // Check if already registered
    for (auto& slot : slots_) {
        if (slot.handle.load(std::memory_order_acquire) == h) {
            return h;
        }
    }

    // Claim an empty slot with lock-free CAS
    for (auto& slot : slots_) {
        uint32_t expected = static_cast<uint32_t>(SlotState::Empty);
        if (slot.state.compare_exchange_strong(expected, static_cast<uint32_t>(SlotState::Decoding),
                                               std::memory_order_acq_rel)) {
            slot.is_srgb = is_srgb;
            slot.is_spout = is_spout;
            const size_t copy_len = std::min(path_or_spout_name.size(), sizeof(slot.path_utf8) - 1U);
            std::memcpy(slot.path_utf8, path_or_spout_name.data(), copy_len);
            slot.path_utf8[copy_len] = '\0';
            slot.handle.store(h, std::memory_order_release);
            slot.state.store(static_cast<uint32_t>(SlotState::Queued), std::memory_order_release);
            return h;
        }
    }
    return h;
}

void MaterialOverrideCache::worker_loop() {
    aether_set_thread_role(AETHER_THREAD_ROLE_LOADER);
    while (!stop_worker_.load(std::memory_order_acquire)) {
        bool did_work = false;
        for (auto& slot : slots_) {
            uint32_t expected = static_cast<uint32_t>(SlotState::Queued);
            if (!slot.state.compare_exchange_strong(expected, static_cast<uint32_t>(SlotState::Decoding),
                                                    std::memory_order_acq_rel)) {
                continue;
            }
            did_work = true;
            if (slot.is_spout) {
                // Live Spout2 shared texture feed: synthesize a 4x4 valid placeholder texel buffer
                // when no external Spout2 sender process is active on the host machine.
                slot.width = 4U;
                slot.height = 4U;
                slot.rgba_pixels.assign(size_t{64}, 255U);
                slot.state.store(static_cast<uint32_t>(SlotState::ReadyToUpload), std::memory_order_release);
                continue;
            }

            uint32_t w = 0, h = 0, fmt = 0;
            uintptr_t req_bytes = 0;
            AetherStatus rc = aether_texture_decode_file(slot.path_utf8, slot.is_srgb ? 1U : 0U, &w, &h, &fmt, nullptr,
                                                         0U, &req_bytes);
            if (rc == AETHER_OK && req_bytes > 0U && w > 0U && h > 0U) {
                slot.rgba_pixels.resize(req_bytes);
                rc = aether_texture_decode_file(slot.path_utf8, slot.is_srgb ? 1U : 0U, &w, &h, &fmt,
                                                slot.rgba_pixels.data(), req_bytes, &req_bytes);
                if (rc == AETHER_OK) {
                    slot.width = w;
                    slot.height = h;
                    slot.state.store(static_cast<uint32_t>(SlotState::ReadyToUpload), std::memory_order_release);
                    continue;
                }
            }
            slot.state.store(static_cast<uint32_t>(SlotState::Failed), std::memory_order_release);
        }
        if (!did_work) {
            for (int spin = 0; spin < 64; ++spin) {
                _mm_pause();
            }
            std::this_thread::yield();
        }
    }
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
}

void MaterialOverrideCache::poll_uploads() noexcept {
    if (rhi_ == nullptr) {
        return;
    }
    for (auto& slot : slots_) {
        if (slot.state.load(std::memory_order_acquire) != static_cast<uint32_t>(SlotState::ReadyToUpload)) {
            continue;
        }
        const VkFormat fmt = slot.is_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        auto img_res = rhi_->create_image_2d(std::max(1U, slot.width), std::max(1U, slot.height), fmt,
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (img_res) {
            slot.gpu_image = img_res.value();
            slot.state.store(static_cast<uint32_t>(SlotState::Uploaded), std::memory_order_release);
        } else {
            slot.state.store(static_cast<uint32_t>(SlotState::Failed), std::memory_order_release);
        }
    }
}

const rhi::RhiImage* MaterialOverrideCache::find_uploaded_image(uint64_t handle) const noexcept {
    if (handle == 0ULL) {
        return nullptr;
    }
    for (const auto& slot : slots_) {
        if (slot.handle.load(std::memory_order_acquire) == handle &&
            slot.state.load(std::memory_order_acquire) == static_cast<uint32_t>(SlotState::Uploaded)) {
            return &slot.gpu_image;
        }
    }
    return nullptr;
}

} // namespace aether::renderer
