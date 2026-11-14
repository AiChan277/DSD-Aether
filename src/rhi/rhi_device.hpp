#pragma once

#include "core/expected.hpp"
#include "core/hw_enum.hpp"

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aether::rhi {

struct RhiDeviceConfig {
    bool enable_validation{true};
    bool require_validation{false};
    bool enable_gpu_assisted{false};
    bool headless{false}; // true omits VK_KHR_surface/VK_KHR_win32_surface when creating offscreen test devices
    const HardwareInventory* inventory{nullptr};
    std::array<uint8_t, 8> preferred_luid{};
    bool use_preferred_luid{false};
    uint32_t preferred_vendor_id{0};
    uint32_t preferred_device_id{0};
    bool enable_ray_tracing{true};
    bool prefer_integrated{false};
    std::filesystem::path pipeline_cache_dir{};
};

struct RhiBuffer {
    VkBuffer handle{VK_NULL_HANDLE};
    VmaAllocation allocation{nullptr};
    VmaAllocationInfo info{};
    VkDeviceSize size{0};
    VkDeviceAddress device_address{0};
};

struct RhiImage {
    VkImage handle{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VmaAllocation allocation{nullptr};
    VkFormat format{VK_FORMAT_UNDEFINED};
    uint32_t width{0};
    uint32_t height{0};
};

struct GpuAdapterDescriptor {
    std::string device_name{};
    uint32_t vendor_id{0};
    uint32_t device_id{0};
    VkPhysicalDeviceType device_type{VK_PHYSICAL_DEVICE_TYPE_OTHER};
    uint32_t api_version{0};
    uint32_t driver_version{0};
    uint64_t dedicated_vram_mb{0};
    uint64_t host_visible_vram_mb{0};
    bool rebar_enabled{false};
    bool supports_ray_query{false};
    bool supports_accel_struct{false};
    bool supports_calibrated_timestamps{false};
    bool supports_external_memory_win32{false};
    std::array<uint8_t, 8> luid{};
    bool luid_valid{false};
    std::array<uint8_t, VK_UUID_SIZE> device_uuid{};
    std::array<uint8_t, VK_UUID_SIZE> driver_uuid{};
    uint32_t score{0};
};

struct VramBudgetInfo {
    uint64_t budget_bytes{0};
    uint64_t usage_bytes{0};
    bool exact_budget_extension{false};
};

struct RhiExtDispatch {
    PFN_vkGetBufferDeviceAddress get_buffer_device_address{nullptr};
    PFN_vkCreateAccelerationStructureKHR create_acceleration_structure{nullptr};
    PFN_vkDestroyAccelerationStructureKHR destroy_acceleration_structure{nullptr};
    PFN_vkGetAccelerationStructureBuildSizesKHR get_acceleration_structure_build_sizes{nullptr};
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build_acceleration_structures{nullptr};
    PFN_vkGetAccelerationStructureDeviceAddressKHR get_acceleration_structure_device_address{nullptr};
    PFN_vkGetCalibratedTimestampsKHR get_calibrated_timestamps{nullptr};
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR get_physical_device_calibrateable_time_domains{nullptr};
    PFN_vkGetMemoryHostPointerPropertiesEXT get_memory_host_pointer_properties{nullptr};
};

struct SelectedGpuInfo {
    std::string device_name{};
    uint32_t vendor_id{0};
    uint32_t device_id{0};
    uint32_t api_version{0};
    uint32_t driver_version{0};
    uint64_t dedicated_vram_mb{0};
    uint64_t host_visible_vram_mb{0};
    bool is_discrete{false};
    bool rebar_enabled{false};
    bool texture_compression_bc{false};
    bool fill_mode_non_solid{false};
    std::array<uint8_t, 8> luid{};
    bool luid_valid{false};
    std::array<uint8_t, VK_UUID_SIZE> device_uuid{};
    std::array<uint8_t, VK_UUID_SIZE> driver_uuid{};
    bool supports_ray_query{false};
    bool supports_accel_struct{false};
    bool rt_enabled{false};
    bool calibrated_timestamps_active{false};
    bool memory_budget_active{false};
    bool external_memory_win32_active{false};
    bool external_memory_host_active{false};
    uint32_t min_imported_host_pointer_alignment{4096};
    float timestamp_period_ns{1.0f};
    uint32_t max_sampled_images{0};
    uint32_t max_storage_images{0};
    uint32_t max_samplers{0};
    uint32_t min_accel_struct_scratch_offset_alignment{128};
};

/**
 * Core Vulkan 1.3 RHI device owning `VkInstance`, debug validation messenger,
 * `VkPhysicalDevice`, `VkDevice`, queues, `VmaAllocator`, and timeline semaphores (`§10.1`, `HR-08`).
 */
class RhiDevice {
  public:
    RhiDevice() = default;
    ~RhiDevice();

    RhiDevice(const RhiDevice&) = delete;
    RhiDevice& operator=(const RhiDevice&) = delete;

    [[nodiscard]] static Expected<RhiDevice*> create(const RhiDeviceConfig& cfg);
    void destroy() noexcept;

    [[nodiscard]] VkInstance instance() const noexcept { return instance_; }
    [[nodiscard]] VkPhysicalDevice physical_device() const noexcept { return physical_device_; }
    [[nodiscard]] VkDevice device() const noexcept { return device_; }
    [[nodiscard]] VkQueue graphics_queue() const noexcept { return graphics_queue_; }
    [[nodiscard]] uint32_t graphics_family() const noexcept { return graphics_family_; }
    [[nodiscard]] VkQueue compute_queue() const noexcept { return compute_queue_; }
    [[nodiscard]] uint32_t compute_family() const noexcept { return compute_family_; }
    [[nodiscard]] VkQueue transfer_queue() const noexcept { return transfer_queue_; }
    [[nodiscard]] uint32_t transfer_family() const noexcept { return transfer_family_; }
    [[nodiscard]] VmaAllocator allocator() const noexcept { return allocator_; }
    [[nodiscard]] VkSemaphore frame_timeline() const noexcept { return frame_timeline_; }
    [[nodiscard]] VkSemaphore output_timeline() const noexcept { return output_timeline_; }
    [[nodiscard]] VkPipelineCache pipeline_cache() const noexcept { return pipeline_cache_; }
    [[nodiscard]] const SelectedGpuInfo& gpu_info() const noexcept { return gpu_info_; }
    [[nodiscard]] const RhiExtDispatch& ext() const noexcept { return ext_; }
    [[nodiscard]] bool rt_supported() const noexcept { return gpu_info_.rt_enabled; }

    [[nodiscard]] bool validation_active() const noexcept { return validation_active_; }
    [[nodiscard]] bool sync_validation_active() const noexcept { return sync_validation_active_; }
    [[nodiscard]] uint64_t validation_error_count() const noexcept {
        return validation_errors_.load(std::memory_order_acquire);
    }
    [[nodiscard]] uint64_t validation_warning_count() const noexcept {
        return validation_warnings_.load(std::memory_order_acquire);
    }

    /** Creates a VMA-backed GPU buffer (`HR-08` clean). */
    [[nodiscard]] Expected<RhiBuffer> create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                                    VmaMemoryUsage mem_usage,
                                                    VmaAllocationCreateFlags vma_flags = 0) const;
    void destroy_buffer(RhiBuffer& buf) const noexcept;
    [[nodiscard]] VkDeviceAddress buffer_device_address(VkBuffer buffer) const noexcept;

    /** Creates a 2D VMA-backed image + default color/depth view. */
    [[nodiscard]] Expected<RhiImage> create_image_2d(uint32_t width, uint32_t height, VkFormat format,
                                                     VkImageUsageFlags usage,
                                                     VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                                     bool concurrent_transfer = false) const;
    [[nodiscard]] Expected<VkImageView> create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                                          uint32_t base_mip = 0, uint32_t mip_count = 1) const;
    void destroy_image_view(VkImageView view) const noexcept;
    void destroy_image(RhiImage& img) const noexcept;

    /** Enumerates available Vulkan 1.3+ GPUs without requiring device creation (`P4-01`). */
    [[nodiscard]] static Expected<std::vector<GpuAdapterDescriptor>>
    enumerate_vulkan_devices(const RhiDeviceConfig& cfg = {});

    /** Scores a Vulkan physical device according to AETHER vendor priority (RTX -> RM-B -> iGPU). */
    [[nodiscard]] static uint32_t score_adapter_for_vendor(uint32_t vendor_id, VkPhysicalDeviceType type, bool rt_ready,
                                                           bool rebar, uint64_t dedicated_vram_mb) noexcept;

    /** Queries current process VRAM budget/usage via `VK_EXT_memory_budget` when supported. */
    [[nodiscard]] VramBudgetInfo query_vram_budget() const noexcept;

    /** Samples calibrated GPU timestamp + host QPC nanosecond pair (`VK_KHR_calibrated_timestamps`). */
    [[nodiscard]] Expected<void> calibrated_timestamp_ns(uint64_t& gpu_ns, uint64_t& cpu_qpc_ns) const noexcept;

    /** Persists VkPipelineCache to disk when `pipeline_cache_dir` is configured (`§10.1`). */
    void save_pipeline_cache() const noexcept;

    /** Injects/queries device-lost state and rebuilds VkDevice + VmaAllocator (`§4.8`). */
    void mark_device_lost() noexcept { device_lost_.store(true, std::memory_order_release); }
    [[nodiscard]] bool is_device_lost() const noexcept { return device_lost_.load(std::memory_order_acquire); }
    [[nodiscard]] Expected<void> recreate_after_device_lost(const RhiDeviceConfig& cfg);

    /** Records a Vulkan 1.3 `vkCmdPipelineBarrier2` image layout transition cleanly. */
    static void cmd_transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                                     VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                                     VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                                     VkAccessFlags2 dst_access,
                                     VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) noexcept;

    /** Waits for GPU idle safely before destruction/resize. */
    void wait_idle() const noexcept;

    /** Callback used internally by VkDebugUtilsMessengerEXT. */
    void on_validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                               const VkDebugUtilsMessengerCallbackDataEXT* data) noexcept;

  private:
    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debug_messenger_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue graphics_queue_{VK_NULL_HANDLE};
    uint32_t graphics_family_{UINT32_MAX};
    VkQueue compute_queue_{VK_NULL_HANDLE};
    uint32_t compute_family_{UINT32_MAX};
    VkQueue transfer_queue_{VK_NULL_HANDLE};
    uint32_t transfer_family_{UINT32_MAX};
    VmaAllocator allocator_{nullptr};
    VkSemaphore frame_timeline_{VK_NULL_HANDLE};
    VkSemaphore output_timeline_{VK_NULL_HANDLE};
    VkPipelineCache pipeline_cache_{VK_NULL_HANDLE};
    std::filesystem::path pipeline_cache_path_{};
    SelectedGpuInfo gpu_info_{};
    RhiExtDispatch ext_{};

    bool validation_active_{false};
    bool sync_validation_active_{false};
    std::atomic<bool> device_lost_{false};
    std::atomic<uint64_t> validation_errors_{0};
    std::atomic<uint64_t> validation_warnings_{0};
};

} // namespace aether::rhi
