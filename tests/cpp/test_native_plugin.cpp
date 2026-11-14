#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/plugin_runner/native_plugin_abi.h"

namespace {

struct TestPluginCtx {
    uint32_t magic{0x54504C47u};
    uint64_t call_count{0};
};

int32_t test_describe(AetherNativePluginDesc* out_desc) {
    if (out_desc == nullptr) {
        return -1;
    }
    *out_desc = AetherNativePluginDesc{};
    out_desc->abi_version = AETHER_NATIVE_PLUGIN_ABI_VERSION;
    out_desc->num_inputs = 4;
    out_desc->num_outputs = 4;
    out_desc->flags = 0;
    std::snprintf(out_desc->id, sizeof(out_desc->id), "com.aether.test.native");
    std::snprintf(out_desc->name, sizeof(out_desc->name), "Aether Test Native Plugin");
    std::snprintf(out_desc->version, sizeof(out_desc->version), "1.0.0");
    return 0;
}

int32_t test_init(const char* /*config_json_utf8*/, uint32_t /*config_len*/, void** out_user_ctx) {
    if (out_user_ctx == nullptr) {
        return -1;
    }
    *out_user_ctx = new TestPluginCtx{};
    return 0;
}

int32_t test_process(void* user_ctx, double /*time_sec*/, uint64_t /*frame_index*/, const float* inputs,
                     uint32_t num_inputs, float* outputs, uint32_t num_outputs, char* err_buf, uint32_t err_buf_cap) {
    auto* ctx = static_cast<TestPluginCtx*>(user_ctx);
    if (ctx != nullptr) {
        ++ctx->call_count;
    }
    const float mode = (inputs != nullptr && num_inputs > 0) ? inputs[0] : 0.0f;

    if (mode > 0.5f && mode < 1.5f) {
        // Mode 1: deliberate fault inside plugin DLL
        RaiseException(0xC0000005u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return -1;
    }

    if (mode > 1.5f && mode < 2.5f) {
        // Mode 2: attempt to commit >512 MiB (Job Object limit should reject this)
        std::vector<void*> chunks;
        constexpr SIZE_T kChunkBytes = 32ULL * 1024ULL * 1024ULL;
        bool allocated_over_limit = true;
        for (int i = 0; i < 24; ++i) {
            void* mem = VirtualAlloc(nullptr, kChunkBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (mem == nullptr) {
                allocated_over_limit = false;
                break;
            }
            std::memset(mem, 0xA5, 4096);
            chunks.push_back(mem);
        }
        for (void* mem : chunks) {
            VirtualFree(mem, 0, MEM_RELEASE);
        }
        if (!allocated_over_limit) {
            if (err_buf != nullptr && err_buf_cap > 0) {
                std::snprintf(err_buf, err_buf_cap, "OOM blocked by 512 MiB Job Object limit");
            }
            if (outputs != nullptr && num_outputs > 0) {
                outputs[0] = 1.0f;
            }
            return -12;
        }
        if (outputs != nullptr && num_outputs > 0) {
            outputs[0] = 0.0f;
        }
        return -13;
    }

    if (mode > 2.5f && mode < 3.5f) {
        // Mode 3: deliberate hang/deadlock inside plugin DLL
        HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (never != nullptr) {
            (void)WaitForSingleObject(never, INFINITE);
            CloseHandle(never);
        }
        return -2;
    }

    if (mode > 3.5f && mode < 4.5f) {
        // Mode 4: attempt to spawn child process (ActiveProcessLimit=1 must deny)
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        wchar_t cmd[] = L"cmd.exe /c exit 0";
        const BOOL ok =
            CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (ok != FALSE) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (outputs != nullptr && num_outputs > 0) {
            outputs[0] = (ok == FALSE) ? 1.0f : 0.0f;
        }
        return 0;
    }

    // Mode 0: normal deterministic transform
    for (uint32_t i = 0; i < num_outputs; ++i) {
        const float x = (inputs != nullptr && i < num_inputs) ? inputs[i] : 0.0f;
        outputs[i] = x * 2.0f + 1.0f;
    }
    return 0;
}

void test_shutdown(void* user_ctx) {
    delete static_cast<TestPluginCtx*>(user_ctx);
}

const AetherNativePluginV1 kVtable{
    sizeof(AetherNativePluginV1),
    AETHER_NATIVE_PLUGIN_ABI_VERSION,
    &test_describe,
    &test_init,
    &test_process,
    &test_shutdown,
};

} // namespace

extern "C" __declspec(dllexport) const AetherNativePluginV1* aether_native_plugin_v1(void) {
    return &kVtable;
}
