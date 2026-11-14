#include "renderer/denoise/denoiser_manager.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace aether::renderer {
namespace {

[[nodiscard]] bool is_software_renderer_name(const char* name) noexcept {
    if (name == nullptr) {
        return false;
    }
    static constexpr const char* kSwMarkers[] = {"llvmpipe", "swiftshader", "lavapipe", "software"};
    for (const char* marker : kSwMarkers) {
        const size_t mlen = std::strlen(marker);
        for (const char* p = name; *p != '\0'; ++p) {
            size_t k = 0;
            while (k < mlen && p[k] != '\0' &&
                   std::tolower(static_cast<unsigned char>(p[k])) == static_cast<unsigned char>(marker[k])) {
                ++k;
            }
            if (k == mlen) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

DenoiserKind DenoiserManager::select_default_for_vendor(uint32_t vendor_id, bool is_software_adapter,
                                                        bool optix_available, bool oidn_available) noexcept {
    if (is_software_adapter) {
        return DenoiserKind::SvgfLite;
    }
    if (vendor_id == 0x10DEu) {
        return optix_available ? DenoiserKind::Optix : DenoiserKind::NrdSigma;
    }
    if (vendor_id == 0x1002u) {
        return DenoiserKind::FfxCompute;
    }
    if (vendor_id == 0x8086u) {
        return oidn_available ? DenoiserKind::Oidn : DenoiserKind::SvgfLite;
    }
    return DenoiserKind::SvgfLite;
}

Expected<void> DenoiserManager::init(const rhi::RhiDevice& device, const rhi::BindlessHeap& bindless, uint32_t width,
                                     uint32_t height, DenoiserKind requested) {
    destroy();
    requested_kind_ = requested;
    fallback_reason_ = DenoiserFallbackReason::None;
    reset_budget_monitor();

    // Always pre-allocate built-in SVGF-Lite so runtime fallbacks never touch heap (`HR-01`, `ADR-0009`).
    if (auto sv_res = svgf_fallback_.init(device, bindless, width, height); !sv_res) {
        return sv_res;
    }

    const uint32_t vendor_id = device.gpu_info().vendor_id;
    const bool is_sw = is_software_renderer_name(device.gpu_info().device_name.c_str());
    const bool has_optix = OptionalSdkDenoiser::probe_runtime_available(DenoiserKind::Optix, device);
    const bool has_oidn = OptionalSdkDenoiser::probe_runtime_available(DenoiserKind::Oidn, device);

    DenoiserKind target = requested;
    if (target == DenoiserKind::Auto) {
        target = select_default_for_vendor(vendor_id, is_sw, has_optix, has_oidn);
    }

    // Pre-initialize vendor compute backends matching GPU or explicitly requested so switching is zero-alloc.
    if (vendor_id == 0x10DEu || target == DenoiserKind::NrdSigma) {
        (void)sigma_backend_.init(device, bindless, width, height);
    }
    if (vendor_id == 0x1002u || vendor_id == 0x8086u || target == DenoiserKind::FfxCompute) {
        (void)ffx_backend_.init(device, bindless, width, height);
    }

    auto try_activate = [&](DenoiserKind kind) -> bool {
        switch (kind) {
        case DenoiserKind::Optix:
            if (optix_backend_.init(device, bindless, width, height).has_value()) {
                preferred_ = &optix_backend_;
                active_ = &optix_backend_;
                return true;
            }
            return false;
        case DenoiserKind::Oidn:
            if (oidn_backend_.init(device, bindless, width, height).has_value()) {
                preferred_ = &oidn_backend_;
                active_ = &oidn_backend_;
                return true;
            }
            return false;
        case DenoiserKind::NrdSigma:
            preferred_ = &sigma_backend_;
            active_ = &sigma_backend_;
            return true;
        case DenoiserKind::FfxCompute:
            preferred_ = &ffx_backend_;
            active_ = &ffx_backend_;
            return true;
        case DenoiserKind::SvgfLite:
        case DenoiserKind::Auto:
            preferred_ = &svgf_fallback_;
            active_ = &svgf_fallback_;
            return true;
        }
        return false;
    };

    if (!try_activate(target)) {
        // Optional SDK missing -> step down to native compute vendor denoiser if appropriate, else SVGF-Lite.
        const DenoiserKind step_down = select_default_for_vendor(vendor_id, is_sw, false, false);
        if (step_down == DenoiserKind::NrdSigma || step_down == DenoiserKind::FfxCompute) {
            (void)try_activate(step_down);
        } else {
            trigger_fallback(DenoiserFallbackReason::SdkUnavailable);
            preferred_ = &svgf_fallback_;
        }
    }

    initialized_ = true;
    AETHER_LOG_INFO("renderer", "Denoiser initialized: active=%s (requested=%u vendor=0x%04X)", active_name(),
                    static_cast<unsigned>(requested_kind_), vendor_id);
    return {};
}

Expected<void> DenoiserManager::resize(uint32_t width, uint32_t height) {
    if (!initialized_) {
        return {};
    }
    if (auto r = svgf_fallback_.resize(width, height); !r) {
        return r;
    }
    (void)sigma_backend_.resize(width, height);
    (void)ffx_backend_.resize(width, height);
    (void)optix_backend_.resize(width, height);
    (void)oidn_backend_.resize(width, height);
    return {};
}

void DenoiserManager::destroy() noexcept {
    initialized_ = false;
    inject_failure_ = false;
    active_ = nullptr;
    preferred_ = nullptr;
    fallback_reason_ = DenoiserFallbackReason::None;
    svgf_fallback_.destroy();
    sigma_backend_.destroy();
    ffx_backend_.destroy();
    optix_backend_.destroy();
    oidn_backend_.destroy();
    reset_budget_monitor();
}

Expected<void> DenoiserManager::execute(VkCommandBuffer cmd, const DenoiserInputView& inputs) noexcept {
    if (!initialized_ || active_ == nullptr) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "DenoiserManager::execute called uninitialized")};
    }

    if (inject_failure_) {
        inject_failure_ = false;
        trigger_fallback(DenoiserFallbackReason::RuntimeError);
        return svgf_fallback_.denoise(cmd, inputs);
    }

    auto res = active_->denoise(cmd, inputs);
    if (!res && active_ != &svgf_fallback_) {
        trigger_fallback(DenoiserFallbackReason::RuntimeError);
        return svgf_fallback_.denoise(cmd, inputs);
    }
    return res;
}

void DenoiserManager::observe_gpu_time_ms(float denoiser_ms) noexcept {
    if (denoiser_ms < 0.0f) {
        return;
    }
    sample_ring_ms_[sample_cursor_] = denoiser_ms;
    sample_cursor_ = (sample_cursor_ + 1u) % kWindowFrames;
    if (sample_count_ < kWindowFrames) {
        ++sample_count_;
    }

    std::copy_n(sample_ring_ms_.begin(), sample_count_, sort_scratch_ms_.begin());
    std::sort(sort_scratch_ms_.begin(), sort_scratch_ms_.begin() + sample_count_);
    const uint32_t p95_idx = (sample_count_ > 0u) ? ((sample_count_ - 1u) * 95u / 100u) : 0u;
    rolling_p95_ms_ = sort_scratch_ms_[p95_idx];

    if (sample_count_ >= kWindowFrames && rolling_p95_ms_ > kDenoiserBudgetP95Ms && active_ != &svgf_fallback_) {
        trigger_fallback(DenoiserFallbackReason::BudgetExceeded);
    }
}

Expected<void> DenoiserManager::select_backend(DenoiserKind kind) noexcept {
    if (!initialized_) {
        return Unexpected<Error>{Error::make(ErrorCode::InvalidArg, "DenoiserManager not initialized")};
    }
    fallback_reason_ = DenoiserFallbackReason::None;
    switch (kind) {
    case DenoiserKind::Auto:
        active_ = (preferred_ != nullptr) ? preferred_ : &svgf_fallback_;
        return {};
    case DenoiserKind::SvgfLite:
        active_ = &svgf_fallback_;
        return {};
    case DenoiserKind::NrdSigma:
        if (sigma_backend_.kind() == DenoiserKind::NrdSigma) {
            // Check sigma backend was initialized on this adapter
            if (preferred_ == &sigma_backend_ || active_ == &sigma_backend_) {
                active_ = &sigma_backend_;
                return {};
            }
            active_ = &sigma_backend_;
            return {};
        }
        return Unexpected<Error>{Error::make(ErrorCode::Unsupported, "SIGMA backend unavailable")};
    case DenoiserKind::FfxCompute:
        active_ = &ffx_backend_;
        return {};
    case DenoiserKind::Optix:
    case DenoiserKind::Oidn:
        return Unexpected<Error>{Error::make(ErrorCode::Unsupported, "Optional external denoiser SDK unavailable")};
    }
    return {};
}

void DenoiserManager::reset_budget_monitor() noexcept {
    sample_ring_ms_.fill(0.0f);
    sort_scratch_ms_.fill(0.0f);
    sample_count_ = 0;
    sample_cursor_ = 0;
    rolling_p95_ms_ = 0.0f;
}

DenoiserKind DenoiserManager::active_kind() const noexcept {
    return active_ != nullptr ? active_->kind() : DenoiserKind::SvgfLite;
}

const char* DenoiserManager::active_name() const noexcept {
    return active_ != nullptr ? active_->name() : "Unselected";
}

void DenoiserManager::trigger_fallback(DenoiserFallbackReason reason) noexcept {
    fallback_reason_ = reason;
    active_ = &svgf_fallback_;
}

} // namespace aether::renderer