#include "renderer/frame_graph.hpp"

#include "animation/spring_bone.hpp"

#include "core/clock.hpp"
#include "core/log.hpp"
#include "generated/phase4_shaders_spv.hpp"
#include "renderer/frame_globals_builder.hpp"
#include "renderer/sdf_generator.hpp"

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
using detail::scene_light_dir_to_light;

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
    if (rw == prev_rw && rh == prev_rh) {
        if (viewport_out_.sampled_slot != UINT32_MAX) {
            bindless_.release_sampled_image(viewport_out_.sampled_slot);
            viewport_out_.sampled_slot = UINT32_MAX;
        }
        device_->destroy_image(viewport_out_.img);
        viewport_out_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto img_res = device_->create_image_2d(vw, vh, VK_FORMAT_B8G8R8A8_UNORM,
                                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                                VK_IMAGE_ASPECT_COLOR_BIT);
        if (!img_res) {
            return Unexpected<Error>{img_res.error()};
        }
        viewport_out_.img = img_res.value();
        auto slot_res = bindless_.register_sampled_image(viewport_out_.img.view);
        if (!slot_res) {
            return Unexpected<Error>{slot_res.error()};
        }
        viewport_out_.sampled_slot = slot_res.value();
        return {};
    }
    if (auto res = create_size_dependent_targets(rw, rh); !res) {
        return res;
    }
    if (auto res = denoiser_.resize(rw, rh); !res) {
        return res;
    }
    have_prev_view_proj_ = false;
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
    have_prev_view_proj_ = false;
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

    // 128x128 Light Orbit Orb target
    {
        auto res = device_->create_image_2d(kOrbitOrbSize, kOrbitOrbSize, VK_FORMAT_B8G8R8A8_UNORM,
                                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                            VK_IMAGE_ASPECT_COLOR_BIT);
        if (!res) {
            return Unexpected<Error>{res.error()};
        }
        orbit_orb_out_.img = res.value();
        orbit_orb_out_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto s = bindless_.register_sampled_image(orbit_orb_out_.img.view);
        if (!s) {
            return Unexpected<Error>{s.error()};
        }
        orbit_orb_out_.sampled_slot = s.value();
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
    drop(orbit_orb_out_);
    device_->destroy_image(orbit_orb_depth_);
    orbit_orb_depth_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    drop(csm_depth_);
    drop(blue_noise_64_);
}

