#include "renderer/frame_graph.hpp"

#include "animation/spring_bone.hpp"

#include "core/clock.hpp"
#include "core/log.hpp"
#include "generated/phase4_shaders_spv.hpp"
#include "renderer/frame_globals_builder.hpp"
#include "renderer/sdf_generator.hpp"
#include "renderer/transparent_pass.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace aether::renderer {
namespace {

using detail::build_camera_matrices;
using detail::build_csm_ortho_matrix;
using detail::cross3;
using detail::dot3;
using detail::kDegToRad;
using detail::kPi;
using detail::Mat4Vec3;
using detail::mul_mat4;
using detail::normalize3;
using detail::studio_light_dir_to_light;

inline void bind_pipeline_checked(VkCommandBuffer cmd, VkPipelineBindPoint bp, VkPipeline pso,
                                  const char* tag) noexcept {
    if (pso == VK_NULL_HANDLE) {
        AETHER_LOG_ERROR("renderer", "VK_NULL_HANDLE pipeline at %s", tag);
        return;
    }
    vkCmdBindPipeline(cmd, bp, pso);
}

[[nodiscard]] float half_to_float(uint16_t h) noexcept {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16u;
    const uint32_t exp = (h >> 10u) & 0x1Fu;
    uint32_t mant = h & 0x03FFu;
    uint32_t bits = 0;
    if (exp == 0u) {
        if (mant == 0u) {
            bits = sign;
        } else {
            uint32_t e = 127u - 15u + 1u;
            while ((mant & 0x0400u) == 0u) {
                mant <<= 1u;
                --e;
            }
            mant &= 0x03FFu;
            bits = sign | (e << 23u) | (mant << 13u);
        }
    } else if (exp == 31u) {
        bits = sign | 0x7F800000u | (mant << 13u);
    } else {
        bits = sign | ((exp + (127u - 15u)) << 23u) | (mant << 13u);
    }
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

[[nodiscard]] Expected<VkShaderModule> create_spv_module(const rhi::RhiDevice& dev, shaders::Phase4ShaderId id,
                                                         bool allow_disk_reload) {
    const auto words = shaders::load_phase4_shader_words(id, allow_disk_reload);
    if (words.size() < 5) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "Empty Phase4 SPIR-V shader words")};
    }
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = words.size() * sizeof(uint32_t);
    smci.pCode = words.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device(), &smci, nullptr, &mod) != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkCreateShaderModule failed")};
    }
    return mod;
}

void cmd_buffer_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                        VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) noexcept {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = src_stage;
    mb.srcAccessMask = src_access;
    mb.dstStageMask = dst_stage;
    mb.dstAccessMask = dst_access;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void cmd_image_copy_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                            VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                            VkAccessFlags2 dst_access, VkImageAspectFlags aspect) noexcept {
    if (image == VK_NULL_HANDLE) {
        return;
    }
    rhi::RhiDevice::cmd_transition_image(cmd, image, old_layout, new_layout, src_stage, src_access, dst_stage,
                                         dst_access, aspect);
}

} // namespace

Expected<void> FrameGraph::init(rhi::RhiDevice& device, const RendererConfig& cfg) {
    destroy();
    if (device.device() == VK_NULL_HANDLE) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "FrameGraph::init null RhiDevice")};
    }
    device_ = &device;
    cfg_ = cfg;
    cfg_.width = std::max(cfg_.width, 16u);
    cfg_.height = std::max(cfg_.height, 16u);
    heatmap_scratch_.resize(4096u);
    stage_overlay_scratch_.resize(4096u);

    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(device_->physical_device(), VK_FORMAT_B8G8R8A8_UNORM, &fp);
    bgra8_supports_storage_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;

    rhi::BindlessConfig bcfg{};
    bcfg.enable_rt_set = device_->rt_supported();
    if (auto res = bindless_.init(*device_, bcfg); !res) {
        destroy();
        return res;
    }
    if (auto res = rebar_.init(*device_); !res) {
        destroy();
        return res;
    }
    if (auto res = timing_.init(*device_); !res) {
        destroy();
        return res;
    }
    if (device_->rt_supported()) {
        if (auto res = accel_.init(*device_); !res) {
            destroy();
            return res;
        }
    }
    if (auto res =
            denoiser_.init(*device_, bindless_, effective_output_width(), effective_output_height(), cfg_.denoiser);
        !res) {
        destroy();
        return res;
    }
    if (auto res = create_static_targets(); !res) {
        destroy();
        return res;
    }
    if (auto res = create_size_dependent_targets(effective_output_width(), effective_output_height()); !res) {
        destroy();
        return res;
    }
    if (auto res = create_pipelines(false); !res) {
        destroy();
        return res;
    }

    // Seed disk mtime tracking for Debug hot-reload polling
    for (size_t i = 0; i < shaders::kPhase4Shaders.size(); ++i) {
        std::error_code ec;
        const auto p = std::filesystem::path(shaders::kPhase4DiskShaderDir) / shaders::kPhase4Shaders[i].spv_filename;
        const auto t = std::filesystem::last_write_time(p, ec);
        shader_mtimes_[i] = (!ec) ? t.time_since_epoch().count() : 0;
    }
    return {};
}

Expected<void> FrameGraph::resize(uint32_t viewport_w, uint32_t viewport_h, uint32_t output_w, uint32_t output_h) {
    if (device_ == nullptr || device_->device() == VK_NULL_HANDLE) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "FrameGraph::resize uninitialized")};
    }
    const uint32_t prev_rw = effective_output_width();
    const uint32_t prev_rh = effective_output_height();
    const uint32_t vw = std::max(viewport_w, 16u);
    const uint32_t vh = std::max(viewport_h, 16u);
    const uint32_t ow = (output_w == UINT32_MAX) ? cfg_.output_width : ((output_w > 0) ? std::max(output_w, 16u) : 0u);
    const uint32_t oh = (output_h == UINT32_MAX) ? cfg_.output_height : ((output_h > 0) ? std::max(output_h, 16u) : 0u);
    if (vw == cfg_.width && vh == cfg_.height && ow == cfg_.output_width && oh == cfg_.output_height) {
        return {};
    }
    device_->wait_idle();
    cfg_.width = vw;
    cfg_.height = vh;
    cfg_.output_width = ow;
    cfg_.output_height = oh;
    const uint32_t rw = effective_output_width();
    const uint32_t rh = effective_output_height();
    auto rebuild_vp_target = [&](RegisteredImage& target, uint32_t tw, uint32_t th) -> Expected<void> {
        if (target.sampled_slot != UINT32_MAX) {
            bindless_.release_sampled_image(target.sampled_slot);
            target.sampled_slot = UINT32_MAX;
        }
        device_->destroy_image(target.img);
        target.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto img_res = device_->create_image_2d(tw, th, VK_FORMAT_B8G8R8A8_UNORM,
                                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                                VK_IMAGE_ASPECT_COLOR_BIT);
        if (!img_res) {
            return Unexpected<Error>{img_res.error()};
        }
        target.img = img_res.value();
        auto slot_res = bindless_.register_sampled_image(target.img.view);
        if (!slot_res) {
            return Unexpected<Error>{slot_res.error()};
        }
        target.sampled_slot = slot_res.value();
        return {};
    };
    const uint32_t q_w = std::max(vw / 2u, 16u);
    const uint32_t q_h = std::max(vh / 2u, 16u);
    if (rw == prev_rw && rh == prev_rh) {
        if (auto r = rebuild_vp_target(viewport_out_, vw, vh); !r)
            return r;
        if (auto r = rebuild_vp_target(stage_viewport_out_, vw, vh); !r)
            return r;
        if (auto r = rebuild_vp_target(quad_top_out_, q_w, q_h); !r)
            return r;
        if (auto r = rebuild_vp_target(quad_right_out_, q_w, q_h); !r)
            return r;
        stage_rt_w_ = vw;
        stage_rt_h_ = vh;
        stage_active_w_ = vw;
        stage_active_h_ = vh;
        quad_top_rt_w_ = q_w;
        quad_top_rt_h_ = q_h;
        quad_top_active_w_ = q_w;
        quad_top_active_h_ = q_h;
        quad_right_rt_w_ = q_w;
        quad_right_rt_h_ = q_h;
        quad_right_active_w_ = q_w;
        quad_right_active_h_ = q_h;
        return {};
    }
    if (auto res = create_size_dependent_targets(rw, rh); !res) {
        return res;
    }
    if (auto res = denoiser_.resize(rw, rh); !res) {
        return res;
    }
    for (auto& hb : history_) {
        hb.have_prev_view_proj = false;
    }
    return {};
}

Expected<void> FrameGraph::ensure_stage_target_sizes(uint32_t stage_w, uint32_t stage_h, uint32_t qtop_w,
                                                     uint32_t qtop_h, uint32_t qright_w, uint32_t qright_h,
                                                     bool& out_rebuilt) {
    out_rebuilt = false;
    if (device_ == nullptr || device_->device() == VK_NULL_HANDLE) {
        return Unexpected<Error>{
            Error::make(ErrorCode::InvalidArg, "FrameGraph::ensure_stage_target_sizes uninitialized")};
    }
    const uint32_t sw = std::clamp(stage_w, 16u, 4096u);
    const uint32_t sh = std::clamp(stage_h, 16u, 4096u);
    const uint32_t tw = std::clamp(qtop_w, 16u, 4096u);
    const uint32_t th = std::clamp(qtop_h, 16u, 4096u);
    const uint32_t rw = std::clamp(qright_w, 16u, 4096u);
    const uint32_t rh = std::clamp(qright_h, 16u, 4096u);

    const bool need_stage =
        (stage_viewport_out_.img.handle == VK_NULL_HANDLE) || (sw > stage_rt_w_) || (sh > stage_rt_h_);
    const bool need_qtop =
        (quad_top_out_.img.handle == VK_NULL_HANDLE) || (tw > quad_top_rt_w_) || (th > quad_top_rt_h_);
    const bool need_qright =
        (quad_right_out_.img.handle == VK_NULL_HANDLE) || (rw > quad_right_rt_w_) || (rh > quad_right_rt_h_);

    if (!need_stage && !need_qtop && !need_qright) {
        stage_active_w_ = sw;
        stage_active_h_ = sh;
        quad_top_active_w_ = tw;
        quad_top_active_h_ = th;
        quad_right_active_w_ = rw;
        quad_right_active_h_ = rh;
        return {};
    }

    device_->wait_idle();
    auto rebuild = [&](RegisteredImage& target, uint32_t nw, uint32_t nh) -> Expected<void> {
        if (target.sampled_slot != UINT32_MAX) {
            bindless_.release_sampled_image(target.sampled_slot);
            target.sampled_slot = UINT32_MAX;
        }
        device_->destroy_image(target.img);
        target.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto img_res = device_->create_image_2d(nw, nh, VK_FORMAT_B8G8R8A8_UNORM,
                                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                                VK_IMAGE_ASPECT_COLOR_BIT);
        if (!img_res) {
            return Unexpected<Error>{img_res.error()};
        }
        target.img = img_res.value();
        auto slot_res = bindless_.register_sampled_image(target.img.view);
        if (!slot_res) {
            return Unexpected<Error>{slot_res.error()};
        }
        target.sampled_slot = slot_res.value();
        return {};
    };
    auto cap_dim = [](uint32_t need, uint32_t cur) noexcept -> uint32_t {
        const uint32_t m = std::max(need, cur);
        return std::min(4096u, ((m + 63u) / 64u) * 64u);
    };

    if (need_stage) {
        const uint32_t nw = cap_dim(sw, stage_rt_w_);
        const uint32_t nh = cap_dim(sh, stage_rt_h_);
        if (auto r = rebuild(stage_viewport_out_, nw, nh); !r)
            return r;
        stage_rt_w_ = nw;
        stage_rt_h_ = nh;
        out_rebuilt = true;
    }
    if (need_qtop) {
        const uint32_t nw = cap_dim(tw, quad_top_rt_w_);
        const uint32_t nh = cap_dim(th, quad_top_rt_h_);
        if (auto r = rebuild(quad_top_out_, nw, nh); !r)
            return r;
        quad_top_rt_w_ = nw;
        quad_top_rt_h_ = nh;
        out_rebuilt = true;
    }
    if (need_qright) {
        const uint32_t nw = cap_dim(rw, quad_right_rt_w_);
        const uint32_t nh = cap_dim(rh, quad_right_rt_h_);
        if (auto r = rebuild(quad_right_out_, nw, nh); !r)
            return r;
        quad_right_rt_w_ = nw;
        quad_right_rt_h_ = nh;
        out_rebuilt = true;
    }
    stage_active_w_ = sw;
    stage_active_h_ = sh;
    quad_top_active_w_ = tw;
    quad_top_active_h_ = th;
    quad_right_active_w_ = rw;
    quad_right_active_h_ = rh;
    return {};
}

void FrameGraph::destroy() noexcept {
    if (device_ != nullptr && device_->device() != VK_NULL_HANDLE) {
        if (!device_->is_device_lost()) {
            device_->wait_idle();
        }
    }

    destroy_pipelines();
    destroy_size_dependent_targets();
    destroy_static_targets();

    if (device_ != nullptr) {
        device_->destroy_buffer(skinned_vertices_buf_);
        device_->destroy_buffer(material_buf_);
        device_->destroy_buffer(material_grade_buf_);
        device_->destroy_buffer(submesh_draw_buf_);
        for (const uint32_t s : avatar_tex_slots_) {
            if (s != UINT32_MAX && s != fallback_white_slot_) {
                bindless_.release_sampled_image(s);
            }
        }
        if (baked_face_sdf_slot_ != UINT32_MAX) {
            bindless_.release_sampled_image(baked_face_sdf_slot_);
            baked_face_sdf_slot_ = UINT32_MAX;
        }
        device_->destroy_image(baked_face_sdf_img_);
        if (fallback_white_slot_ != UINT32_MAX) {
            bindless_.release_sampled_image(fallback_white_slot_);
            fallback_white_slot_ = UINT32_MAX;
        }
        device_->destroy_image(fallback_white_tex_);
    }

    avatar_tex_slots_.clear();
    cached_submeshes_.clear();
    cached_materials_.clear();
    opaque_draw_indices_.clear();
    cutout_draw_indices_.clear();
    transparent_draw_indices_.clear();
    avatar_face_sdf_slot_ = UINT32_MAX;
    bound_avatar_ = nullptr;
    accel_.destroy_avatar_structures();
    invalidate_temporal_history();
    last_rt_avatar_ = nullptr;
    last_rt_pose_hash_ = 0;
    last_rt_as_update_us_ = 0;

    denoiser_.destroy();
    accel_.destroy();
    timing_.destroy();
    rebar_.destroy();
    bindless_.destroy();
    device_ = nullptr;
}

