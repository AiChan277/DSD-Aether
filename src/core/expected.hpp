#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#if defined(AETHER_WITH_TRACY) && AETHER_WITH_TRACY
#include <tracy/Tracy.hpp>
#define AETHER_ZONE(name) ZoneScopedN(name)
#define AETHER_FRAME_MARK() FrameMark
#else
#define AETHER_ZONE(name) ((void)0)
#define AETHER_FRAME_MARK() ((void)0)
#endif

namespace aether {

enum class ErrorCode : int32_t {
    Ok = 0,
    InvalidArg = -1,
    IoError = -2,
    ParseError = -3,
    Unsupported = -4,
    BufferTooSmall = -5,
    LimitExceeded = -6,
    RustPanic = -7,
    Cancelled = -8,
    OutOfMemory = -9,
    Timeout = -10,
    VulkanError = -11,
    Internal = -12,
};

struct Error {
    ErrorCode code{ErrorCode::Ok};
    std::array<char, 256> message{};

    static Error make(ErrorCode c, std::string_view msg) noexcept {
        Error e{};
        e.code = c;
        const size_t n = msg.size() < (e.message.size() - 1) ? msg.size() : (e.message.size() - 1);
        if (n > 0) {
            std::memcpy(e.message.data(), msg.data(), n);
        }
        e.message[n] = '\0';
        return e;
    }

    [[nodiscard]] std::string_view what() const noexcept { return std::string_view{message.data()}; }
    [[nodiscard]] const char* c_str() const noexcept { return message.data(); }
};

template <typename E> struct Unexpected {
    E error;
};

template <typename E> [[nodiscard]] constexpr Unexpected<std::decay_t<E>> make_unexpected(E&& err) {
    return Unexpected<std::decay_t<E>>{std::forward<E>(err)};
}

[[nodiscard]] inline Unexpected<Error> fail(ErrorCode c, std::string_view msg) noexcept {
    return Unexpected<Error>{Error::make(c, msg)};
}

/**
 * Lightweight C++20 `Expected<T, E>` without exceptions on normal control flow.
 * Note: Hot-path functions must instantiate `Expected` with trivial/fixed types to obey HR-01.
 */
template <typename T, typename E = Error> class Expected {
  public:
    constexpr Expected(const T& val) : storage_(std::in_place_index<0>, val) {}
    constexpr Expected(T&& val) noexcept(std::is_nothrow_move_constructible_v<T>)
        : storage_(std::in_place_index<0>, std::move(val)) {}
    constexpr Expected(const Unexpected<E>& err) : storage_(std::in_place_index<1>, err.error) {}
    constexpr Expected(Unexpected<E>&& err) noexcept(std::is_nothrow_move_constructible_v<E>)
        : storage_(std::in_place_index<1>, std::move(err.error)) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] constexpr T& value() & noexcept { return std::get<0>(storage_); }
    [[nodiscard]] constexpr const T& value() const& noexcept { return std::get<0>(storage_); }
    [[nodiscard]] constexpr T&& value() && noexcept { return std::get<0>(std::move(storage_)); }

    [[nodiscard]] constexpr E& error() & noexcept { return std::get<1>(storage_); }
    [[nodiscard]] constexpr const E& error() const& noexcept { return std::get<1>(storage_); }

    [[nodiscard]] constexpr T value_or(T fallback) const noexcept {
        return has_value() ? std::get<0>(storage_) : std::move(fallback);
    }

  private:
    std::variant<T, E> storage_;
};

template <typename E> class Expected<void, E> {
  public:
    constexpr Expected() noexcept : has_val_(true), err_{} {}
    constexpr Expected(const Unexpected<E>& err) : has_val_(false), err_(err.error) {}
    constexpr Expected(Unexpected<E>&& err) noexcept(std::is_nothrow_move_constructible_v<E>)
        : has_val_(false), err_(std::move(err.error)) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return has_val_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_val_; }
    [[nodiscard]] constexpr const E& error() const& noexcept { return err_; }

  private:
    bool has_val_;
    E err_;
};

} // namespace aether
