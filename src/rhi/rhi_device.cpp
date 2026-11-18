#include "rhi/rhi_device.hpp"

#include "core/log.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

namespace aether::rhi {
namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT type,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data) {
    if (user_data != nullptr) {
        static_cast<RhiDevice*>(user_data)->on_validation_message(severity, type, data);
    }
    return VK_FALSE;
}

[[nodiscard]] bool has_ext(const std::vector<VkExtensionProperties>& exts, const char* name) noexcept {
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_layer(const std::vector<VkLayerProperties>& layers, const char* name) noexcept {
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool contains_ci(const char* haystack, const char* needle) noexcept {
    if (haystack == nullptr || needle == nullptr) {
        return false;
    }
    const size_t nlen = std::strlen(needle);
    if (nlen == 0) {
        return true;
    }
    for (const char* p = haystack; *p != '\0'; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] != '\0' &&
               std::tolower(static_cast<unsigned char>(p[i])) == std::tolower(static_cast<unsigned char>(needle[i]))) {
            ++i;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}

struct EvaluatedPhysicalDevice {
    VkPhysicalDevice handle{VK_NULL_HANDLE};
    GpuAdapterDescriptor desc{};
    SelectedGpuInfo info{};
    uint32_t graphics_family{UINT32_MAX};
    uint32_t compute_family{UINT32_MAX};
    uint32_t transfer_family{UINT32_MAX};
    uint32_t graphics_family_queues{1};
    uint32_t transfer_queue_index{0};
    bool suitable{false};
    bool has_swapchain{false};
    bool has_memory_budget{false};
    bool has_calibrated_timestamps{false};
    bool has_ext_mem_win32{false};
    bool has_ext_sem_win32{false};
    bool has_ext_mem_host{false};
    bool has_accel_struct{false};
    bool has_ray_query{false};
    bool has_deferred_host_ops{false};
    bool has_as_update_after_bind{false};
    bool has_shader_int64{false};
    bool has_storage_write_no_fmt{false};
    bool has_storage_read_no_fmt{false};
    bool has_maintenance4{false};
    bool has_host_query_reset{false};
    bool has_scalar_block_layout{false};
    bool has_nonuniform_sampled{false};
    bool has_nonuniform_storage{false};
    bool has_shader_draw_parameters{false};
    bool has_demote_to_helper{false};
    bool has_shader_float16{false};
};

[[nodiscard]] EvaluatedPhysicalDevice evaluate_physical_device(VkPhysicalDevice pd, bool need_swapchain) {
    EvaluatedPhysicalDevice eval{};
    eval.handle = pd;

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_mem_props{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceIDProperties id_props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    id_props.pNext = &host_mem_props;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR as_props{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    as_props.pNext = &id_props;
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &as_props;
    vkGetPhysicalDeviceProperties2(pd, &props2);

    const auto& p = props2.properties;
    eval.info.device_name = p.deviceName;
    eval.info.vendor_id = p.vendorID;
    eval.info.device_id = p.deviceID;
    eval.info.api_version = p.apiVersion;
    eval.info.driver_version = p.driverVersion;
    eval.info.is_discrete = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
    eval.info.timestamp_period_ns = p.limits.timestampPeriod;
    eval.info.max_sampled_images = p.limits.maxDescriptorSetSampledImages;
    eval.info.max_storage_images = p.limits.maxDescriptorSetStorageImages;
    eval.info.max_samplers = p.limits.maxDescriptorSetSamplers;
    eval.info.luid_valid = (id_props.deviceLUIDValid == VK_TRUE);
    if (eval.info.luid_valid) {
        std::memcpy(eval.info.luid.data(), id_props.deviceLUID, sizeof(eval.info.luid));
    }
    std::memcpy(eval.info.device_uuid.data(), id_props.deviceUUID, VK_UUID_SIZE);
    std::memcpy(eval.info.driver_uuid.data(), id_props.driverUUID, VK_UUID_SIZE);

    if (p.apiVersion < VK_API_VERSION_1_3) {
        return eval;
    }

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs.data());

    for (uint32_t i = 0; i < qf_count; ++i) {
        const auto flags = qfs[i].queueFlags;
        if (eval.graphics_family == UINT32_MAX && (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                                                      (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            eval.graphics_family = i;
        }
        if ((flags & VK_QUEUE_COMPUTE_BIT) != 0 && (flags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
            eval.compute_family == UINT32_MAX) {
            eval.compute_family = i;
        }
        if ((flags & VK_QUEUE_TRANSFER_BIT) != 0 && (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0 &&
            eval.transfer_family == UINT32_MAX) {
            eval.transfer_family = i;
        }
    }
    if (eval.graphics_family == UINT32_MAX) {
        return eval;
    }
    eval.graphics_family_queues = qfs[eval.graphics_family].queueCount;
    if (eval.compute_family == UINT32_MAX) {
        eval.compute_family = eval.graphics_family;
    }
    if (eval.transfer_family == UINT32_MAX) {
        eval.transfer_family = eval.graphics_family;
        if (eval.graphics_family_queues >= 2u) {
            eval.transfer_queue_index = 1u;
        }
    }

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, exts.data());

    eval.has_swapchain = has_ext(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (need_swapchain && !eval.has_swapchain) {
        return eval;
    }
    eval.has_memory_budget = has_ext(exts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    eval.has_calibrated_timestamps = has_ext(exts, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    eval.has_ext_mem_win32 = has_ext(exts, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
    eval.has_ext_sem_win32 = has_ext(exts, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
    eval.has_ext_mem_host = has_ext(exts, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (eval.has_ext_mem_host && host_mem_props.minImportedHostPointerAlignment > 0u) {
        eval.info.min_imported_host_pointer_alignment =
            static_cast<uint32_t>(host_mem_props.minImportedHostPointerAlignment);
    }
    eval.has_accel_struct = has_ext(exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    eval.has_ray_query = has_ext(exts, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    eval.has_deferred_host_ops = has_ext(exts, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

    VkPhysicalDeviceRayQueryFeaturesKHR rq_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as_feat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    as_feat.pNext = &rq_feat;

    VkPhysicalDeviceVulkan13Features vk13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    vk13.pNext = &as_feat;
    VkPhysicalDeviceVulkan12Features vk12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vk12.pNext = &vk13;
    VkPhysicalDeviceVulkan11Features vk11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    vk11.pNext = &vk12;
    VkPhysicalDeviceFeatures2 feat2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    feat2.pNext = &vk11;
    vkGetPhysicalDeviceFeatures2(pd, &feat2);

    if (!vk12.timelineSemaphore || !vk12.bufferDeviceAddress || !vk12.descriptorIndexing ||
        !vk12.descriptorBindingSampledImageUpdateAfterBind || !vk12.descriptorBindingStorageImageUpdateAfterBind ||
        !vk12.descriptorBindingPartiallyBound || !vk12.runtimeDescriptorArray || !vk13.dynamicRendering ||
        !vk13.synchronization2) {
        return eval;
    }

    eval.info.texture_compression_bc = (feat2.features.textureCompressionBC == VK_TRUE);
    eval.info.fill_mode_non_solid = (feat2.features.fillModeNonSolid == VK_TRUE);
    eval.has_shader_int64 = (feat2.features.shaderInt64 == VK_TRUE);
    eval.has_storage_write_no_fmt = (feat2.features.shaderStorageImageWriteWithoutFormat == VK_TRUE);
    eval.has_storage_read_no_fmt = (feat2.features.shaderStorageImageReadWithoutFormat == VK_TRUE);
    eval.has_maintenance4 = (vk13.maintenance4 == VK_TRUE);
    eval.has_host_query_reset = (vk12.hostQueryReset == VK_TRUE);
    eval.has_scalar_block_layout = (vk12.scalarBlockLayout == VK_TRUE);
    eval.has_nonuniform_sampled = (vk12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE);
    eval.has_nonuniform_storage = (vk12.shaderStorageImageArrayNonUniformIndexing == VK_TRUE);
    eval.has_shader_draw_parameters = (vk11.shaderDrawParameters == VK_TRUE);
    eval.has_demote_to_helper = (vk13.shaderDemoteToHelperInvocation == VK_TRUE);
    eval.has_shader_float16 = (vk12.shaderFloat16 == VK_TRUE);
    eval.info.supports_fp16 = eval.has_shader_float16;

    eval.info.supports_accel_struct = eval.has_accel_struct && (as_feat.accelerationStructure == VK_TRUE);
    eval.has_as_update_after_bind = (as_feat.descriptorBindingAccelerationStructureUpdateAfterBind == VK_TRUE);
    eval.info.supports_ray_query = eval.has_ray_query && (rq_feat.rayQuery == VK_TRUE);
    if (eval.info.supports_accel_struct) {
        eval.info.min_accel_struct_scratch_offset_alignment =
            std::max(128u, as_props.minAccelerationStructureScratchOffsetAlignment);
    }

    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    uint64_t device_local_bytes = 0;
    uint64_t rebar_bytes = 0;
    uint64_t host_vis_bytes = 0;
    for (uint32_t t = 0; t < mem.memoryTypeCount; ++t) {
        const auto mflags = mem.memoryTypes[t].propertyFlags;
        const uint32_t hi = mem.memoryTypes[t].heapIndex;
        if (hi >= mem.memoryHeapCount) {
            continue;
        }
        const uint64_t hsz = mem.memoryHeaps[hi].size;
        if ((mem.memoryHeaps[hi].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            device_local_bytes = std::max(device_local_bytes, hsz);
        }
        if ((mflags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
            (mflags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
            rebar_bytes = std::max(rebar_bytes, hsz);
        }
        if ((mflags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
            host_vis_bytes = std::max(host_vis_bytes, hsz);
        }
    }
    eval.info.dedicated_vram_mb = device_local_bytes / (1024ULL * 1024ULL);
    eval.info.host_visible_vram_mb = host_vis_bytes / (1024ULL * 1024ULL);
    eval.info.rebar_enabled = rebar_bytes >= (256ULL * 1024ULL * 1024ULL);

    bool all_device_local_host_visible = (mem.memoryTypeCount > 0u);
    bool saw_device_local = false;
    for (uint32_t t = 0; t < mem.memoryTypeCount; ++t) {
        const auto mflags = mem.memoryTypes[t].propertyFlags;
        if ((mflags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            saw_device_local = true;
            if ((mflags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
                all_device_local_host_visible = false;
            }
        }
    }
    eval.info.is_uma =
        (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) || (saw_device_local && all_device_local_host_visible);
    eval.info.is_intel_igpu = (eval.info.vendor_id == 0x8086u) && eval.info.is_uma;
    eval.info.estimated_bandwidth_gbps =
        eval.info.is_uma ? 80u : (eval.info.is_discrete ? (eval.info.dedicated_vram_mb >= 8192u ? 320u : 224u) : 64u);

    const bool rt_ready = eval.info.supports_accel_struct && eval.info.supports_ray_query && eval.has_deferred_host_ops;
    eval.info.recommended_lighting_mode =
        (eval.info.is_uma || eval.info.is_intel_igpu || !rt_ready) ? uint8_t{0} : uint8_t{1};

    eval.desc.device_name = eval.info.device_name;
    eval.desc.vendor_id = eval.info.vendor_id;
    eval.desc.device_id = eval.info.device_id;
    eval.desc.device_type = p.deviceType;
    eval.desc.api_version = eval.info.api_version;
    eval.desc.driver_version = eval.info.driver_version;
    eval.desc.dedicated_vram_mb = eval.info.dedicated_vram_mb;
    eval.desc.host_visible_vram_mb = eval.info.host_visible_vram_mb;
    eval.desc.rebar_enabled = eval.info.rebar_enabled;
    eval.desc.is_uma = eval.info.is_uma;
    eval.desc.is_intel_igpu = eval.info.is_intel_igpu;
    eval.desc.supports_fp16 = eval.info.supports_fp16;
    eval.desc.supports_ray_query = eval.info.supports_ray_query;
    eval.desc.supports_accel_struct = eval.info.supports_accel_struct;
    eval.desc.supports_calibrated_timestamps = eval.has_calibrated_timestamps;
    eval.desc.supports_external_memory_win32 = eval.has_ext_mem_win32;
    eval.desc.estimated_bandwidth_gbps = eval.info.estimated_bandwidth_gbps;
    eval.desc.recommended_lighting_mode = eval.info.recommended_lighting_mode;
    eval.desc.luid = eval.info.luid;
    eval.desc.luid_valid = eval.info.luid_valid;
    eval.desc.device_uuid = eval.info.device_uuid;
    eval.desc.driver_uuid = eval.info.driver_uuid;
    eval.desc.score = RhiDevice::score_adapter_for_vendor(eval.info.vendor_id, p.deviceType, rt_ready,
                                                          eval.info.rebar_enabled, eval.info.dedicated_vram_mb);

    eval.suitable = true;
    return eval;
}

[[nodiscard]] Expected<VkInstance> create_vk_instance(const RhiDeviceConfig& cfg, bool& out_validation_active,
                                                      bool& out_sync_active) {
    out_validation_active = false;
    out_sync_active = false;

    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());

    uint32_t ext_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, exts.data());

    const bool has_khronos_val = has_layer(layers, "VK_LAYER_KHRONOS_validation");
    std::vector<VkExtensionProperties> val_exts;
    if (has_khronos_val) {
        uint32_t val_ext_count = 0;
        vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &val_ext_count, nullptr);
        if (val_ext_count > 0) {
            val_exts.resize(val_ext_count);
            vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &val_ext_count, val_exts.data());
        }
    }

    std::vector<const char*> enabled_layers;
    std::vector<const char*> enabled_exts;
    if (!cfg.headless) {
        if (!has_ext(exts, VK_KHR_SURFACE_EXTENSION_NAME) || !has_ext(exts, VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
            return make_unexpected(Error::make(ErrorCode::VulkanError, "Missing Win32 Vulkan surface extensions"));
        }
        enabled_exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        enabled_exts.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    }

    const bool has_debug_utils =
        has_ext(exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) || has_ext(val_exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const bool has_val_features = has_ext(exts, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) ||
                                  has_ext(val_exts, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);

    if (cfg.enable_validation && has_khronos_val && has_debug_utils) {
        enabled_layers.push_back("VK_LAYER_KHRONOS_validation");
        enabled_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        out_validation_active = true;
        if (has_val_features) {
            enabled_exts.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            out_sync_active = true;
        }
    } else if (cfg.require_validation) {
        AETHER_LOG_ERROR("rhi",
                         "Validation layer init blocked: has_khronos_val=%d has_debug_utils=%d (layers=%zu exts=%zu)",
                         has_khronos_val ? 1 : 0, has_debug_utils ? 1 : 0, layers.size(), exts.size());
        return make_unexpected(Error::make(ErrorCode::VulkanError, "Required Vulkan validation layer not available"));
    }

    if (cfg.require_validation && !out_sync_active) {
        AETHER_LOG_ERROR("rhi", "Validation features missing: has_val_features=%d", has_val_features ? 1 : 0);
        return make_unexpected(
            Error::make(ErrorCode::VulkanError, "VK_EXT_validation_features required for sync validation"));
    }

    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "AETHER-2.5D";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 4, 0);
    app_info.pEngineName = "AETHER-RHI";
    app_info.engineVersion = VK_MAKE_VERSION(0, 4, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;

    std::array<VkValidationFeatureEnableEXT, 2> val_enables{
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
        VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
    };
    VkValidationFeaturesEXT val_feat{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    val_feat.enabledValidationFeatureCount = cfg.enable_gpu_assisted ? 2u : 1u;
    val_feat.pEnabledValidationFeatures = val_enables.data();

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app_info;
    ici.enabledLayerCount = static_cast<uint32_t>(enabled_layers.size());
    ici.ppEnabledLayerNames = enabled_layers.data();
    ici.enabledExtensionCount = static_cast<uint32_t>(enabled_exts.size());
    ici.ppEnabledExtensionNames = enabled_exts.data();
    if (out_sync_active) {
        ici.pNext = &val_feat;
    }

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        return make_unexpected(Error::make(ErrorCode::VulkanError, "vkCreateInstance failed"));
    }
    return inst;
}

[[nodiscard]] VkPipelineCache load_pipeline_cache_from_disk(VkDevice device, const std::filesystem::path& path) {
    VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    std::vector<uint8_t> bytes;
    if (!path.empty() && std::filesystem::is_regular_file(path)) {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (ifs.good()) {
            const auto sz = static_cast<size_t>(ifs.tellg());
            if (sz >= 16 && sz <= 64ULL * 1024ULL * 1024ULL) {
                bytes.resize(sz);
                ifs.seekg(0);
                if (!ifs.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(sz))) {
                    bytes.clear();
                }
            }
        }
    }
    pcci.initialDataSize = bytes.size();
    pcci.pInitialData = bytes.empty() ? nullptr : bytes.data();

    VkPipelineCache cache = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(device, &pcci, nullptr, &cache) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return cache;
}

} // namespace

RhiDevice::~RhiDevice() {
    destroy();
}

void RhiDevice::on_validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                      VkDebugUtilsMessageTypeFlagsEXT type,
                                      const VkDebugUtilsMessengerCallbackDataEXT* data) noexcept {
    (void)type;
    const char* msg = (data != nullptr && data->pMessage != nullptr) ? data->pMessage : "";
    const char* id_name = (data != nullptr && data->pMessageIdName != nullptr) ? data->pMessageIdName : "";

    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        validation_errors_.fetch_add(1, std::memory_order_acq_rel);
        AETHER_LOG_ERROR("vk-val", "[%s] %s", id_name, msg);
        std::fprintf(stderr, "[ERROR][vk-val] [%s] %s\n", id_name, msg);
        std::fflush(stderr);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        // Ignore harmless loader warnings unrelated to application API usage
        if (contains_ci(msg, "loader") && contains_ci(msg, "json")) {
            return;
        }
        // Ignore GPU-Assisted Validation internal device-feature promotion notices on vkCreateDevice
        if (contains_ci(id_name, "GPU-Assisted-Validation") && contains_ci(msg, "Internal Warning:")) {
            AETHER_LOG_INFO("vk-val", "[%s] %s", id_name, msg);
            return;
        }
        validation_warnings_.fetch_add(1, std::memory_order_acq_rel);
        AETHER_LOG_WARN("vk-val", "[%s] %s", id_name, msg);
        std::fprintf(stderr, "[WARN][vk-val] [%s] %s\n", id_name, msg);
        std::fflush(stderr);
    }
}

uint32_t RhiDevice::score_adapter_for_vendor(uint32_t vendor_id, VkPhysicalDeviceType type, bool rt_ready, bool rebar,
                                             uint64_t dedicated_vram_mb) noexcept {
    uint32_t score = 0;
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        score += 1000u;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        score += 200u;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        score += 50u;
        break;
    default:
        score += 10u;
        break;
    }
    if (rt_ready) {
        score += 500u;
    }
    if (rebar) {
        score += 100u;
    }
    score += static_cast<uint32_t>(std::min<uint64_t>(dedicated_vram_mb / 512ULL, 200ULL));

    if (vendor_id == 0x10DEu) {
        score += 40u;
    } else if (vendor_id == 0x1002u) {
        score += 30u;
    } else if (vendor_id == 0x8086u) {
        score += 20u;
    }
    return score;
}

Expected<std::vector<GpuAdapterDescriptor>> RhiDevice::enumerate_vulkan_devices(const RhiDeviceConfig& cfg) {
    RhiDeviceConfig probe_cfg = cfg;
    probe_cfg.enable_validation = false;
    probe_cfg.require_validation = false;
    probe_cfg.headless = true;

    bool dummy_v = false, dummy_s = false;
    auto inst_res = create_vk_instance(probe_cfg, dummy_v, dummy_s);
    if (!inst_res) {
        return make_unexpected(inst_res.error());
    }
    VkInstance inst = inst_res.value();

    uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(inst, &pd_count, nullptr);
    std::vector<VkPhysicalDevice> pds(pd_count);
    vkEnumeratePhysicalDevices(inst, &pd_count, pds.data());

    std::vector<GpuAdapterDescriptor> out;
    out.reserve(pd_count);
    for (VkPhysicalDevice pd : pds) {
        const auto eval = evaluate_physical_device(pd, false);
        if (eval.suitable) {
            out.push_back(eval.desc);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const GpuAdapterDescriptor& a, const GpuAdapterDescriptor& b) { return a.score > b.score; });

    vkDestroyInstance(inst, nullptr);
    return out;
}

Expected<RhiDevice*> RhiDevice::create(const RhiDeviceConfig& cfg) {
    auto dev = std::unique_ptr<RhiDevice>(new RhiDevice());

    auto inst_res = create_vk_instance(cfg, dev->validation_active_, dev->sync_validation_active_);
    if (!inst_res) {
        return make_unexpected(inst_res.error());
    }
    dev->instance_ = inst_res.value();

    if (dev->validation_active_) {
        auto create_dbg = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(dev->instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create_dbg != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT dbg_ci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            dbg_ci.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            dbg_ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbg_ci.pfnUserCallback = vk_debug_callback;
            dbg_ci.pUserData = dev.get();
            (void)create_dbg(dev->instance_, &dbg_ci, nullptr, &dev->debug_messenger_);
        }
    }

    uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(dev->instance_, &pd_count, nullptr);
    if (pd_count == 0) {
        return make_unexpected(Error::make(ErrorCode::VulkanError, "No Vulkan physical devices available"));
    }
    std::vector<VkPhysicalDevice> pds(pd_count);
    vkEnumeratePhysicalDevices(dev->instance_, &pd_count, pds.data());

    std::vector<EvaluatedPhysicalDevice> candidates;
    for (VkPhysicalDevice pd : pds) {
        auto e = evaluate_physical_device(pd, !cfg.headless);
        if (e.suitable) {
            candidates.push_back(std::move(e));
        }
    }
    if (candidates.empty()) {
        return make_unexpected(Error::make(ErrorCode::VulkanError, "No Vulkan 1.3 capable GPU satisfies requirements"));
    }

    // Select adapter respecting LUID, vendor/device override, integrated flag, and hardware inventory
    const EvaluatedPhysicalDevice* chosen = nullptr;
    if (cfg.use_preferred_luid) {
        for (const auto& c : candidates) {
            if (c.info.luid_valid && c.info.luid == cfg.preferred_luid) {
                chosen = &c;
                break;
            }
        }
    }
    if (chosen == nullptr && cfg.preferred_vendor_id != 0) {
        for (const auto& c : candidates) {
            if (c.info.vendor_id == cfg.preferred_vendor_id &&
                (cfg.preferred_device_id == 0 || c.info.device_id == cfg.preferred_device_id)) {
                if (chosen == nullptr || c.desc.score > chosen->desc.score) {
                    chosen = &c;
                }
            }
        }
    }
    if (chosen == nullptr && cfg.prefer_integrated) {
        for (const auto& c : candidates) {
            if (c.desc.device_type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                if (chosen == nullptr || c.desc.score > chosen->desc.score) {
                    chosen = &c;
                }
            }
        }
    }
    if (chosen == nullptr) {
        for (const auto& c : candidates) {
            if (chosen == nullptr || c.desc.score > chosen->desc.score) {
                chosen = &c;
            }
        }
    }

    dev->physical_device_ = chosen->handle;
    dev->graphics_family_ = chosen->graphics_family;
    dev->compute_family_ = chosen->compute_family;
    dev->transfer_family_ = chosen->transfer_family;
    dev->gpu_info_ = chosen->info;

    std::set<uint32_t> unique_families{dev->graphics_family_, dev->compute_family_, dev->transfer_family_};
    const float q_prios[2] = {1.0f, 0.75f};
    std::vector<VkDeviceQueueCreateInfo> q_infos;
    for (uint32_t fam : unique_families) {
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = fam;
        qci.queueCount = (fam == dev->graphics_family_ && chosen->transfer_queue_index > 0u) ? 2u : 1u;
        qci.pQueuePriorities = q_prios;
        q_infos.push_back(qci);
    }

    std::vector<const char*> dev_exts;
    if (!cfg.headless && chosen->has_swapchain) {
        dev_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    if (chosen->has_memory_budget) {
        dev_exts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        dev->gpu_info_.memory_budget_active = true;
    }
    if (chosen->has_calibrated_timestamps) {
        dev_exts.push_back(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        dev->gpu_info_.calibrated_timestamps_active = true;
    }
    if (chosen->has_ext_mem_win32 && chosen->has_ext_sem_win32) {
        dev_exts.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
        dev_exts.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
        dev->gpu_info_.external_memory_win32_active = true;
    }
    if (chosen->has_ext_mem_host) {
        dev_exts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        dev->gpu_info_.external_memory_host_active = true;
    }

    const bool rt_enable = cfg.enable_ray_tracing && chosen->info.supports_accel_struct &&
                           chosen->info.supports_ray_query && chosen->has_deferred_host_ops;
    if (rt_enable) {
        dev_exts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        dev_exts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        dev_exts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        dev->gpu_info_.rt_enabled = true;
    }

    VkPhysicalDeviceRayQueryFeaturesKHR rq_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rq_feat.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as_feat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    as_feat.accelerationStructure = VK_TRUE;
    as_feat.descriptorBindingAccelerationStructureUpdateAfterBind =
        chosen->has_as_update_after_bind ? VK_TRUE : VK_FALSE;
    as_feat.pNext = &rq_feat;

    VkPhysicalDeviceVulkan13Features vk13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    vk13.dynamicRendering = VK_TRUE;
    vk13.synchronization2 = VK_TRUE;
    vk13.maintenance4 = chosen->has_maintenance4 ? VK_TRUE : VK_FALSE;
    vk13.shaderDemoteToHelperInvocation = chosen->has_demote_to_helper ? VK_TRUE : VK_FALSE;
    if (rt_enable) {
        vk13.pNext = &as_feat;
    }

    VkPhysicalDeviceVulkan12Features vk12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vk12.timelineSemaphore = VK_TRUE;
    vk12.bufferDeviceAddress = VK_TRUE;
    vk12.descriptorIndexing = VK_TRUE;
    vk12.hostQueryReset = chosen->has_host_query_reset ? VK_TRUE : VK_FALSE;
    vk12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    vk12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    vk12.descriptorBindingPartiallyBound = VK_TRUE;
    vk12.runtimeDescriptorArray = VK_TRUE;
    vk12.shaderSampledImageArrayNonUniformIndexing = chosen->has_nonuniform_sampled ? VK_TRUE : VK_FALSE;
    vk12.shaderStorageImageArrayNonUniformIndexing = chosen->has_nonuniform_storage ? VK_TRUE : VK_FALSE;
    vk12.scalarBlockLayout = chosen->has_scalar_block_layout ? VK_TRUE : VK_FALSE;
    vk12.shaderFloat16 = chosen->has_shader_float16 ? VK_TRUE : VK_FALSE;
    vk12.pNext = &vk13;

    VkPhysicalDeviceVulkan11Features vk11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    vk11.shaderDrawParameters = chosen->has_shader_draw_parameters ? VK_TRUE : VK_FALSE;
    vk11.pNext = &vk12;

    VkPhysicalDeviceFeatures2 feat2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    feat2.pNext = &vk11;
    feat2.features.fillModeNonSolid = chosen->info.fill_mode_non_solid ? VK_TRUE : VK_FALSE;
    feat2.features.textureCompressionBC = chosen->info.texture_compression_bc ? VK_TRUE : VK_FALSE;
    feat2.features.shaderInt64 = chosen->has_shader_int64 ? VK_TRUE : VK_FALSE;
    feat2.features.shaderStorageImageWriteWithoutFormat = chosen->has_storage_write_no_fmt ? VK_TRUE : VK_FALSE;
    feat2.features.shaderStorageImageReadWithoutFormat = chosen->has_storage_read_no_fmt ? VK_TRUE : VK_FALSE;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &feat2;
    dci.queueCreateInfoCount = static_cast<uint32_t>(q_infos.size());
    dci.pQueueCreateInfos = q_infos.data();
    dci.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
    dci.ppEnabledExtensionNames = dev_exts.data();

    const VkResult d_res = vkCreateDevice(dev->physical_device_, &dci, nullptr, &dev->device_);
    if (d_res != VK_SUCCESS) {
        AETHER_LOG_ERROR("rhi", "vkCreateDevice failed with VkResult=%d", static_cast<int>(d_res));
        return make_unexpected(Error::make(ErrorCode::VulkanError, "vkCreateDevice failed"));
    }

    vkGetDeviceQueue(dev->device_, dev->graphics_family_, 0, &dev->graphics_queue_);
    vkGetDeviceQueue(dev->device_, dev->compute_family_, 0, &dev->compute_queue_);
    vkGetDeviceQueue(dev->device_, dev->transfer_family_, chosen->transfer_queue_index, &dev->transfer_queue_);

    dev->ext_.get_buffer_device_address =
        reinterpret_cast<PFN_vkGetBufferDeviceAddress>(vkGetDeviceProcAddr(dev->device_, "vkGetBufferDeviceAddress"));
    if (rt_enable) {
        dev->ext_.create_acceleration_structure = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
            vkGetDeviceProcAddr(dev->device_, "vkCreateAccelerationStructureKHR"));
        dev->ext_.destroy_acceleration_structure = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
            vkGetDeviceProcAddr(dev->device_, "vkDestroyAccelerationStructureKHR"));
        dev->ext_.get_acceleration_structure_build_sizes =
            reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
                vkGetDeviceProcAddr(dev->device_, "vkGetAccelerationStructureBuildSizesKHR"));
        dev->ext_.cmd_build_acceleration_structures = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            vkGetDeviceProcAddr(dev->device_, "vkCmdBuildAccelerationStructuresKHR"));
        dev->ext_.get_acceleration_structure_device_address =
            reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
                vkGetDeviceProcAddr(dev->device_, "vkGetAccelerationStructureDeviceAddressKHR"));
    }
    if (dev->gpu_info_.calibrated_timestamps_active) {
        dev->ext_.get_calibrated_timestamps = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
            vkGetDeviceProcAddr(dev->device_, "vkGetCalibratedTimestampsKHR"));
        dev->ext_.get_physical_device_calibrateable_time_domains =
            reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
                vkGetInstanceProcAddr(dev->instance_, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"));
    }
    if (dev->gpu_info_.external_memory_host_active) {
        dev->ext_.get_memory_host_pointer_properties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
            vkGetDeviceProcAddr(dev->device_, "vkGetMemoryHostPointerPropertiesEXT"));
    }

    VmaAllocatorCreateInfo vma_ci{};
    vma_ci.physicalDevice = dev->physical_device_;
    vma_ci.device = dev->device_;
    vma_ci.instance = dev->instance_;
    vma_ci.vulkanApiVersion = VK_API_VERSION_1_3;
    vma_ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (dev->gpu_info_.memory_budget_active) {
        vma_ci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    if (vmaCreateAllocator(&vma_ci, &dev->allocator_) != VK_SUCCESS) {
        return make_unexpected(Error::make(ErrorCode::VulkanError, "vmaCreateAllocator failed"));
    }

    auto create_timeline = [&](VkSemaphore& out_sem) -> Expected<void> {
        VkSemaphoreTypeCreateInfo stci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        stci.initialValue = 0;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sci.pNext = &stci;
        if (vkCreateSemaphore(dev->device_, &sci, nullptr, &out_sem) != VK_SUCCESS) {
            return make_unexpected(Error::make(ErrorCode::VulkanError, "vkCreateSemaphore(timeline) failed"));
        }
        return {};
    };
    if (auto r = create_timeline(dev->frame_timeline_); !r) {
        return make_unexpected(r.error());
    }
    if (auto r = create_timeline(dev->output_timeline_); !r) {
        return make_unexpected(r.error());
    }

    if (!cfg.pipeline_cache_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cfg.pipeline_cache_dir, ec);
        dev->pipeline_cache_path_ = cfg.pipeline_cache_dir / "aether_vk_pipeline_cache.bin";
    }
    dev->pipeline_cache_ = load_pipeline_cache_from_disk(dev->device_, dev->pipeline_cache_path_);

    AETHER_LOG_INFO("rhi", "Vulkan 1.3 GPU selected: %s (vendor=0x%04X RT=%d ReBAR=%d Val=%d SyncVal=%d)",
                    dev->gpu_info_.device_name.c_str(), dev->gpu_info_.vendor_id, dev->gpu_info_.rt_enabled ? 1 : 0,
                    dev->gpu_info_.rebar_enabled ? 1 : 0, dev->validation_active_ ? 1 : 0,
                    dev->sync_validation_active_ ? 1 : 0);

    return dev.release();
}

void RhiDevice::save_pipeline_cache() const noexcept {
    if (device_ == VK_NULL_HANDLE || pipeline_cache_ == VK_NULL_HANDLE || pipeline_cache_path_.empty() ||
        is_device_lost()) {
        return;
    }
    try {
        size_t sz = 0;
        if (vkGetPipelineCacheData(device_, pipeline_cache_, &sz, nullptr) != VK_SUCCESS || sz == 0) {
            return;
        }
        std::vector<uint8_t> data(sz);
        if (vkGetPipelineCacheData(device_, pipeline_cache_, &sz, data.data()) != VK_SUCCESS) {
            return;
        }
        std::ofstream ofs(pipeline_cache_path_, std::ios::binary | std::ios::trunc);
        if (ofs.good()) {
            ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(sz));
        }
    } catch (...) {
        AETHER_LOG_WARN("rhi", "Failed to save Vulkan pipeline cache (OOM or I/O error)");
    }
}

void RhiDevice::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (!is_device_lost()) {
            vkDeviceWaitIdle(device_);
            save_pipeline_cache();
        }
        if (pipeline_cache_ != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
            pipeline_cache_ = VK_NULL_HANDLE;
        }
        if (frame_timeline_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, frame_timeline_, nullptr);
            frame_timeline_ = VK_NULL_HANDLE;
        }
        if (output_timeline_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, output_timeline_, nullptr);
            output_timeline_ = VK_NULL_HANDLE;
        }
    }
    if (allocator_ != nullptr) {
        vmaDestroyAllocator(allocator_);
        allocator_ = nullptr;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE && debug_messenger_ != VK_NULL_HANDLE) {
        auto destroy_dbg = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_dbg != nullptr) {
            destroy_dbg(instance_, debug_messenger_, nullptr);
        }
        debug_messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    physical_device_ = VK_NULL_HANDLE;
    graphics_queue_ = VK_NULL_HANDLE;
    compute_queue_ = VK_NULL_HANDLE;
    transfer_queue_ = VK_NULL_HANDLE;
    graphics_family_ = UINT32_MAX;
    compute_family_ = UINT32_MAX;
    transfer_family_ = UINT32_MAX;
    ext_ = RhiExtDispatch{};
    gpu_info_ = SelectedGpuInfo{};
    validation_active_ = false;
    sync_validation_active_ = false;
}

Expected<RhiBuffer> RhiDevice::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage mem_usage,
                                             VmaAllocationCreateFlags vma_flags) const {
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr || size == 0) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Invalid RhiDevice or zero buffer size"));
    }
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo aci{};
    aci.usage = mem_usage;
    aci.flags = vma_flags;

    RhiBuffer out{};
    out.size = size;
    if (vmaCreateBuffer(allocator_, &bci, &aci, &out.handle, &out.allocation, &out.info) != VK_SUCCESS) {
        return make_unexpected(Error::make(ErrorCode::OutOfMemory, "vmaCreateBuffer failed"));
    }
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0 && ext_.get_buffer_device_address != nullptr) {
        out.device_address = buffer_device_address(out.handle);
    }
    return out;
}

Expected<RhiBuffer> RhiDevice::create_uma_zero_copy_buffer(VkDeviceSize size, VkBufferUsageFlags usage) const {
    const VmaAllocationCreateFlags flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT |
        (gpu_info_.is_uma ? VmaAllocationCreateFlags{0} : VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT);
    const VmaMemoryUsage mem = gpu_info_.is_uma ? VMA_MEMORY_USAGE_AUTO : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    return create_buffer(size, usage, mem, flags);
}

void RhiDevice::destroy_buffer(RhiBuffer& buf) const noexcept {
    if (allocator_ != nullptr && buf.handle != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buf.handle, buf.allocation);
    }
    buf = RhiBuffer{};
}

