#include "rhi/bindless.hpp"

#include <algorithm>

namespace aether::rhi {
namespace {

VkSamplerCreateInfo make_sampler_ci(SamplerSlot slot) noexcept {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.minLod = 0.0f;
    ci.maxLod = 12.0f;
    ci.maxAnisotropy = 1.0f;

    switch (slot) {
    case SamplerSlot::LinearClamp:
        ci.magFilter = VK_FILTER_LINEAR;
        ci.minFilter = VK_FILTER_LINEAR;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        break;
    case SamplerSlot::NearestClamp:
        ci.magFilter = VK_FILTER_NEAREST;
        ci.minFilter = VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        break;
    case SamplerSlot::LinearRepeat:
        ci.magFilter = VK_FILTER_LINEAR;
        ci.minFilter = VK_FILTER_LINEAR;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        break;
    case SamplerSlot::NearestRepeat:
        ci.magFilter = VK_FILTER_NEAREST;
        ci.minFilter = VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        break;
    case SamplerSlot::ShadowCompare:
        ci.magFilter = VK_FILTER_LINEAR;
        ci.minFilter = VK_FILTER_LINEAR;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        ci.compareEnable = VK_TRUE;
        ci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        break;
    case SamplerSlot::Count:
        break;
    }
    return ci;
}

} // namespace

BindlessHeap::~BindlessHeap() {
    destroy();
}

Expected<void> BindlessHeap::init(const RhiDevice& device, const BindlessConfig& cfg) {
    destroy();
    if (device.device() == VK_NULL_HANDLE) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "BindlessHeap::init null RhiDevice"));
    }

    device_ = &device;
    const auto& info = device.gpu_info();

    const uint32_t max_uint = std::clamp(cfg.max_uint_sampled_images, 32u, 256u);
    const uint32_t hw_sampled_cap =
        (info.max_sampled_images > max_uint + 16u) ? (info.max_sampled_images - max_uint) : 1024u;
    sampled_capacity_ = std::clamp(cfg.max_sampled_images, 256u, hw_sampled_cap);
    uint_sampled_capacity_ = max_uint;
    storage_capacity_ = std::clamp(cfg.max_storage_images, 64u, std::max(64u, info.max_storage_images));
    sampler_capacity_ = static_cast<uint32_t>(SamplerSlot::Count);

    // 1. Create Set 0 layout: binding 0 SAMPLED_IMAGE(float), binding 1 SAMPLER, binding 2 STORAGE_IMAGE, binding 3
    // SAMPLED_IMAGE(uint)
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = sampled_capacity_;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = sampler_capacity_;
    bindings[1].stageFlags = VK_SHADER_STAGE_ALL;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = storage_capacity_;
    bindings[2].stageFlags = VK_SHADER_STAGE_ALL;

    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[3].descriptorCount = uint_sampled_capacity_;
    bindings[3].stageFlags = VK_SHADER_STAGE_ALL;

    std::array<VkDescriptorBindingFlags, 4> bflags{};
    bflags[0] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    bflags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    bflags[2] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    bflags[3] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo bfci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfci.bindingCount = static_cast<uint32_t>(bflags.size());
    bfci.pBindingFlags = bflags.data();

    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.pNext = &bfci;
    dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    dslci.bindingCount = static_cast<uint32_t>(bindings.size());
    dslci.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(device_->device(), &dslci, nullptr, &global_layout_) != VK_SUCCESS) {
        destroy();
        return make_unexpected(Error::make(ErrorCode::VulkanError, "vkCreateDescriptorSetLayout(bindless) failed"));
    }

    const bool use_rt = cfg.enable_rt_set && device_->rt_supported();
    if (use_rt) {
        VkDescriptorSetLayoutBinding rt_binding{};
        rt_binding.binding = 0;
        rt_binding.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        rt_binding.descriptorCount = 1;
        rt_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorBindingFlags rt_bflag =
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo rt_bfci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        rt_bfci.bindingCount = 1;
        rt_bfci.pBindingFlags = &rt_bflag;

        VkDescriptorSetLayoutCreateInfo rt_dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        rt_dsl.pNext = &rt_bfci;
        rt_dsl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        rt_dsl.bindingCount = 1;
        rt_dsl.pBindings = &rt_binding;

        if (vkCreateDescriptorSetLayout(device_->device(), &rt_dsl, nullptr, &rt_layout_) != VK_SUCCESS) {
            destroy();
            return make_unexpected(Error::make(ErrorCode::VulkanError, "vkCreateDescriptorSetLayout(RT TLAS) failed"));
        }
    }

    // 2. Create descriptor pool with UPDATE_AFTER_BIND
    std::array<VkDescriptorPoolSize, 4> pool_sizes{};
    pool_sizes[0] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampled_capacity_ + uint_sampled_capacity_};
    pool_sizes[1] = {VK_DESCRIPTOR_TYPE_SAMPLER, sampler_capacity_};
    pool_sizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage_capacity_};
    pool_sizes[3] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u};

    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    dpci.maxSets = use_rt ? 2u : 1u;
    dpci.poolSizeCount = use_rt ? 4u : 3u;
    dpci.pPoolSizes = pool_sizes.data();

    if (vkCreateDescriptorPool(device_->device(), &dpci, nullptr, &pool_) != VK_SUCCESS) {
        destroy();
        return make_unexpected(Error::make(ErrorCode::VulkanError, "vkCreateDescriptorPool(bindless) failed"));
    }

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &global_layout_;
    if (vkAllocateDescriptorSets(device_->device(), &dsai, &global_set_) != VK_SUCCESS) {
        destroy();
        return make_unexpected(Error::make(ErrorCode::VulkanError, "vkAllocateDescriptorSets(global) failed"));
    }

    if (use_rt) {
        dsai.pSetLayouts = &rt_layout_;
        if (vkAllocateDescriptorSets(device_->device(), &dsai, &rt_set_) != VK_SUCCESS) {
            destroy();
            return make_unexpected(Error::make(ErrorCode::VulkanError, "vkAllocateDescriptorSets(RT) failed"));
        }
    }

    // 3. Create & bind static samplers in binding 1
    std::array<VkDescriptorImageInfo, static_cast<size_t>(SamplerSlot::Count)> sampler_infos{};
    for (size_t i = 0; i < static_samplers_.size(); ++i) {
        const auto sci = make_sampler_ci(static_cast<SamplerSlot>(i));
        if (vkCreateSampler(device_->device(), &sci, nullptr, &static_samplers_[i]) != VK_SUCCESS) {
            destroy();
            return make_unexpected(Error::make(ErrorCode::VulkanError, "vkCreateSampler(bindless) failed"));
        }
        sampler_infos[i].sampler = static_samplers_[i];
    }

    VkWriteDescriptorSet write_samplers{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write_samplers.dstSet = global_set_;
    write_samplers.dstBinding = 1;
    write_samplers.dstArrayElement = 0;
    write_samplers.descriptorCount = sampler_capacity_;
    write_samplers.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write_samplers.pImageInfo = sampler_infos.data();
    vkUpdateDescriptorSets(device_->device(), 1, &write_samplers, 0, nullptr);

    // 4. Preallocate free index stacks for zero-alloc runtime registration (`HR-01`)
    sampled_free_stack_.resize(sampled_capacity_);
    for (uint32_t i = 0; i < sampled_capacity_; ++i) {
        sampled_free_stack_[i] = sampled_capacity_ - 1u - i;
    }
    sampled_free_top_ = sampled_capacity_;

    storage_free_stack_.resize(storage_capacity_);
    for (uint32_t i = 0; i < storage_capacity_; ++i) {
        storage_free_stack_[i] = storage_capacity_ - 1u - i;
    }
    storage_free_top_ = storage_capacity_;

    uint_sampled_free_stack_.resize(uint_sampled_capacity_);
    for (uint32_t i = 0; i < uint_sampled_capacity_; ++i) {
        uint_sampled_free_stack_[i] = uint_sampled_capacity_ - 1u - i;
    }
    uint_sampled_free_top_ = uint_sampled_capacity_;

    sampled_in_use_ = 0;
    storage_in_use_ = 0;
    uint_sampled_in_use_ = 0;
    return {};
}

