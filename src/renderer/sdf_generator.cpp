#include "renderer/sdf_generator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace aether::renderer {
namespace {

struct Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

[[nodiscard]] Vec3 normalize3(Vec3 v) noexcept {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-6f) {
        return {0.0f, 0.0f, 1.0f};
    }
    return {v.x / len, v.y / len, v.z / len};
}

[[nodiscard]] Vec3 cross3(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] float dot3(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] float smoothstep(float e0, float e1, float x) noexcept {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

Expected<SdfBakeOutput> OfflineSdfGenerator::bake_face_sdf(const SdfBakeInput& input) {
    if (input.vertices == nullptr || input.vertex_count == 0 || input.resolution < 8 || input.resolution > 2048) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "Invalid SdfBakeInput parameters")};
    }

    const uint32_t res = input.resolution;
    const uint32_t steps = std::clamp(input.azimuth_steps, 4u, 64u);

    const Vec3 fwd = normalize3({input.forward_axis[0], input.forward_axis[1], input.forward_axis[2]});
    Vec3 right_in{input.right_axis[0], input.right_axis[1], input.right_axis[2]};
    if (std::sqrt(dot3(right_in, right_in)) < 1e-4f) {
        const Vec3 world_up = (std::abs(fwd.y) < 0.98f) ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
        right_in = cross3(world_up, fwd);
    }
    const Vec3 up = normalize3(cross3(fwd, right_in));
    const Vec3 right = normalize3(cross3(up, fwd));

    // Compute bounds over vertices tagged `sdf_face_mask > 0.1f` (fallback to all vertices if untagged).
    bool any_tagged = false;
    for (uint32_t i = 0; i < input.vertex_count; ++i) {
        if (input.vertices[i].sdf_face_mask > 0.1f) {
            any_tagged = true;
            break;
        }
    }

    float min_x = 1e9f, max_x = -1e9f;
    float min_y = 1e9f, max_y = -1e9f;
    for (uint32_t i = 0; i < input.vertex_count; ++i) {
        const auto& v = input.vertices[i];
        if (any_tagged && v.sdf_face_mask <= 0.1f) {
            continue;
        }
        const Vec3 p{v.position[0], v.position[1], v.position[2]};
        const float px = dot3(p, right);
        const float py = dot3(p, up);
        min_x = std::min(min_x, px);
        max_x = std::max(max_x, px);
        min_y = std::min(min_y, py);
        max_y = std::max(max_y, py);
    }
    const float span_x = std::max(max_x - min_x, 1e-4f);
    const float span_y = std::max(max_y - min_y, 1e-4f);

    std::vector<float> accum_nx(static_cast<size_t>(res) * res, 0.0f);
    std::vector<float> accum_ny(static_cast<size_t>(res) * res, 0.0f);
    std::vector<float> accum_nz(static_cast<size_t>(res) * res, 0.0f);
    std::vector<float> accum_w(static_cast<size_t>(res) * res, 0.0f);

    auto project_xy = [&](const AetherVertex25D& v, float& ox, float& oy) noexcept {
        const Vec3 p{v.position[0], v.position[1], v.position[2]};
        const float u = (dot3(p, right) - min_x) / span_x;
        const float w_y = (dot3(p, up) - min_y) / span_y;
        ox = u * static_cast<float>(res - 1u);
        oy = w_y * static_cast<float>(res - 1u);
    };

    const uint32_t tri_count = (input.indices != nullptr) ? (input.index_count / 3u) : (input.vertex_count / 3u);
    for (uint32_t t = 0; t < tri_count; ++t) {
        const uint32_t i0 = (input.indices != nullptr) ? input.indices[t * 3u + 0u] : (t * 3u + 0u);
        const uint32_t i1 = (input.indices != nullptr) ? input.indices[t * 3u + 1u] : (t * 3u + 1u);
        const uint32_t i2 = (input.indices != nullptr) ? input.indices[t * 3u + 2u] : (t * 3u + 2u);
        if (i0 >= input.vertex_count || i1 >= input.vertex_count || i2 >= input.vertex_count) {
            continue;
        }
        const auto& v0 = input.vertices[i0];
        const auto& v1 = input.vertices[i1];
        const auto& v2 = input.vertices[i2];
        if (any_tagged && v0.sdf_face_mask <= 0.1f && v1.sdf_face_mask <= 0.1f && v2.sdf_face_mask <= 0.1f) {
            continue;
        }

        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
        project_xy(v0, x0, y0);
        project_xy(v1, x1, y1);
        project_xy(v2, x2, y2);

        const float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
        if (std::abs(area) < 1e-5f) {
            continue;
        }
        const float inv_area = 1.0f / area;

        const uint32_t bx0 = static_cast<uint32_t>(
            std::clamp(static_cast<int>(std::floor(std::min({x0, x1, x2}))), 0, static_cast<int>(res - 1)));
        const uint32_t bx1 = static_cast<uint32_t>(
            std::clamp(static_cast<int>(std::ceil(std::max({x0, x1, x2}))), 0, static_cast<int>(res - 1)));
        const uint32_t by0 = static_cast<uint32_t>(
            std::clamp(static_cast<int>(std::floor(std::min({y0, y1, y2}))), 0, static_cast<int>(res - 1)));
        const uint32_t by1 = static_cast<uint32_t>(
            std::clamp(static_cast<int>(std::ceil(std::max({y0, y1, y2}))), 0, static_cast<int>(res - 1)));

        for (uint32_t py = by0; py <= by1; ++py) {
            for (uint32_t px = bx0; px <= bx1; ++px) {
                const float fx = static_cast<float>(px) + 0.5f;
                const float fy = static_cast<float>(py) + 0.5f;
                const float w0 = ((x1 - fx) * (y2 - fy) - (y1 - fy) * (x2 - fx)) * inv_area;
                const float w1 = ((x2 - fx) * (y0 - fy) - (y2 - fy) * (x0 - fx)) * inv_area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < -0.02f || w1 < -0.02f || w2 < -0.02f) {
                    continue;
                }
                const float nx = w0 * v0.proxy_normal[0] + w1 * v1.proxy_normal[0] + w2 * v2.proxy_normal[0];
                const float ny = w0 * v0.proxy_normal[1] + w1 * v1.proxy_normal[1] + w2 * v2.proxy_normal[1];
                const float nz = w0 * v0.proxy_normal[2] + w1 * v1.proxy_normal[2] + w2 * v2.proxy_normal[2];
                const size_t idx = static_cast<size_t>(py) * res + px;
                accum_nx[idx] += nx;
                accum_ny[idx] += ny;
                accum_nz[idx] += nz;
                accum_w[idx] += 1.0f;
            }
        }
    }

    // Integrate visibility across 16 horizontal azimuth angles for every covered texel; fill uncovered margin smoothly.
    std::vector<float> field(static_cast<size_t>(res) * res, 0.5f);
    for (uint32_t py = 0; py < res; ++py) {
        for (uint32_t px = 0; px < res; ++px) {
            const size_t idx = static_cast<size_t>(py) * res + px;
            Vec3 n{};
            if (accum_w[idx] > 0.0f) {
                n = normalize3({accum_nx[idx], accum_ny[idx], accum_nz[idx]});
            } else {
                const float u = static_cast<float>(px) / static_cast<float>(res - 1u);
                const float sin_x = std::clamp((u * 2.0f - 1.0f) * 0.75f, -0.9f, 0.9f);
                const float cos_z = std::sqrt(std::max(1.0f - sin_x * sin_x, 0.05f));
                n = {right.x * sin_x + fwd.x * cos_z, right.y * sin_x + fwd.y * cos_z, right.z * sin_x + fwd.z * cos_z};
                n = normalize3(n);
            }

            float lit_sum = 0.0f;
            for (uint32_t s = 0; s < steps; ++s) {
                const float theta = ((static_cast<float>(s) + 0.5f) / static_cast<float>(steps)) * 3.14159265f;
                const float c = std::cos(theta);
                const float sn = std::sin(theta);
                const Vec3 ldir{fwd.x * c + right.x * sn, fwd.y * c + right.y * sn, fwd.z * c + right.z * sn};
                lit_sum += smoothstep(-0.04f, 0.04f, dot3(n, ldir));
            }
            field[idx] = std::clamp(lit_sum / static_cast<float>(steps), 0.0f, 1.0f);
        }
    }

    // Light 3x3 smoothing pass to eliminate quantized angular banding across gradients
    SdfBakeOutput out{};
    out.width = res;
    out.height = res;
    out.r8_pixels.resize(static_cast<size_t>(res) * res);
    for (uint32_t py = 0; py < res; ++py) {
        for (uint32_t px = 0; px < res; ++px) {
            float sum = 0.0f;
            int cnt = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                const int sy = std::clamp(static_cast<int>(py) + dy, 0, static_cast<int>(res) - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    const int sx = std::clamp(static_cast<int>(px) + dx, 0, static_cast<int>(res) - 1);
                    sum += field[static_cast<size_t>(sy) * res + static_cast<uint32_t>(sx)];
                    ++cnt;
                }
            }
            const float avg = sum / static_cast<float>(cnt);
            out.r8_pixels[static_cast<size_t>(py) * res + px] =
                static_cast<uint8_t>(std::clamp(std::lround(avg * 255.0f), 0L, 255L));
        }
    }
    return out;
}