Expected<void> FrameGraph::create_size_dependent_targets(uint32_t w, uint32_t h) {
    destroy_size_dependent_targets();

    auto make_reg = [&](RegisteredImage& dst, uint32_t rw, uint32_t rh, VkFormat fmt, VkImageUsageFlags usage,
                        bool sampled, bool storage, bool uint_sampled,
                        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                        bool concurrent_transfer = false) -> Expected<void> {
        auto img_res = device_->create_image_2d(rw, rh, fmt, usage, aspect, concurrent_transfer);
        if (!img_res) {
            return Unexpected<Error>{img_res.error()};
        }
        dst.img = img_res.value();
        dst.layout = VK_IMAGE_LAYOUT_UNDEFINED;
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
    const VkImageUsageFlags stor_sampled =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (auto r = make_reg(gb_n_, w, h, VK_FORMAT_R16G16_UNORM, att_sampled, true, false, false); !r)
        return r;
    if (auto r = make_reg(gb_mv_, w, h, VK_FORMAT_R16G16_SFLOAT, att_sampled, true, false, false); !r)
        return r;
    if (auto r = make_reg(gb_mat_, w, h, VK_FORMAT_R16_UINT, att_sampled, false, false, true); !r)
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
    if (auto r = make_reg(bounce_denoised_, hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, stor_sampled, true, true, false);
        !r)
        return r;
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
                              bgra8_supports_storage_, false, VK_IMAGE_ASPECT_COLOR_BIT, true);
            !r) {
            return r;
        }
        if (auto r = make_reg(slot.bgra8_straight, w, h, VK_FORMAT_B8G8R8A8_UNORM, bgra_ring_usage, true,
                              bgra8_supports_storage_, false, VK_IMAGE_ASPECT_COLOR_BIT, true);
            !r) {
            return r;
        }
        if (auto r = make_reg(slot.bottom_up_bgra8, w, h, VK_FORMAT_B8G8R8A8_UNORM, bgra_ring_usage, true,
                              bgra8_supports_storage_, false, VK_IMAGE_ASPECT_COLOR_BIT, true);
            !r) {
            return r;
        }
        if (auto r = make_reg(slot.nv12_y, w, h, VK_FORMAT_R8_UNORM, stor_sampled, false, true, false,
                              VK_IMAGE_ASPECT_COLOR_BIT, true);
            !r)
            return r;
        if (auto r = make_reg(slot.nv12_uv, std::max(w / 2u, 1u), std::max(h / 2u, 1u), VK_FORMAT_R8G8_UNORM,
                              stor_sampled, false, true, false, VK_IMAGE_ASPECT_COLOR_BIT, true);
            !r) {
            return r;
        }
    }

    const uint32_t vw = std::max(cfg_.width, 16u);
    const uint32_t vh = std::max(cfg_.height, 16u);
    if (auto r = make_reg(viewport_out_, vw, vh, VK_FORMAT_B8G8R8A8_UNORM, att_sampled, true, false, false); !r)
        return r;

    const VkImageUsageFlags hist_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (auto r = make_reg(prev_normal_, w, h, VK_FORMAT_R16G16_UNORM, hist_usage, true, false, false); !r)
        return r;
    if (auto r = make_reg(prev_rt_shadow_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, hist_usage, true, false, false); !r)
        return r;
    if (auto r = make_reg(prev_rim_occ_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, hist_usage, true, false, false); !r)
        return r;
    if (auto r = make_reg(prev_bounce_, hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, hist_usage, true, false, false); !r)
        return r;

    {
        auto pd_res = device_->create_image_2d(w, h, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        if (!pd_res) {
            return Unexpected<Error>{pd_res.error()};
        }
        prev_depth_.img = pd_res.value();
        device_->destroy_image_view(prev_depth_.img.view);
        auto pdv =
            device_->create_image_view(prev_depth_.img.handle, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!pdv) {
            return Unexpected<Error>{pdv.error()};
        }
        prev_depth_.img.view = pdv.value();
        prev_depth_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto ps = bindless_.register_sampled_image(prev_depth_.img.view);
        if (!ps) {
            return Unexpected<Error>{ps.error()};
        }
        prev_depth_.sampled_slot = ps.value();
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
    drop(prev_depth_);
    drop(prev_normal_);
    drop(prev_rt_shadow_);
    drop(prev_rim_occ_);
    drop(prev_bounce_);
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
        destroy_pso(pso_live2d_blend_);
        destroy_pso(pso_live2d_opaque_wire_);
        destroy_pso(pso_live2d_double_sided_wire_);
        destroy_pso(pso_live2d_blend_wire_);
        destroy_pso(pso_orbit_orb_);
        destroy_pso(pso_outline_hull_);
        destroy_pso(pso_crease_lines_);
        destroy_pso(pso_bloom_down_);
        destroy_pso(pso_bloom_up_);
        destroy_pso(pso_tone_composite_);
        destroy_pso(pso_encode_bgra_);
        destroy_pso(pso_encode_nv12_);
        destroy_pso(pso_viewport_compose_);
        destroy_pso(pso_heatmap_);
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
            blend_atts[i].colorWriteMask =
                gd.color_write_disable
                    ? VkColorComponentFlags{0}
                    : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                       VK_COLOR_COMPONENT_A_BIT);
            if (gd.premul_blend) {
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
        VK_FORMAT_R16_UINT,
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
                            VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_CULL_MODE_NONE, true, false,
                            false, true, false},
                           pso_live2d_blend_);
        !r) {
        return r;
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
    if (auto r =
            build_gfx({shaders::Phase4ShaderId::OrbitOrbVs, shaders::Phase4ShaderId::OrbitOrbFs, &kLdrFmt, 1,
                       VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_CULL_MODE_NONE, false, false, false, false, false},
                      pso_orbit_orb_);
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
    drop_p(pso_live2d_blend_);
    drop_p(pso_live2d_opaque_wire_);
    drop_p(pso_live2d_double_sided_wire_);
    drop_p(pso_live2d_blend_wire_);
    drop_p(pso_orbit_orb_);
    drop_p(pso_outline_hull_);
    drop_p(pso_crease_lines_);
    drop_p(pso_bloom_down_);
    drop_p(pso_bloom_up_);
    drop_p(pso_tone_composite_);
    drop_p(pso_encode_bgra_);
    drop_p(pso_encode_nv12_);
    drop_p(pso_viewport_compose_);
    drop_p(pso_heatmap_);

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
    device_->destroy_buffer(submesh_draw_buf_);
    cached_submeshes_.clear();
    cached_materials_.clear();
    opaque_draw_indices_.clear();
    cutout_draw_indices_.clear();
    transparent_draw_indices_.clear();
    avatar_face_sdf_slot_ = UINT32_MAX;
    bound_avatar_ = nullptr;
    have_prev_view_proj_ = false;
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
    const uint32_t w = effective_output_width();
    const uint32_t h = effective_output_height();
    if (w == 0 || h == 0 || out_ring_[0].bgra8.img.handle == VK_NULL_HANDLE) {
        return;
    }

    const uint32_t out_slot = req.frame_slot % kOutputRingSlots;
    OutputSlotTargets& os = out_ring_[out_slot];
    last_output_slot_ = out_slot;
    const uint32_t fslot = req.frame_slot % rhi::GpuTimingRing::kFrameSlots;
    const uint32_t hw = std::max(w / 2u, 1u);
    const uint32_t hh = std::max(h / 2u, 1u);
    const uint32_t gx = (w + 7u) / 8u;
    const uint32_t gy = (h + 7u) / 8u;
    const LightingRig& rig = *req.rig;

    timing_.cmd_reset_frame(cmd, fslot);
    timing_.begin_scope(cmd, fslot, rhi::GpuPassId::FrameTotal);
    rebar_.begin_frame(req.frame_index, req.completed_frame_index);

    auto fg_alloc_res = rebar_.allocate(sizeof(GpuFrameGlobals), 64);
    if (!fg_alloc_res) {
        return;
    }
    const auto& fg_alloc = fg_alloc_res.value();
    GpuFrameGlobals fg{};
    GpuMat4 next_prev_vp{};
    const bool rt_hardware_ready =
        device_->rt_supported() && accel_.is_ready() && pso_ray_effects_ != VK_NULL_HANDLE;
    const uint8_t eff_mode = detail::populate_shared_frame_globals(
        req, w, h, has_avatar, rt_hardware_ready, have_prev_view_proj_, prev_view_proj_, fg, next_prev_vp);
    if (has_avatar) {
        prev_view_proj_ = next_prev_vp;
    }
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
    fg.gbuffer_extra_ids[6] = prev_depth_.sampled_slot;
    fg.gbuffer_extra_ids[7] = prev_normal_.sampled_slot;

    fg.history_ids[0] = prev_rt_shadow_.sampled_slot;
    fg.history_ids[1] = prev_rim_occ_.sampled_slot;
    fg.history_ids[2] = rim_occ_raw_.sampled_slot;
    fg.history_ids[3] = prev_depth_.sampled_slot;

    fg.output_storage_ids[0] = out_rgba16f_.storage_slot;
    fg.output_storage_ids[1] = bgra8_supports_storage_ ? os.bgra8.storage_slot : out_rgba8_scratch_.storage_slot;
    fg.output_storage_ids[2] = os.nv12_y.storage_slot;
    fg.output_storage_ids[3] = os.nv12_uv.storage_slot;

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
        // Before overwriting current GBuffer / Denoised targets, snapshot previous frame history if valid
        if (have_prev_view_proj_) {
            auto copy_same = [&](RegisteredImage& src, RegisteredImage& dst, VkPipelineStageFlags2 src_st,
                                 VkAccessFlags2 src_ac, uint32_t cw, uint32_t ch,
                                 VkImageAspectFlags asp = VK_IMAGE_ASPECT_COLOR_BIT) {
                const VkImageLayout src_prev = src.layout;
                trans(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_st, src_ac, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT, asp);
                trans(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, asp);
                VkImageCopy region{};
                region.srcSubresource = {asp, 0, 0, 1};
                region.dstSubresource = {asp, 0, 0, 1};
                region.extent = {cw, ch, 1};
                vkCmdCopyImage(cmd, src.img.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.img.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                trans(src, src_prev, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, asp);
                trans(dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT, asp);
            };
            copy_same(gb_n_, prev_normal_, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, w, h);
            copy_same(rt_shadow_denoised_, prev_rt_shadow_,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT, w, h);
            copy_same(rim_occ_denoised_, prev_rim_occ_,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT, w, h);
            copy_same(bounce_denoised_, prev_bounce_,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT, hw, hh);

            // Depth history copy (both aspects in region list so D32_SFLOAT_S8_UINT stays valid)
            rhi::RhiDevice::cmd_transition_image(
                cmd, depth_stencil_.handle, depth_layout_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
            depth_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            trans(prev_depth_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
            std::array<VkImageCopy, 2> dregions{};
            dregions[0].srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            dregions[0].dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            dregions[0].extent = {w, h, 1};
            dregions[1].srcSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
            dregions[1].dstSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
            dregions[1].extent = {w, h, 1};
            vkCmdCopyImage(cmd, depth_stencil_.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, prev_depth_.img.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, dregions.data());
            trans(prev_depth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        }

        // === PASS 1: Compute Skin/Pack -> GpuSkinnedVertex ===
        timing_.begin_scope(cmd, fslot, rhi::GpuPassId::SkinMorph);
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
            pc.param1 = have_prev_view_proj_ ? 1u : 0u;
            bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso_skin_morph_, "skin_morph");
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_global_, 0, 1, &set0, 0, nullptr);
            vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, (pc.param0 + 63u) / 64u, 1, 1);
        }
        timing_.end_scope(cmd, fslot, rhi::GpuPassId::SkinMorph);

        const VkPipelineStageFlags2 skin_dst_stage =
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
            (use_rt ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR : 0ULL);
        cmd_buffer_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, skin_dst_stage,
                           VK_ACCESS_2_SHADER_READ_BIT);

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
                // Always refit immediately whenever pose or morph target hash changed so rasterizer & BLAS have 0.0-px drift.
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
            }
        }

        // === PASS 3: G-Buffer + D32_S8 Depth/Stencil Prepass ===
        timing_.begin_scope(cmd, fslot, rhi::GpuPassId::Prepass);
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
        timing_.end_scope(cmd, fslot, rhi::GpuPassId::Prepass);

        // === PASS 4: Inline RayQuery OR CSM Fallback Shadow/Rim/Bounce ===
        trans(rt_shadow_raw_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        trans(rim_occ_raw_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        trans(bounce_half_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

        if (use_rt) {
            timing_.begin_scope(cmd, fslot, rhi::GpuPassId::RayTrace);
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
            pc.param3 = ((w / 2u) << 16u) | (have_prev_view_proj_ ? 1u : 0u);
            vkCmdPushConstants(cmd, pipe_layout_rt_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, gx, gy, 1);
            timing_.end_scope(cmd, fslot, rhi::GpuPassId::RayTrace);
        } else {
            timing_.begin_scope(cmd, fslot, rhi::GpuPassId::CsmFallback);
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
                    vkCmdDrawIndexed(cmd, cached_submeshes_[idx].index_count, 1, cached_submeshes_[idx].index_offset, 0,
                                     0);
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
            timing_.end_scope(cmd, fslot, rhi::GpuPassId::CsmFallback);
        }

        trans(rt_shadow_raw_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        trans(rim_occ_raw_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        trans(bounce_half_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);

        // === PASS 5: Multi-vendor Spatio-Temporal Denoise ===
        timing_.begin_scope(cmd, fslot, rhi::GpuPassId::Denoise);
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
                have_prev_view_proj_
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
            dvi.history_shadow_sampled_slot =
                have_prev_view_proj_ ? prev_rt_shadow_.sampled_slot : rt_shadow_raw_.sampled_slot;
            dvi.history_rim_sampled_slot =
                have_prev_view_proj_ ? prev_rim_occ_.sampled_slot : rim_occ_raw_.sampled_slot;
            dvi.history_bounce_sampled_slot =
                have_prev_view_proj_ ? prev_bounce_.sampled_slot : bounce_half_.sampled_slot;
            dvi.gb_normal_sampled_slot = gb_n_.sampled_slot;
            dvi.gb_mv_sampled_slot = gb_mv_.sampled_slot;
            dvi.depth_sampled_slot = depth_sampled_slot_;
            dvi.prev_depth_sampled_slot = have_prev_view_proj_ ? prev_depth_.sampled_slot : depth_sampled_slot_;
            dvi.prev_normal_sampled_slot = have_prev_view_proj_ ? prev_normal_.sampled_slot : gb_n_.sampled_slot;
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
        timing_.end_scope(cmd, fslot, rhi::GpuPassId::Denoise);

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

            timing_.begin_scope(cmd, fslot, rhi::GpuPassId::Live2dNpr);
            const bool use_wire = req.wireframe && wireframe_supported_ && (pso_live2d_opaque_wire_ != VK_NULL_HANDLE);
            auto draw_npr_list = [&](const std::vector<uint32_t>& list, bool transparent_bucket) {
                VkPipeline cur = VK_NULL_HANDLE;
                for (const uint32_t idx : list) {
                    const auto& d = cached_submeshes_[idx];
                    VkPipeline want =
                        transparent_bucket
                            ? (use_wire ? pso_live2d_blend_wire_ : pso_live2d_blend_)
                            : ((d.cull_mode != 0u)
                                   ? (use_wire ? pso_live2d_double_sided_wire_ : pso_live2d_double_sided_)
                                   : (use_wire ? pso_live2d_opaque_wire_ : pso_live2d_opaque_));
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
                                (transparent_bucket ? AETHER_SUBMESH_FLAG_TRANSPARENT : 0u);
                    vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
                    vkCmdDrawIndexed(cmd, d.index_count, 1, d.index_offset, 0, 0);
                }
            };
            draw_npr_list(opaque_draw_indices_, false);
            draw_npr_list(cutout_draw_indices_, false);
            timing_.end_scope(cmd, fslot, rhi::GpuPassId::Live2dNpr);

            // Pass 7: Inverted-hull outlines (front-face culled back faces)
            timing_.begin_scope(cmd, fslot, rhi::GpuPassId::OutlineHull);
            const float outline_mul = std::max(0.0f, req.outline_width_scale) * std::max(0.0f, rig.outline_width_scale);
            if (!use_wire && outline_mul > 0.0f) {
                bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso_outline_hull_, "outline_hull");
                for (const uint32_t idx : opaque_draw_indices_) {
                    const auto& d = cached_submeshes_[idx];
                    if (cached_materials_[d.material_index].outline_color_width[3] <= 0.0001f) {
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
            timing_.end_scope(cmd, fslot, rhi::GpuPassId::OutlineHull);

            // Pass 8: Sorted Transparent NPR (eye highlights, lashes, translucent garments)
            draw_npr_list(transparent_draw_indices_, true);
            vkCmdEndRendering(cmd);
        }

        trans(hdr_scene_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);

        // === PASS 9: Screen-Space Crease Lines (masked by D32_S8 stencil & 0x01) ===
        timing_.begin_scope(cmd, fslot, rhi::GpuPassId::CreaseLines);
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
        timing_.end_scope(cmd, fslot, rhi::GpuPassId::CreaseLines);

        // === PASS 10: 6-Mip Dual-Kawase Premultiplied Bloom ===
        timing_.begin_scope(cmd, fslot, rhi::GpuPassId::PostBloom);
        bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso_bloom_down_, "bloom_down");
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_global_, 0, 1, &set0, 0, nullptr);
        uint32_t src_w = w;
        uint32_t src_h = h;
        uint32_t src_slot = hdr_scene_.sampled_slot;
        for (uint32_t i = 0; i < kBloomMipCount; ++i) {
            auto& dst = bloom_mips_[i];
            trans(dst, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
            GpuPushConstants pc{};
            pc.param0 = src_slot;
            pc.param1 = dst.storage_slot;
            pc.param2 = dst.img.width;
            pc.param3 = dst.img.height;
            pc.fparam0 = (i == 0) ? rig.bloom_threshold : 0.0f;
            pc.fparam1 = 1.0f / static_cast<float>(src_w);
            pc.fparam2 = 1.0f / static_cast<float>(src_h);
            vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, (dst.img.width + 7u) / 8u, (dst.img.height + 7u) / 8u, 1);
            trans(dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            src_w = dst.img.width;
            src_h = dst.img.height;
            src_slot = dst.sampled_slot;
        }

        bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso_bloom_up_, "bloom_up");
        for (int32_t i = static_cast<int32_t>(kBloomMipCount) - 2; i >= 0; --i) {
            const auto& lo = bloom_mips_[static_cast<size_t>(i) + 1U];
            auto& hi = bloom_mips_[static_cast<size_t>(i)];
            trans(hi, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
            GpuPushConstants pc{};
            pc.param0 = lo.sampled_slot;
            pc.param1 = hi.storage_slot;
            pc.param2 = hi.img.width;
            pc.param3 = hi.img.height;
            pc.fparam1 = 1.0f / static_cast<float>(lo.img.width);
            pc.fparam2 = 1.0f / static_cast<float>(lo.img.height);
            vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, (hi.img.width + 7u) / 8u, (hi.img.height + 7u) / 8u, 1);
            trans(hi, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        }
        timing_.end_scope(cmd, fslot, rhi::GpuPassId::PostBloom);

        // === PASS 11: Composite + Tonemap -> OUT_RGBA16F ===
        timing_.begin_scope(cmd, fslot, rhi::GpuPassId::ToneMap);
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
        timing_.end_scope(cmd, fslot, rhi::GpuPassId::ToneMap);
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
    timing_.begin_scope(cmd, fslot, rhi::GpuPassId::OutputEncode);
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
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_global_, 0, 1, &set0, 0, nullptr);
            vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, gx, gy, 1);
        }
    }

    cmd_buffer_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
    timing_.end_scope(cmd, fslot, rhi::GpuPassId::OutputEncode);
    timing_.end_scope(cmd, fslot, rhi::GpuPassId::FrameTotal);

    if (has_avatar) {
        have_prev_view_proj_ = true;
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

    const uint32_t w = cfg_.width;
    const uint32_t h = cfg_.height;
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
    const auto aspect_layout = core::compute_viewport_aspect_layout(
        static_cast<float>(w), static_cast<float>(h), static_cast<float>(effective_output_width()),
        static_cast<float>(effective_output_height()), req.viewport_aspect_mode);
    PreviewCamera heat_cam = req.camera;
    heat_cam.fovy_rad = aspect_layout.effective_fovy_rad(req.camera.fovy_rad);
    GpuFrameGlobals fg{};
    Mat4Vec3 eye{};
    build_camera_matrices(heat_cam, aspect_layout.frame_aspect, fg.view, fg.inv_view, fg.proj, fg.inv_proj,
                          fg.view_proj, eye);
    fg.viewport_size[0] = aspect_layout.frame_width();
    fg.viewport_size[1] = aspect_layout.frame_height();
    fg.viewport_size[2] = 1.0f / aspect_layout.frame_width();
    fg.viewport_size[3] = 1.0f / aspect_layout.frame_height();
    const Mat4Vec3 kd = scene_light_dir_to_light(req.rig->lights[0]);
    fg.lights[0].pos_or_dir[0] = kd.x;
    fg.lights[0].pos_or_dir[1] = kd.y;
    fg.lights[0].pos_or_dir[2] = kd.z;
    fg.lights[0].color_intensity[0] = req.rig->lights[0].color[0] * req.rig->lights[0].intensity;
    fg.lights[0].color_intensity[1] = req.rig->lights[0].color[1] * req.rig->lights[0].intensity;
    fg.lights[0].color_intensity[2] = req.rig->lights[0].color[2] * req.rig->lights[0].intensity;
    std::memcpy(fg_alloc.mapped, &fg, sizeof(fg));

    timing_.cmd_reset_scope(cmd, fslot, rhi::GpuPassId::ViewportCompose);
    timing_.begin_scope(cmd, fslot, rhi::GpuPassId::ViewportCompose);

    // Pass 13A: Studio Viewport Checkerboard + Optional Alt+C Skeleton Heatmap
    trans(viewport_out_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    {
        VkRenderingAttachmentInfo col_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        col_att.imageView = viewport_out_.img.view;
        col_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        col_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        col_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        col_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {w, h}};
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
        pc.param1 = (req.show_checkerboard ? 1u : 0u) | (aspect_layout.explicit_matte ? 2u : 0u);
        pc.param2 = std::bit_cast<uint32_t>(aspect_layout.uv_du);
        pc.param3 = std::bit_cast<uint32_t>(aspect_layout.uv_dv);
        pc.fparam0 = aspect_layout.norm_x0;
        pc.fparam1 = aspect_layout.norm_y0;
        pc.fparam2 = aspect_layout.norm_x1;
        pc.fparam3 = aspect_layout.norm_y1;
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
                            const Mat4Vec3 ref = (std::abs(axis.y) < 0.9f) ? Mat4Vec3{0.0f, 1.0f, 0.0f}
                                                                            : Mat4Vec3{1.0f, 0.0f, 0.0f};
                            const Mat4Vec3 u = normalize3(cross3(axis, ref));
                            const Mat4Vec3 v = cross3(axis, u);
                            const std::array<float, 3> pmid{0.5f * (p0[0] + p1[0]), 0.5f * (p0[1] + p1[1]),
                                                            0.5f * (p0[2] + p1[2])};
                            push_ring(p0, u, v, r, 12, kTwoPiF, cat, c);
                            push_ring(pmid, u, v, r, 12, kTwoPiF, cat, c);
                            push_ring(p1, u, v, r, 12, kTwoPiF, cat, c);
                            for (const Mat4Vec3& rad : {u, Mat4Vec3{-u.x, -u.y, -u.z}, v, Mat4Vec3{-v.x, -v.y, -v.z}}) {
                                push_seg({p0[0] + r * rad.x, p0[1] + r * rad.y, p0[2] + r * rad.z},
                                         {p1[0] + r * rad.x, p1[1] + r * rad.y, p1[2] + r * rad.z}, 2.0f, cat, 1.0f,
                                         c);
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
                    const int32_t hx0 =
                        std::clamp(static_cast<int32_t>(std::floor(aspect_layout.frame_x0)), 0, static_cast<int32_t>(max_x0));
                    const int32_t hy0 =
                        std::clamp(static_cast<int32_t>(std::floor(aspect_layout.frame_y0)), 0, static_cast<int32_t>(max_y0));
                    const uint32_t hw = static_cast<uint32_t>(std::max(1.0f, std::round(aspect_layout.frame_width())));
                    const uint32_t hh = static_cast<uint32_t>(std::max(1.0f, std::round(aspect_layout.frame_height())));
                    const VkViewport hvp{aspect_layout.frame_x0, aspect_layout.frame_y0,
                                         aspect_layout.frame_width(), aspect_layout.frame_height(), 0.0f, 1.0f};
                    const VkRect2D hsc{{hx0, hy0},
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
        vkCmdEndRendering(cmd);
    }
    trans(viewport_out_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);

    // Pass 13B: 128x128 Light Orbit Orb Preview
    if (req.show_orbit_orb) {
        trans(orbit_orb_out_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo oa{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        oa.imageView = orbit_orb_out_.img.view;
        oa.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        oa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        oa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        oa.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo ori{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ori.renderArea = {{0, 0}, {kOrbitOrbSize, kOrbitOrbSize}};
        ori.layerCount = 1;
        ori.colorAttachmentCount = 1;
        ori.pColorAttachments = &oa;
        vkCmdBeginRendering(cmd, &ori);
        const VkViewport ovp{0.0f, 0.0f, static_cast<float>(kOrbitOrbSize), static_cast<float>(kOrbitOrbSize),
                             0.0f, 1.0f};
        const VkRect2D osc{{0, 0}, {kOrbitOrbSize, kOrbitOrbSize}};
        vkCmdSetViewport(cmd, 0, 1, &ovp);
        vkCmdSetScissor(cmd, 0, 1, &osc);
        bind_pipeline_checked(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso_orbit_orb_, "orbit_orb");
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_global_, 0, 1, &set0, 0, nullptr);
        GpuPushConstants opc{};
        opc.frame_globals_bda = fg_alloc.device_address;
        vkCmdPushConstants(cmd, pipe_layout_global_, VK_SHADER_STAGE_ALL, 0, sizeof(opc), &opc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        trans(orbit_orb_out_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    }

    timing_.end_scope(cmd, fslot, rhi::GpuPassId::ViewportCompose);
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
    o.orbit_orb_image = orbit_orb_out_.img.handle;
    o.orbit_orb_view = orbit_orb_out_.img.view;
    o.rt_shadow_raw_image = rt_shadow_raw_.img.handle;
    o.rt_shadow_denoised_image = rt_shadow_denoised_.img.handle;
    o.hdr_scene_image = hdr_scene_.img.handle;
    o.width = cfg_.width;
    o.height = cfg_.height;
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
