#include "core/log.hpp"

#include "core/clock.hpp"
#include "core/spsc_ring.hpp"

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace aether {

namespace {

constexpr size_t kMaxLogThreads = 32;
constexpr size_t kRingCapacity = 256;

struct ProducerChannel {
    std::atomic<bool> in_use{false};
    SpscRing<LogRecord, kRingCapacity> ring{};
};

struct ThreadChannelHandle {
    ProducerChannel* channel{nullptr};
    ~ThreadChannelHandle() noexcept {
        if (channel != nullptr) {
            channel->in_use.store(false, std::memory_order_release);
            channel = nullptr;
        }
    }
};

std::array<ProducerChannel, kMaxLogThreads> g_channels{};
std::atomic<uint8_t> g_min_level{static_cast<uint8_t>(LogLevel::Debug)};
std::atomic<uint64_t> g_fallback_dropped{0};

thread_local uint8_t t_log_role = 0;
thread_local ThreadChannelHandle t_channel{};

ProducerChannel* acquire_thread_channel() noexcept {
    if (t_channel.channel != nullptr) {
        return t_channel.channel;
    }
    for (auto& ch : g_channels) {
        bool expected = false;
        if (ch.in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            t_channel.channel = &ch;
            return &ch;
        }
    }
    return nullptr;
}

uint32_t current_os_thread_id() noexcept {
#if defined(_WIN32)
    return static_cast<uint32_t>(::GetCurrentThreadId());
#else
    return 0;
#endif
}

} // namespace

void LogSystem::set_min_level(LogLevel level) noexcept {
    g_min_level.store(static_cast<uint8_t>(level), std::memory_order_release);
}

LogLevel LogSystem::min_level() noexcept {
    return static_cast<LogLevel>(g_min_level.load(std::memory_order_acquire));
}

void LogSystem::set_thread_role(uint8_t role) noexcept {
    t_log_role = role;
}

void LogSystem::write(LogLevel level, const char* category, const char* fmt, ...) noexcept {
    if (static_cast<uint8_t>(level) < g_min_level.load(std::memory_order_relaxed)) {
        return;
    }

    ProducerChannel* ch = acquire_thread_channel();
    if (ch == nullptr) {
        g_fallback_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    LogRecord rec{};
    rec.timestamp_us = now_us();
    rec.thread_id = current_os_thread_id();
    rec.level = level;
    rec.thread_role = t_log_role;

    if (category != nullptr) {
        std::snprintf(rec.category, sizeof(rec.category), "%s", category);
    }

    if (fmt != nullptr) {
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(rec.message, sizeof(rec.message), fmt, args);
        va_end(args);
    }

    if (!ch->ring.try_push(rec)) {
        // SPSC try_push already increments ch->ring.dropped_count().
        return;
    }
}

uint32_t LogSystem::drain(LogDrainCallback callback, void* user_data, uint32_t max_records) noexcept {
    uint32_t drained = 0;
    LogRecord rec{};
    for (auto& ch : g_channels) {
        while (drained < max_records && ch.ring.try_pop(rec)) {
            if (callback != nullptr) {
                callback(rec, user_data);
            }
            ++drained;
        }
        if (drained >= max_records) {
            break;
        }
    }
    return drained;
}

uint64_t LogSystem::dropped_count() noexcept {
    uint64_t total = g_fallback_dropped.load(std::memory_order_relaxed);
    for (const auto& ch : g_channels) {
        total += ch.ring.dropped_count();
    }
    return total;
}

const char* LogSystem::level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Fatal:
        return "FATAL";
    }
    return "INFO";
}

} // namespace aether
