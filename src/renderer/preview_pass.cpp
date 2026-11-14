#include "renderer/preview_pass.hpp"

#include "aether_ffi_contract.h"
#include "generated/preview_shaders_spv.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aether::renderer {
namespace {

struct PreviewPushConstants {
    float mvp[16]{};
    float base_color[4]{1.0f, 1.0f, 1.0f, 1.0f};
    float shade_color[4]{0.72f, 0.72f, 0.78f, 1.0f};
    float rim_and_shift[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float params[4]{0.85f, 0.5f, 0.0f, 0.0f};
};
static_assert(sizeof(PreviewPushConstants) == 128);

struct BonePushConstants {
    float mvp[16]{};
};
static_assert(sizeof(BonePushConstants) == 64);

struct Mat4RowMajor {
    float m[16]{};
};

Mat4RowMajor mul_mat4(const Mat4RowMajor& a, const Mat4RowMajor& b) noexcept {
    Mat4RowMajor r{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[row * 4 + k] * b.m[k * 4 + col];
            }
            r.m[row * 4 + col] = sum;
        }
    }
    return r;
}

struct Vec3f {
    float x, y, z;
};

Vec3f sub3(Vec3f a, Vec3f b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3f cross3(Vec3f a, Vec3f b) noexcept {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

float dot3(Vec3f a, Vec3f b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3f norm3(Vec3f v) noexcept {
    const float len = std::sqrt(dot3(v, v));
    if (len > 1e-6f) {
        const float inv = 1.0f / len;
        return {v.x * inv, v.y * inv, v.z * inv};
    }
    return {0.0f, 0.0f, 1.0f};
}

Mat4RowMajor look_at_rh(Vec3f eye, Vec3f center, Vec3f up) noexcept {
    const Vec3f f = norm3(sub3(center, eye));
    const Vec3f s = norm3(cross3(f, up));
    const Vec3f u = cross3(s, f);

    Mat4RowMajor v{};
    v.m[0] = s.x;
    v.m[1] = s.y;
    v.m[2] = s.z;
    v.m[3] = -dot3(s, eye);

    v.m[4] = u.x;
    v.m[5] = u.y;
    v.m[6] = u.z;
    v.m[7] = -dot3(u, eye);

    v.m[8] = -f.x;
    v.m[9] = -f.y;
    v.m[10] = -f.z;
    v.m[11] = dot3(f, eye);

    v.m[15] = 1.0f;
    return v;
}

// Right-handed perspective projecting camera -Z into Vulkan depth [0, 1] with +Y up
// (paired with negative viewport height in PreviewPass::record so CCW stays CCW).
Mat4RowMajor perspective_rh_zo(float fovy_rad, float aspect, float z_near, float z_far) noexcept {
    const float f = 1.0f / std::tan(fovy_rad * 0.5f);
    Mat4RowMajor p{};
    p.m[0] = f / std::max(0.05f, aspect);
    p.m[5] = f;
    p.m[10] = z_far / (z_near - z_far);
    p.m[11] = (z_far * z_near) / (z_near - z_far);
    p.m[14] = -1.0f;
    return p;
}

Mat4RowMajor compute_camera_mvp(const PreviewCamera& cam, float aspect) noexcept {
    const float dist = std::clamp(cam.distance, 0.25f, 25.0f);
    const float pitch = std::clamp(cam.pitch_rad, -1.45f, 1.45f);
    const float cp = std::cos(pitch);
    const Vec3f eye{
        cam.target_x + dist * cp * std::sin(cam.yaw_rad),
        cam.target_y + dist * std::sin(pitch),
        cam.target_z + dist * cp * std::cos(cam.yaw_rad),
    };
    const Mat4RowMajor view = look_at_rh(eye, {cam.target_x, cam.target_y, cam.target_z}, {0.0f, 1.0f, 0.0f});
    const Mat4RowMajor proj = perspective_rh_zo(cam.fovy_rad, aspect, 0.05f, 60.0f);
    return mul_mat4(proj, view);
}

template <size_t N> Expected<VkShaderModule> create_shader_module(VkDevice dev, const std::array<uint32_t, N>& words) {
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = words.size() * sizeof(uint32_t);
    smci.pCode = words.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &smci, nullptr, &mod) != VK_SUCCESS) {
        return fail(ErrorCode::VulkanError, "vkCreateShaderModule failed");
    }
    return mod;
}

} // namespace