VkDeviceAddress RhiDevice::buffer_device_address(VkBuffer buffer) const noexcept {
    if (device_ == VK_NULL_HANDLE || buffer == VK_NULL_HANDLE || ext_.get_buffer_device_address == nullptr) {
        return 0;
    }
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = buffer;
    return ext_.get_buffer_device_address(device_, &ai);
}

Expected<VkImageView> RhiDevice::create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                                   uint32_t base_mip, uint32_t mip_count) const {
    if (device_ == VK_NULL_HANDLE || image == VK_NULL_HANDLE) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "create_image_view invalid args"));
    }
    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = format;
    ivci.subresourceRange.aspectMask = aspect;
    ivci.subresourceRange.baseMipLevel = base_mip;
    ivci.subresourceRange.levelCount = mip_count;
    ivci.subresourceRange.baseArrayLayer = 0;
    ivci.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device_, &ivci, nullptr, &view) != VK_SUCCESS) {
        return make_unexpected(Error::make(ErrorCode::VulkanError, "vkCreateImageView failed"));
    }
    return view;
}

void RhiDevice::destroy_image_view(VkImageView view) const noexcept {
    if (device_ != VK_NULL_HANDLE && view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, view, nullptr);
    }
}

Expected<RhiImage> RhiDevice::create_image_2d(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                                              VkImageAspectFlags aspect, bool concurrent_transfer) const {
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr || width == 0 || height == 0) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "create_image_2d invalid args"));
    }
    const uint32_t concurrent_fams[2] = {graphics_family_, transfer_family_};
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (concurrent_transfer && graphics_family_ != UINT32_MAX && transfer_family_ != UINT32_MAX &&
        graphics_family_ != transfer_family_) {
        ici.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ici.queueFamilyIndexCount = 2;
        ici.pQueueFamilyIndices = concurrent_fams;
    } else {
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    RhiImage out{};
    out.format = format;
    out.width = width;
    out.height = height;
    if (vmaCreateImage(allocator_, &ici, &aci, &out.handle, &out.allocation, nullptr) != VK_SUCCESS) {
        return make_unexpected(Error::make(ErrorCode::OutOfMemory, "vmaCreateImage failed"));
    }

    auto view_res = create_image_view(out.handle, format, aspect, 0, 1);
    if (!view_res) {
        vmaDestroyImage(allocator_, out.handle, out.allocation);
        return make_unexpected(view_res.error());
    }
    out.view = view_res.value();
    return out;
}

