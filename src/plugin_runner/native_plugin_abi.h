#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
#include <cstddef>
#include <type_traits>
extern "C" {
#endif

#define AETHER_NATIVE_PLUGIN_ABI_VERSION 1u
#define AETHER_NATIVE_PLUGIN_MAX_IO 16u
#define AETHER_NATIVE_PLUGIN_RING_CAPACITY 32u
#define AETHER_NATIVE_SHM_MAGIC 0x414E5031u /* 'ANP1' */

typedef struct AetherNativePluginDesc {
    uint32_t abi_version;
    uint32_t num_inputs;
    uint32_t num_outputs;
    uint32_t flags;
    char id[64];
    char name[64];
    char version[32];
    uint32_t reserved[4];
} AetherNativePluginDesc;

typedef int32_t (*AetherNativeDescribeFn)(AetherNativePluginDesc* out_desc);
typedef int32_t (*AetherNativeInitFn)(const char* config_json_utf8, uint32_t config_len, void** out_user_ctx);
typedef int32_t (*AetherNativeProcessFn)(void* user_ctx, double time_sec, uint64_t frame_index, const float* inputs,
                                         uint32_t num_inputs, float* outputs, uint32_t num_outputs, char* err_buf,
                                         uint32_t err_buf_cap);
typedef void (*AetherNativeShutdownFn)(void* user_ctx);

typedef struct AetherNativePluginV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    AetherNativeDescribeFn describe;
    AetherNativeInitFn init;
    AetherNativeProcessFn process;
    AetherNativeShutdownFn shutdown;
} AetherNativePluginV1;

typedef const AetherNativePluginV1* (*AetherNativeEntryFn)(void);

#define AETHER_NATIVE_EXPORT_SYMBOL "aether_native_plugin_v1"

#ifdef __cplusplus
} // extern "C"

namespace aether::native_plugin {

static_assert(std::atomic<uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free across SHM");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "32-bit atomics must be lock-free across SHM");
static_assert(std::atomic<int32_t>::is_always_lock_free, "int32 atomics must be lock-free across SHM");

enum class RequestOp : uint32_t {
    Describe = 1,
    Process = 2,
    Shutdown = 3,
};

struct alignas(64) RequestSlot {
    uint32_t op{0};
    uint32_t seq{0};
    uint64_t frame_index{0};
    double time_sec{0.0};
    uint32_t input_count{0};
    uint32_t output_count{0};
    float inputs[AETHER_NATIVE_PLUGIN_MAX_IO]{};
    uint8_t reserved[416]{};
};
static_assert(sizeof(RequestSlot) == 512, "RequestSlot must remain 512 bytes per ADR-0014");

struct alignas(64) ResponseSlot {
    int32_t status{0};
    uint32_t seq{0};
    uint32_t output_count{0};
    uint32_t desc_present{0};
    float outputs[AETHER_NATIVE_PLUGIN_MAX_IO]{};
    AetherNativePluginDesc desc{};
    char error_msg[256]{};
    uint8_t extra[3568]{};
};
static_assert(sizeof(ResponseSlot) == 4096, "ResponseSlot must remain 4096 bytes per ADR-0014");

template <typename SlotT, size_t Capacity> struct ShmSpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0 && Capacity >= 2, "Ring capacity must be power of two");

    struct alignas(64) AlignedIndex {
        std::atomic<uint64_t> value{0};
        uint8_t pad[56]{};
    };
    static_assert(sizeof(AlignedIndex) == 64, "AlignedIndex must span a full 64-byte cache line");

    AlignedIndex head{};
    AlignedIndex tail{};
    SlotT slots[Capacity]{};

    [[nodiscard]] bool try_push(const SlotT& item) noexcept {
        const uint64_t h = head.value.load(std::memory_order_relaxed);
        const uint64_t t = tail.value.load(std::memory_order_acquire);
        if ((h - t) >= Capacity) {
            return false;
        }
        slots[h & (Capacity - 1)] = item;
        head.value.store(h + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(SlotT& out) noexcept {
        const uint64_t t = tail.value.load(std::memory_order_relaxed);
        const uint64_t h = head.value.load(std::memory_order_acquire);
        if (h == t || (h - t) > Capacity) {
            return false;
        }
        out = slots[t & (Capacity - 1)];
        tail.value.store(t + 1, std::memory_order_release);
        return true;
    }
};

struct alignas(64) ShmBlock {
    uint32_t magic{AETHER_NATIVE_SHM_MAGIC};
    uint32_t abi_version{AETHER_NATIVE_PLUGIN_ABI_VERSION};
    std::atomic<uint64_t> heartbeat_qpc_us{0};
    std::atomic<uint32_t> active_flag{0}; /* 0=init, 1=ready, 2=exited/error */
    std::atomic<int32_t> init_status{0};
    uint32_t dll_path_len{0};
    uint32_t config_json_len{0};
    char dll_path_utf8[1024]{};
    char config_json_utf8[2048]{};
    AetherNativePluginDesc plugin_desc{};
    ShmSpscRing<RequestSlot, AETHER_NATIVE_PLUGIN_RING_CAPACITY> requests{};
    ShmSpscRing<ResponseSlot, AETHER_NATIVE_PLUGIN_RING_CAPACITY> responses{};
};

} // namespace aether::native_plugin
#endif
