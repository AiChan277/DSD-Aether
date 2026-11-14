#pragma once

#include "aether_ffi_contract.h"
#include "core/expected.hpp"
#include "rhi/rhi_device.hpp"

#include <cstdint>
#include <vector>

namespace aether::renderer {

struct SdfBakeInput {
    uint32_t resolution{256};
    uint32_t azimuth_steps{16};
    const AetherVertex25D* vertices{nullptr};
    uint32_t vertex_count{0};
    const uint32_t* indices{nullptr};
    uint32_t index_count{0};
    float forward_axis[3]{0.0f, 0.0f, 1.0f};
    float right_axis[3]{1.0f, 0.0f, 0.0f};
};

struct SdfBakeOutput {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> r8_pixels{};
};

/**
 * Offline 2D face shadow SDF generator (`§10.6.2`, `P4-10`).
 * Projects/samples face proxy normals across `16` horizontal light azimuth angles into UV space and
 * computes a monotonic R8 signed-distance shadow field (`0..255`, threshold `0.5 = 127.5`) for avatars
 * lacking an authored face SDF map.
 */
class OfflineSdfGenerator {
  public:
    [[nodiscard]] static Expected<SdfBakeOutput> bake_face_sdf(const SdfBakeInput& input);

    [[nodiscard]] static Expected<rhi::RhiImage> upload_r8_texture(const rhi::RhiDevice& device,
                                                                   const SdfBakeOutput& baked);
};

} // namespace aether::renderer