void BindlessHeap::destroy() noexcept {
    if (device_ != nullptr && device_->device() != VK_NULL_HANDLE) {
        const VkDevice dev = device_->device();
        for (auto& s : static_samplers_) {
            if (s != VK_NULL_HANDLE) {
                vkDestroySampler(dev, s, nullptr);
                s = VK_NULL_HANDLE;
            }
        }
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(dev, pool_, nullptr);
        }
        if (rt_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(dev, rt_layout_, nullptr);
        }
        if (global_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(dev, global_layout_, nullptr);
        }
    }
    pool_ = VK_NULL_HANDLE;
    global_layout_ = VK_NULL_HANDLE;
    global_set_ = VK_NULL_HANDLE;
    rt_layout_ = VK_NULL_HANDLE;
    rt_set_ = VK_NULL_HANDLE;
    sampled_free_stack_.clear();
    storage_free_stack_.clear();
    uint_sampled_free_stack_.clear();
    sampled_free_top_ = 0;
    storage_free_top_ = 0;
    uint_sampled_free_top_ = 0;
    sampled_in_use_ = 0;
    storage_in_use_ = 0;
    uint_sampled_in_use_ = 0;
    device_ = nullptr;
}

Expected<uint32_t> BindlessHeap::register_sampled_image(VkImageView view, VkImageLayout layout) noexcept {
    if (device_ == nullptr || view == VK_NULL_HANDLE) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Invalid BindlessHeap or image view"));
    }
    if (sampled_free_top_ == 0) {
        return make_unexpected(Error::make(ErrorCode::LimitExceeded, "Bindless sampled image heap full"));
    }
    const uint32_t slot = sampled_free_stack_[--sampled_free_top_];
    ++sampled_in_use_;
    update_sampled_image(slot, view, layout);
    return slot;
}

