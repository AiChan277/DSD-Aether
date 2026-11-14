#include "renderer/denoise/vendor_denoisers.hpp"

#include "renderer/denoise/denoiser_compute_helper.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>

namespace aether::renderer {
namespace {

[[nodiscard]] bool can_load_system_dll(const char* dll_name) noexcept {
    if (dll_name == nullptr) {
        return false;
    }
    if (::GetModuleHandleA(dll_name) != nullptr) {
        return true;
    }
    HMODULE mod = ::LoadLibraryExA(dll_name, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (mod != nullptr) {
        ::FreeLibrary(mod);
        return true;
    }
    return false;
}

} // namespace

Expected<void> NvidiaSigmaDenoiser::init(const rhi::RhiDevice& device, const rhi::BindlessHeap& bindless,
                                         uint32_t width, uint32_t height) {
    destroy();
    width_ = std::max(width, 1u);
    height_ = std::max(height, 1u);
    device_ = &device;
    bindless_set_ = bindless.global_set();

    auto res = detail::create_compute_denoiser_pipeline(device, bindless, shaders::Phase4ShaderId::DenoiseSigma,
                                                        pipeline_layout_, shader_module_, pipeline_);
    if (!res) {
        destroy();
        return res;
    }
    return {};
}

Expected<void> NvidiaSigmaDenoiser::resize(uint32_t width, uint32_t height) {
    width_ = std::max(width, 1u);
    height_ = std::max(height, 1u);
    return {};
}

Expected<void> NvidiaSigmaDenoiser::denoise(VkCommandBuffer cmd, const DenoiserInputView& inputs) noexcept {
    return detail::dispatch_compute_denoiser(device_, pipeline_, pipeline_layout_, bindless_set_, cmd, width_, height_,
                                             inputs);
}

void NvidiaSigmaDenoiser::destroy() noexcept {
    detail::destroy_compute_denoiser_pipeline(device_, pipeline_layout_, pipeline_, shader_module_);
    bindless_set_ = VK_NULL_HANDLE;
    device_ = nullptr;
}

Expected<void> FidelityFxComputeDenoiser::init(const rhi::RhiDevice& device, const rhi::BindlessHeap& bindless,
                                               uint32_t width, uint32_t height) {
    destroy();
    width_ = std::max(width, 1u);
    height_ = std::max(height, 1u);
    device_ = &device;
    bindless_set_ = bindless.global_set();

    auto res = detail::create_compute_denoiser_pipeline(device, bindless, shaders::Phase4ShaderId::DenoiseFfx,
                                                        pipeline_layout_, shader_module_, pipeline_);
    if (!res) {
        destroy();
        return res;
    }
    return {};
}

Expected<void> FidelityFxComputeDenoiser::resize(uint32_t width, uint32_t height) {
    width_ = std::max(width, 1u);
    height_ = std::max(height, 1u);
    return {};
}

Expected<void> FidelityFxComputeDenoiser::denoise(VkCommandBuffer cmd, const DenoiserInputView& inputs) noexcept {
    return detail::dispatch_compute_denoiser(device_, pipeline_, pipeline_layout_, bindless_set_, cmd, width_, height_,
                                             inputs);
}

void FidelityFxComputeDenoiser::destroy() noexcept {
    detail::destroy_compute_denoiser_pipeline(device_, pipeline_layout_, pipeline_, shader_module_);
    bindless_set_ = VK_NULL_HANDLE;
    device_ = nullptr;
}

bool OptionalSdkDenoiser::probe_runtime_available(DenoiserKind sdk_kind, const rhi::RhiDevice& device) noexcept {
    if (sdk_kind == DenoiserKind::Optix) {
        if (device.gpu_info().vendor_id != 0x10DEu) {
            return false;
        }
        // Requires both NVIDIA OptiX driver and AETHER's optional external OptiX SDK interop bridge DLL (`ADR-0009`).
        return can_load_system_dll("nvoptix.dll") && can_load_system_dll("aether_optix_interop.dll");
    }
    if (sdk_kind == DenoiserKind::Oidn) {
        return device.gpu_info().external_memory_win32_active && can_load_system_dll("OpenImageDenoise.dll");
    }
    return false;
}

Expected<void> OptionalSdkDenoiser::init(const rhi::RhiDevice& device, const rhi::BindlessHeap& bindless,
                                         uint32_t width, uint32_t height) {
    (void)bindless;
    (void)width;
    (void)height;
    runtime_loaded_ = probe_runtime_available(kind_, device);
    if (!runtime_loaded_) {
        return Unexpected<Error>{Error::make(ErrorCode::Unsupported, "Optional denoiser SDK runtime not installed")};
    }
    // Optional native CUDA/SYCL SDK interop execution bridge is external to the core build (`HR-09`, `ADR-0009`).
    runtime_loaded_ = false;
    return Unexpected<Error>{
        Error::make(ErrorCode::Unsupported, "Optional denoiser SDK bridge disabled in core distribution")};
}

Expected<void> OptionalSdkDenoiser::resize(uint32_t width, uint32_t height) {
    (void)width;
    (void)height;
    return {};
}

Expected<void> OptionalSdkDenoiser::denoise(VkCommandBuffer cmd, const DenoiserInputView& inputs) noexcept {
    (void)cmd;
    (void)inputs;
    return Unexpected<Error>{Error::make(ErrorCode::Unsupported, "Optional SDK denoiser unavailable")};
}

void OptionalSdkDenoiser::destroy() noexcept {
    runtime_loaded_ = false;
}

} // namespace aether::renderer