PreviewCamera fit_camera_to_avatar(const GpuAvatar& avatar) noexcept {
    PreviewCamera cam{};
    const float min_y = avatar.desc.aabb_min[1];
    const float max_y = avatar.desc.aabb_max[1];
    const float min_x = avatar.desc.aabb_min[0];
    const float max_x = avatar.desc.aabb_max[0];
    const float min_z = avatar.desc.aabb_min[2];
    const float max_z = avatar.desc.aabb_max[2];

    const float dx = max_x - min_x;
    const float dy = max_y - min_y;
    const float dz = max_z - min_z;
    const float span = std::max({dy, dx, dz, 0.5f});

    cam.target_x = 0.5f * (min_x + max_x);
    cam.target_y = min_y + dy * 0.56f;
    cam.target_z = 0.5f * (min_z + max_z);
    cam.yaw_rad = 0.28f;
    cam.pitch_rad = 0.06f;
    cam.fovy_rad = 0.2805f; // ~85mm portrait telephoto FOV for authentic 2D Live2D framing
    cam.distance = std::clamp((span * 0.72f) / std::tan(cam.fovy_rad * 0.5f), 0.7f, 24.0f);
    return cam;
}

Expected<void> PreviewPass::init(rhi::RhiDevice& rhi, VkFormat color_format) {
    destroy();
    device_ = &rhi;
    color_format_ = color_format;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 1.0f;
    if (vkCreateSampler(device_->device(), &sci, nullptr, &linear_sampler_) != VK_SUCCESS) {
        destroy();
        return fail(ErrorCode::VulkanError, "vkCreateSampler failed");
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 2;
    dslci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_->device(), &dslci, nullptr, &mat_set_layout_) != VK_SUCCESS) {
        destroy();
        return fail(ErrorCode::VulkanError, "vkCreateDescriptorSetLayout failed");
    }

    return create_pipelines();
}

