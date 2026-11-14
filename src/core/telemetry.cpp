#include "core/telemetry.hpp"

#include <algorithm>
#include <array>

namespace aether {

void RollingLatencyStats::reset() noexcept {
    window_.fill(0.0f);
    write_cursor_ = 0;
    filled_ = 0;
    latest_ = 0.0f;
}

void RollingLatencyStats::add_sample_ms(float ms) noexcept {
    const float clamped = ms < 0.0f ? 0.0f : ms;
    latest_ = clamped;
    window_[write_cursor_] = clamped;
    write_cursor_ = (write_cursor_ + 1U) % kWindowSize;
    if (filled_ < kWindowSize) {
        ++filled_;
    }
}

LatencySummary RollingLatencyStats::compute_summary() const noexcept {
    LatencySummary out{};
    out.latest_ms = latest_;
    if (filled_ == 0) {
        return out;
    }

    std::array<float, kWindowSize> sorted{};
    float sum = 0.0f;
    float max_val = 0.0f;
    for (size_t i = 0; i < filled_; ++i) {
        const float v = window_[i];
        sorted[i] = v;
        sum += v;
        if (v > max_val) {
            max_val = v;
        }
        size_t bin = static_cast<size_t>(v);
        if (bin >= out.bins.size()) {
            bin = out.bins.size() - 1U;
        }
        ++out.bins[bin];
    }

    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(filled_));

    const auto at_pct = [&](float pct) noexcept -> float {
        const size_t idx = static_cast<size_t>(pct * static_cast<float>(filled_ - 1U));
        return sorted[idx < filled_ ? idx : (filled_ - 1U)];
    };

    out.sample_count = static_cast<uint32_t>(filled_);
    out.mean_ms = sum / static_cast<float>(filled_);
    out.p50_ms = at_pct(0.50f);
    out.p95_ms = at_pct(0.95f);
    out.p99_ms = at_pct(0.99f);
    out.max_ms = max_val;
    return out;
}

} // namespace aether