void BindlessHeap::update_sampled_image(uint32_t slot, VkImageView view, VkImageLayout layout) const noexcept {
    if (device_ == nullptr || global_set_ == VK_NULL_HANDLE || view == VK_NULL_HANDLE || slot >= sampled_capacity_) {
        return;
    }
    VkDescriptorImageInfo img_info{};
    img_info.imageView = view;
    img_info.imageLayout = layout;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = global_set_;
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
}

void BindlessHeap::release_sampled_image(uint32_t slot) noexcept {
    if (slot < sampled_capacity_ && sampled_free_top_ < sampled_free_stack_.size()) {
        sampled_free_stack_[sampled_free_top_++] = slot;
        if (sampled_in_use_ > 0) {
            --sampled_in_use_;
        }
    }
}

Expected<uint32_t> BindlessHeap::register_storage_image(VkImageView view, VkImageLayout layout) noexcept {
    if (device_ == nullptr || view == VK_NULL_HANDLE) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Invalid BindlessHeap or storage view"));
    }
    if (storage_free_top_ == 0) {
        return make_unexpected(Error::make(ErrorCode::LimitExceeded, "Bindless storage image heap full"));
    }
    const uint32_t slot = storage_free_stack_[--storage_free_top_];
    ++storage_in_use_;
    update_storage_image(slot, view, layout);
    return slot;
}

void BindlessHeap::update_storage_image(uint32_t slot, VkImageView view, VkImageLayout layout) const noexcept {
    if (device_ == nullptr || global_set_ == VK_NULL_HANDLE || view == VK_NULL_HANDLE || slot >= storage_capacity_) {
        return;
    }
    VkDescriptorImageInfo img_info{};
    img_info.imageView = view;
    img_info.imageLayout = layout;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = global_set_;
    write.dstBinding = 2;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
}

void BindlessHeap::release_storage_image(uint32_t slot) noexcept {
    if (slot < storage_capacity_ && storage_free_top_ < storage_free_stack_.size()) {
        storage_free_stack_[storage_free_top_++] = slot;
        if (storage_in_use_ > 0) {
            --storage_in_use_;
        }
    }
}

Expected<uint32_t> BindlessHeap::register_uint_sampled_image(VkImageView view, VkImageLayout layout) noexcept {
    if (device_ == nullptr || view == VK_NULL_HANDLE) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Invalid BindlessHeap or uint view"));
    }
    if (uint_sampled_free_top_ == 0) {
        return make_unexpected(Error::make(ErrorCode::LimitExceeded, "Bindless uint sampled heap full"));
    }
    const uint32_t slot = uint_sampled_free_stack_[--uint_sampled_free_top_];
    ++uint_sampled_in_use_;
    update_uint_sampled_image(slot, view, layout);
    return slot;
}

void BindlessHeap::update_uint_sampled_image(uint32_t slot, VkImageView view, VkImageLayout layout) const noexcept {
    if (device_ == nullptr || global_set_ == VK_NULL_HANDLE || view == VK_NULL_HANDLE ||
        slot >= uint_sampled_capacity_) {
        return;
    }
    VkDescriptorImageInfo img_info{};
    img_info.imageView = view;
    img_info.imageLayout = layout;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = global_set_;
    write.dstBinding = 3;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
}

void BindlessHeap::release_uint_sampled_image(uint32_t slot) noexcept {
    if (slot < uint_sampled_capacity_ && uint_sampled_free_top_ < uint_sampled_free_stack_.size()) {
        uint_sampled_free_stack_[uint_sampled_free_top_++] = slot;
        if (uint_sampled_in_use_ > 0) {
            --uint_sampled_in_use_;
        }
    }
}

Expected<void> BindlessHeap::update_tlas(VkAccelerationStructureKHR tlas) noexcept {
    if (device_ == nullptr || rt_set_ == VK_NULL_HANDLE) {
        return make_unexpected(Error::make(ErrorCode::Unsupported, "BindlessHeap RT Set 1 is not active"));
    }
    if (tlas == VK_NULL_HANDLE) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "BindlessHeap::update_tlas null handle"));
    }
    VkWriteDescriptorSetAccelerationStructureKHR as_write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    as_write.accelerationStructureCount = 1;
    as_write.pAccelerationStructures = &tlas;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.pNext = &as_write;
    write.dstSet = rt_set_;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
    return {};
}

} // namespace aether::rhi
