#include "renderer/denoise/svgf_lite_denoiser.hpp"

#include "renderer/denoise/denoiser_compute_helper.hpp"

#include <algorithm>

namespace aether::renderer {

Expected<void> SvgfLiteDenoiser::init(const rhi::RhiDevice& device, const rhi::BindlessHeap& bindless, uint32_t width,
                                      uint32_t height) {
    destroy();
    width_ = std::max(width, 1u);
    height_ = std::max(height, 1u);
    device_ = &device;
    bindless_set_ = bindless.global_set();

    auto res = detail::create_compute_denoiser_pipeline(device, bindless, shaders::Phase4ShaderId::DenoiseSvgf,
                                                        pipeline_layout_, shader_module_, pipeline_);
    if (!res) {
        destroy();
        return res;
    }
    return {};
}

Expected<void> SvgfLiteDenoiser::resize(uint32_t width, uint32_t height) {
    width_ = std::max(width, 1u);
    height_ = std::max(height, 1u);
    return {};
}

Expected<void> SvgfLiteDenoiser::denoise(VkCommandBuffer cmd, const DenoiserInputView& inputs) noexcept {
    return detail::dispatch_compute_denoiser(device_, pipeline_, pipeline_layout_, bindless_set_, cmd, width_, height_,
                                             inputs);
}

void SvgfLiteDenoiser::destroy() noexcept {
    detail::destroy_compute_denoiser_pipeline(device_, pipeline_layout_, pipeline_, shader_module_);
    bindless_set_ = VK_NULL_HANDLE;
    device_ = nullptr;
}

} // namespace aether::renderer