#pragma once

#include "core/expected.hpp"
#include "generated/phase4_shaders_spv.hpp"
#include "renderer/denoise/denoiser_backend.hpp"
#include "renderer/gpu_structs.hpp"
#include "rhi/bindless.hpp"
#include "rhi/rhi_device.hpp"

#include <algorithm>
#include <cstdint>

namespace aether::renderer::detail {

[[nodiscard]] inline Expected<void>
create_compute_denoiser_pipeline(const rhi::RhiDevice& device, const rhi::BindlessHeap& bindless,
                                 shaders::Phase4ShaderId shader_id, VkPipelineLayout& out_layout,
                                 VkShaderModule& out_module, VkPipeline& out_pipeline) {
    if (device.device() == VK_NULL_HANDLE || bindless.global_layout() == VK_NULL_HANDLE) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "Invalid device or bindless heap for denoiser")};
    }

    const auto spv = shaders::load_phase4_shader_words(shader_id, false);
    if (spv.size() < 5) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "Empty SPIR-V binary for denoiser shader")};
    }

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spv.size() * sizeof(uint32_t);
    smci.pCode = spv.data();
    if (vkCreateShaderModule(device.device(), &smci, nullptr, &out_module) != VK_SUCCESS) {
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkCreateShaderModule(denoiser) failed")};
    }

    VkPushConstantRange pc_range{};
    pc_range.stageFlags = VK_SHADER_STAGE_ALL;
    pc_range.offset = 0;
    pc_range.size = sizeof(GpuPushConstants);

    const VkDescriptorSetLayout set_layout = bindless.global_layout();
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pc_range;
    if (vkCreatePipelineLayout(device.device(), &plci, nullptr, &out_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device.device(), out_module, nullptr);
        out_module = VK_NULL_HANDLE;
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkCreatePipelineLayout(denoiser) failed")};
    }

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = out_module;
    cpci.stage.pName = "main";
    cpci.layout = out_layout;
    if (vkCreateComputePipelines(device.device(), device.pipeline_cache(), 1, &cpci, nullptr, &out_pipeline) !=
        VK_SUCCESS) {
        vkDestroyPipelineLayout(device.device(), out_layout, nullptr);
        vkDestroyShaderModule(device.device(), out_module, nullptr);
        out_layout = VK_NULL_HANDLE;
        out_module = VK_NULL_HANDLE;
        return Unexpected<Error>{Error::make(ErrorCode::VulkanError, "vkCreateComputePipelines(denoiser) failed")};
    }
    return {};
}

inline void destroy_compute_denoiser_pipeline(const rhi::RhiDevice* device, VkPipelineLayout& layout,
                                              VkPipeline& pipeline, VkShaderModule& mod) noexcept {
    if (device != nullptr && device->device() != VK_NULL_HANDLE) {
        const VkDevice dev = device->device();
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(dev, pipeline, nullptr);
        }
        if (layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(dev, layout, nullptr);
        }
        if (mod != VK_NULL_HANDLE) {
            vkDestroyShaderModule(dev, mod, nullptr);
        }
    }
    pipeline = VK_NULL_HANDLE;
    layout = VK_NULL_HANDLE;
    mod = VK_NULL_HANDLE;
}

[[nodiscard]] inline Expected<void> dispatch_compute_denoiser(const rhi::RhiDevice* device, VkPipeline pipeline,
                                                              VkPipelineLayout layout, VkDescriptorSet bindless_set,
                                                              VkCommandBuffer cmd, uint32_t width, uint32_t height,
                                                              const DenoiserInputView& inputs) noexcept {
    if (device == nullptr || pipeline == VK_NULL_HANDLE || layout == VK_NULL_HANDLE || bindless_set == VK_NULL_HANDLE ||
        cmd == VK_NULL_HANDLE) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "Denoiser backend not initialized")};
    }
    const uint32_t w = std::max(inputs.width != 0 ? inputs.width : width, 1u);
    const uint32_t h = std::max(inputs.height != 0 ? inputs.height : height, 1u);
    const uint32_t gx = (w + 7u) / 8u;
    const uint32_t gy = (h + 7u) / 8u;
    const uint32_t has_hist_flag = (inputs.frame_index > 0) ? (1u << 16u) : 0u;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &bindless_set, 0, nullptr);

    auto record_channel = [&](uint32_t raw_slot, uint32_t hist_slot, uint32_t dst_slot, float alpha_scale) noexcept {
        GpuPushConstants pc{};
        pc.frame_globals_bda = inputs.frame_globals_bda;
        pc.param0 = (raw_slot & 0xFFFFu) | ((hist_slot & 0xFFFFu) << 16u);
        pc.param1 = (dst_slot & 0xFFFFu) | has_hist_flag;
        pc.param2 = w;
        pc.param3 = h;
        pc.fparam0 = std::clamp(inputs.temporal_alpha * alpha_scale, 0.05f, 1.0f);
        pc.fparam1 = 1.0f;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, gx, gy, 1);
    };

    if (inputs.raw_shadow_sampled_slot != UINT32_MAX && inputs.denoised_shadow_storage_slot != UINT32_MAX) {
        const uint32_t hist = (inputs.history_shadow_sampled_slot != UINT32_MAX) ? inputs.history_shadow_sampled_slot
                                                                                 : inputs.raw_shadow_sampled_slot;
        record_channel(inputs.raw_shadow_sampled_slot, hist, inputs.denoised_shadow_storage_slot, 1.0f);
    }

    if (inputs.raw_rim_sampled_slot != UINT32_MAX && inputs.denoised_rim_storage_slot != UINT32_MAX &&
        inputs.denoised_rim_storage_slot != inputs.denoised_shadow_storage_slot) {
        const uint32_t hist = (inputs.history_rim_sampled_slot != UINT32_MAX) ? inputs.history_rim_sampled_slot
                                                                              : inputs.raw_rim_sampled_slot;
        record_channel(inputs.raw_rim_sampled_slot, hist, inputs.denoised_rim_storage_slot, 1.15f);
    }
    return {};
}

} // namespace aether::renderer::detail