Expected<rhi::RhiImage> OfflineSdfGenerator::upload_r8_texture(const rhi::RhiDevice& device,
                                                               const SdfBakeOutput& baked) {
    if (device.device() == VK_NULL_HANDLE || baked.width == 0 || baked.height == 0 ||
        baked.r8_pixels.size() != static_cast<size_t>(baked.width) * baked.height) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "Invalid R8 SDF bake payload for GPU upload")};
    }

    auto img_res =
        device.create_image_2d(baked.width, baked.height, VK_FORMAT_R8_UNORM,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!img_res) {
        return Unexpected<Error>{img_res.error()};
    }
    rhi::RhiImage img = img_res.value();

    const VkDeviceSize byte_size = baked.r8_pixels.size();
    auto stg_res =
        device.create_buffer(byte_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!stg_res) {
        device.destroy_image(img);
        return Unexpected<Error>{stg_res.error()};
    }
    rhi::RhiBuffer stg = stg_res.value();
    std::memcpy(stg.info.pMappedData, baked.r8_pixels.data(), byte_size);
    vmaFlushAllocation(device.allocator(), stg.allocation, 0, byte_size);

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = device.graphics_family();
    cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(device.device(), &cpci, nullptr, &pool) != VK_SUCCESS) {
        device.destroy_buffer(stg);
        device.destroy_image(img);
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkCreateCommandPool(sdf upload) failed")};
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device.device(), &cbai, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(device.device(), pool, nullptr);
        device.destroy_buffer(stg);
        device.destroy_image(img);
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkAllocateCommandBuffers(sdf upload) failed")};
    }

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    (void)vkBeginCommandBuffer(cmd, &bi);

    device.cmd_transition_image(cmd, img.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {baked.width, baked.height, 1};
    vkCmdCopyBufferToImage(cmd, stg.handle, img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    device.cmd_transition_image(cmd, img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    (void)vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(device.graphics_queue(), 1, &si, VK_NULL_HANDLE) != VK_SUCCESS ||
        vkQueueWaitIdle(device.graphics_queue()) != VK_SUCCESS) {
        vkDestroyCommandPool(device.device(), pool, nullptr);
        device.destroy_buffer(stg);
        device.destroy_image(img);
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "GPU upload submit failed for R8 SDF texture")};
    }

    vkDestroyCommandPool(device.device(), pool, nullptr);
    device.destroy_buffer(stg);
    return img;
}

} // namespace aether::renderer