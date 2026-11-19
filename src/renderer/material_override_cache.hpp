#pragma once

#include "aether_ffi_contract.h"
#include "renderer/gpu_structs.hpp"
#include "rhi/rhi_device.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace aether::renderer {

/// Computes deterministic FNV-1a 32-bit hash of the ASCII-lowercased string `s`
/// (matching Rust `fnv1a_lower_32` in `crates/aether-plugin-host`).
[[nodiscard]] constexpr uint32_t fnv1a_lower_32(std::string_view s) noexcept {
    uint32_t h = 0x811C9DC5u;
    for (char ch : s) {
        const auto b = static_cast<uint8_t>((ch >= 'A' && ch <= 'Z') ? (ch + ('a' - 'A')) : ch);
        h ^= static_cast<uint32_t>(b);
        h *= 0x01000193u;
    }
    return h;
}

/// Applies non-destructive node graph material overrides (`AetherMaterialOverrideBuffer`)
/// on top of `base_materials` (which may already include PMX `Material Morphs`) into
/// pre-allocated `out_materials` and `out_grades` with **zero per-frame heap allocations**
/// once `out_materials` and `out_grades` have been sized to `base_materials.size()`.
///
/// Disconnecting an override node (or passing an empty `overrides` buffer) immediately
/// restores `out_materials` to `base_materials` and `out_grades` to identity.
void apply_material_overrides_non_destructive(
    std::span<const AetherNprMaterial> base_materials,
    std::string_view avatar_strings,
    const AetherMaterialOverrideBuffer& overrides,
    std::vector<AetherNprMaterial>& out_materials,
    std::vector<GpuMaterialGrade>& out_grades) noexcept;

/// Lock-free async texture asset & live Spout2 override cache (`HR-01`, `HR-02`).
/// Uses zero `std::mutex` / `condition_variable` / `Sleep` primitives.
class MaterialOverrideCache {
public:
    static constexpr size_t kMaxCachedTextures = 32U;

    enum class SlotState : uint32_t {
        Empty = 0,
        Queued = 1,
        Decoding = 2,
        ReadyToUpload = 3,
        Uploaded = 4,
        Failed = 5,
    };

    struct DecodedSlot {
        std::atomic<uint64_t> handle{0};
        std::atomic<uint32_t> state{static_cast<uint32_t>(SlotState::Empty)};
        bool is_srgb{true};
        bool is_spout{false};
        char path_utf8[512]{};
        uint32_t width{0};
        uint32_t height{0};
        std::vector<uint8_t> rgba_pixels;
        rhi::RhiImage gpu_image{};
        uint32_t bindless_slot{0xFFFFFFFFu};
    };

    MaterialOverrideCache() = default;
    ~MaterialOverrideCache() { shutdown(); }

    MaterialOverrideCache(const MaterialOverrideCache&) = delete;
    MaterialOverrideCache& operator=(const MaterialOverrideCache&) = delete;

    void init(rhi::RhiDevice* rhi) noexcept;
    void shutdown() noexcept;

    /// Registers a file path or Spout2 sender name for asynchronous background decode
    /// (called from UI / graph update, never blocks render or simulation thread).
    /// Returns the deterministic 64-bit `AetherTextureHandle`.
    uint64_t register_texture_source(std::string_view path_or_spout_name,
                                     bool is_srgb,
                                     bool is_spout) noexcept;

    /// Polls completed background decodes and uploads them to GPU bindless slots on the render thread.
    void poll_uploads() noexcept;

    /// Looks up the uploaded GPU image for `handle`, or `nullptr` if still loading / unbound.
    [[nodiscard]] const rhi::RhiImage* find_uploaded_image(uint64_t handle) const noexcept;

private:
    void worker_loop() noexcept;

    rhi::RhiDevice* rhi_{nullptr};
    std::array<DecodedSlot, kMaxCachedTextures> slots_{};
    std::atomic<bool> stop_worker_{false};
    std::thread worker_;
};

}  // namespace aether::renderer