Expected<void> PreviewPass::create_pipelines() {
    VkDevice dev = device_->device();

    auto mv = create_shader_module(dev, spv::kPreviewMeshVertSpv);
    auto mf = create_shader_module(dev, spv::kPreviewMeshFragSpv);
    auto bv = create_shader_module(dev, spv::kPreviewBonesVertSpv);
    auto bf = create_shader_module(dev, spv::kPreviewBonesFragSpv);
    if (!mv || !mf || !bv || !bf) {
        if (mv)
            vkDestroyShaderModule(dev, mv.value(), nullptr);
        if (mf)
            vkDestroyShaderModule(dev, mf.value(), nullptr);
        if (bv)
            vkDestroyShaderModule(dev, bv.value(), nullptr);
        if (bf)
            vkDestroyShaderModule(dev, bf.value(), nullptr);
        return fail(ErrorCode::VulkanError, "Failed to create shader modules");
    }

    auto destroy_mods = [&]() {
        vkDestroyShaderModule(dev, mv.value(), nullptr);
        vkDestroyShaderModule(dev, mf.value(), nullptr);
        vkDestroyShaderModule(dev, bv.value(), nullptr);
        vkDestroyShaderModule(dev, bf.value(), nullptr);
    };

    VkPushConstantRange mesh_pc{};
    mesh_pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    mesh_pc.offset = 0;
    mesh_pc.size = sizeof(PreviewPushConstants);

    VkPipelineLayoutCreateInfo mplci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    mplci.setLayoutCount = 1;
    mplci.pSetLayouts = &mat_set_layout_;
    mplci.pushConstantRangeCount = 1;
    mplci.pPushConstantRanges = &mesh_pc;
    if (vkCreatePipelineLayout(dev, &mplci, nullptr, &mesh_pipe_layout_) != VK_SUCCESS) {
        destroy_mods();
        return fail(ErrorCode::VulkanError, "vkCreatePipelineLayout(mesh) failed");
    }

    VkPushConstantRange bone_pc{};
    bone_pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bone_pc.offset = 0;
    bone_pc.size = sizeof(BonePushConstants);

    VkPipelineLayoutCreateInfo bplci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    bplci.pushConstantRangeCount = 1;
    bplci.pPushConstantRanges = &bone_pc;
    if (vkCreatePipelineLayout(dev, &bplci, nullptr, &bone_pipe_layout_) != VK_SUCCESS) {
        destroy_mods();
        return fail(ErrorCode::VulkanError, "vkCreatePipelineLayout(bone) failed");
    }

    VkPipelineShaderStageCreateInfo mesh_stages[2]{};
    mesh_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    mesh_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    mesh_stages[0].module = mv.value();
    mesh_stages[0].pName = "main";
    mesh_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    mesh_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    mesh_stages[1].module = mf.value();
    mesh_stages[1].pName = "main";

    VkVertexInputBindingDescription vbinds[2]{};
    vbinds[0].binding = 0;
    vbinds[0].stride = sizeof(AetherVertex25D);
    vbinds[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vbinds[1].binding = 1;
    vbinds[1].stride = sizeof(AetherVertexSurface);
    vbinds[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vattrs[5]{};
    vattrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(AetherVertex25D, position))};
    vattrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(AetherVertex25D, true_normal))};
    vattrs[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(AetherVertex25D, proxy_normal))};
    vattrs[3] = {4, 1, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(AetherVertexSurface, uv0))};
    vattrs[4] = {5, 1, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(AetherVertexSurface, color_rgba8))};

    VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vis.vertexBindingDescriptionCount = 2;
    vis.pVertexBindingDescriptions = vbinds;
    vis.vertexAttributeDescriptionCount = 5;
    vis.pVertexAttributeDescriptions = vattrs;

    VkPipelineInputAssemblyStateCreateInfo ias{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_BACK_BIT;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;

    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyns{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyns.dynamicStateCount = 2;
    dyns.pDynamicStates = dyn_states;

    VkPipelineRenderingCreateInfo dyn_ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    dyn_ri.colorAttachmentCount = 1;
    dyn_ri.pColorAttachmentFormats = &color_format_;
    dyn_ri.depthAttachmentFormat = depth_format_;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.pNext = &dyn_ri;
    gpci.stageCount = 2;
    gpci.pStages = mesh_stages;
    gpci.pVertexInputState = &vis;
    gpci.pInputAssemblyState = &ias;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &rast;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cbs;
    gpci.pDynamicState = &dyns;
    gpci.layout = mesh_pipe_layout_;

    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &mesh_pipe_cull_) != VK_SUCCESS) {
        destroy_mods();
        return fail(ErrorCode::VulkanError, "vkCreateGraphicsPipelines(cull) failed");
    }

    rast.cullMode = VK_CULL_MODE_NONE;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &mesh_pipe_double_sided_) != VK_SUCCESS) {
        destroy_mods();
        return fail(ErrorCode::VulkanError, "vkCreateGraphicsPipelines(double_sided) failed");
    }

    ds.depthWriteEnable = VK_FALSE;
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &mesh_pipe_blend_) != VK_SUCCESS) {
        destroy_mods();
        return fail(ErrorCode::VulkanError, "vkCreateGraphicsPipelines(blend) failed");
    }

    if (device_->gpu_info().fill_mode_non_solid) {
        rast.polygonMode = VK_POLYGON_MODE_LINE;
        rast.cullMode = VK_CULL_MODE_NONE;
        ds.depthWriteEnable = VK_TRUE;
        cba.blendEnable = VK_FALSE;
        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &mesh_pipe_wireframe_) != VK_SUCCESS) {
            destroy_mods();
            return fail(ErrorCode::VulkanError, "vkCreateGraphicsPipelines(wireframe) failed");
        }
    }

    // Bone line overlay pipeline (always visible over mesh, line-list topology)
    VkPipelineShaderStageCreateInfo bone_stages[2]{};
    bone_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    bone_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    bone_stages[0].module = bv.value();
    bone_stages[0].pName = "main";
    bone_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    bone_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    bone_stages[1].module = bf.value();
    bone_stages[1].pName = "main";

    VkVertexInputBindingDescription b_bind{};
    b_bind.binding = 0;
    b_bind.stride = sizeof(BoneLineVertex);
    b_bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription b_attrs[2]{};
    b_attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(BoneLineVertex, pos))};
    b_attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(BoneLineVertex, color))};

    VkPipelineVertexInputStateCreateInfo b_vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    b_vis.vertexBindingDescriptionCount = 1;
    b_vis.pVertexBindingDescriptions = &b_bind;
    b_vis.vertexAttributeDescriptionCount = 2;
    b_vis.pVertexAttributeDescriptions = b_attrs;

    VkPipelineInputAssemblyStateCreateInfo b_ias{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    b_ias.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_NONE;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    cba.blendEnable = VK_TRUE;

    gpci.pStages = bone_stages;
    gpci.pVertexInputState = &b_vis;
    gpci.pInputAssemblyState = &b_ias;
    gpci.layout = bone_pipe_layout_;

    const VkResult b_rc = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &bone_pipe_);
    destroy_mods();
    if (b_rc != VK_SUCCESS) {
        return fail(ErrorCode::VulkanError, "vkCreateGraphicsPipelines(bones) failed");
    }

    return Expected<void>{};
}