Expected<void> FrameGraph::create_static_targets() {
    if (device_ == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "null device")};
    }

    // 1x1 white fallback texture initialized to 0xFFFFFFFF in SHADER_READ_ONLY_OPTIMAL
    if (fallback_white_tex_.handle == VK_NULL_HANDLE) {
        auto fw_res = device_->create_image_2d(1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                               VK_IMAGE_ASPECT_COLOR_BIT);
        if (!fw_res) {
            return Unexpected<Error>{fw_res.error()};
        }
        fallback_white_tex_ = fw_res.value();

        auto stg_res = device_->create_buffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                                              VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                  VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!stg_res) {
            return Unexpected<Error>{stg_res.error()};
        }
        rhi::RhiBuffer stg = stg_res.value();
        static constexpr uint8_t kWhite[4] = {255, 255, 255, 255};
        std::memcpy(stg.info.pMappedData, kWhite, 4);
        vmaFlushAllocation(device_->allocator(), stg.allocation, 0, 4);

        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = device_->graphics_family();
        cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkCreateCommandPool(device_->device(), &cpci, nullptr, &pool);

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device_->device(), &cbai, &cmd);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        cmd_image_copy_barrier(cmd, fallback_white_tex_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy reg{};
        reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reg.imageExtent = {1, 1, 1};
        vkCmdCopyBufferToImage(cmd, stg.handle, fallback_white_tex_.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &reg);
        cmd_image_copy_barrier(cmd, fallback_white_tex_.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(device_->graphics_queue(), 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(device_->graphics_queue());
        vkDestroyCommandPool(device_->device(), pool, nullptr);
        device_->destroy_buffer(stg);

        auto slot_res = bindless_.register_sampled_image(fallback_white_tex_.view);
        if (!slot_res) {
            return Unexpected<Error>{slot_res.error()};
        }
        fallback_white_slot_ = slot_res.value();
    }

    // 2048x2048 CSM depth target
    {
        auto res = device_->create_image_2d(kCsmSize, kCsmSize, VK_FORMAT_D32_SFLOAT,
                                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                            VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!res) {
            return Unexpected<Error>{res.error()};
        }
        csm_depth_.img = res.value();
        csm_depth_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto s = bindless_.register_sampled_image(csm_depth_.img.view);
        if (!s) {
            return Unexpected<Error>{s.error()};
        }
        csm_depth_.sampled_slot = s.value();
    }

    // Heitz (2016) 64x64 LTC LUTs & 256x64 ANSI/IES LM-63-19 LUT (uploaded once at init)
    auto upload_static_tex = [&](RegisteredImage& dst, uint32_t tw, uint32_t th, VkFormat fmt, const void* src_bytes,
                                 size_t byte_len) -> Expected<void> {
        if (dst.img.handle != VK_NULL_HANDLE) {
            return {};
        }
        auto ir = device_->create_image_2d(tw, th, fmt, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                           VK_IMAGE_ASPECT_COLOR_BIT);
        if (!ir) {
            return Unexpected<Error>{ir.error()};
        }
        dst.img = ir.value();

        auto stg_res = device_->create_buffer(byte_len, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                                              VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                  VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!stg_res) {
            return Unexpected<Error>{stg_res.error()};
        }
        rhi::RhiBuffer stg = stg_res.value();
        std::memcpy(stg.info.pMappedData, src_bytes, byte_len);
        vmaFlushAllocation(device_->allocator(), stg.allocation, 0, byte_len);

        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = device_->graphics_family();
        cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkCreateCommandPool(device_->device(), &cpci, nullptr, &pool);

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device_->device(), &cbai, &cmd);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        cmd_image_copy_barrier(cmd, dst.img.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy reg{};
        reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reg.imageExtent = {tw, th, 1};
        vkCmdCopyBufferToImage(cmd, stg.handle, dst.img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
        cmd_image_copy_barrier(cmd, dst.img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(device_->graphics_queue(), 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(device_->graphics_queue());
        vkDestroyCommandPool(device_->device(), pool, nullptr);
        device_->destroy_buffer(stg);

        dst.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        auto s = bindless_.register_sampled_image(dst.img.view);
        if (!s) {
            return Unexpected<Error>{s.error()};
        }
        dst.sampled_slot = s.value();
        return {};
    };

    if (ltc_inv_m_.img.handle == VK_NULL_HANDLE || ltc_amp_.img.handle == VK_NULL_HANDLE) {
        std::array<float, static_cast<size_t>(kLtcLutSize) * kLtcLutSize * 4> inv_m{};
        std::array<float, static_cast<size_t>(kLtcLutSize) * kLtcLutSize * 4> amp{};
        generate_heitz_ltc_luts(inv_m, amp);
        if (auto r = upload_static_tex(ltc_inv_m_, kLtcLutSize, kLtcLutSize, VK_FORMAT_R32G32B32A32_SFLOAT,
                                       inv_m.data(), inv_m.size() * sizeof(float));
            !r) {
            return r;
        }
        if (auto r = upload_static_tex(ltc_amp_, kLtcLutSize, kLtcLutSize, VK_FORMAT_R32G32B32A32_SFLOAT, amp.data(),
                                       amp.size() * sizeof(float));
            !r) {
            return r;
        }
    }
    if (ies_lut_.img.handle == VK_NULL_HANDLE) {
        std::array<float, static_cast<size_t>(kIesLutWidth) * kIesLutRows> ies{};
        generate_ies_lm63_lut(ies);
        if (auto r = upload_static_tex(ies_lut_, kIesLutWidth, kIesLutRows, VK_FORMAT_R32_SFLOAT, ies.data(),
                                       ies.size() * sizeof(float));
            !r) {
            return r;
        }
    }
    return {};
}

void FrameGraph::destroy_static_targets() noexcept {
    if (device_ == nullptr) {
        return;
    }
    auto drop = [&](RegisteredImage& r) {
        if (r.sampled_slot != UINT32_MAX) {
            bindless_.release_sampled_image(r.sampled_slot);
            r.sampled_slot = UINT32_MAX;
        }
        if (r.storage_slot != UINT32_MAX) {
            bindless_.release_storage_image(r.storage_slot);
            r.storage_slot = UINT32_MAX;
        }
        if (r.uint_slot != UINT32_MAX) {
            bindless_.release_uint_sampled_image(r.uint_slot);
            r.uint_slot = UINT32_MAX;
        }
        device_->destroy_image(r.img);
        r.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    drop(csm_depth_);
    drop(blue_noise_64_);
    drop(ltc_inv_m_);
    drop(ltc_amp_);
    drop(ies_lut_);
}

Expected<void> FrameGraph::create_size_dependent_targets(uint32_t w, uint32_t h) {
    destroy_size_dependent_targets();

    auto set_img_name = [&, pfn = device_->validation_active()
                                      ? reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
                                            vkGetInstanceProcAddr(device_->instance(), "vkSetDebugUtilsObjectNameEXT"))
                                      : nullptr](VkImage img, const char* nm) {
        if (pfn != nullptr && img != VK_NULL_HANDLE && nm != nullptr) {
            VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            ni.objectType = VK_OBJECT_TYPE_IMAGE;
            ni.objectHandle = reinterpret_cast<uint64_t>(img);
            ni.pObjectName = nm;
            (void)pfn(device_->device(), &ni);
        }
    };

    auto make_reg = [&](RegisteredImage& dst, uint32_t rw, uint32_t rh, VkFormat fmt, VkImageUsageFlags usage,
                        bool sampled, bool storage, bool uint_sampled,
                        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, bool concurrent_transfer = false,
                        const char* dbg_name = nullptr) -> Expected<void> {
        auto img_res = device_->create_image_2d(rw, rh, fmt, usage, aspect, concurrent_transfer);
        if (!img_res) {
            return Unexpected<Error>{img_res.error()};
        }
        dst.img = img_res.value();
        dst.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        set_img_name(dst.img.handle, dbg_name);
        if (sampled) {
            auto s = bindless_.register_sampled_image(dst.img.view);
            if (!s) {
                return Unexpected<Error>{s.error()};
            }
            dst.sampled_slot = s.value();
        }
        if (storage) {
            auto s = bindless_.register_storage_image(dst.img.view);
            if (!s) {
                return Unexpected<Error>{s.error()};
            }
            dst.storage_slot = s.value();
        }
        if (uint_sampled) {
            auto s = bindless_.register_uint_sampled_image(dst.img.view);
            if (!s) {
                return Unexpected<Error>{s.error()};
            }
            dst.uint_slot = s.value();
        }
        return {};
    };

    const uint32_t hw = std::max(w / 2u, 1u);
    const uint32_t hh = std::max(h / 2u, 1u);
    const VkImageUsageFlags att_sampled =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageUsageFlags stor_sampled = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (auto r = make_reg(gb_n_, w, h, VK_FORMAT_R16G16_UNORM, att_sampled, true, false, false); !r)
        return r;
    if (auto r = make_reg(gb_mv_, w, h, VK_FORMAT_R16G16_SFLOAT, att_sampled, true, false, false); !r)
        return r;
    if (auto r = make_reg(gb_mat_, w, h, VK_FORMAT_R32_UINT, att_sampled, false, false, true); !r)
        return r;
    if (auto r = make_reg(gb_misc_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, att_sampled, true, false, false); !r)
        return r;

    // D32_SFLOAT_S8_UINT + separate Depth and Stencil read-only bindless views (§10.4)
    {
        auto ds_res = device_->create_image_2d(w, h, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        if (!ds_res) {
            return Unexpected<Error>{ds_res.error()};
        }
        depth_stencil_ = ds_res.value();
        depth_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

        auto dv =
            device_->create_image_view(depth_stencil_.handle, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!dv) {
            return Unexpected<Error>{dv.error()};
        }
        depth_only_view_ = dv.value();

        auto sv = device_->create_image_view(depth_stencil_.handle, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                             VK_IMAGE_ASPECT_STENCIL_BIT);
        if (!sv) {
            return Unexpected<Error>{sv.error()};
        }
        stencil_only_view_ = sv.value();

        auto ds_slot =
            bindless_.register_sampled_image(depth_only_view_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        if (!ds_slot) {
            return Unexpected<Error>{ds_slot.error()};
        }
        depth_sampled_slot_ = ds_slot.value();

        auto ss_slot =
            bindless_.register_uint_sampled_image(stencil_only_view_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        if (!ss_slot) {
            return Unexpected<Error>{ss_slot.error()};
        }
        stencil_uint_slot_ = ss_slot.value();
    }

    if (auto r = make_reg(rt_shadow_raw_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, stor_sampled, true, true, false); !r)
        return r;
    if (auto r = make_reg(rt_shadow_denoised_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, stor_sampled, true, true, false);
        !r)
        return r;
    if (auto r = make_reg(rim_occ_raw_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, stor_sampled, true, true, false); !r)
        return r;
    if (auto r = make_reg(rim_occ_denoised_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, stor_sampled, true, true, false); !r)
        return r;
    if (auto r = make_reg(bounce_half_, hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, stor_sampled, true, true, false); !r)
        return r;
    if (auto r = make_reg(bounce_denoised_, hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, stor_sampled, true, true, false); !r)
        return r;
    if (auto r = make_reg(aov_backscatter_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, stor_sampled, true, true, false); !r)
        return r;
    if (auto r = make_reg(aov_cornea_spec_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, stor_sampled, true, true, false); !r)
        return r;
    {
        const VkDeviceSize res_bytes =
            static_cast<VkDeviceSize>(hw) * static_cast<VkDeviceSize>(hh) * sizeof(GpuReSTIRReservoir);
        for (auto& hb : history_) {
            for (auto& rb : hb.reservoir_bufs) {
                auto bres = device_->create_buffer(
                    res_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
                if (!bres) {
                    return Unexpected<Error>{bres.error()};
                }
                rb = bres.value();
            }
        }
    }
    if (auto r = make_reg(crease_mask_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, stor_sampled, true, true, false); !r)
        return r;
    if (auto r = make_reg(hdr_scene_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, att_sampled, true, false, false); !r)
        return r;

    uint32_t mw = std::max(w / 2u, 1u);
    uint32_t mh = std::max(h / 2u, 1u);
    for (uint32_t i = 0; i < kBloomMipCount; ++i) {
        if (auto r = make_reg(bloom_mips_[i], mw, mh, VK_FORMAT_R16G16B16A16_SFLOAT, stor_sampled, true, true, false);
            !r) {
            return r;
        }
        mw = std::max(mw / 2u, 1u);
        mh = std::max(mh / 2u, 1u);
    }

    if (auto r = make_reg(out_rgba16f_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                          stor_sampled | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true, true, false);
        !r) {
        return r;
    }

    if (!bgra8_supports_storage_) {
        if (auto r = make_reg(out_rgba8_scratch_, w, h, VK_FORMAT_R8G8B8A8_UNORM, stor_sampled, false, true, false); !r)
            return r;
    }
    const VkImageUsageFlags bgra_ring_usage =
        bgra8_supports_storage_
            ? (stor_sampled | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            : (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    for (uint32_t s = 0; s < kOutputRingSlots; ++s) {
        auto& slot = out_ring_[s];
        if (auto r = make_reg(slot.bgra8, w, h, VK_FORMAT_B8G8R8A8_UNORM, bgra_ring_usage, true,
                              bgra8_supports_storage_, false, VK_IMAGE_ASPECT_COLOR_BIT, true, "out_ring_bgra8");
            !r) {
            return r;
        }
        if (auto r = make_reg(slot.bgra8_straight, w, h, VK_FORMAT_B8G8R8A8_UNORM, bgra_ring_usage, true,
                              bgra8_supports_storage_, false, VK_IMAGE_ASPECT_COLOR_BIT, true, "out_ring_straight");
            !r) {
            return r;
        }
        if (auto r = make_reg(slot.bottom_up_bgra8, w, h, VK_FORMAT_B8G8R8A8_UNORM, bgra_ring_usage, true,
                              bgra8_supports_storage_, false, VK_IMAGE_ASPECT_COLOR_BIT, true, "out_ring_bottomup");
            !r) {
            return r;
        }
        if (auto r = make_reg(slot.nv12_y, w, h, VK_FORMAT_R8_UNORM, stor_sampled, false, true, false,
                              VK_IMAGE_ASPECT_COLOR_BIT, true, "out_ring_nv12_y");
            !r)
            return r;
        if (auto r = make_reg(slot.nv12_uv, std::max(w / 2u, 1u), std::max(h / 2u, 1u), VK_FORMAT_R8G8_UNORM,
                              stor_sampled, false, true, false, VK_IMAGE_ASPECT_COLOR_BIT, true, "out_ring_nv12_uv");
            !r) {
            return r;
        }
    }

    const uint32_t vw = std::max(cfg_.width, 16u);
    const uint32_t vh = std::max(cfg_.height, 16u);
    const uint32_t qw = std::max(vw / 2u, 16u);
    const uint32_t qh = std::max(vh / 2u, 16u);
    if (auto r = make_reg(viewport_out_, vw, vh, VK_FORMAT_B8G8R8A8_UNORM, att_sampled, true, false, false,
                          VK_IMAGE_ASPECT_COLOR_BIT, false, "viewport_out");
        !r)
        return r;
    if (auto r = make_reg(stage_viewport_out_, vw, vh, VK_FORMAT_B8G8R8A8_UNORM, att_sampled, true, false, false,
                          VK_IMAGE_ASPECT_COLOR_BIT, false, "stage_viewport_out");
        !r)
        return r;
    if (auto r = make_reg(quad_top_out_, qw, qh, VK_FORMAT_B8G8R8A8_UNORM, att_sampled, true, false, false,
                          VK_IMAGE_ASPECT_COLOR_BIT, false, "quad_top_out");
        !r)
        return r;
    if (auto r = make_reg(quad_right_out_, qw, qh, VK_FORMAT_B8G8R8A8_UNORM, att_sampled, true, false, false,
                          VK_IMAGE_ASPECT_COLOR_BIT, false, "quad_right_out");
        !r)
        return r;
    stage_rt_w_ = vw;
    stage_rt_h_ = vh;
    stage_active_w_ = vw;
    stage_active_h_ = vh;
    quad_top_rt_w_ = qw;
    quad_top_rt_h_ = qh;
    quad_top_active_w_ = qw;
    quad_top_active_h_ = qh;
    quad_right_rt_w_ = qw;
    quad_right_rt_h_ = qh;
    quad_right_active_w_ = qw;
    quad_right_active_h_ = qh;

    const float pip_scale = std::min({1.0f, 640.0f / static_cast<float>(w), 360.0f / static_cast<float>(h)});
    const uint32_t pip_w = std::max(16u, static_cast<uint32_t>(std::lround(static_cast<float>(w) * pip_scale)));
    const uint32_t pip_h = std::max(16u, static_cast<uint32_t>(std::lround(static_cast<float>(h) * pip_scale)));
    if (auto r = make_reg(stage_broadcast_pip_out_, pip_w, pip_h, VK_FORMAT_B8G8R8A8_UNORM, att_sampled, true, false,
                          false, VK_IMAGE_ASPECT_COLOR_BIT, false, "stage_broadcast_pip_out");
        !r)
        return r;
    stage_pip_w_ = pip_w;
    stage_pip_h_ = pip_h;
    prev_stage_scene_w_ = 0;
    prev_stage_scene_h_ = 0;

    const VkImageUsageFlags hist_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (auto& hb : history_) {
        if (auto r = make_reg(hb.prev_normal, w, h, VK_FORMAT_R16G16_UNORM, hist_usage, true, false, false,
                              VK_IMAGE_ASPECT_COLOR_BIT, false, "hist_prev_normal");
            !r)
            return r;
        if (auto r = make_reg(hb.prev_rt_shadow, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, hist_usage, true, false, false,
                              VK_IMAGE_ASPECT_COLOR_BIT, false, "hist_prev_rt_shadow");
            !r)
            return r;
        if (auto r = make_reg(hb.prev_rim_occ, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, hist_usage, true, false, false,
                              VK_IMAGE_ASPECT_COLOR_BIT, false, "hist_prev_rim_occ");
            !r)
            return r;
        if (auto r = make_reg(hb.prev_bounce, hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, hist_usage, true, false, false,
                              VK_IMAGE_ASPECT_COLOR_BIT, false, "hist_prev_bounce");
            !r)
            return r;

        auto pd_res = device_->create_image_2d(w, h, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        if (!pd_res) {
            return Unexpected<Error>{pd_res.error()};
        }
        hb.prev_depth.img = pd_res.value();
        device_->destroy_image_view(hb.prev_depth.img.view);
        auto pdv = device_->create_image_view(hb.prev_depth.img.handle, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                              VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!pdv) {
            return Unexpected<Error>{pdv.error()};
        }
        hb.prev_depth.img.view = pdv.value();
        hb.prev_depth.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto ps = bindless_.register_sampled_image(hb.prev_depth.img.view);
        if (!ps) {
            return Unexpected<Error>{ps.error()};
        }
        hb.prev_depth.sampled_slot = ps.value();
    }

    return {};
}

void FrameGraph::destroy_size_dependent_targets() noexcept {
    if (device_ == nullptr) {
        return;
    }
    auto drop = [&](RegisteredImage& r) {
        if (r.sampled_slot != UINT32_MAX) {
            bindless_.release_sampled_image(r.sampled_slot);
            r.sampled_slot = UINT32_MAX;
        }
        if (r.storage_slot != UINT32_MAX) {
            bindless_.release_storage_image(r.storage_slot);
            r.storage_slot = UINT32_MAX;
        }
        if (r.uint_slot != UINT32_MAX) {
            bindless_.release_uint_sampled_image(r.uint_slot);
            r.uint_slot = UINT32_MAX;
        }
        device_->destroy_image(r.img);
        r.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    drop(gb_n_);
    drop(gb_mv_);
    drop(gb_mat_);
    drop(gb_misc_);

    if (depth_sampled_slot_ != UINT32_MAX) {
        bindless_.release_sampled_image(depth_sampled_slot_);
        depth_sampled_slot_ = UINT32_MAX;
    }
    if (stencil_uint_slot_ != UINT32_MAX) {
        bindless_.release_uint_sampled_image(stencil_uint_slot_);
        stencil_uint_slot_ = UINT32_MAX;
    }
    device_->destroy_image_view(depth_only_view_);
    depth_only_view_ = VK_NULL_HANDLE;
    device_->destroy_image_view(stencil_only_view_);
    stencil_only_view_ = VK_NULL_HANDLE;
    device_->destroy_image(depth_stencil_);
    depth_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    drop(rt_shadow_raw_);
    drop(rt_shadow_denoised_);
    drop(rim_occ_raw_);
    drop(rim_occ_denoised_);
    drop(bounce_half_);
    drop(bounce_denoised_);
    drop(aov_backscatter_);
    drop(aov_cornea_spec_);
    drop(crease_mask_);
    drop(hdr_scene_);
    for (auto& m : bloom_mips_) {
        drop(m);
    }
    drop(out_rgba16f_);
    drop(out_rgba8_scratch_);
    drop(out_bgra8_);
    drop(out_nv12_y_);
    drop(out_nv12_uv_);
    for (auto& slot : out_ring_) {
        drop(slot.bgra8);
        drop(slot.bgra8_straight);
        drop(slot.bottom_up_bgra8);
        drop(slot.nv12_y);
        drop(slot.nv12_uv);
    }
    drop(viewport_out_);
    drop(stage_viewport_out_);
    drop(quad_top_out_);
    drop(quad_right_out_);
    drop(stage_broadcast_pip_out_);
    stage_rt_w_ = stage_rt_h_ = stage_active_w_ = stage_active_h_ = 0;
    quad_top_rt_w_ = quad_top_rt_h_ = quad_top_active_w_ = quad_top_active_h_ = 0;
    quad_right_rt_w_ = quad_right_rt_h_ = quad_right_active_w_ = quad_right_active_h_ = 0;
    stage_pip_w_ = stage_pip_h_ = 0;
    prev_stage_scene_w_ = prev_stage_scene_h_ = 0;
    for (auto& hb : history_) {
        for (auto& rb : hb.reservoir_bufs) {
            device_->destroy_buffer(rb);
        }
        drop(hb.prev_depth);
        drop(hb.prev_normal);
        drop(hb.prev_rt_shadow);
        drop(hb.prev_rim_occ);
        drop(hb.prev_bounce);
        hb.have_prev_view_proj = false;
    }
    denoiser_.invalidate_temporal_history();
}

Expected<void> FrameGraph::create_pipelines(bool reload_only) {
    if (device_ == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "null device")};
    }

    if (reload_only) {
        device_->wait_idle();
        auto destroy_pso = [&](VkPipeline& p) {
            if (p != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_->device(), p, nullptr);
                p = VK_NULL_HANDLE;
            }
        };
        destroy_pso(pso_skin_morph_);
        destroy_pso(pso_prepass_);
        destroy_pso(pso_ray_effects_);
        destroy_pso(pso_csm_shadow_);
        destroy_pso(pso_csm_fallback_);
        destroy_pso(pso_live2d_opaque_);
        destroy_pso(pso_live2d_double_sided_);
        destroy_pso(pso_live2d_blend_cull_);
        destroy_pso(pso_live2d_blend_);
        destroy_pso(pso_live2d_additive_cull_);
        destroy_pso(pso_live2d_additive_nocull_);
        destroy_pso(pso_live2d_opaque_wire_);
        destroy_pso(pso_live2d_double_sided_wire_);
        destroy_pso(pso_live2d_blend_wire_);
        destroy_pso(pso_outline_hull_);
        destroy_pso(pso_crease_lines_);
        destroy_pso(pso_bloom_down_);
        destroy_pso(pso_bloom_up_);
        destroy_pso(pso_tone_composite_);
        destroy_pso(pso_encode_bgra_);
        destroy_pso(pso_encode_nv12_);
        destroy_pso(pso_viewport_compose_);
        destroy_pso(pso_heatmap_);
        destroy_pso(pso_stage_schematic_);
        pipelines_ready_ = false;
    }

    if (pipe_layout_global_ == VK_NULL_HANDLE) {
        VkDescriptorSetLayout set0 = bindless_.global_layout();
        VkPushConstantRange pcr{VK_SHADER_STAGE_ALL, 0, sizeof(GpuPushConstants)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &set0;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(device_->device(), &plci, nullptr, &pipe_layout_global_) != VK_SUCCESS) {
            return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkCreatePipelineLayout(global) failed")};
        }
    }

    if (pipe_layout_rt_ == VK_NULL_HANDLE && bindless_.rt_enabled()) {
        std::array<VkDescriptorSetLayout, 2> sets = {bindless_.global_layout(), bindless_.rt_layout()};
        VkPushConstantRange pcr{VK_SHADER_STAGE_ALL, 0, sizeof(GpuPushConstants)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = static_cast<uint32_t>(sets.size());
        plci.pSetLayouts = sets.data();
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(device_->device(), &plci, nullptr, &pipe_layout_rt_) != VK_SUCCESS) {
            return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkCreatePipelineLayout(rt) failed")};
        }
    }

    const bool disk = cfg_.allow_disk_shader_reload;
    auto build_compute = [&](shaders::Phase4ShaderId id, VkPipelineLayout layout,
                             VkPipeline& out_pso) -> Expected<void> {
        auto mod_res = create_spv_module(*device_, id, disk);
        if (!mod_res) {
            return Unexpected<Error>{mod_res.error()};
        }
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = mod_res.value();
        ci.stage.pName = "main";
        ci.layout = layout;
        const VkResult r =
            vkCreateComputePipelines(device_->device(), device_->pipeline_cache(), 1, &ci, nullptr, &out_pso);
        vkDestroyShaderModule(device_->device(), mod_res.value(), nullptr);
        if (r != VK_SUCCESS || out_pso == VK_NULL_HANDLE) {
            AETHER_LOG_ERROR("renderer", "vkCreateComputePipelines(shader=%u) r=%d pso=%p", static_cast<unsigned>(id),
                             static_cast<int>(r), static_cast<void*>(out_pso));
            out_pso = VK_NULL_HANDLE;
            return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkCreateComputePipelines failed")};
        }
        return {};
    };

    struct GfxDesc {
        shaders::Phase4ShaderId vs{shaders::Phase4ShaderId::PrepassVs};
        shaders::Phase4ShaderId fs{shaders::Phase4ShaderId::PrepassFs};
        const VkFormat* color_formats{nullptr};
        uint32_t color_count{0};
        VkFormat depth_format{VK_FORMAT_UNDEFINED};
        VkFormat stencil_format{VK_FORMAT_UNDEFINED};
        VkCullModeFlags cull{VK_CULL_MODE_BACK_BIT};
        bool depth_test{false};
        bool depth_write{false};
        bool stencil_write{false};
        bool premul_blend{false};
        bool dynamic_cull{false};
        VkPolygonMode polygon_mode{VK_POLYGON_MODE_FILL};
        VkCompareOp depth_compare{VK_COMPARE_OP_LESS_OR_EQUAL};
        VkCompareOp stencil_compare{VK_COMPARE_OP_ALWAYS};
        VkStencilOp stencil_pass{VK_STENCIL_OP_REPLACE};
        uint8_t stencil_read_mask{0xFFu};
        uint8_t stencil_write_mask{0xFFu};
        bool color_write_disable{false};
        bool additive_blend{false};
    };

    auto build_gfx = [&](const GfxDesc& gd, VkPipeline& out_pso) -> Expected<void> {
        auto vs_res = create_spv_module(*device_, gd.vs, disk);
        if (!vs_res) {
            return Unexpected<Error>{vs_res.error()};
        }
        auto fs_res = create_spv_module(*device_, gd.fs, disk);
        if (!fs_res) {
            vkDestroyShaderModule(device_->device(), vs_res.value(), nullptr);
            return Unexpected<Error>{fs_res.error()};
        }

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs_res.value();
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs_res.value();
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = gd.polygon_mode;
        rs.cullMode = gd.cull;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = gd.depth_test ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = gd.depth_write ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = gd.depth_compare;
        if (gd.stencil_write) {
            ds.stencilTestEnable = VK_TRUE;
            VkStencilOpState st{};
            st.failOp = VK_STENCIL_OP_KEEP;
            st.passOp = gd.stencil_pass;
            st.depthFailOp = VK_STENCIL_OP_KEEP;
            st.compareOp = gd.stencil_compare;
            st.compareMask = gd.stencil_read_mask;
            st.writeMask = gd.stencil_write_mask;
            st.reference = 1u;
            ds.front = st;
            ds.back = st;
        }

        std::array<VkPipelineColorBlendAttachmentState, 4> blend_atts{};
        for (uint32_t i = 0; i < gd.color_count; ++i) {
            blend_atts[i].colorWriteMask = gd.color_write_disable
                                               ? VkColorComponentFlags{0}
                                               : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
            if (gd.additive_blend) {
                blend_atts[i].blendEnable = VK_TRUE;
                blend_atts[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                blend_atts[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                blend_atts[i].colorBlendOp = VK_BLEND_OP_ADD;
                blend_atts[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                blend_atts[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                blend_atts[i].alphaBlendOp = VK_BLEND_OP_ADD;
            } else if (gd.premul_blend) {
                blend_atts[i].blendEnable = VK_TRUE;
                blend_atts[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                blend_atts[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                blend_atts[i].colorBlendOp = VK_BLEND_OP_ADD;
                blend_atts[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                blend_atts[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                blend_atts[i].alphaBlendOp = VK_BLEND_OP_ADD;
            }
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = gd.color_count;
        cb.pAttachments = blend_atts.data();

        std::array<VkDynamicState, 4> dyn_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                                 VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE};
        uint32_t dyn_count = 2;
        if (gd.stencil_write) {
            dyn_states[dyn_count++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
        }
        if (gd.dynamic_cull) {
            dyn_states[dyn_count++] = VK_DYNAMIC_STATE_CULL_MODE;
        }
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = dyn_count;
        dyn.pDynamicStates = dyn_states.data();

        VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rend.colorAttachmentCount = gd.color_count;
        rend.pColorAttachmentFormats = gd.color_formats;
        rend.depthAttachmentFormat = gd.depth_format;
        rend.stencilAttachmentFormat = gd.stencil_format;

        VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpci.pNext = &rend;
        gpci.stageCount = static_cast<uint32_t>(stages.size());
        gpci.pStages = stages.data();
        gpci.pVertexInputState = &vi;
        gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms;
        gpci.pDepthStencilState = &ds;
        gpci.pColorBlendState = &cb;
        gpci.pDynamicState = &dyn;
        gpci.layout = pipe_layout_global_;

        const VkResult r =
            vkCreateGraphicsPipelines(device_->device(), device_->pipeline_cache(), 1, &gpci, nullptr, &out_pso);
        vkDestroyShaderModule(device_->device(), vs_res.value(), nullptr);
        vkDestroyShaderModule(device_->device(), fs_res.value(), nullptr);
        if (r != VK_SUCCESS || out_pso == VK_NULL_HANDLE) {
            AETHER_LOG_ERROR("renderer", "vkCreateGraphicsPipelines(vs=%u, fs=%u) r=%d pso=%p",
                             static_cast<unsigned>(gd.vs), static_cast<unsigned>(gd.fs), static_cast<int>(r),
                             static_cast<void*>(out_pso));
            out_pso = VK_NULL_HANDLE;
            return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkCreateGraphicsPipelines failed")};
        }
        return {};
    };

    if (auto r = build_compute(shaders::Phase4ShaderId::SkinMorph, pipe_layout_global_, pso_skin_morph_); !r)
        return r;
    if (pipe_layout_rt_ != VK_NULL_HANDLE) {
        if (auto r = build_compute(shaders::Phase4ShaderId::RayEffects, pipe_layout_rt_, pso_ray_effects_); !r)
            return r;
    }
    if (auto r = build_compute(shaders::Phase4ShaderId::CsmFallbackCs, pipe_layout_global_, pso_csm_fallback_); !r)
        return r;
    if (auto r = build_compute(shaders::Phase4ShaderId::CreaseLines, pipe_layout_global_, pso_crease_lines_); !r)
        return r;
    if (auto r = build_compute(shaders::Phase4ShaderId::BloomDown, pipe_layout_global_, pso_bloom_down_); !r)
        return r;
    if (auto r = build_compute(shaders::Phase4ShaderId::BloomUp, pipe_layout_global_, pso_bloom_up_); !r)
        return r;
    if (auto r = build_compute(shaders::Phase4ShaderId::ToneComposite, pipe_layout_global_, pso_tone_composite_); !r)
        return r;
    if (auto r = build_compute(shaders::Phase4ShaderId::EncodeBgra, pipe_layout_global_, pso_encode_bgra_); !r)
        return r;
    if (auto r = build_compute(shaders::Phase4ShaderId::EncodeNv12, pipe_layout_global_, pso_encode_nv12_); !r)
        return r;

    static constexpr std::array<VkFormat, 4> kPrepassFormats = {
        VK_FORMAT_R16G16_UNORM,
        VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_R32_UINT,
        VK_FORMAT_R16G16B16A16_SFLOAT,
    };
    const VkFormat kHdrFmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkFormat kLdrFmt = VK_FORMAT_B8G8R8A8_UNORM;

    if (auto r = build_gfx({shaders::Phase4ShaderId::PrepassVs, shaders::Phase4ShaderId::PrepassFs,
                            kPrepassFormats.data(), 4, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                            VK_CULL_MODE_BACK_BIT, true, true, true, false, true},
                           pso_prepass_);
        !r) {
        return r;
    }
    if (auto r =
            build_gfx({shaders::Phase4ShaderId::CsmShadowVs, shaders::Phase4ShaderId::CsmShadowFs, nullptr, 0,
                       VK_FORMAT_D32_SFLOAT, VK_FORMAT_UNDEFINED, VK_CULL_MODE_NONE, true, true, false, false, false},
                      pso_csm_shadow_);
        !r) {
        return r;
    }
    if (auto r = build_gfx({shaders::Phase4ShaderId::Live2dNprVs, shaders::Phase4ShaderId::Live2dNprFs, &kHdrFmt, 1,
                            VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_CULL_MODE_BACK_BIT, true,
                            false, false, false, false},
                           pso_live2d_opaque_);
        !r) {
        return r;
    }
    if (auto r = build_gfx({shaders::Phase4ShaderId::Live2dNprVs, shaders::Phase4ShaderId::Live2dNprFs, &kHdrFmt, 1,
                            VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_CULL_MODE_NONE, true, false,
                            false, false, false},
                           pso_live2d_double_sided_);
        !r) {
        return r;
    }
    if (auto r = build_gfx({shaders::Phase4ShaderId::Live2dNprVs, shaders::Phase4ShaderId::Live2dNprFs, &kHdrFmt, 1,
                            VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_CULL_MODE_BACK_BIT, true,
                            false, false, true, false},
                           pso_live2d_blend_cull_);
        !r) {
        return r;
    }
    if (auto r = build_gfx({shaders::Phase4ShaderId::Live2dNprVs, shaders::Phase4ShaderId::Live2dNprFs, &kHdrFmt, 1,
                            VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_CULL_MODE_NONE, true, false,
                            false, true, false},
                           pso_live2d_blend_);
        !r) {
        return r;
    }
    {
        GfxDesc add_cull{shaders::Phase4ShaderId::Live2dNprVs,
                         shaders::Phase4ShaderId::Live2dNprFs,
                         &kHdrFmt,
                         1,
                         VK_FORMAT_D32_SFLOAT_S8_UINT,
                         VK_FORMAT_D32_SFLOAT_S8_UINT,
                         VK_CULL_MODE_BACK_BIT,
                         true,
                         false,
                         false,
                         false,
                         false};
        add_cull.additive_blend = true;
        if (auto r = build_gfx(add_cull, pso_live2d_additive_cull_); !r) {
            return r;
        }
        add_cull.cull = VK_CULL_MODE_NONE;
        if (auto r = build_gfx(add_cull, pso_live2d_additive_nocull_); !r) {
            return r;
        }
    }
    wireframe_supported_ = device_->gpu_info().fill_mode_non_solid;
    if (wireframe_supported_) {
        if (auto r = build_gfx({shaders::Phase4ShaderId::Live2dNprVs, shaders::Phase4ShaderId::Live2dNprFs, &kHdrFmt, 1,
                                VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_CULL_MODE_BACK_BIT, true,
                                false, false, false, false, VK_POLYGON_MODE_LINE},
                               pso_live2d_opaque_wire_);
            !r) {
            return r;
        }
        if (auto r = build_gfx({shaders::Phase4ShaderId::Live2dNprVs, shaders::Phase4ShaderId::Live2dNprFs, &kHdrFmt, 1,
                                VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_CULL_MODE_NONE, true,
                                false, false, false, false, VK_POLYGON_MODE_LINE},
                               pso_live2d_double_sided_wire_);
            !r) {
            return r;
        }
        if (auto r = build_gfx({shaders::Phase4ShaderId::Live2dNprVs, shaders::Phase4ShaderId::Live2dNprFs, &kHdrFmt, 1,
                                VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_CULL_MODE_NONE, true,
                                false, false, true, false, VK_POLYGON_MODE_LINE},
                               pso_live2d_blend_wire_);
            !r) {
            return r;
        }
    }
    if (auto r = build_gfx({shaders::Phase4ShaderId::OutlineHullVs, shaders::Phase4ShaderId::OutlineHullFs, &kHdrFmt, 1,
                            VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_CULL_MODE_FRONT_BIT, true,
                            false, false, true, false},
                           pso_outline_hull_);
        !r) {
        return r;
    }
    if (auto r = build_gfx({shaders::Phase4ShaderId::ViewportComposeVs, shaders::Phase4ShaderId::ViewportComposeFs,
                            &kLdrFmt, 1, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_CULL_MODE_NONE, false, false,
                            false, false, false},
                           pso_viewport_compose_);
        !r) {
        return r;
    }
    if (auto r =
            build_gfx({shaders::Phase4ShaderId::HeatmapVs, shaders::Phase4ShaderId::HeatmapFs, &kLdrFmt, 1,
                       VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_CULL_MODE_NONE, false, false, false, true, false},
                      pso_heatmap_);
        !r) {
        return r;
    }
    if (auto r = build_gfx({shaders::Phase4ShaderId::StageSchematicVs, shaders::Phase4ShaderId::StageSchematicFs,
                            &kLdrFmt, 1, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_CULL_MODE_NONE, false, false,
                            false, true, false},
                           pso_stage_schematic_);
        !r) {
        return r;
    }

    pipelines_ready_ = true;
    return {};
}

void FrameGraph::destroy_pipelines() noexcept {
    if (device_ == nullptr || device_->device() == VK_NULL_HANDLE) {
        return;
    }
    auto drop_p = [&](VkPipeline& p) {
        if (p != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_->device(), p, nullptr);
            p = VK_NULL_HANDLE;
        }
    };
    drop_p(pso_skin_morph_);
    drop_p(pso_prepass_);
    drop_p(pso_ray_effects_);
    drop_p(pso_csm_shadow_);
    drop_p(pso_csm_fallback_);
    drop_p(pso_live2d_opaque_);
    drop_p(pso_live2d_double_sided_);
    drop_p(pso_live2d_blend_cull_);
    drop_p(pso_live2d_blend_);
    drop_p(pso_live2d_additive_cull_);
    drop_p(pso_live2d_additive_nocull_);
    drop_p(pso_live2d_opaque_wire_);
    drop_p(pso_live2d_double_sided_wire_);
    drop_p(pso_live2d_blend_wire_);
    drop_p(pso_outline_hull_);
    drop_p(pso_crease_lines_);
    drop_p(pso_bloom_down_);
    drop_p(pso_bloom_up_);
    drop_p(pso_tone_composite_);
    drop_p(pso_encode_bgra_);
    drop_p(pso_encode_nv12_);
    drop_p(pso_viewport_compose_);
    drop_p(pso_heatmap_);
    drop_p(pso_stage_schematic_);

    if (pipe_layout_rt_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_->device(), pipe_layout_rt_, nullptr);
        pipe_layout_rt_ = VK_NULL_HANDLE;
    }
    if (pipe_layout_global_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_->device(), pipe_layout_global_, nullptr);
        pipe_layout_global_ = VK_NULL_HANDLE;
    }
    pipelines_ready_ = false;
    wireframe_supported_ = false;
}

Expected<void> FrameGraph::apply_face_sdf(const SdfBakeOutput& baked) {
    if (device_ == nullptr || bound_avatar_ == nullptr || cached_materials_.empty()) {
        return Unexpected<Error>{
            Error::make(ErrorCode::InvalidArg, "FrameGraph::apply_face_sdf requires bound avatar")};
    }

    device_->wait_idle();

    auto up = OfflineSdfGenerator::upload_r8_texture(*device_, baked);
    if (!up) {
        return Unexpected<Error>{up.error()};
    }

    auto reg = bindless_.register_sampled_image(up.value().view);
    if (!reg) {
        device_->destroy_image(up.value());
        return Unexpected<Error>{reg.error()};
    }

    if (baked_face_sdf_slot_ != UINT32_MAX) {
        bindless_.release_sampled_image(baked_face_sdf_slot_);
        baked_face_sdf_slot_ = UINT32_MAX;
    }
    device_->destroy_image(baked_face_sdf_img_);

    baked_face_sdf_img_ = up.value();
    baked_face_sdf_slot_ = reg.value();
    avatar_face_sdf_slot_ = baked_face_sdf_slot_;

    // Prefer face-tagged materials / face-submesh materials lacking an authored SDF; fall back to all materials.
    std::vector<bool> target_mat(cached_materials_.size(), false);
    bool any_face_mat = false;
    for (const auto& sm : cached_submeshes_) {
        if ((sm.flags & AETHER_SUBMESH_FLAG_FACE) != 0u && sm.material_index < target_mat.size()) {
            target_mat[sm.material_index] = true;
            any_face_mat = true;
        }
    }
    for (size_t i = 0; i < cached_materials_.size(); ++i) {
        const uint32_t mflags = cached_materials_[i].extra_slots[2];
        if ((mflags & (AETHER_MAT_FLAG_SDF_FACE | AETHER_MAT_FLAG_RECEIVES_HAIR_SHADOW)) != 0u &&
            (mflags & AETHER_MAT_FLAG_HAIR_FRONT_STENCIL) == 0u) {
            target_mat[i] = true;
            any_face_mat = true;
        }
    }
    for (size_t i = 0; i < cached_materials_.size(); ++i) {
        if (!any_face_mat || target_mat[i]) {
            cached_materials_[i].tex_slots[2] = baked_face_sdf_slot_;
            cached_materials_[i].extra_slots[2] |= (AETHER_MAT_FLAG_SDF_FACE | kMatExtraAuthoredSdfBit);
        }
    }

    if (material_buf_.info.pMappedData != nullptr) {
        const size_t bytes = cached_materials_.size() * sizeof(GpuMaterial);
        std::memcpy(material_buf_.info.pMappedData, cached_materials_.data(), bytes);
        vmaFlushAllocation(device_->allocator(), material_buf_.allocation, 0, bytes);
    }

    AETHER_LOG_INFO("renderer", "Applied baked %ux%u R8 face SDF to bindless slot %u", baked.width, baked.height,
                    baked_face_sdf_slot_);
    return {};
}

void FrameGraph::invalidate_temporal_history() noexcept {
    for (auto& hb : history_) {
        hb.have_prev_view_proj = false;
    }
    denoiser_.invalidate_temporal_history();
}

Expected<void> FrameGraph::bind_avatar(const GpuAvatar* avatar) {
    if (device_ == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "FrameGraph::bind_avatar uninitialized")};
    }
    device_->wait_idle();

    if (baked_face_sdf_slot_ != UINT32_MAX) {
        bindless_.release_sampled_image(baked_face_sdf_slot_);
        baked_face_sdf_slot_ = UINT32_MAX;
    }
    device_->destroy_image(baked_face_sdf_img_);

    for (const uint32_t s : avatar_tex_slots_) {
        if (s != UINT32_MAX && s != fallback_white_slot_) {
            bindless_.release_sampled_image(s);
        }
    }
    avatar_tex_slots_.clear();
    device_->destroy_buffer(skinned_vertices_buf_);
    device_->destroy_buffer(material_buf_);
    device_->destroy_buffer(material_grade_buf_);
    device_->destroy_buffer(submesh_draw_buf_);
    cached_submeshes_.clear();
    cached_materials_.clear();
    active_materials_scratch_.clear();
    active_grades_scratch_.clear();
    opaque_draw_indices_.clear();
    cutout_draw_indices_.clear();
    transparent_draw_indices_.clear();
    avatar_face_sdf_slot_ = UINT32_MAX;
    bound_avatar_ = nullptr;
    accel_.destroy_avatar_structures();
    invalidate_temporal_history();
    last_rt_avatar_ = nullptr;
    last_rt_as_update_us_ = 0ULL;
    last_rt_pose_hash_ = 0ULL;

    if (avatar == nullptr) {
        return {};
    }
    const uint32_t vtx_count = avatar->requirements.vertex_count;
    if (vtx_count == 0 || avatar->index_buf.handle == VK_NULL_HANDLE) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "FrameGraph::bind_avatar empty geometry")};
    }

    // 1) Allocate skinned vertex buffer (Pass 1 destination + Pass 2 BLAS source)
    VkBufferUsageFlags sk_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (device_->rt_supported()) {
        sk_usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }
    auto sv_res = device_->create_buffer(static_cast<VkDeviceSize>(vtx_count) * sizeof(GpuSkinnedVertex), sk_usage,
                                         VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    if (!sv_res) {
        return Unexpected<Error>{sv_res.error()};
    }
    skinned_vertices_buf_ = sv_res.value();

    // 2) Register avatar textures into bindless table
    avatar_tex_slots_.resize(avatar->textures.size(), fallback_white_slot_);
    for (size_t i = 0; i < avatar->textures.size(); ++i) {
        if (avatar->textures[i].view != VK_NULL_HANDLE) {
            auto reg = bindless_.register_sampled_image(avatar->textures[i].view);
            if (!reg) {
                return Unexpected<Error>{reg.error()};
            }
            avatar_tex_slots_[i] = reg.value();
            if (i < avatar->texture_descs.size() &&
                (avatar->texture_descs[i].usage_flags & AETHER_TEX_USAGE_SDF_FACE) != 0u &&
                avatar_face_sdf_slot_ == UINT32_MAX) {
                avatar_face_sdf_slot_ = reg.value();
            }
        }
    }

    // 3) Build unified GpuMaterial table, classify submeshes, and upload host-coherent buffers
    detail::populate_materials_and_submeshes(*avatar, avatar_tex_slots_, fallback_white_slot_, avatar_face_sdf_slot_,
                                             cached_materials_, cached_submeshes_, opaque_draw_indices_,
                                             cutout_draw_indices_, transparent_draw_indices_);

    auto mb_res = device_->create_buffer(
        cached_materials_.size() * sizeof(GpuMaterial),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!mb_res) {
        return Unexpected<Error>{mb_res.error()};
    }
    material_buf_ = mb_res.value();
    std::memcpy(material_buf_.info.pMappedData, cached_materials_.data(),
                cached_materials_.size() * sizeof(GpuMaterial));
    vmaFlushAllocation(device_->allocator(), material_buf_.allocation, 0,
                       cached_materials_.size() * sizeof(GpuMaterial));

    active_materials_scratch_ = cached_materials_;
    active_grades_scratch_.assign(cached_materials_.size(), GpuMaterialGrade{});
    auto mgb_res = device_->create_buffer(
        active_grades_scratch_.size() * sizeof(GpuMaterialGrade),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!mgb_res) {
        return Unexpected<Error>{mgb_res.error()};
    }
    material_grade_buf_ = mgb_res.value();
    std::memcpy(material_grade_buf_.info.pMappedData, active_grades_scratch_.data(),
                active_grades_scratch_.size() * sizeof(GpuMaterialGrade));
    vmaFlushAllocation(device_->allocator(), material_grade_buf_.allocation, 0,
                       active_grades_scratch_.size() * sizeof(GpuMaterialGrade));

    auto sub_buf_res = device_->create_buffer(
        cached_submeshes_.size() * sizeof(GpuSubmeshDraw),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!sub_buf_res) {
        return Unexpected<Error>{sub_buf_res.error()};
    }
    submesh_draw_buf_ = sub_buf_res.value();
    std::memcpy(submesh_draw_buf_.info.pMappedData, cached_submeshes_.data(),
                cached_submeshes_.size() * sizeof(GpuSubmeshDraw));
    vmaFlushAllocation(device_->allocator(), submesh_draw_buf_.allocation, 0,
                       cached_submeshes_.size() * sizeof(GpuSubmeshDraw));

    if (!avatar->submeshes.empty()) {
        auto by_queue = [&](uint32_t a, uint32_t b) {
            return avatar->submeshes[a].render_queue_offset < avatar->submeshes[b].render_queue_offset;
        };
        std::stable_sort(opaque_draw_indices_.begin(), opaque_draw_indices_.end(), by_queue);
        std::stable_sort(cutout_draw_indices_.begin(), cutout_draw_indices_.end(), by_queue);
        std::stable_sort(transparent_draw_indices_.begin(), transparent_draw_indices_.end(), by_queue);
    }

    if (device_->rt_supported()) {
        if (auto as_res = accel_.prepare_avatar(*avatar); !as_res) {
            return Unexpected<Error>{as_res.error()};
        }
    }

    bound_avatar_ = avatar;
    return {};
}

static float srgb_to_linear_ch(float c) noexcept {
    c = std::clamp(c, 0.0f, 1.0f);
    return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

void FrameGraph::record_critical_passes(VkCommandBuffer cmd, const FrameRenderRequest& req) noexcept {
    if (!pipelines_ready_ || cmd == VK_NULL_HANDLE || req.rig == nullptr) {
        return;
    }

    const bool use_external =
        (req.external_color_image != VK_NULL_HANDLE && req.external_width > 0U && req.external_height > 0U);
    const bool has_avatar = !use_external && (req.avatar != nullptr && bound_avatar_ == req.avatar &&
                                              req.avatar->requirements.vertex_count > 0);
    const uint32_t rw = effective_output_width();
    const uint32_t rh = effective_output_height();
    if (rw == 0 || rh == 0 || out_ring_[0].bgra8.img.handle == VK_NULL_HANDLE) {
        return;
    }

    const bool is_stage_view = (req.view_kind == RenderViewKind::LightingStage);
    uint32_t dst_rt_w = rw;
    uint32_t dst_rt_h = rh;
    if (is_stage_view) {
        if (req.stage_output_slot == FrameRenderRequest::StageOutputSlot::QuadTop) {
            dst_rt_w = (quad_top_rt_w_ > 0u) ? quad_top_rt_w_ : std::max(cfg_.width / 2u, 16u);
            dst_rt_h = (quad_top_rt_h_ > 0u) ? quad_top_rt_h_ : std::max(cfg_.height / 2u, 16u);
        } else if (req.stage_output_slot == FrameRenderRequest::StageOutputSlot::QuadRight) {
            dst_rt_w = (quad_right_rt_w_ > 0u) ? quad_right_rt_w_ : std::max(cfg_.width / 2u, 16u);
            dst_rt_h = (quad_right_rt_h_ > 0u) ? quad_right_rt_h_ : std::max(cfg_.height / 2u, 16u);
        } else {
            dst_rt_w = (stage_rt_w_ > 0u) ? stage_rt_w_ : std::max(cfg_.width, 16u);
            dst_rt_h = (stage_rt_h_ > 0u) ? stage_rt_h_ : std::max(cfg_.height, 16u);
        }
    }
    const detail::StageRenderRegion sreg =
        is_stage_view
            ? detail::compute_stage_render_region(req, rw, rh, dst_rt_w, dst_rt_h)
            : detail::StageRenderRegion{rw,   rh,   rw,   rh,   static_cast<float>(rw) / static_cast<float>(rh),
                                        1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                        1.0f, 1.0f, 1.0f};
    const uint32_t w = sreg.scene_w;
    const uint32_t h = sreg.scene_h;

    const uint32_t out_slot =
        (req.view_kind == RenderViewKind::Broadcast) ? (req.frame_slot % kOutputRingSlots) : last_output_slot_;
    OutputSlotTargets& os = out_ring_[out_slot];
    if (req.view_kind == RenderViewKind::Broadcast) {
        last_output_slot_ = out_slot;
    }
    const uint32_t fslot = req.frame_slot % rhi::GpuTimingRing::kFrameSlots;
    const uint32_t hw = std::max(w / 2u, 1u);
    const uint32_t hh = std::max(h / 2u, 1u);
    const uint32_t gx = (w + 7u) / 8u;
    const uint32_t gy = (h + 7u) / 8u;
    const LightingRig& rig = *req.rig;

    const bool record_timing = !req.skip_skin_and_blas;
    const bool record_stage_scope =
        is_stage_view && (req.stage_output_slot == FrameRenderRequest::StageOutputSlot::MainStage);
    if (record_timing) {
        timing_.cmd_reset_frame(cmd, fslot);
        rebar_.begin_frame(req.frame_index, req.completed_frame_index);
    }
    if (record_stage_scope) {
        if (!record_timing) {
            timing_.cmd_reset_scope(cmd, fslot, rhi::GpuPassId::LightingStage);
        }
        timing_.begin_scope(cmd, fslot, rhi::GpuPassId::LightingStage);
    }
    auto t_begin = [&](rhi::GpuPassId id) noexcept {
        if (record_timing) {
            timing_.begin_scope(cmd, fslot, id);
        }
    };
    auto t_end = [&](rhi::GpuPassId id) noexcept {
        if (record_timing) {
            timing_.end_scope(cmd, fslot, id);
        }
    };
    t_begin(rhi::GpuPassId::FrameTotal);

    auto fg_alloc_res = rebar_.allocate(sizeof(GpuFrameGlobals), 64);
    if (!fg_alloc_res) {
        if (record_stage_scope) {
            timing_.end_scope(cmd, fslot, rhi::GpuPassId::LightingStage);
        }
        return;
    }
    const bool allow_view_hist =
        !is_stage_view || (req.stage_output_slot == FrameRenderRequest::StageOutputSlot::MainStage);
    const uint32_t view_bank = is_stage_view ? 1u : 0u;
    TemporalHistoryBank& hb = history_[view_bank];
    const bool stage_size_changed =
        is_stage_view && allow_view_hist && (w != prev_stage_scene_w_ || h != prev_stage_scene_h_);
    if (is_stage_view && allow_view_hist) {
        prev_stage_scene_w_ = w;
        prev_stage_scene_h_ = h;
    }
    const bool fast_stage_pan = is_stage_view && req.stage_fast_panning;
    const bool hist_valid = allow_view_hist && hb.have_prev_view_proj && !fast_stage_pan && !stage_size_changed;

    const auto& fg_alloc = fg_alloc_res.value();
    GpuFrameGlobals fg{};
    GpuMat4 next_prev_vp{};
    const bool rt_hardware_ready = device_->rt_supported() && accel_.is_ready() && pso_ray_effects_ != VK_NULL_HANDLE;
    const std::array<float, 4> uv_scale{sreg.uv_scale_x, sreg.uv_scale_y, sreg.uv_max_u, sreg.uv_max_v};
    const std::array<float, 4> half_uv_scale{sreg.half_uv_scale_x, sreg.half_uv_scale_y, sreg.half_uv_max_u,
                                             sreg.half_uv_max_v};
    const uint8_t eff_mode = detail::populate_shared_frame_globals(
        req, w, h, has_avatar, rt_hardware_ready, hist_valid, hb.prev_view_proj, fg, next_prev_vp,
        is_stage_view ? sreg.proj_aspect : 0.0f, &uv_scale, &half_uv_scale);
    if (has_avatar && allow_view_hist) {
        hb.prev_view_proj = next_prev_vp;
    }
    last_pivot_ws_ = {fg.neck_seam_head_str[0], fg.neck_seam_head_str[1], fg.neck_seam_head_str[2]};
    last_eye_ws_ = {fg.camera_pos_world[0], fg.camera_pos_world[1], fg.camera_pos_world[2]};
    const bool use_rt = (eff_mode != 0u);

    fg.gbuffer_ids[0] = gb_n_.sampled_slot;
    fg.gbuffer_ids[1] = gb_mv_.sampled_slot;
    fg.gbuffer_ids[2] = gb_mat_.uint_slot;
    fg.gbuffer_ids[3] = gb_misc_.sampled_slot;
    fg.gbuffer_ids[4] = rt_shadow_raw_.sampled_slot;
    fg.gbuffer_ids[5] = crease_mask_.sampled_slot;
    fg.gbuffer_ids[6] = bloom_mips_[0].sampled_slot;
    fg.gbuffer_ids[7] = hdr_scene_.sampled_slot;

    fg.gbuffer_extra_ids[0] = depth_sampled_slot_;
    fg.gbuffer_extra_ids[1] = stencil_uint_slot_;
    fg.gbuffer_extra_ids[2] = rt_shadow_denoised_.sampled_slot;
    fg.gbuffer_extra_ids[3] = rim_occ_denoised_.sampled_slot;
    fg.gbuffer_extra_ids[4] = bounce_denoised_.sampled_slot;
    fg.gbuffer_extra_ids[5] = csm_depth_.sampled_slot;
    fg.gbuffer_extra_ids[6] = hb.prev_depth.sampled_slot;
    fg.gbuffer_extra_ids[7] = hb.prev_normal.sampled_slot;

    fg.history_ids[0] = hb.prev_rt_shadow.sampled_slot;
    fg.history_ids[1] = hb.prev_rim_occ.sampled_slot;
    fg.history_ids[2] = rim_occ_raw_.sampled_slot;
    fg.history_ids[3] = hb.prev_depth.sampled_slot;

    fg.output_storage_ids[0] = out_rgba16f_.storage_slot;
    fg.output_storage_ids[1] = bgra8_supports_storage_ ? os.bgra8.storage_slot : out_rgba8_scratch_.storage_slot;
    fg.output_storage_ids[2] = os.nv12_y.storage_slot;
    fg.output_storage_ids[3] = os.nv12_uv.storage_slot;

    auto sl_alloc_res = rebar_.allocate(sizeof(StudioLightGPU) * kMaxStudioLights, 64);
    if (sl_alloc_res) {
        const auto& sl_alloc = sl_alloc_res.value();
        const LightingRig fallback_rig = LightingRig::default_three_point();
        const LightingRig& active_rig = (req.rig != nullptr) ? *req.rig : fallback_rig;
        const std::array<float, 3> head_target_ws = last_pivot_ws_;
        const std::array<float, 3> eye_pos_ws = last_eye_ws_;
        const uint32_t sl_count =
            pack_studio_lights_gpu(active_rig, head_target_ws, eye_pos_ws, studio_lights_scratch_);
        std::memcpy(sl_alloc.mapped, studio_lights_scratch_.data(), sizeof(StudioLightGPU) * kMaxStudioLights);
        fg.studio_lights_bda = sl_alloc.device_address;
        fg.studio_slots0[0] = ltc_inv_m_.sampled_slot;
        fg.studio_slots0[1] = ltc_amp_.sampled_slot;
        fg.studio_slots0[2] = ies_lut_.sampled_slot;
        fg.studio_slots0[3] = sl_count;
    }
    fg.studio_slots1[0] = rt_shadow_denoised_.sampled_slot;
    fg.studio_slots1[1] = bounce_denoised_.sampled_slot;
    fg.studio_slots1[2] = aov_backscatter_.sampled_slot;
    fg.studio_slots1[3] = aov_cornea_spec_.sampled_slot;
    fg.studio_uav_slots[0] = rt_shadow_raw_.storage_slot;
    fg.studio_uav_slots[1] = bounce_half_.storage_slot;
    fg.studio_uav_slots[2] = aov_backscatter_.storage_slot;
    fg.studio_uav_slots[3] = aov_cornea_spec_.storage_slot;
    const uint32_t cur_res = static_cast<uint32_t>(req.frame_index & 1ULL);
    fg.reservoir_curr_bda = hb.reservoir_bufs[cur_res].device_address;
    fg.reservoir_prev_bda = hb.reservoir_bufs[1u - cur_res].device_address;

    if (req.avatar != nullptr && req.avatar == bound_avatar_ && !cached_materials_.empty()) {
        detail::update_gpu_materials_with_overrides(*req.avatar, cached_materials_, req.material_overrides,
                                                    active_materials_scratch_, active_grades_scratch_);
        if (material_buf_.info.pMappedData != nullptr) {
            std::memcpy(material_buf_.info.pMappedData, active_materials_scratch_.data(),
                        active_materials_scratch_.size() * sizeof(GpuMaterial));
            vmaFlushAllocation(device_->allocator(), material_buf_.allocation, 0,
                               active_materials_scratch_.size() * sizeof(GpuMaterial));
        }
        if (material_grade_buf_.info.pMappedData != nullptr) {
            std::memcpy(material_grade_buf_.info.pMappedData, active_grades_scratch_.data(),
                        active_grades_scratch_.size() * sizeof(GpuMaterialGrade));
            vmaFlushAllocation(device_->allocator(), material_grade_buf_.allocation, 0,
                               active_grades_scratch_.size() * sizeof(GpuMaterialGrade));
        }
        const uint32_t ps = req.frame_slot % GpuAvatar::kPoseSlotCount;
        fg.material_grade_bda = material_grade_buf_.device_address;
        fg.pmx_sdef_bda = req.avatar->pmx_sdef_buf.device_address;
        fg.skin_buf_bda = req.avatar->skin_buf.device_address;
        fg.uv_delta_bda = req.avatar->dynamic_uv_delta_bufs[ps].device_address;
        fg.joint_mats_bda = req.avatar->dynamic_joint_mat_bufs[ps].device_address;
    }

    std::memcpy(fg_alloc.mapped, &fg, sizeof(fg));

    VkDescriptorSet set0 = bindless_.global_set();
    const VkViewport neg_vp{0.0f, static_cast<float>(h), static_cast<float>(w), -static_cast<float>(h), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {w, h}};

    auto trans = [&](RegisteredImage& r, VkImageLayout dst_lay, VkPipelineStageFlags2 src_st, VkAccessFlags2 src_ac,
                     VkPipelineStageFlags2 dst_st, VkAccessFlags2 dst_ac,
                     VkImageAspectFlags asp = VK_IMAGE_ASPECT_COLOR_BIT) {
        rhi::RhiDevice::cmd_transition_image(cmd, r.img.handle, r.layout, dst_lay, src_st, src_ac, dst_st, dst_ac, asp);
        r.layout = dst_lay;
    };

    if (has_avatar) {
        // Only reuse the skinned vertex buffer if THIS graph skinned THIS avatar for THIS frame. With an external
        // (DirectX 12 / 11) Broadcast frame the Vulkan graph never runs Pass 1, so the LightingStage pass must skin.
        const bool skinned_this_frame =
            (last_skinned_avatar_ == req.avatar) && (last_skinned_frame_index_ == req.frame_index);
        const bool reuse_skin_and_tlas = req.skip_skin_and_blas && skinned_this_frame && (!use_rt || accel_.is_ready());
        last_skinned_avatar_ = req.avatar;
        last_skinned_frame_index_ = req.frame_index;
        if (!reuse_skin_and_tlas) {
            // === PASS 1: Compute Skin/Pack -> GpuSkinnedVertex ===
            t_begin(rhi::GpuPassId::SkinMorph);
            {
                const uint32_t pose_slot = static_cast<uint32_t>(req.frame_index % GpuAvatar::kPoseSlotCount);
                const VkDeviceAddress dyn_bda = (req.avatar->dynamic_vertex25d_bufs[pose_slot].device_address != 0)
                                                    ? req.avatar->dynamic_vertex25d_bufs[pose_slot].device_address
                                                    : req.avatar->vertex25d_buf.device_address;
                GpuPushConstants pc{};
                pc.frame_globals_bda = fg_alloc.device_address;
                pc.draw_data_bda = dyn_bda;
                pc.vertex_buf_bda = skinned_vertices_buf_.device_address;
                pc.aux_buf_bda = req.avatar->surface_buf.device_address;
                pc.param0 = req.avatar->requirements.vertex_count;
                pc.param1 = hist_valid ? 1u : 0u;
                bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso_skin_morph_, "skin_morph");
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_global_, 0, 1, &set0, 0,
                                        nullptr);
                vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
                vkCmdDispatch(cmd, (pc.param0 + 63u) / 64u, 1, 1);
                ++skin_dispatch_count_;
            }
            t_end(rhi::GpuPassId::SkinMorph);

            const VkPipelineStageFlags2 skin_dst_stage =
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                (use_rt ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR : 0ULL);
            cmd_buffer_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                               skin_dst_stage, VK_ACCESS_2_SHADER_READ_BIT);

            // === PASS 2: BLAS refit (or 300-frame rebuild) + TLAS rebuild ===
            if (use_rt) {
                const uint32_t pose_slot = static_cast<uint32_t>(req.frame_index % GpuAvatar::kPoseSlotCount);
                bool run_as_update = true;
                if (req.throttle_rt_accel_struct) {
                    uint64_t pose_hash = 14695981039346656037ULL;
                    auto hash_u32 = [&](uint32_t v) noexcept {
                        pose_hash ^= static_cast<uint64_t>(v);
                        pose_hash *= 1099511628211ULL;
                    };
                    auto hash_u64 = [&](uint64_t v) noexcept {
                        pose_hash ^= v;
                        pose_hash *= 1099511628211ULL;
                    };
                    auto hash_f32 = [&](float f) noexcept {
                        uint32_t bits = 0;
                        std::memcpy(&bits, &f, sizeof(bits));
                        hash_u32(bits);
                    };
                    hash_u32(req.avatar->requirements.vertex_count);
                    hash_u64(req.avatar->slot_applied_pose_seq[pose_slot]);
                    hash_f32(req.avatar_pos[0]);
                    hash_f32(req.avatar_pos[1]);
                    hash_f32(req.avatar_pos[2]);
                    hash_f32(req.avatar_rot_y_deg);
                    hash_f32(req.avatar_scale);
                    if (req.bone_matrices != nullptr && req.bone_count > 0u) {
                        const uint32_t b_cnt = std::min<uint32_t>(req.bone_count, req.avatar->requirements.joint_count);
                        for (uint32_t bi = 0; bi < b_cnt; ++bi) {
                            for (uint32_t k = 0; k < 12u; ++k) {
                                hash_f32(req.bone_matrices[bi][k]);
                            }
                        }
                    }
                    const bool must_build =
                        (!accel_.is_ready() || req.avatar != last_rt_avatar_ || last_rt_as_update_us_ == 0ULL);
                    // Always refit immediately whenever pose or morph target hash changed so rasterizer & BLAS have
                    // 0.0-px drift.
                    run_as_update = must_build || (pose_hash != last_rt_pose_hash_);
                    if (run_as_update) {
                        last_rt_avatar_ = req.avatar;
                        last_rt_pose_hash_ = pose_hash;
                        last_rt_as_update_us_ = now_us();
                    }
                }
                if (run_as_update) {
                    accel_.record_update(cmd, *req.avatar, skinned_vertices_buf_.device_address, req.frame_slot,
                                         req.frame_index, bindless_, false);
                    ++blas_update_count_;
                }
            }
        }

        // === PASS 3: G-Buffer + D32_S8 Depth/Stencil Prepass ===
        t_begin(rhi::GpuPassId::Prepass);
        trans(gb_n_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        trans(gb_mv_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        trans(gb_mat_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        trans(gb_misc_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        rhi::RhiDevice::cmd_transition_image(
            cmd, depth_stencil_.handle, depth_layout_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        depth_layout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        {
            std::array<VkRenderingAttachmentInfo, 4> cols{};
            auto init_col = [&](size_t idx, VkImageView v, VkClearColorValue cc) {
                cols[idx].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                cols[idx].imageView = v;
                cols[idx].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                cols[idx].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                cols[idx].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                cols[idx].clearValue.color = cc;
            };
            init_col(0, gb_n_.img.view, {{0.5f, 0.5f, 0.0f, 0.0f}});
            init_col(1, gb_mv_.img.view, {{0.0f, 0.0f, 0.0f, 0.0f}});
            init_col(2, gb_mat_.img.view, {{0u, 0u, 0u, 0u}});
            init_col(3, gb_misc_.img.view, {{0.0f, 0.0f, 0.0f, 0.0f}});

            VkRenderingAttachmentInfo d_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            d_att.imageView = depth_stencil_.view;
            d_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            d_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            d_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            d_att.clearValue.depthStencil = {1.0f, 0u};

            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = scissor;
            ri.layerCount = 1;
            ri.colorAttachmentCount = 4;
            ri.pColorAttachments = cols.data();
            ri.pDepthAttachment = &d_att;
            ri.pStencilAttachment = &d_att;

            vkCmdBeginRendering(cmd, &ri);
            bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso_prepass_, "prepass");
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_global_, 0, 1, &set0, 0, nullptr);
            vkCmdSetViewport(cmd, 0, 1, &neg_vp);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdBindIndexBuffer(cmd, req.avatar->index_buf.handle, 0, VK_INDEX_TYPE_UINT32);

            auto draw_prepass_bucket = [&](const std::vector<uint32_t>& list) {
                for (const uint32_t idx : list) {
                    const auto& d = cached_submeshes_[idx];
                    vkCmdSetCullMode(cmd, (d.cull_mode != 0u) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
                    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, d.stencil_ref);
                    GpuPushConstants pc{};
                    pc.frame_globals_bda = fg_alloc.device_address;
                    pc.vertex_buf_bda = skinned_vertices_buf_.device_address;
                    pc.aux_buf_bda = material_buf_.device_address;
                    pc.param0 = d.material_index;
                    pc.param1 =
                        d.flags | ((d.alpha_mode == AETHER_ALPHA_MODE_MASK) ? AETHER_SUBMESH_FLAG_ALPHA_CUTOUT : 0u);
                    vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
                    vkCmdDrawIndexed(cmd, d.index_count, 1, d.index_offset, 0, 0);
                }
            };
            draw_prepass_bucket(opaque_draw_indices_);
            draw_prepass_bucket(cutout_draw_indices_);
            vkCmdEndRendering(cmd);
        }

        trans(gb_n_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);
        trans(gb_mv_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);
        trans(gb_mat_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);
        trans(gb_misc_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);
        rhi::RhiDevice::cmd_transition_image(
            cmd, depth_stencil_.handle, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        depth_layout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        t_end(rhi::GpuPassId::Prepass);

        // === PASS 4: Inline RayQuery OR CSM Fallback Shadow/Rim/Bounce ===
        trans(rt_shadow_raw_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        trans(rim_occ_raw_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        trans(bounce_half_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        trans(aov_backscatter_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        trans(aov_cornea_spec_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (!use_rt) {
            constexpr VkClearColorValue kZeroClear{{0.0f, 0.0f, 0.0f, 0.0f}};
            constexpr VkImageSubresourceRange kColorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
            vkCmdClearColorImage(cmd, aov_backscatter_.img.handle, VK_IMAGE_LAYOUT_GENERAL, &kZeroClear, 1u,
                                 &kColorRange);
            vkCmdClearColorImage(cmd, aov_cornea_spec_.img.handle, VK_IMAGE_LAYOUT_GENERAL, &kZeroClear, 1u,
                                 &kColorRange);
        }

        if (use_rt) {
            t_begin(rhi::GpuPassId::RayTrace);
            std::array<VkDescriptorSet, 2> sets = {set0, bindless_.rt_set()};
            bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso_ray_effects_, "ray_effects");
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_rt_, 0, 2, sets.data(), 0,
                                    nullptr);
            GpuPushConstants pc{};
            pc.frame_globals_bda = fg_alloc.device_address;
            pc.draw_data_bda = accel_.tri_meta_bda();
            pc.vertex_buf_bda = skinned_vertices_buf_.device_address;
            pc.aux_buf_bda = material_buf_.device_address;
            pc.param0 = rt_shadow_raw_.storage_slot;
            pc.param1 = rim_occ_raw_.storage_slot;
            pc.param2 = bounce_half_.storage_slot;
            pc.param3 = ((w / 2u) << 16u) | (hist_valid ? 1u : 0u);
            vkCmdPushConstants(cmd, pipe_layout_rt_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, gx, gy, 1);
            t_end(rhi::GpuPassId::RayTrace);
        } else {
            t_begin(rhi::GpuPassId::CsmFallback);
            if (req.render_csm_shadow) {
                trans(csm_depth_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
                VkRenderingAttachmentInfo sd_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                sd_att.imageView = csm_depth_.img.view;
                sd_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                sd_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                sd_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                sd_att.clearValue.depthStencil = {1.0f, 0u};
                VkRenderingInfo sri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                sri.renderArea = {{0, 0}, {kCsmSize, kCsmSize}};
                sri.layerCount = 1;
                sri.pDepthAttachment = &sd_att;
                vkCmdBeginRendering(cmd, &sri);
                bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso_csm_shadow_, "csm_shadow");
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_global_, 0, 1, &set0, 0,
                                        nullptr);
                const VkViewport svp{0.0f, 0.0f, static_cast<float>(kCsmSize), static_cast<float>(kCsmSize),
                                     0.0f, 1.0f};
                const VkRect2D ssc{{0, 0}, {kCsmSize, kCsmSize}};
                vkCmdSetViewport(cmd, 0, 1, &svp);
                vkCmdSetScissor(cmd, 0, 1, &ssc);
                vkCmdBindIndexBuffer(cmd, req.avatar->index_buf.handle, 0, VK_INDEX_TYPE_UINT32);
                GpuPushConstants pc{};
                pc.frame_globals_bda = fg_alloc.device_address;
                pc.vertex_buf_bda = skinned_vertices_buf_.device_address;
                pc.param0 = 0;
                vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
                for (const uint32_t idx : opaque_draw_indices_) {
                    const auto& d = cached_submeshes_[idx];
                    const auto& gm = active_materials_scratch_[d.material_index];
                    if (!is_shadow_caster_submesh(d, gm)) {
                        continue;
                    }
                    vkCmdDrawIndexed(cmd, d.index_count, 1, d.index_offset, 0, 0);
                }
                vkCmdEndRendering(cmd);
                trans(csm_depth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            }

            bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso_csm_fallback_, "csm_fallback");
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_global_, 0, 1, &set0, 0, nullptr);
            GpuPushConstants pc{};
            pc.frame_globals_bda = fg_alloc.device_address;
            pc.param0 = rt_shadow_raw_.storage_slot;
            pc.param1 = rim_occ_raw_.storage_slot;
            pc.param2 = bounce_half_.storage_slot;
            pc.param3 = req.render_csm_shadow ? csm_depth_.sampled_slot : 0xFFFFu;
            vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, gx, gy, 1);
            t_end(rhi::GpuPassId::CsmFallback);
        }

        trans(rt_shadow_raw_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        trans(rim_occ_raw_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        trans(bounce_half_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);
        trans(aov_backscatter_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);
        trans(aov_cornea_spec_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);

        // === PASS 5: Multi-vendor Spatio-Temporal Denoise ===
        t_begin(rhi::GpuPassId::Denoise);
        trans(rt_shadow_denoised_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        trans(rim_occ_denoised_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        trans(bounce_denoised_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        {
            DenoiserInputView dvi{};
            dvi.width = w;
            dvi.height = h;
            dvi.frame_index =
                hist_valid
                    ? (static_cast<uint32_t>(req.frame_index) == 0u ? 1u : static_cast<uint32_t>(req.frame_index))
                    : 0u;
            dvi.temporal_alpha = (eff_mode >= 2u) ? 0.10f : 0.20f;
            dvi.frame_globals_bda = fg_alloc.device_address;
            dvi.raw_shadow_sampled_slot = rt_shadow_raw_.sampled_slot;
            dvi.denoised_shadow_storage_slot = rt_shadow_denoised_.storage_slot;
            dvi.raw_rim_sampled_slot = rim_occ_raw_.sampled_slot;
            dvi.denoised_rim_storage_slot = rim_occ_denoised_.storage_slot;
            dvi.raw_bounce_sampled_slot = bounce_half_.sampled_slot;
            dvi.denoised_bounce_storage_slot = bounce_denoised_.storage_slot;
            dvi.bounce_width = hw;
            dvi.bounce_height = hh;
            dvi.history_shadow_sampled_slot = hist_valid ? hb.prev_rt_shadow.sampled_slot : rt_shadow_raw_.sampled_slot;
            dvi.history_rim_sampled_slot = hist_valid ? hb.prev_rim_occ.sampled_slot : rim_occ_raw_.sampled_slot;
            dvi.history_bounce_sampled_slot = hist_valid ? hb.prev_bounce.sampled_slot : bounce_half_.sampled_slot;
            dvi.gb_normal_sampled_slot = gb_n_.sampled_slot;
            dvi.gb_mv_sampled_slot = gb_mv_.sampled_slot;
            dvi.depth_sampled_slot = depth_sampled_slot_;
            dvi.prev_depth_sampled_slot = hist_valid ? hb.prev_depth.sampled_slot : depth_sampled_slot_;
            dvi.prev_normal_sampled_slot = hist_valid ? hb.prev_normal.sampled_slot : gb_n_.sampled_slot;
            (void)denoiser_.execute(cmd, dvi);
        }
        trans(rt_shadow_denoised_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);
        trans(rim_occ_denoised_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);
        trans(bounce_denoised_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);
        t_end(rhi::GpuPassId::Denoise);

        // Snapshot current view's G-buffer / depth / denoised AOVs into its own TemporalHistoryBank
        // immediately after Pass 5 so interleaved Broadcast <-> LightingStage views never cross-contaminate.
        if (allow_view_hist) {
            auto copy_hist = [&](RegisteredImage& src, RegisteredImage& dst, VkPipelineStageFlags2 src_st,
                                 VkAccessFlags2 src_ac, uint32_t cw, uint32_t ch) {
                const VkImageLayout src_prev = src.layout;
                const bool dst_undef = (dst.layout == VK_IMAGE_LAYOUT_UNDEFINED);
                trans(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_st, src_ac, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT);
                trans(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      dst_undef ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      dst_undef ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.extent = {cw, ch, 1};
                vkCmdCopyImage(cmd, src.img.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.img.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                trans(src, src_prev, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT);
                trans(dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT);
            };
            copy_hist(gb_n_, hb.prev_normal, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, w, h);
            copy_hist(rt_shadow_denoised_, hb.prev_rt_shadow, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT, w, h);
            copy_hist(rim_occ_denoised_, hb.prev_rim_occ, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT, w, h);
            copy_hist(bounce_denoised_, hb.prev_bounce, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT, hw, hh);

            const bool pd_undef = (hb.prev_depth.layout == VK_IMAGE_LAYOUT_UNDEFINED);
            rhi::RhiDevice::cmd_transition_image(
                cmd, depth_stencil_.handle, depth_layout_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
            depth_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            trans(hb.prev_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  pd_undef ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  pd_undef ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
            std::array<VkImageCopy, 2> dregions{};
            dregions[0].srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            dregions[0].dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            dregions[0].extent = {w, h, 1};
            dregions[1].srcSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
            dregions[1].dstSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
            dregions[1].extent = {w, h, 1};
            vkCmdCopyImage(cmd, depth_stencil_.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hb.prev_depth.img.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, dregions.data());
            rhi::RhiDevice::cmd_transition_image(
                cmd, depth_stencil_.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
            depth_layout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            trans(hb.prev_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        }

        // === PASSES 6, 7, 8: Live2D NPR Opaque/Cutout -> Inverted-Hull Outlines -> Transparent NPR ===
        trans(hdr_scene_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        {
            VkRenderingAttachmentInfo hdr_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            hdr_att.imageView = hdr_scene_.img.view;
            hdr_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            hdr_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            hdr_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            hdr_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

            VkRenderingAttachmentInfo d_ro{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            d_ro.imageView = depth_stencil_.view;
            d_ro.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            d_ro.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            d_ro.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = scissor;
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &hdr_att;
            ri.pDepthAttachment = &d_ro;
            ri.pStencilAttachment = &d_ro;

            vkCmdBeginRendering(cmd, &ri);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_global_, 0, 1, &set0, 0, nullptr);
            vkCmdSetViewport(cmd, 0, 1, &neg_vp);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdBindIndexBuffer(cmd, req.avatar->index_buf.handle, 0, VK_INDEX_TYPE_UINT32);

            t_begin(rhi::GpuPassId::Live2dNpr);
            const bool use_wire = req.wireframe && wireframe_supported_ && (pso_live2d_opaque_wire_ != VK_NULL_HANDLE);
            auto draw_npr_list = [&](const std::vector<uint32_t>& list, bool transparent_bucket) {
                VkPipeline cur = VK_NULL_HANDLE;
                for (const uint32_t idx : list) {
                    const auto& d = cached_submeshes_[idx];
                    const auto& gm = active_materials_scratch_[d.material_index];
                    const bool is_add = transparent_bucket && is_additive_submesh(d, gm);
                    VkPipeline want = VK_NULL_HANDLE;
                    if (transparent_bucket) {
                        if (use_wire) {
                            want = pso_live2d_blend_wire_;
                        } else if (is_add) {
                            want = (d.cull_mode != 0u) ? pso_live2d_additive_nocull_ : pso_live2d_additive_cull_;
                        } else {
                            want = (d.cull_mode != 0u) ? pso_live2d_blend_ : pso_live2d_blend_cull_;
                        }
                    } else {
                        want = (d.cull_mode != 0u)
                                   ? (use_wire ? pso_live2d_double_sided_wire_ : pso_live2d_double_sided_)
                                   : (use_wire ? pso_live2d_opaque_wire_ : pso_live2d_opaque_);
                    }
                    if (want != cur) {
                        bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want,
                                              transparent_bucket ? "live2d_blend" : "live2d_opaque_or_ds");
                        cur = want;
                    }
                    GpuPushConstants pc{};
                    pc.frame_globals_bda = fg_alloc.device_address;
                    pc.vertex_buf_bda = skinned_vertices_buf_.device_address;
                    pc.aux_buf_bda = material_buf_.device_address;
                    pc.param0 = d.material_index;
                    pc.param1 = d.flags |
                                ((d.alpha_mode == AETHER_ALPHA_MODE_MASK) ? AETHER_SUBMESH_FLAG_ALPHA_CUTOUT : 0u) |
                                (transparent_bucket ? AETHER_SUBMESH_FLAG_TRANSPARENT : 0u) |
                                (is_add ? AETHER_SUBMESH_FLAG_ADDITIVE : 0u);
                    vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
                    vkCmdDrawIndexed(cmd, d.index_count, 1, d.index_offset, 0, 0);
                }
            };
            draw_npr_list(opaque_draw_indices_, false);
            draw_npr_list(cutout_draw_indices_, false);
            t_end(rhi::GpuPassId::Live2dNpr);

            // Pass 7: Inverted-hull outlines (front-face culled back faces)
            t_begin(rhi::GpuPassId::OutlineHull);
            const float outline_mul = std::max(0.0f, req.outline_width_scale) * std::max(0.0f, rig.outline_width_scale);
            if (!use_wire && outline_mul > 0.0f) {
                bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso_outline_hull_, "outline_hull");
                for (const uint32_t idx : opaque_draw_indices_) {
                    const auto& d = cached_submeshes_[idx];
                    const auto& gm = active_materials_scratch_[d.material_index];
                    if (!is_outline_enabled_submesh(d, gm)) {
                        continue;
                    }
                    GpuPushConstants pc{};
                    pc.frame_globals_bda = fg_alloc.device_address;
                    pc.vertex_buf_bda = skinned_vertices_buf_.device_address;
                    pc.aux_buf_bda = material_buf_.device_address;
                    pc.param0 = d.material_index;
                    pc.param1 = d.flags;
                    pc.fparam0 = outline_mul;
                    vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
                    vkCmdDrawIndexed(cmd, d.index_count, 1, d.index_offset, 0, 0);
                }
            }
            t_end(rhi::GpuPassId::OutlineHull);

            // Pass 8: Sorted Transparent NPR (eye highlights, lashes, translucent garments)
            t_begin(rhi::GpuPassId::TransparentPass);
            draw_npr_list(transparent_draw_indices_, true);
            t_end(rhi::GpuPassId::TransparentPass);
            vkCmdEndRendering(cmd);
        }

        trans(hdr_scene_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);

        // === PASS 9: Screen-Space Crease Lines (masked by D32_S8 stencil & 0x01) ===
        t_begin(rhi::GpuPassId::CreaseLines);
        trans(crease_mask_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        {
            GpuPushConstants pc{};
            pc.frame_globals_bda = fg_alloc.device_address;
            pc.param0 = crease_mask_.storage_slot;
            pc.param2 = w;
            pc.param3 = h;
            pc.fparam0 = rig.crease_threshold_deg;
            bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso_crease_lines_, "crease_lines");
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_global_, 0, 1, &set0, 0, nullptr);
            vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, gx, gy, 1);
        }
        trans(crease_mask_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        t_end(rhi::GpuPassId::CreaseLines);

        // === PASS 10: 6-Mip Dual-Kawase Premultiplied Bloom ===
        t_begin(rhi::GpuPassId::PostBloom);
        bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso_bloom_down_, "bloom_down");
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_global_, 0, 1, &set0, 0, nullptr);
        const bool subrect_bloom = (w < rw) || (h < rh);
        auto pack_subrect = [](uint32_t psw, uint32_t psh, uint32_t asw, uint32_t ash) noexcept -> uint64_t {
            return static_cast<uint64_t>(std::min(psw, 0xFFFFu)) |
                   (static_cast<uint64_t>(std::min(psh, 0xFFFFu)) << 16u) |
                   (static_cast<uint64_t>(std::min(asw, 0xFFFFu)) << 32u) |
                   (static_cast<uint64_t>(std::min(ash, 0xFFFFu)) << 48u);
        };
        std::array<uint32_t, kBloomMipCount> act_mip_w{};
        std::array<uint32_t, kBloomMipCount> act_mip_h{};
        uint32_t phys_sw = rw;
        uint32_t phys_sh = rh;
        uint32_t act_sw = w;
        uint32_t act_sh = h;
        uint32_t src_slot = hdr_scene_.sampled_slot;
        for (uint32_t i = 0; i < kBloomMipCount; ++i) {
            auto& dst = bloom_mips_[i];
            trans(dst, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
            const uint32_t act_dw = subrect_bloom ? std::min(dst.img.width, std::max(act_sw / 2u, 1u)) : dst.img.width;
            const uint32_t act_dh =
                subrect_bloom ? std::min(dst.img.height, std::max(act_sh / 2u, 1u)) : dst.img.height;
            act_mip_w[i] = act_dw;
            act_mip_h[i] = act_dh;
            GpuPushConstants pc{};
            pc.vertex_buf_bda = subrect_bloom ? pack_subrect(phys_sw, phys_sh, act_sw, act_sh) : 0ULL;
            pc.param0 = src_slot;
            pc.param1 = dst.storage_slot;
            pc.param2 = act_dw;
            pc.param3 = act_dh;
            pc.fparam0 = (i == 0) ? rig.bloom_threshold : 0.0f;
            pc.fparam1 = 1.0f / static_cast<float>(phys_sw);
            pc.fparam2 = 1.0f / static_cast<float>(phys_sh);
            vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, (act_dw + 7u) / 8u, (act_dh + 7u) / 8u, 1);
            trans(dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            phys_sw = dst.img.width;
            phys_sh = dst.img.height;
            act_sw = act_dw;
            act_sh = act_dh;
            src_slot = dst.sampled_slot;
        }

        bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso_bloom_up_, "bloom_up");
        for (int32_t i = static_cast<int32_t>(kBloomMipCount) - 2; i >= 0; --i) {
            const auto& lo = bloom_mips_[static_cast<size_t>(i) + 1U];
            auto& hi = bloom_mips_[static_cast<size_t>(i)];
            trans(hi, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
            const uint32_t lo_act_w = act_mip_w[static_cast<size_t>(i) + 1U];
            const uint32_t lo_act_h = act_mip_h[static_cast<size_t>(i) + 1U];
            const uint32_t hi_act_w = act_mip_w[static_cast<size_t>(i)];
            const uint32_t hi_act_h = act_mip_h[static_cast<size_t>(i)];
            GpuPushConstants pc{};
            pc.vertex_buf_bda = subrect_bloom ? pack_subrect(lo.img.width, lo.img.height, lo_act_w, lo_act_h) : 0ULL;
            pc.param0 = lo.sampled_slot;
            pc.param1 = hi.storage_slot;
            pc.param2 = hi_act_w;
            pc.param3 = hi_act_h;
            pc.fparam1 = 1.0f / static_cast<float>(lo.img.width);
            pc.fparam2 = 1.0f / static_cast<float>(lo.img.height);
            vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, (hi_act_w + 7u) / 8u, (hi_act_h + 7u) / 8u, 1);
            trans(hi, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        }
        t_end(rhi::GpuPassId::PostBloom);

        // === PASS 11: Composite + Tonemap -> OUT_RGBA16F ===
        t_begin(rhi::GpuPassId::ToneMap);
        trans(out_rgba16f_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        {
            GpuPushConstants pc{};
            pc.frame_globals_bda = fg_alloc.device_address;
            pc.param0 = hdr_scene_.sampled_slot;
            pc.param1 = out_rgba16f_.storage_slot;
            pc.param2 = w;
            pc.param3 = h;
            bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso_tone_composite_, "tone_composite");
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_global_, 0, 1, &set0, 0, nullptr);
            vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, gx, gy, 1);
        }
        trans(out_rgba16f_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);
        t_end(rhi::GpuPassId::ToneMap);
    } else if (use_external) {
        record_external_color_blit(cmd, req, w, h);
        trans(out_rgba16f_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);
    } else {
        trans(out_rgba16f_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue zero_col{{0.0f, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, out_rgba16f_.img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero_col, 1, &sub);
        trans(out_rgba16f_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);
    }

    // === PASS 12: Output Encode -> 3-Slot Ring OUT_BGRA8* & OUT_NV12 (kept in GENERAL for T4 consumer) ===
    t_begin(rhi::GpuPassId::OutputEncode);
    auto encode_bgra_target = [&](RegisteredImage& dst_bgra, bool straight_alpha, bool bottom_up) {
        if (dst_bgra.img.handle == VK_NULL_HANDLE) {
            return;
        }
        RegisteredImage& encode_dst = bgra8_supports_storage_ ? dst_bgra : out_rgba8_scratch_;
        trans(encode_dst, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

        GpuPushConstants pc{};
        pc.frame_globals_bda = fg_alloc.device_address;
        pc.param0 = out_rgba16f_.sampled_slot;
        pc.param1 = encode_dst.storage_slot;
        pc.param2 = w;
        pc.param3 = h;
        pc.fparam0 = straight_alpha ? 1.0f : 0.0f;
        pc.fparam1 = bottom_up ? 1.0f : 0.0f;
        bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso_encode_bgra_, "encode_bgra");
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_global_, 0, 1, &set0, 0, nullptr);
        vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, gx, gy, 1);

        if (!bgra8_supports_storage_) {
            trans(out_rgba8_scratch_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            trans(dst_bgra, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageBlit bl{};
            bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bl.srcOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
            bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bl.dstOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
            vkCmdBlitImage(cmd, out_rgba8_scratch_.img.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dst_bgra.img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
            trans(dst_bgra, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
        }
    };

    if (req.view_kind == RenderViewKind::Broadcast) {
        if (req.encode_output_bgra) {
            encode_bgra_target(os.bgra8, !req.premul_alpha, false);
            if (req.need_straight_alpha) {
                encode_bgra_target(os.bgra8_straight, true, false);
            }
            if (req.need_bottom_up) {
                encode_bgra_target(os.bottom_up_bgra8, !req.premul_alpha, true);
            }
        }

        if (req.encode_output_nv12) {
            trans(os.nv12_y, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
            trans(os.nv12_uv, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
            {
                GpuPushConstants pc{};
                pc.frame_globals_bda = fg_alloc.device_address;
                pc.param0 = out_rgba16f_.sampled_slot;
                pc.param1 = os.nv12_y.storage_slot;
                pc.param2 = w;
                pc.param3 = h;
                pc.fparam0 = srgb_to_linear_ch(req.non_alpha_bg_rgb[0]);
                pc.fparam1 = srgb_to_linear_ch(req.non_alpha_bg_rgb[1]);
                pc.fparam2 = srgb_to_linear_ch(req.non_alpha_bg_rgb[2]);
                bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso_encode_nv12_, "encode_nv12");
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_global_, 0, 1, &set0, 0,
                                        nullptr);
                vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
                vkCmdDispatch(cmd, gx, gy, 1);
            }
        }
    }

    cmd_buffer_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
    t_end(rhi::GpuPassId::OutputEncode);
    t_end(rhi::GpuPassId::FrameTotal);
    if (record_stage_scope) {
        timing_.end_scope(cmd, fslot, rhi::GpuPassId::LightingStage);
    }

    if (has_avatar && allow_view_hist) {
        hb.have_prev_view_proj = true;
    } else if (!has_avatar && allow_view_hist) {
        hb.have_prev_view_proj = false;
    }
}

void FrameGraph::record_external_color_blit(VkCommandBuffer cmd, const FrameRenderRequest& req, uint32_t w,
                                            uint32_t h) noexcept {
    // Leaves OUT_RGBA16F in TRANSFER_DST_OPTIMAL holding either the external image or a transparent clear.
    rhi::RhiDevice::cmd_transition_image(cmd, out_rgba16f_.img.handle, out_rgba16f_.layout,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                         VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                         VK_ACCESS_2_TRANSFER_WRITE_BIT);
    out_rgba16f_.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    const VkImageSubresourceRange sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const uint32_t gfx = device_->graphics_family();
    const bool foreign = req.external_is_foreign;
    auto ext_barrier = [&](VkImageLayout old_l, VkImageLayout new_l, VkPipelineStageFlags2 src_st,
                           VkAccessFlags2 src_ac, VkPipelineStageFlags2 dst_st, VkAccessFlags2 dst_ac, uint32_t src_qf,
                           uint32_t dst_qf) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = src_st;
        b.srcAccessMask = src_ac;
        b.dstStageMask = dst_st;
        b.dstAccessMask = dst_ac;
        b.oldLayout = old_l;
        b.newLayout = new_l;
        b.srcQueueFamilyIndex = src_qf;
        b.dstQueueFamilyIndex = dst_qf;
        b.image = req.external_color_image;
        b.subresourceRange = sub;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    };

    if (req.external_layout == VK_IMAGE_LAYOUT_UNDEFINED && req.external_staging_buffer == VK_NULL_HANDLE) {
        // First frame of the shared-texture path: establish GENERAL; its contents are not valid yet.
        ext_barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, VK_QUEUE_FAMILY_IGNORED,
                    VK_QUEUE_FAMILY_IGNORED);
        if (foreign) {
            ext_barrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, gfx, VK_QUEUE_FAMILY_EXTERNAL);
        }
        const VkClearColorValue zero_col{{0.0f, 0.0f, 0.0f, 0.0f}};
        vkCmdClearColorImage(cmd, out_rgba16f_.img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero_col, 1, &sub);
        return;
    }

    VkImageLayout cur = req.external_layout;
    if (req.external_staging_buffer != VK_NULL_HANDLE) {
        ext_barrier(cur, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {req.external_width, req.external_height, 1};
        vkCmdCopyBufferToImage(cmd, req.external_staging_buffer, req.external_color_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        ext_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
    } else {
        // Acquire the D3D11-written texture from the external queue family.
        ext_barrier(cur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    foreign ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED,
                    foreign ? gfx : VK_QUEUE_FAMILY_IGNORED);
    }

    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {static_cast<int32_t>(req.external_width), static_cast<int32_t>(req.external_height), 1};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.dstOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
    vkCmdBlitImage(cmd, req.external_color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out_rgba16f_.img.handle,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);

    // Hand the image back in GENERAL (released to D3D11 on the shared path).
    ext_barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                foreign ? gfx : VK_QUEUE_FAMILY_IGNORED, foreign ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED);
}

void FrameGraph::record_ui_passes(VkCommandBuffer cmd, const FrameRenderRequest& req) noexcept {
    if (!pipelines_ready_ || cmd == VK_NULL_HANDLE || req.rig == nullptr) {
        return;
    }

    const bool is_stage = (req.view_kind == RenderViewKind::LightingStage);
    RegisteredImage* dst_vp = &viewport_out_;
    uint32_t rt_w = cfg_.width;
    uint32_t rt_h = cfg_.height;
    if (is_stage) {
        if (req.stage_output_slot == FrameRenderRequest::StageOutputSlot::QuadTop) {
            dst_vp = &quad_top_out_;
            rt_w = (quad_top_rt_w_ > 0u) ? quad_top_rt_w_ : std::max(cfg_.width / 2u, 16u);
            rt_h = (quad_top_rt_h_ > 0u) ? quad_top_rt_h_ : std::max(cfg_.height / 2u, 16u);
        } else if (req.stage_output_slot == FrameRenderRequest::StageOutputSlot::QuadRight) {
            dst_vp = &quad_right_out_;
            rt_w = (quad_right_rt_w_ > 0u) ? quad_right_rt_w_ : std::max(cfg_.width / 2u, 16u);
            rt_h = (quad_right_rt_h_ > 0u) ? quad_right_rt_h_ : std::max(cfg_.height / 2u, 16u);
        } else {
            dst_vp = &stage_viewport_out_;
            rt_w = (stage_rt_w_ > 0u) ? stage_rt_w_ : std::max(cfg_.width, 16u);
            rt_h = (stage_rt_h_ > 0u) ? stage_rt_h_ : std::max(cfg_.height, 16u);
        }
    }
    if (dst_vp->img.handle == VK_NULL_HANDLE) {
        return;
    }
    rt_w = (dst_vp->img.width > 0u) ? dst_vp->img.width : rt_w;
    rt_h = (dst_vp->img.height > 0u) ? dst_vp->img.height : rt_h;
    const uint32_t eff_w = effective_output_width();
    const uint32_t eff_h = effective_output_height();
    const detail::StageRenderRegion sreg =
        is_stage ? detail::compute_stage_render_region(req, eff_w, eff_h, rt_w, rt_h)
                 : detail::StageRenderRegion{
                       rt_w, rt_h, eff_w, eff_h, static_cast<float>(rt_w) / static_cast<float>(std::max(1u, rt_h)),
                       1.0f, 1.0f, 1.0f,  1.0f,  1.0f,
                       1.0f, 1.0f, 1.0f};
    const uint32_t w = sreg.target_w;
    const uint32_t h = sreg.target_h;
    if (is_stage) {
        if (req.stage_output_slot == FrameRenderRequest::StageOutputSlot::QuadTop) {
            quad_top_active_w_ = w;
            quad_top_active_h_ = h;
        } else if (req.stage_output_slot == FrameRenderRequest::StageOutputSlot::QuadRight) {
            quad_right_active_w_ = w;
            quad_right_active_h_ = h;
        } else {
            stage_active_w_ = w;
            stage_active_h_ = h;
        }
    }
    const uint32_t fslot = req.frame_slot % rhi::GpuTimingRing::kFrameSlots;
    VkDescriptorSet set0 = bindless_.global_set();

    auto trans = [&](RegisteredImage& r, VkImageLayout dst_lay, VkPipelineStageFlags2 src_st, VkAccessFlags2 src_ac,
                     VkPipelineStageFlags2 dst_st, VkAccessFlags2 dst_ac) {
        rhi::RhiDevice::cmd_transition_image(cmd, r.img.handle, r.layout, dst_lay, src_st, src_ac, dst_st, dst_ac,
                                             VK_IMAGE_ASPECT_COLOR_BIT);
        r.layout = dst_lay;
    };

    if (out_rgba16f_.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        timing_.cmd_reset_frame(cmd, fslot);
        rebar_.begin_frame(req.frame_index, req.completed_frame_index);
        trans(out_rgba16f_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue zero_col{{0.0f, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, out_rgba16f_.img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero_col, 1, &sub);
        trans(out_rgba16f_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    auto fg_alloc_res = rebar_.allocate(sizeof(GpuFrameGlobals), 64);
    if (!fg_alloc_res) {
        return;
    }
    const auto& fg_alloc = fg_alloc_res.value();
    core::ViewportAspectLayout aspect_layout{};
    PreviewCamera heat_cam = req.camera;
    float ui_aspect = sreg.proj_aspect;
    float ui_w_f = static_cast<float>(w);
    float ui_h_f = static_cast<float>(h);
    if (!is_stage) {
        aspect_layout = core::compute_viewport_aspect_layout(static_cast<float>(rt_w), static_cast<float>(rt_h),
                                                             static_cast<float>(eff_w), static_cast<float>(eff_h),
                                                             req.viewport_aspect_mode);
        heat_cam.fovy_rad = aspect_layout.effective_fovy_rad(req.camera.fovy_rad);
        ui_aspect = aspect_layout.frame_aspect;
        ui_w_f = aspect_layout.frame_width();
        ui_h_f = aspect_layout.frame_height();
    }
    GpuFrameGlobals fg{};
    for (int i = 0; i < 4; ++i) {
        fg.target_uv_scale[i] = 1.0f;
        fg.target_half_uv_scale[i] = 1.0f;
    }
    Mat4Vec3 eye{};
    build_camera_matrices(heat_cam, ui_aspect, fg.view, fg.inv_view, fg.proj, fg.inv_proj, fg.view_proj, eye);
    fg.viewport_size[0] = ui_w_f;
    fg.viewport_size[1] = ui_h_f;
    fg.viewport_size[2] = 1.0f / ui_w_f;
    fg.viewport_size[3] = 1.0f / ui_h_f;
    fg.stage_camera_params[0] = heat_cam.is_orthographic ? 1.0f : 0.0f;
    fg.stage_camera_params[1] = heat_cam.ortho_height;
    fg.stage_camera_params[2] = is_stage ? static_cast<float>(static_cast<uint8_t>(req.stage_diag_mode)) : 0.0f;
    fg.stage_camera_params[3] = (is_stage && req.stage_fast_panning) ? 1.0f : 0.0f;
    const StudioLightSpec* dom_key = req.rig->dominant_key_light();
    if (dom_key != nullptr) {
        const Mat4Vec3 kd = studio_light_dir_to_light(*dom_key, {0.0f, 1.32f, 0.0f}, {eye.x, eye.y, eye.z});
        fg.photometric_params0[0] = kd.x;
        fg.photometric_params0[1] = kd.y;
        fg.photometric_params0[2] = kd.z;
        fg.photometric_params0[3] = 1.0f;
        fg.photometric_params1[0] = dom_key->tint_rgb[0] * dom_key->intensity_ev100;
        fg.photometric_params1[1] = dom_key->tint_rgb[1] * dom_key->intensity_ev100;
        fg.photometric_params1[2] = dom_key->tint_rgb[2] * dom_key->intensity_ev100;
        fg.photometric_params1[3] = 1.0f;
    } else {
        fg.photometric_params0[0] = 0.42f;
        fg.photometric_params0[1] = 0.56f;
        fg.photometric_params0[2] = 0.71f;
        fg.photometric_params0[3] = 0.0f;
        fg.photometric_params1[0] = 0.0f;
        fg.photometric_params1[1] = 0.0f;
        fg.photometric_params1[2] = 0.0f;
        fg.photometric_params1[3] = 0.0f;
    }
    std::memcpy(fg_alloc.mapped, &fg, sizeof(fg));

    if (!is_stage) {
        timing_.cmd_reset_scope(cmd, fslot, rhi::GpuPassId::ViewportCompose);
        timing_.begin_scope(cmd, fslot, rhi::GpuPassId::ViewportCompose);
    }

    // Pass 13A: Studio Viewport Checkerboard + Optional Alt+C Skeleton Heatmap / Lighting Stage Schematic Overlay
    trans(*dst_vp, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    {
        VkRenderingAttachmentInfo col_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        col_att.imageView = dst_vp->img.view;
        col_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        col_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        col_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        col_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {rt_w, rt_h}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &col_att;
        vkCmdBeginRendering(cmd, &ri);

        const VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        const VkRect2D sc{{0, 0}, {w, h}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso_viewport_compose_, "viewport_compose");
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_global_, 0, 1, &set0, 0, nullptr);
        GpuPushConstants pc{};
        pc.frame_globals_bda = fg_alloc.device_address;
        pc.param0 = out_rgba16f_.sampled_slot;
        if (is_stage) {
            pc.param1 = (req.show_checkerboard ? 1u : 0u) | 4u;
            pc.param2 = std::bit_cast<uint32_t>(sreg.uv_scale_x);
            pc.param3 = std::bit_cast<uint32_t>(sreg.uv_scale_y);
            pc.fparam0 = 0.0f;
            pc.fparam1 = 0.0f;
            pc.fparam2 = sreg.uv_max_u;
            pc.fparam3 = sreg.uv_max_v;
        } else {
            pc.param1 = (req.show_checkerboard ? 1u : 0u) | (aspect_layout.explicit_matte ? 2u : 0u);
            pc.param2 = std::bit_cast<uint32_t>(aspect_layout.uv_du);
            pc.param3 = std::bit_cast<uint32_t>(aspect_layout.uv_dv);
            pc.fparam0 = aspect_layout.norm_x0;
            pc.fparam1 = aspect_layout.norm_y0;
            pc.fparam2 = aspect_layout.norm_x1;
            pc.fparam3 = aspect_layout.norm_y1;
        }
        vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        if (req.show_skeleton_heatmap || req.show_rig_colliders) {
            const float av_scale = std::clamp(req.avatar_scale, 0.25f, 4.0f);
            const float av_yaw_rad = req.avatar_rot_y_deg * (3.1415926535f / 180.0f);
            const float av_cos = std::cos(av_yaw_rad);
            const float av_sin = std::sin(av_yaw_rad);
            uint32_t bone_cnt = 0;
            auto xform_pt = [&](const std::array<float, 3>& p) -> std::array<float, 3> {
                return {
                    (av_cos * p[0] + av_sin * p[2]) * av_scale + req.avatar_pos[0],
                    p[1] * av_scale + req.avatar_pos[1],
                    (-av_sin * p[0] + av_cos * p[2]) * av_scale + req.avatar_pos[2],
                };
            };
            auto to_ndc = [&](const std::array<float, 3>& p, float out_xy[2]) -> bool {
                const float x = fg.view_proj.col0[0] * p[0] + fg.view_proj.col1[0] * p[1] +
                                fg.view_proj.col2[0] * p[2] + fg.view_proj.col3[0];
                const float y = fg.view_proj.col0[1] * p[0] + fg.view_proj.col1[1] * p[1] +
                                fg.view_proj.col2[1] * p[2] + fg.view_proj.col3[1];
                const float pw = fg.view_proj.col0[3] * p[0] + fg.view_proj.col1[3] * p[1] +
                                 fg.view_proj.col2[3] * p[2] + fg.view_proj.col3[3];
                if (pw <= 0.05f) {
                    return false;
                }
                out_xy[0] = x / pw;
                // Negative viewport mapped +Y up -> top of screen (`clip.y < 0` in heatmap positive viewport)
                out_xy[1] = -(y / pw);
                return true;
            };
            auto push_seg = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, float width_px,
                                uint32_t cat, float stress, uint32_t tag) {
                if (bone_cnt >= heatmap_scratch_.size()) {
                    return;
                }
                float p0[2]{}, p1[2]{};
                if (!to_ndc(a, p0) || !to_ndc(b, p1)) {
                    return;
                }
                auto& item = heatmap_scratch_[bone_cnt++];
                item.p0_ndc_weight[0] = p0[0];
                item.p0_ndc_weight[1] = p0[1];
                item.p0_ndc_weight[2] = stress;
                item.p0_ndc_weight[3] = width_px;
                item.p1_ndc_pad[0] = p1[0];
                item.p1_ndc_pad[1] = p1[1];
                item.p1_ndc_pad[2] = static_cast<float>(cat);
                item.p1_ndc_pad[3] = static_cast<float>(tag);
            };
            if (req.show_skeleton_heatmap && req.avatar != nullptr && !req.avatar->joints.empty()) {
                const auto& jpos = (req.avatar->joint_anim_world_pos.size() == req.avatar->joints.size())
                                       ? req.avatar->joint_anim_world_pos
                                       : req.avatar->joint_rest_world_pos;
                for (size_t j = 0; j < req.avatar->joints.size() && bone_cnt < heatmap_scratch_.size(); ++j) {
                    const int32_t p = req.avatar->joints[j].parent_index;
                    if (p < 0 || static_cast<size_t>(p) >= jpos.size() || j >= jpos.size()) {
                        continue;
                    }
                    const float stress = (req.joint_stress != nullptr && j < req.joint_stress_count)
                                             ? std::clamp(req.joint_stress[j], 0.0f, 1.0f)
                                             : 0.25f;
                    const uint32_t cat = (stress > 0.85f) ? 2u : ((stress > 0.60f) ? 1u : 0u);
                    push_seg(xform_pt(jpos[static_cast<size_t>(p)]), xform_pt(jpos[j]), 4.0f, cat, stress,
                             static_cast<uint32_t>(j));
                }
            }
            if (req.show_rig_colliders) {
                if (req.spring_segments != nullptr) {
                    for (uint32_t s = 0; s < req.spring_segment_count && bone_cnt < heatmap_scratch_.size(); ++s) {
                        const auto& seg = req.spring_segments[s];
                        const uint32_t cat = (seg.in_contact != 0U) ? 6u : 5u;
                        push_seg(xform_pt({seg.head[0], seg.head[1], seg.head[2]}),
                                 xform_pt({seg.tail[0], seg.tail[1], seg.tail[2]}), 2.2f, cat, 0.5f, s);
                    }
                }
                if (req.spring_colliders != nullptr) {
                    constexpr float kTwoPiF = 6.283185307f;
                    auto add_pt = [](const std::array<float, 3>& o, const Mat4Vec3& u, const Mat4Vec3& v, float r,
                                     float cu, float sv) -> std::array<float, 3> {
                        return {o[0] + r * (cu * u.x + sv * v.x), o[1] + r * (cu * u.y + sv * v.y),
                                o[2] + r * (cu * u.z + sv * v.z)};
                    };
                    auto push_ring = [&](const std::array<float, 3>& center, const Mat4Vec3& u, const Mat4Vec3& v,
                                         float r, uint32_t segs, float arc_rad, uint32_t cat, uint32_t tag) {
                        std::array<float, 3> prev = add_pt(center, u, v, r, 1.0f, 0.0f);
                        for (uint32_t k = 1; k <= segs; ++k) {
                            const float ang = arc_rad * (static_cast<float>(k) / static_cast<float>(segs));
                            const std::array<float, 3> cur = add_pt(center, u, v, r, std::cos(ang), std::sin(ang));
                            push_seg(prev, cur, 2.0f, cat, 1.0f, tag);
                            prev = cur;
                        }
                    };
                    for (uint32_t c = 0; c < req.spring_collider_count && bone_cnt < heatmap_scratch_.size(); ++c) {
                        const auto& col = req.spring_colliders[c];
                        const uint32_t cat = (col.in_contact != 0U) ? 4u : 3u;
                        const std::array<float, 3> p0 = xform_pt({col.p0[0], col.p0[1], col.p0[2]});
                        const std::array<float, 3> p1 = xform_pt({col.p1[0], col.p1[1], col.p1[2]});
                        const float r = std::max(col.radius, 0.003f) * av_scale;
                        const Mat4Vec3 ax_x{av_cos, 0.0f, -av_sin};
                        const Mat4Vec3 ax_y{0.0f, 1.0f, 0.0f};
                        const Mat4Vec3 ax_z{av_sin, 0.0f, av_cos};
                        const Mat4Vec3 d{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
                        const float dlen = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                        if (col.shape == 0U || dlen < 1e-4f) {
                            // Procedural 3-axis wireframe sphere
                            push_ring(p0, ax_x, ax_y, r, 14, kTwoPiF, cat, c);
                            push_ring(p0, ax_x, ax_z, r, 14, kTwoPiF, cat, c);
                            push_ring(p0, ax_y, ax_z, r, 14, kTwoPiF, cat, c);
                        } else {
                            // Procedural 3-axis wireframe capsule (3 equatorial rings + 4 rails + 4 hemisphere arcs)
                            const Mat4Vec3 axis = {d.x / dlen, d.y / dlen, d.z / dlen};
                            const Mat4Vec3 ref =
                                (std::abs(axis.y) < 0.9f) ? Mat4Vec3{0.0f, 1.0f, 0.0f} : Mat4Vec3{1.0f, 0.0f, 0.0f};
                            const Mat4Vec3 u = normalize3(cross3(axis, ref));
                            const Mat4Vec3 v = cross3(axis, u);
                            const std::array<float, 3> pmid{0.5f * (p0[0] + p1[0]), 0.5f * (p0[1] + p1[1]),
                                                            0.5f * (p0[2] + p1[2])};
                            push_ring(p0, u, v, r, 12, kTwoPiF, cat, c);
                            push_ring(pmid, u, v, r, 12, kTwoPiF, cat, c);
                            push_ring(p1, u, v, r, 12, kTwoPiF, cat, c);
                            for (const Mat4Vec3& rad : {u, Mat4Vec3{-u.x, -u.y, -u.z}, v, Mat4Vec3{-v.x, -v.y, -v.z}}) {
                                push_seg({p0[0] + r * rad.x, p0[1] + r * rad.y, p0[2] + r * rad.z},
                                         {p1[0] + r * rad.x, p1[1] + r * rad.y, p1[2] + r * rad.z}, 2.0f, cat, 1.0f, c);
                            }
                            const Mat4Vec3 neg_axis{-axis.x, -axis.y, -axis.z};
                            push_ring(p0, u, neg_axis, r, 6, 3.14159265f, cat, c);
                            push_ring(p0, v, neg_axis, r, 6, 3.14159265f, cat, c);
                            push_ring(p1, u, axis, r, 6, 3.14159265f, cat, c);
                            push_ring(p1, v, axis, r, 6, 3.14159265f, cat, c);
                        }
                    }
                }
            }
            if (bone_cnt > 0) {
                auto hb_res = rebar_.allocate(static_cast<VkDeviceSize>(bone_cnt) * sizeof(GpuHeatmapBone), 16);
                if (hb_res) {
                    std::memcpy(hb_res.value().mapped, heatmap_scratch_.data(),
                                static_cast<size_t>(bone_cnt) * sizeof(GpuHeatmapBone));
                    const uint32_t max_x0 = w > 0u ? w - 1u : 0u;
                    const uint32_t max_y0 = h > 0u ? h - 1u : 0u;
                    const int32_t hx0 = std::clamp(static_cast<int32_t>(std::floor(aspect_layout.frame_x0)), 0,
                                                   static_cast<int32_t>(max_x0));
                    const int32_t hy0 = std::clamp(static_cast<int32_t>(std::floor(aspect_layout.frame_y0)), 0,
                                                   static_cast<int32_t>(max_y0));
                    const uint32_t hw = static_cast<uint32_t>(std::max(1.0f, std::round(aspect_layout.frame_width())));
                    const uint32_t hh = static_cast<uint32_t>(std::max(1.0f, std::round(aspect_layout.frame_height())));
                    const VkViewport hvp{aspect_layout.frame_x0,
                                         aspect_layout.frame_y0,
                                         aspect_layout.frame_width(),
                                         aspect_layout.frame_height(),
                                         0.0f,
                                         1.0f};
                    const VkRect2D hsc{
                        {hx0, hy0},
                        {std::min(hw, w > static_cast<uint32_t>(hx0) ? w - static_cast<uint32_t>(hx0) : 1u),
                         std::min(hh, h > static_cast<uint32_t>(hy0) ? h - static_cast<uint32_t>(hy0) : 1u)}};
                    vkCmdSetViewport(cmd, 0, 1, &hvp);
                    vkCmdSetScissor(cmd, 0, 1, &hsc);
                    bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso_heatmap_, "heatmap");
                    GpuPushConstants hpc{};
                    hpc.frame_globals_bda = fg_alloc.device_address;
                    hpc.draw_data_bda = hb_res.value().device_address;
                    vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(hpc), &hpc);
                    vkCmdDraw(cmd, 6, bone_cnt, 0, 0);
                }
            }
        }

        // Pass 13C: Schematic-Only Lighting Stage Overlay (strictly isolated to RenderViewKind::LightingStage)
        const bool allow_schematic = is_stage && req.show_stage_schematic && (pso_stage_schematic_ != VK_NULL_HANDLE);
        if (allow_schematic) {
            StageSchematicBuildParams sp{};
            sp.selected_light_idx = req.selected_studio_light_idx;
            sp.show_grid = req.show_stage_grid;
            sp.show_frustum = req.has_studio_frustum;
            sp.has_studio_frustum = req.has_studio_frustum;
            sp.studio_cam = req.studio_camera;
            sp.studio_aspect = req.studio_frustum_aspect;
            sp.pivot_ws = last_pivot_ws_;
            sp.eye_pos_ws = last_eye_ws_;
            const uint32_t elem_cnt = build_stage_schematic_overlay(*req.rig, sp, stage_overlay_scratch_);
            if (elem_cnt > 0u) {
                if (auto sb = rebar_.allocate(static_cast<VkDeviceSize>(elem_cnt) * sizeof(GpuStageOverlayElem), 16u)) {
                    std::memcpy(sb.value().mapped, stage_overlay_scratch_.data(),
                                static_cast<size_t>(elem_cnt) * sizeof(GpuStageOverlayElem));
                    // Negative-height viewport anchored at y = h (NOT y = w): any w != h previously shifted the
                    // grid / schematic / frustum overlay by (w - h) px, pushing it out of wide stage canvases.
                    const VkViewport svp{0.0f,
                                         static_cast<float>(std::max(h, 1u)),
                                         static_cast<float>(w),
                                         -static_cast<float>(std::max(h, 1u)),
                                         0.0f,
                                         1.0f};
                    const VkRect2D ssc{{0, 0}, {w, h}};
                    vkCmdSetViewport(cmd, 0, 1, &svp);
                    vkCmdSetScissor(cmd, 0, 1, &ssc);
                    bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso_stage_schematic_,
                                          "stage_schematic");
                    GpuPushConstants spc{};
                    spc.frame_globals_bda = fg_alloc.device_address;
                    spc.draw_data_bda = sb.value().device_address;
                    vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(spc), &spc);
                    vkCmdDraw(cmd, elem_cnt * 6u, 1u, 0u, 0u);
                }
            }
        }

        vkCmdEndRendering(cmd);
    }
    trans(*dst_vp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);

    // Optional clean fixed-aspect Broadcast Monitor pass for LightingStage PiP / Quad Broadcast cell
    if (!is_stage && req.encode_stage_broadcast_pip && stage_broadcast_pip_out_.img.handle != VK_NULL_HANDLE) {
        const uint32_t pw = stage_pip_w_;
        const uint32_t ph = stage_pip_h_;
        trans(stage_broadcast_pip_out_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo pa{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pa.imageView = stage_broadcast_pip_out_.img.view;
        pa.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        pa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        pa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        pa.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingInfo pri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        pri.renderArea = {{0, 0}, {pw, ph}};
        pri.layerCount = 1;
        pri.colorAttachmentCount = 1;
        pri.pColorAttachments = &pa;
        vkCmdBeginRendering(cmd, &pri);
        const VkViewport pvp{0.0f, 0.0f, static_cast<float>(pw), static_cast<float>(ph), 0.0f, 1.0f};
        const VkRect2D psc{{0, 0}, {pw, ph}};
        vkCmdSetViewport(cmd, 0, 1, &pvp);
        vkCmdSetScissor(cmd, 0, 1, &psc);
        bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso_viewport_compose_, "viewport_compose");
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_global_, 0, 1, &set0, 0, nullptr);
        GpuPushConstants ppc{};
        ppc.frame_globals_bda = fg_alloc.device_address;
        ppc.param0 = out_rgba16f_.sampled_slot;
        ppc.param1 = req.show_checkerboard ? 1u : 0u;
        ppc.param2 = std::bit_cast<uint32_t>(1.0f);
        ppc.param3 = std::bit_cast<uint32_t>(1.0f);
        ppc.fparam0 = 0.0f;
        ppc.fparam1 = 0.0f;
        ppc.fparam2 = 1.0f;
        ppc.fparam3 = 1.0f;
        vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(ppc), &ppc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        trans(stage_broadcast_pip_out_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    }

    if (!is_stage) {
        timing_.end_scope(cmd, fslot, rhi::GpuPassId::ViewportCompose);
    }
}

uint32_t FrameGraph::poll_shader_hot_reload() {
    if (!cfg_.allow_disk_shader_reload || !pipelines_ready_ || device_ == nullptr) {
        return 0;
    }
    uint32_t changed = 0;
    for (size_t i = 0; i < shaders::kPhase4Shaders.size(); ++i) {
        std::error_code ec;
        const auto p = std::filesystem::path(shaders::kPhase4DiskShaderDir) / shaders::kPhase4Shaders[i].spv_filename;
        const auto t = std::filesystem::last_write_time(p, ec);
        if (!ec) {
            const int64_t ns = t.time_since_epoch().count();
            if (ns != shader_mtimes_[i]) {
                shader_mtimes_[i] = ns;
                ++changed;
            }
        }
    }
    if (changed > 0) {
        if (!create_pipelines(true)) {
            return 0;
        }
        AETHER_LOG_INFO("renderer", "Hot-reloaded Phase 4 SPIR-V pipelines cleanly");
    }
    return changed;
}

Expected<std::vector<uint8_t>> FrameGraph::readback_image_rgba8(VkImage image, VkImageLayout current_layout, uint32_t w,
                                                                uint32_t h) const {
    if (device_ == nullptr || image == VK_NULL_HANDLE || w == 0 || h == 0) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "Invalid image readback args")};
    }
    device_->wait_idle();
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4ull;
    auto stg_res =
        device_->create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
                               VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!stg_res) {
        return Unexpected<Error>{stg_res.error()};
    }
    rhi::RhiBuffer stg = stg_res.value();

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = device_->graphics_family();
    cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkCreateCommandPool(device_->device(), &cpci, nullptr, &pool);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_->device(), &cbai, &cmd);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    cmd_image_copy_barrier(cmd, image, current_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy reg{};
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stg.handle, 1, &reg);
    cmd_image_copy_barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, current_layout,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(device_->graphics_queue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(device_->graphics_queue());
    vkDestroyCommandPool(device_->device(), pool, nullptr);

    vmaInvalidateAllocation(device_->allocator(), stg.allocation, 0, bytes);
    std::vector<uint8_t> rgba(static_cast<size_t>(bytes));
    const auto* src = static_cast<const uint8_t*>(stg.info.pMappedData);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        rgba[i * 4 + 0] = src[i * 4 + 2]; // BGRA8 -> RGBA8
        rgba[i * 4 + 1] = src[i * 4 + 1];
        rgba[i * 4 + 2] = src[i * 4 + 0];
        rgba[i * 4 + 3] = src[i * 4 + 3];
    }
    device_->destroy_buffer(stg);
    return rgba;
}

Expected<std::vector<float>> FrameGraph::readback_rgba16f(uint32_t w, uint32_t h) const {
    if (device_ == nullptr || out_rgba16f_.img.handle == VK_NULL_HANDLE || w == 0 || h == 0) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "Invalid RGBA16F readback args")};
    }
    device_->wait_idle();
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 8ull;
    auto stg_res =
        device_->create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
                               VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!stg_res) {
        return Unexpected<Error>{stg_res.error()};
    }
    rhi::RhiBuffer stg = stg_res.value();

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = device_->graphics_family();
    cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkCreateCommandPool(device_->device(), &cpci, nullptr, &pool);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_->device(), &cbai, &cmd);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    cmd_image_copy_barrier(cmd, out_rgba16f_.img.handle, out_rgba16f_.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy reg{};
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, out_rgba16f_.img.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stg.handle, 1, &reg);
    cmd_image_copy_barrier(cmd, out_rgba16f_.img.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out_rgba16f_.layout,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(device_->graphics_queue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(device_->graphics_queue());
    vkDestroyCommandPool(device_->device(), pool, nullptr);

    vmaInvalidateAllocation(device_->allocator(), stg.allocation, 0, bytes);
    std::vector<float> out(static_cast<size_t>(w) * h * 4u);
    const auto* src = static_cast<const uint16_t*>(stg.info.pMappedData);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = half_to_float(src[i]);
    }
    device_->destroy_buffer(stg);
    return out;
}

FrameGraphOutputs FrameGraph::outputs_for_slot(uint32_t ring_slot) const noexcept {
    FrameGraphOutputs o{};
    o.rgba16f_image = out_rgba16f_.img.handle;
    o.rgba16f_view = out_rgba16f_.img.view;
    if (out_ring_[0].bgra8.img.handle != VK_NULL_HANDLE) {
        const auto& s = out_ring_[ring_slot % kOutputRingSlots];
        o.bgra8_image = s.bgra8.img.handle;
        o.bgra8_view = s.bgra8.img.view;
        o.bgra8_straight_image = s.bgra8_straight.img.handle;
        o.bottom_up_bgra8_image = s.bottom_up_bgra8.img.handle;
        o.nv12_y_image = s.nv12_y.img.handle;
        o.nv12_uv_image = s.nv12_uv.img.handle;
        o.output_slot = ring_slot % kOutputRingSlots;
    } else {
        o.bgra8_image = out_bgra8_.img.handle;
        o.bgra8_view = out_bgra8_.img.view;
        o.nv12_y_image = out_nv12_y_.img.handle;
        o.nv12_uv_image = out_nv12_uv_.img.handle;
        o.output_slot = 0;
    }
    o.viewport_image = viewport_out_.img.handle;
    o.viewport_view = viewport_out_.img.view;
    o.stage_viewport_image = stage_viewport_out_.img.handle;
    o.stage_viewport_view = stage_viewport_out_.img.view;
    o.quad_top_image = quad_top_out_.img.handle;
    o.quad_top_view = quad_top_out_.img.view;
    o.quad_right_image = quad_right_out_.img.handle;
    o.quad_right_view = quad_right_out_.img.view;
    o.stage_broadcast_pip_image = stage_broadcast_pip_out_.img.handle;
    o.stage_broadcast_pip_view = stage_broadcast_pip_out_.img.view;
    o.rt_shadow_raw_image = rt_shadow_raw_.img.handle;
    o.rt_shadow_denoised_image = rt_shadow_denoised_.img.handle;
    o.hdr_scene_image = hdr_scene_.img.handle;
    o.width = cfg_.width;
    o.height = cfg_.height;
    o.stage_viewport_width = (stage_active_w_ > 0u) ? stage_active_w_ : cfg_.width;
    o.stage_viewport_height = (stage_active_h_ > 0u) ? stage_active_h_ : cfg_.height;
    o.stage_viewport_rt_width = (stage_rt_w_ > 0u) ? stage_rt_w_ : cfg_.width;
    o.stage_viewport_rt_height = (stage_rt_h_ > 0u) ? stage_rt_h_ : cfg_.height;
    o.quad_top_width = (quad_top_active_w_ > 0u) ? quad_top_active_w_ : std::max(cfg_.width / 2u, 16u);
    o.quad_top_height = (quad_top_active_h_ > 0u) ? quad_top_active_h_ : std::max(cfg_.height / 2u, 16u);
    o.quad_top_rt_width = (quad_top_rt_w_ > 0u) ? quad_top_rt_w_ : std::max(cfg_.width / 2u, 16u);
    o.quad_top_rt_height = (quad_top_rt_h_ > 0u) ? quad_top_rt_h_ : std::max(cfg_.height / 2u, 16u);
    o.quad_right_width = (quad_right_active_w_ > 0u) ? quad_right_active_w_ : std::max(cfg_.width / 2u, 16u);
    o.quad_right_height = (quad_right_active_h_ > 0u) ? quad_right_active_h_ : std::max(cfg_.height / 2u, 16u);
    o.quad_right_rt_width = (quad_right_rt_w_ > 0u) ? quad_right_rt_w_ : std::max(cfg_.width / 2u, 16u);
    o.quad_right_rt_height = (quad_right_rt_h_ > 0u) ? quad_right_rt_h_ : std::max(cfg_.height / 2u, 16u);
    o.stage_broadcast_pip_width = stage_pip_w_;
    o.stage_broadcast_pip_height = stage_pip_h_;
    o.output_width = effective_output_width();
    o.output_height = effective_output_height();
    return o;
}

FrameGraphOutputs FrameGraph::outputs() const noexcept {
    return outputs_for_slot(last_output_slot_);
}

std::array<float, 16> compute_view_proj(const PreviewCamera& cam, float aspect) noexcept {
    GpuMat4 view{};
    GpuMat4 inv_view{};
    GpuMat4 proj{};
    GpuMat4 inv_proj{};
    GpuMat4 vp{};
    Mat4Vec3 eye{};
    build_camera_matrices(cam, aspect, view, inv_view, proj, inv_proj, vp, eye);
    std::array<float, 16> out{};
    std::memcpy(out.data() + 0, vp.col0, sizeof(float) * 4U);
    std::memcpy(out.data() + 4, vp.col1, sizeof(float) * 4U);
    std::memcpy(out.data() + 8, vp.col2, sizeof(float) * 4U);
    std::memcpy(out.data() + 12, vp.col3, sizeof(float) * 4U);
    return out;
}

} // namespace aether::renderer
