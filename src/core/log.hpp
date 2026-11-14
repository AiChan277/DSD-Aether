#pragma once

#include <cstdint>

namespace aether {

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
};

/**
 * Fixed 256 B lock-free structured log record (`§4.7`, `HR-01`, `HR-02`).
 * Producer threads copy straight into per-thread SPSC ring slots without heap allocation.
 */
struct alignas(64) LogRecord {
    uint64_t timestamp_us{0};
    uint32_t thread_id{0};
    LogLevel level{LogLevel::Info};
    uint8_t thread_role{0};
    uint16_t reserved{0};
    char category[24]{};
    char message[216]{};
};

static_assert(sizeof(LogRecord) == 256, "LogRecord must remain exactly 256 bytes (§4.7)");

using LogDrainCallback = void (*)(const LogRecord& record, void* user_data);

class LogSystem {
  public:
    /** Sets global minimum log level filter (thread-safe relaxed atomic). */
    static void set_min_level(LogLevel level) noexcept;
    [[nodiscard]] static LogLevel min_level() noexcept;

    /** Tags calling thread role for structured log attribution. */
    static void set_thread_role(uint8_t role) noexcept;

    /** Enqueues one formatted log record into caller's lock-free SPSC ring (zero heap alloc). */
    static void write(LogLevel level, const char* category, const char* fmt, ...) noexcept;

    /** Drains all pending records across every registered producer ring; returns count drained. */
    static uint32_t drain(LogDrainCallback callback, void* user_data, uint32_t max_records = 256) noexcept;

    /** Total records dropped because caller's SPSC ring filled before consumer drained. */
    [[nodiscard]] static uint64_t dropped_count() noexcept;

    /** Formats a human-readable level string ("TRACE".."FATAL"). */
    [[nodiscard]] static const char* level_name(LogLevel level) noexcept;
};

} // namespace aether

#define AETHER_LOG_TRACE(cat, ...) ::aether::LogSystem::write(::aether::LogLevel::Trace, cat, __VA_ARGS__)
#define AETHER_LOG_DEBUG(cat, ...) ::aether::LogSystem::write(::aether::LogLevel::Debug, cat, __VA_ARGS__)
#define AETHER_LOG_INFO(cat, ...) ::aether::LogSystem::write(::aether::LogLevel::Info, cat, __VA_ARGS__)
#define AETHER_LOG_WARN(cat, ...) ::aether::LogSystem::write(::aether::LogLevel::Warn, cat, __VA_ARGS__)
#define AETHER_LOG_ERROR(cat, ...) ::aether::LogSystem::write(::aether::LogLevel::Error, cat, __VA_ARGS__)
#define AETHER_LOG_FATAL(cat, ...) ::aether::LogSystem::write(::aether::LogLevel::Fatal, cat, __VA_ARGS__)