void PreviewPass::destroy_targets() noexcept {
    if (device_ == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        device_->destroy_image(color_targets_[i]);
        device_->destroy_image(depth_targets_[i]);
        color_layouts_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    width_ = 0;
    height_ = 0;
}

void PreviewPass::destroy_descriptors() noexcept {
    if (device_ != nullptr && device_->device() != VK_NULL_HANDLE && mat_desc_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_->device(), mat_desc_pool_, nullptr);
        mat_desc_pool_ = VK_NULL_HANDLE;
    }
    mat_desc_sets_.clear();
    mat_fallback_set_ = VK_NULL_HANDLE;
}

void PreviewPass::destroy() noexcept {
    if (device_ != nullptr && device_->device() != VK_NULL_HANDLE) {
        device_->wait_idle();
        destroy_descriptors();
        destroy_targets();
        VkDevice dev = device_->device();
        auto destroy_pipe = [dev](VkPipeline& p) {
            if (p != VK_NULL_HANDLE) {
                vkDestroyPipeline(dev, p, nullptr);
                p = VK_NULL_HANDLE;
            }
        };
        destroy_pipe(mesh_pipe_cull_);
        destroy_pipe(mesh_pipe_double_sided_);
        destroy_pipe(mesh_pipe_blend_);
        destroy_pipe(mesh_pipe_wireframe_);
        destroy_pipe(bone_pipe_);
        if (mesh_pipe_layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(dev, mesh_pipe_layout_, nullptr);
            mesh_pipe_layout_ = VK_NULL_HANDLE;
        }
        if (bone_pipe_layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(dev, bone_pipe_layout_, nullptr);
            bone_pipe_layout_ = VK_NULL_HANDLE;
        }
        if (mat_set_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(dev, mat_set_layout_, nullptr);
            mat_set_layout_ = VK_NULL_HANDLE;
        }
        if (linear_sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(dev, linear_sampler_, nullptr);
            linear_sampler_ = VK_NULL_HANDLE;
        }
    }
    device_ = nullptr;
}

Expected<void> PreviewPass::ensure_size(uint32_t width, uint32_t height, bool& out_recreated) {
    out_recreated = false;
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArg, "PreviewPass not initialized");
    }
    const uint32_t w = std::clamp(width, 64U, 4096U);
    const uint32_t h = std::clamp(height, 64U, 4096U);
    if (w == width_ && h == height_ && color_targets_[0].handle != VK_NULL_HANDLE) {
        return Expected<void>{};
    }

    device_->wait_idle();
    destroy_targets();

    for (uint32_t i = 0; i < kSlotCount; ++i) {
        auto c_res = device_->create_image_2d(w, h, color_format_,
                                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                              VK_IMAGE_ASPECT_COLOR_BIT);
        if (!c_res) {
            destroy_targets();
            return make_unexpected(c_res.error());
        }
        color_targets_[i] = c_res.value();

        auto d_res = device_->create_image_2d(w, h, depth_format_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                              VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!d_res) {
            destroy_targets();
            return make_unexpected(d_res.error());
        }
        depth_targets_[i] = d_res.value();
        color_layouts_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    width_ = w;
    height_ = h;
    out_recreated = true;
    return Expected<void>{};
}

