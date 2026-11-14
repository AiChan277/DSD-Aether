#include "core/telemetry.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace aether {

void RollingLatencyStats::reset() noexcept {
    window_.fill(0.0f);
    write_cursor_ = 0;
    filled_ = 0;
    latest_ = 0.0f;
}

void RollingLatencyStats::add_sample_ms(float ms) noexcept {
    if (!std::isfinite(ms) || ms < 0.0f || ms > 5000.0f) {
        return;
    }
    latest_ = ms;
    window_[write_cursor_] = ms;
    write_cursor_ = (write_cursor_ + 1U) % kWindowSize;
    if (filled_ < kWindowSize) {
        ++filled_;
    }
}

bool RollingLatencyStats::add_m2p_sample(uint64_t capture_timestamp_us, uint64_t present_timestamp_us,
                                         bool tracking_valid) noexcept {
    if (!tracking_valid || capture_timestamp_us == 0ULL || present_timestamp_us <= capture_timestamp_us) {
        return false;
    }
    const uint64_t diff_us = present_timestamp_us - capture_timestamp_us;
    if (diff_us == 0ULL || diff_us > FrameStamps::kMaxValidM2pUs) {
        return false;
    }
    const float ms = static_cast<float>(diff_us) * 0.001f;
    add_sample_ms(ms);
    return true;
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