void RhiDevice::destroy_image(RhiImage& img) const noexcept {
    if (device_ != VK_NULL_HANDLE && img.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, img.view, nullptr);
    }
    if (allocator_ != nullptr && img.handle != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, img.handle, img.allocation);
    }
    img = RhiImage{};
}

VramBudgetInfo RhiDevice::query_vram_budget() const noexcept {
    VramBudgetInfo info{};
    if (allocator_ == nullptr) {
        return info;
    }
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator_, budgets);

    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem);
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            info.budget_bytes += budgets[i].budget;
            info.usage_bytes += budgets[i].usage;
        }
    }
    info.exact_budget_extension = gpu_info_.memory_budget_active;
    return info;
}

Expected<void> RhiDevice::calibrated_timestamp_ns(uint64_t& gpu_ns, uint64_t& cpu_qpc_ns) const noexcept {
    if (device_ == VK_NULL_HANDLE || ext_.get_calibrated_timestamps == nullptr) {
        return make_unexpected(Error::make(ErrorCode::Unsupported, "Calibrated timestamps extension not active"));
    }
    std::array<VkCalibratedTimestampInfoKHR, 2> infos{};
    infos[0].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
    infos[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    infos[1].timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;

    std::array<uint64_t, 2> ts{};
    uint64_t max_dev = 0;
    if (ext_.get_calibrated_timestamps(device_, 2, infos.data(), ts.data(), &max_dev) != VK_SUCCESS) {
        return make_unexpected(Error::make(ErrorCode::VulkanError, "vkGetCalibratedTimestampsKHR failed"));
    }

    gpu_ns = static_cast<uint64_t>(static_cast<double>(ts[0]) * static_cast<double>(gpu_info_.timestamp_period_ns));
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    const double qpc_to_ns = (freq.QuadPart > 0) ? (1e9 / static_cast<double>(freq.QuadPart)) : 100.0;
    cpu_qpc_ns = static_cast<uint64_t>(static_cast<double>(ts[1]) * qpc_to_ns);
    return {};
}

Expected<void> RhiDevice::recreate_after_device_lost(const RhiDeviceConfig& cfg) {
    const uint64_t prev_errors = validation_errors_.load(std::memory_order_acquire);
    const uint64_t prev_warnings = validation_warnings_.load(std::memory_order_acquire);
    destroy();
    device_lost_.store(false, std::memory_order_release);

    auto fresh_res = RhiDevice::create(cfg);
    if (!fresh_res) {
        return make_unexpected(fresh_res.error());
    }
    RhiDevice* fresh = fresh_res.value();

    instance_ = fresh->instance_;
    debug_messenger_ = fresh->debug_messenger_;
    physical_device_ = fresh->physical_device_;
    device_ = fresh->device_;
    graphics_queue_ = fresh->graphics_queue_;
    graphics_family_ = fresh->graphics_family_;
    compute_queue_ = fresh->compute_queue_;
    compute_family_ = fresh->compute_family_;
    transfer_queue_ = fresh->transfer_queue_;
    transfer_family_ = fresh->transfer_family_;
    allocator_ = fresh->allocator_;
    frame_timeline_ = fresh->frame_timeline_;
    output_timeline_ = fresh->output_timeline_;
    pipeline_cache_ = fresh->pipeline_cache_;
    pipeline_cache_path_ = std::move(fresh->pipeline_cache_path_);
    gpu_info_ = std::move(fresh->gpu_info_);
    ext_ = fresh->ext_;
    validation_active_ = fresh->validation_active_;
    sync_validation_active_ = fresh->sync_validation_active_;
    validation_errors_.store(prev_errors + fresh->validation_errors_.load(), std::memory_order_release);
    validation_warnings_.store(prev_warnings + fresh->validation_warnings_.load(), std::memory_order_release);

    if (debug_messenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        // Rebind debug userData pointer cleanly to `this`
        auto destroy_dbg = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        auto create_dbg = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (destroy_dbg != nullptr) {
            destroy_dbg(instance_, debug_messenger_, nullptr);
            debug_messenger_ = VK_NULL_HANDLE;
        }
        if (create_dbg != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT dbg_ci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            dbg_ci.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            dbg_ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbg_ci.pfnUserCallback = vk_debug_callback;
            dbg_ci.pUserData = this;
            (void)create_dbg(instance_, &dbg_ci, nullptr, &debug_messenger_);
        }
    }

    // Null out handles on `fresh` so deleting it leaves transferred resources intact
    fresh->instance_ = VK_NULL_HANDLE;
    fresh->debug_messenger_ = VK_NULL_HANDLE;
    fresh->physical_device_ = VK_NULL_HANDLE;
    fresh->device_ = VK_NULL_HANDLE;
    fresh->allocator_ = nullptr;
    fresh->frame_timeline_ = VK_NULL_HANDLE;
    fresh->output_timeline_ = VK_NULL_HANDLE;
    fresh->pipeline_cache_ = VK_NULL_HANDLE;
    delete fresh;
    return {};
}

void RhiDevice::cmd_transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                                     VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                                     VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                                     VkAccessFlags2 dst_access, VkImageAspectFlags aspect) noexcept {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void RhiDevice::wait_idle() const noexcept {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
}

} // namespace aether::rhi