Expected<void> PreviewPass::bind_avatar(const GpuAvatar* avatar) {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArg, "PreviewPass not initialized");
    }
    device_->wait_idle();
    destroy_descriptors();

    if (avatar == nullptr || avatar->fallback_white.view == VK_NULL_HANDLE) {
        return Expected<void>{};
    }

    const uint32_t set_count = static_cast<uint32_t>(avatar->materials.size()) + 1U;
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[0].descriptorCount = set_count;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    pool_sizes[1].descriptorCount = set_count;

    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = set_count;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(device_->device(), &dpci, nullptr, &mat_desc_pool_) != VK_SUCCESS) {
        return fail(ErrorCode::VulkanError, "vkCreateDescriptorPool(preview) failed");
    }

    std::vector<VkDescriptorSetLayout> layouts(set_count, mat_set_layout_);
    std::vector<VkDescriptorSet> sets(set_count, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = mat_desc_pool_;
    dsai.descriptorSetCount = set_count;
    dsai.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_->device(), &dsai, sets.data()) != VK_SUCCESS) {
        destroy_descriptors();
        return fail(ErrorCode::VulkanError, "vkAllocateDescriptorSets(preview) failed");
    }

    auto write_set = [&](VkDescriptorSet dst_set, VkImageView img_view) {
        VkDescriptorImageInfo img_info{};
        img_info.imageView = img_view;
        img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo samp_info{};
        samp_info.sampler = linear_sampler_;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = dst_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[0].pImageInfo = &img_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = dst_set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[1].pImageInfo = &samp_info;

        vkUpdateDescriptorSets(device_->device(), 2, writes, 0, nullptr);
    };

    mat_desc_sets_.resize(avatar->materials.size());
    for (size_t i = 0; i < avatar->materials.size(); ++i) {
        mat_desc_sets_[i] = sets[i];
        const uint32_t tex_idx = avatar->materials[i].tex_base;
        VkImageView view = avatar->fallback_white.view;
        if (tex_idx != AETHER_INVALID_INDEX && tex_idx < avatar->textures.size() &&
            avatar->textures[tex_idx].view != VK_NULL_HANDLE) {
            view = avatar->textures[tex_idx].view;
        }
        write_set(mat_desc_sets_[i], view);
    }

    mat_fallback_set_ = sets.back();
    write_set(mat_fallback_set_, avatar->fallback_white.view);
    return Expected<void>{};
}

