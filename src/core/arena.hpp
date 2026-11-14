#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace aether {

/** Fixed-capacity vector (`HR-01`): zero heap allocations. */
template <typename T, size_t Capacity> class FixedVector {
  public:
    constexpr FixedVector() noexcept = default;

    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return size_ >= Capacity; }
    [[nodiscard]] constexpr size_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

    [[nodiscard]] constexpr T* data() noexcept { return data_.data(); }
    [[nodiscard]] constexpr const T* data() const noexcept { return data_.data(); }

    [[nodiscard]] constexpr T& operator[](size_t idx) noexcept {
        assert(idx < size_);
        return data_[idx];
    }
    [[nodiscard]] constexpr const T& operator[](size_t idx) const noexcept {
        assert(idx < size_);
        return data_[idx];
    }

    [[nodiscard]] constexpr bool push_back(const T& v) noexcept {
        if (size_ >= Capacity) {
            return false;
        }
        data_[size_++] = v;
        return true;
    }

    [[nodiscard]] constexpr bool push_back(T&& v) noexcept {
        if (size_ >= Capacity) {
            return false;
        }
        data_[size_++] = std::move(v);
        return true;
    }

    constexpr void clear() noexcept { size_ = 0; }

    [[nodiscard]] constexpr T* begin() noexcept { return data_.data(); }
    [[nodiscard]] constexpr T* end() noexcept { return data_.data() + size_; }
    [[nodiscard]] constexpr const T* begin() const noexcept { return data_.data(); }
    [[nodiscard]] constexpr const T* end() const noexcept { return data_.data() + size_; }

  private:
    std::array<T, Capacity> data_{};
    size_t size_{0};
};

/**
 * Per-frame linear bump allocator (`§4.5`, default 4 MiB).
 * Backing memory is allocated once at construction/init; `alloc_bytes` and `reset`
 * perform zero OS/heap allocations (`HR-01`).
 */
class FrameArena {
  public:
    static constexpr size_t kDefaultCapacityBytes = 4ULL * 1024ULL * 1024ULL;
    static constexpr size_t kBaseAlignment = 64;

    explicit FrameArena(size_t capacity_bytes = kDefaultCapacityBytes) noexcept : capacity_(capacity_bytes) {
        if (capacity_bytes > 0) {
#if defined(_MSC_VER)
            base_ = static_cast<uint8_t*>(_aligned_malloc(capacity_bytes, kBaseAlignment));
#else
            base_ = static_cast<uint8_t*>(std::aligned_alloc(kBaseAlignment, capacity_bytes));
#endif
        }
        if (base_ == nullptr) {
            capacity_ = 0;
        }
    }

    ~FrameArena() noexcept {
        if (base_ != nullptr) {
#if defined(_MSC_VER)
            _aligned_free(base_);
#else
            std::free(base_);
#endif
            base_ = nullptr;
        }
    }

    FrameArena(const FrameArena&) = delete;
    FrameArena& operator=(const FrameArena&) = delete;
    FrameArena(FrameArena&& other) noexcept
        : base_(other.base_), capacity_(other.capacity_), cursor_(other.cursor_), high_water_(other.high_water_) {
        other.base_ = nullptr;
        other.capacity_ = 0;
        other.cursor_ = 0;
        other.high_water_ = 0;
    }

    FrameArena& operator=(FrameArena&& other) noexcept {
        if (this != &other) {
            if (base_ != nullptr) {
#if defined(_MSC_VER)
                _aligned_free(base_);
#else
                std::free(base_);
#endif
            }
            base_ = other.base_;
            capacity_ = other.capacity_;
            cursor_ = other.cursor_;
            high_water_ = other.high_water_;
            other.base_ = nullptr;
            other.capacity_ = 0;
            other.cursor_ = 0;
            other.high_water_ = 0;
        }
        return *this;
    }

    /** Allocates `size_bytes` aligned to `align` (must be power of two) with zero heap calls. */
    [[nodiscard]] void* alloc_bytes(size_t size_bytes, size_t align = alignof(std::max_align_t)) noexcept {
        if (base_ == nullptr || align == 0 || (align & (align - 1)) != 0) {
            return nullptr;
        }
        const size_t mask = align - 1;
        const size_t aligned = (cursor_ + mask) & ~mask;
        if (aligned < cursor_ || size_bytes > (capacity_ - aligned)) {
            return nullptr;
        }
        cursor_ = aligned + size_bytes;
        if (cursor_ > high_water_) {
            high_water_ = cursor_;
        }
        return base_ + aligned;
    }

    template <typename T> [[nodiscard]] T* alloc(size_t count = 1, size_t align = alignof(T)) noexcept {
        static_assert(std::is_trivially_destructible_v<T>, "FrameArena allocates trivially destructible types");
        if (count == 0) {
            return nullptr;
        }
        const size_t bytes = count * sizeof(T);
        if (bytes / sizeof(T) != count) {
            return nullptr;
        }
        void* mem = alloc_bytes(bytes, align < alignof(T) ? alignof(T) : align);
        if (mem == nullptr) {
            return nullptr;
        }
        return new (mem) T[count]();
    }

    void reset() noexcept { cursor_ = 0; }

    [[nodiscard]] size_t used_bytes() const noexcept { return cursor_; }
    [[nodiscard]] size_t capacity_bytes() const noexcept { return capacity_; }
    [[nodiscard]] size_t high_water_bytes() const noexcept { return high_water_; }

  private:
    uint8_t* base_{nullptr};
    size_t capacity_{0};
    size_t cursor_{0};
    size_t high_water_{0};
};

/** Preallocated fixed object pool with O(1) free-list (`§4.5`, zero heap allocs). */
template <typename T, size_t Capacity> class Pool {
  public:
    Pool() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            free_stack_[i] = Capacity - 1U - i;
        }
        free_top_ = Capacity;
    }

    [[nodiscard]] T* acquire() noexcept {
        if (free_top_ == 0) {
            return nullptr;
        }
        const size_t slot = free_stack_[--free_top_];
        return &items_[slot];
    }

    void release(T* ptr) noexcept {
        if (ptr == nullptr) {
            return;
        }
        const std::ptrdiff_t idx = ptr - items_.data();
        if (idx < 0 || static_cast<size_t>(idx) >= Capacity || free_top_ >= Capacity) {
            return;
        }
        free_stack_[free_top_++] = static_cast<size_t>(idx);
    }

    [[nodiscard]] size_t available() const noexcept { return free_top_; }
    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

  private:
    std::array<T, Capacity> items_{};
    std::array<size_t, Capacity> free_stack_{};
    size_t free_top_{0};
};

} // namespace aether