void PreviewPass::record(VkCommandBuffer cmd, uint32_t frame_slot, const GpuAvatar* avatar, const PreviewCamera& cam,
                         const PreviewRenderOptions& opts) noexcept {
    if (device_ == nullptr || cmd == VK_NULL_HANDLE || frame_slot >= kSlotCount ||
        color_targets_[frame_slot].handle == VK_NULL_HANDLE) {
        return;
    }

    const VkImageLayout prev_layout = color_layouts_[frame_slot];
    const VkPipelineStageFlags2 src_stage = (prev_layout == VK_IMAGE_LAYOUT_UNDEFINED)
                                                ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                                                : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    const VkAccessFlags2 src_access =
        (prev_layout == VK_IMAGE_LAYOUT_UNDEFINED) ? 0ULL : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

    rhi::RhiDevice::cmd_transition_image(cmd, color_targets_[frame_slot].handle, prev_layout,
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, src_stage, src_access,
                                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    rhi::RhiDevice::cmd_transition_image(
        cmd, depth_targets_[frame_slot].handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo color_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color_att.imageView = color_targets_[frame_slot].view;
    color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(color_att.clearValue.color.float32, opts.clear_rgba, sizeof(float) * 4);

    VkRenderingAttachmentInfo depth_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth_att.imageView = depth_targets_[frame_slot].view;
    depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_att.clearValue.depthStencil = {1.0f, 0U};

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = VkRect2D{{0, 0}, {width_, height_}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color_att;
    ri.pDepthAttachment = &depth_att;
    vkCmdBeginRendering(cmd, &ri);

    // Negative viewport height keeps right-handed Y-up upright in Vulkan image space while keeping CCW front face.
    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = static_cast<float>(height_);
    vp.width = static_cast<float>(width_);
    vp.height = -static_cast<float>(height_);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{{0, 0}, {width_, height_}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const float aspect = static_cast<float>(width_) / static_cast<float>(std::max(1U, height_));
    const Mat4RowMajor mvp = compute_camera_mvp(cam, aspect);

    if (avatar != nullptr && avatar->vertex25d_buf.handle != VK_NULL_HANDLE &&
        avatar->surface_buf.handle != VK_NULL_HANDLE && avatar->index_buf.handle != VK_NULL_HANDLE &&
        !avatar->submeshes.empty() && mat_fallback_set_ != VK_NULL_HANDLE) {

        const VkBuffer active_v25_buf =
            (frame_slot < GpuAvatar::kPoseSlotCount && avatar->slot_applied_pose_seq[frame_slot] != 0ULL &&
             avatar->dynamic_vertex25d_bufs[frame_slot].handle != VK_NULL_HANDLE)
                ? avatar->dynamic_vertex25d_bufs[frame_slot].handle
                : avatar->vertex25d_buf.handle;
        const VkBuffer vbufs[2] = {active_v25_buf, avatar->surface_buf.handle};
        const VkDeviceSize voffs[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, vbufs, voffs);
        vkCmdBindIndexBuffer(cmd, avatar->index_buf.handle, 0, VK_INDEX_TYPE_UINT32);

        const bool use_wire = (opts.wireframe && mesh_pipe_wireframe_ != VK_NULL_HANDLE);

        for (int pass = 0; pass < 2; ++pass) {
            const bool want_blend = (pass == 1);
            for (const AetherSubmeshDesc& sm : avatar->submeshes) {
                if (sm.index_count == 0) {
                    continue;
                }
                const bool has_mat = (sm.material_index < avatar->materials.size());
                const bool is_blend = has_mat
                                          ? (avatar->materials[sm.material_index].alpha_mode == AETHER_ALPHA_MODE_BLEND)
                                          : ((sm.flags & AETHER_SUBMESH_FLAG_TRANSPARENT) != 0U);
                if (is_blend != want_blend) {
                    continue;
                }

                VkPipeline pipe = mesh_pipe_cull_;
                if (use_wire) {
                    pipe = mesh_pipe_wireframe_;
                } else if (want_blend) {
                    pipe = mesh_pipe_blend_;
                } else if (has_mat &&
                           (avatar->materials[sm.material_index].flags & AETHER_MAT_FLAG_DOUBLE_SIDED) != 0U) {
                    pipe = mesh_pipe_double_sided_;
                }

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                VkDescriptorSet dset =
                    (sm.material_index < mat_desc_sets_.size()) ? mat_desc_sets_[sm.material_index] : mat_fallback_set_;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipe_layout_, 0, 1, &dset, 0,
                                        nullptr);

                PreviewPushConstants pc{};
                std::memcpy(pc.mvp, mvp.m, sizeof(mvp.m));
                if (has_mat) {
                    const AetherNprMaterial& m = avatar->materials[sm.material_index];
                    std::memcpy(pc.base_color, m.base_color, sizeof(float) * 4);
                    std::memcpy(pc.shade_color, m.shade_color_gi, sizeof(float) * 4);
                    pc.rim_and_shift[0] = m.rim_color_mix[0];
                    pc.rim_and_shift[1] = m.rim_color_mix[1];
                    pc.rim_and_shift[2] = m.rim_color_mix[2];
                    pc.rim_and_shift[3] = m.toon_params[0]; // shading_shift
                    pc.params[0] = m.toon_params[1];        // shading_toony
                    pc.params[1] = m.uv_outline_alpha[3];   // alpha_cutoff
                    pc.params[2] = (m.alpha_mode == AETHER_ALPHA_MODE_MASK) ? 1.0f : 0.0f;
                    pc.params[3] = ((m.flags & AETHER_MAT_FLAG_UNLIT) != 0U) ? 1.0f : 0.0f;
                }
                vkCmdPushConstants(cmd, mesh_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(pc), &pc);
                vkCmdDrawIndexed(cmd, sm.index_count, 1, sm.index_offset, 0, 0);
            }
        }

        if (opts.show_bones && avatar->bone_line_vertex_count > 0 && avatar->bone_line_buf.handle != VK_NULL_HANDLE) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bone_pipe_);
            const VkBuffer active_bone_buf =
                (frame_slot < GpuAvatar::kPoseSlotCount && avatar->slot_applied_pose_seq[frame_slot] != 0ULL &&
                 avatar->dynamic_bone_line_bufs[frame_slot].handle != VK_NULL_HANDLE)
                    ? avatar->dynamic_bone_line_bufs[frame_slot].handle
                    : avatar->bone_line_buf.handle;
            const VkDeviceSize boff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &active_bone_buf, &boff);
            BonePushConstants bpc{};
            std::memcpy(bpc.mvp, mvp.m, sizeof(mvp.m));
            vkCmdPushConstants(cmd, bone_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(bpc), &bpc);
            vkCmdDraw(cmd, avatar->bone_line_vertex_count, 1, 0, 0);
        }
    }

    vkCmdEndRendering(cmd);

    rhi::RhiDevice::cmd_transition_image(
        cmd, color_targets_[frame_slot].handle, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    color_layouts_[frame_slot] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

} // namespace aether::renderer
