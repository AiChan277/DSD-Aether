#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <crtdbg.h>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "native_plugin_abi.h"

namespace {

std::atomic<aether::native_plugin::ShmBlock*> g_shm{nullptr};
LARGE_INTEGER g_qpc_freq{};

uint64_t now_qpc_us() noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    if (g_qpc_freq.QuadPart <= 0) {
        return 0;
    }
    const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
    const uint64_t freq = static_cast<uint64_t>(g_qpc_freq.QuadPart);
    const uint64_t sec = ticks / freq;
    const uint64_t rem = ticks % freq;
    return (sec * 1000000ULL) + ((rem * 1000000ULL) / freq);
}

LONG WINAPI aether_runner_exception_filter(EXCEPTION_POINTERS* info) noexcept {
    if (auto* shm = g_shm.load(std::memory_order_acquire)) {
        shm->init_status.store(-1, std::memory_order_relaxed);
        shm->active_flag.store(2u, std::memory_order_release);
    }
    const DWORD code =
        (info != nullptr && info->ExceptionRecord != nullptr) ? info->ExceptionRecord->ExceptionCode : 0xC0000005u;
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

std::wstring utf8_to_wide(const char* src, size_t max_len) {
    if (src == nullptr) {
        return {};
    }
    size_t len = 0;
    while (len < max_len && src[len] != '\0') {
        ++len;
    }
    if (len == 0) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, src, static_cast<int>(len), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, src, static_cast<int>(len), out.data(), needed);
    return out;
}

struct RunnerArgs {
    std::wstring shm_name{};
    std::wstring req_event{};
    std::wstring rsp_event{};
    std::wstring stop_event{};
};

bool parse_args(int argc, char** argv, RunnerArgs& out) {
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string key = argv[i] != nullptr ? argv[i] : "";
        const char* val = argv[i + 1];
        if (key == "--shm") {
            out.shm_name = utf8_to_wide(val, 260);
            ++i;
        } else if (key == "--req-event") {
            out.req_event = utf8_to_wide(val, 260);
            ++i;
        } else if (key == "--rsp-event") {
            out.rsp_event = utf8_to_wide(val, 260);
            ++i;
        } else if (key == "--stop-event") {
            out.stop_event = utf8_to_wide(val, 260);
            ++i;
        }
    }
    return !out.shm_name.empty() && !out.req_event.empty() && !out.rsp_event.empty() && !out.stop_event.empty();
}

int runner_main_impl(int argc, char** argv) {
    QueryPerformanceFrequency(&g_qpc_freq);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    SetUnhandledExceptionFilter(aether_runner_exception_filter);

    RunnerArgs args{};
    if (!parse_args(argc, argv, args)) {
        return 2;
    }

    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, args.shm_name.c_str());
    if (mapping == nullptr) {
        return 3;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(aether::native_plugin::ShmBlock));
    if (view == nullptr) {
        CloseHandle(mapping);
        return 4;
    }

    auto* shm = static_cast<aether::native_plugin::ShmBlock*>(view);
    if (shm->magic != AETHER_NATIVE_SHM_MAGIC || shm->abi_version != AETHER_NATIVE_PLUGIN_ABI_VERSION) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return 5;
    }
    g_shm.store(shm, std::memory_order_release);

    HANDLE req_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, args.req_event.c_str());
    HANDLE rsp_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, args.rsp_event.c_str());
    HANDLE stop_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, args.stop_event.c_str());
    if (req_event == nullptr || rsp_event == nullptr || stop_event == nullptr) {
        if (req_event != nullptr) {
            CloseHandle(req_event);
        }
        if (rsp_event != nullptr) {
            CloseHandle(rsp_event);
        }
        if (stop_event != nullptr) {
            CloseHandle(stop_event);
        }
        shm->init_status.store(-2, std::memory_order_relaxed);
        shm->active_flag.store(2u, std::memory_order_release);
        g_shm.store(nullptr, std::memory_order_release);
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return 6;
    }

    auto cleanup_ipc = [&]() {
        g_shm.store(nullptr, std::memory_order_release);
        CloseHandle(req_event);
        CloseHandle(rsp_event);
        CloseHandle(stop_event);
        UnmapViewOfFile(view);
        CloseHandle(mapping);
    };

    const std::wstring dll_path = utf8_to_wide(shm->dll_path_utf8, sizeof(shm->dll_path_utf8));
    HMODULE plugin_dll = dll_path.empty() ? nullptr : LoadLibraryW(dll_path.c_str());
    if (plugin_dll == nullptr) {
        shm->init_status.store(-3, std::memory_order_relaxed);
        shm->active_flag.store(2u, std::memory_order_release);
        SetEvent(rsp_event);
        cleanup_ipc();
        return 7;
    }

    const FARPROC raw_entry = GetProcAddress(plugin_dll, AETHER_NATIVE_EXPORT_SYMBOL);
    const auto entry = (raw_entry != nullptr) ? std::bit_cast<AetherNativeEntryFn>(raw_entry) : nullptr;
    const AetherNativePluginV1* vtable = entry != nullptr ? entry() : nullptr;
    if (vtable == nullptr || vtable->struct_size < sizeof(AetherNativePluginV1) ||
        vtable->abi_version != AETHER_NATIVE_PLUGIN_ABI_VERSION || vtable->describe == nullptr ||
        vtable->init == nullptr || vtable->process == nullptr) {
        FreeLibrary(plugin_dll);
        shm->init_status.store(-4, std::memory_order_relaxed);
        shm->active_flag.store(2u, std::memory_order_release);
        SetEvent(rsp_event);
        cleanup_ipc();
        return 8;
    }

    AetherNativePluginDesc desc{};
    if (vtable->describe(&desc) != 0) {
        FreeLibrary(plugin_dll);
        shm->init_status.store(-5, std::memory_order_relaxed);
        shm->active_flag.store(2u, std::memory_order_release);
        SetEvent(rsp_event);
        cleanup_ipc();
        return 9;
    }

    void* user_ctx = nullptr;
    const uint32_t cfg_len =
        std::min<uint32_t>(shm->config_json_len, static_cast<uint32_t>(sizeof(shm->config_json_utf8) - 1));
    shm->config_json_utf8[cfg_len] = '\0';
    if (vtable->init(shm->config_json_utf8, cfg_len, &user_ctx) != 0) {
        FreeLibrary(plugin_dll);
        shm->init_status.store(-6, std::memory_order_relaxed);
        shm->active_flag.store(2u, std::memory_order_release);
        SetEvent(rsp_event);
        cleanup_ipc();
        return 10;
    }

    shm->plugin_desc = desc;
    shm->init_status.store(0, std::memory_order_relaxed);
    shm->heartbeat_qpc_us.store(now_qpc_us(), std::memory_order_release);
    shm->active_flag.store(1u, std::memory_order_release);
    SetEvent(rsp_event);

    HANDLE wait_handles[2]{req_event, stop_event};
    bool running = true;
    while (running) {
        shm->heartbeat_qpc_us.store(now_qpc_us(), std::memory_order_release);

        aether::native_plugin::RequestSlot req{};
        bool sent_response = false;
        while (shm->requests.try_pop(req)) {
            if (req.op == static_cast<uint32_t>(aether::native_plugin::RequestOp::Shutdown)) {
                running = false;
                break;
            }
            aether::native_plugin::ResponseSlot rsp{};
            rsp.seq = req.seq;
            if (req.op == static_cast<uint32_t>(aether::native_plugin::RequestOp::Describe)) {
                rsp.status = 0;
                rsp.desc_present = 1;
                rsp.desc = desc;
            } else if (req.op == static_cast<uint32_t>(aether::native_plugin::RequestOp::Process)) {
                const uint32_t in_cnt = std::min<uint32_t>(req.input_count, AETHER_NATIVE_PLUGIN_MAX_IO);
                const uint32_t out_cnt = std::min<uint32_t>(req.output_count, AETHER_NATIVE_PLUGIN_MAX_IO);
                rsp.output_count = out_cnt;
                rsp.status = vtable->process(user_ctx, req.time_sec, req.frame_index, req.inputs, in_cnt, rsp.outputs,
                                             out_cnt, rsp.error_msg, static_cast<uint32_t>(sizeof(rsp.error_msg)));
                rsp.error_msg[sizeof(rsp.error_msg) - 1] = '\0';
            } else {
                rsp.status = -100;
            }
            (void)shm->responses.try_push(rsp);
            sent_response = true;
            shm->heartbeat_qpc_us.store(now_qpc_us(), std::memory_order_release);
        }

        if (sent_response) {
            SetEvent(rsp_event);
        }
        if (!running) {
            break;
        }

        const DWORD wait_rc = WaitForMultipleObjects(2, wait_handles, FALSE, 25);
        if (wait_rc == WAIT_OBJECT_0 + 1) {
            running = false;
        }
    }

    if (vtable->shutdown != nullptr) {
        vtable->shutdown(user_ctx);
    }
    FreeLibrary(plugin_dll);
    shm->active_flag.store(2u, std::memory_order_release);
    cleanup_ipc();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return runner_main_impl(argc, argv);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "aether-plugin-runner exception: %s\n", ex.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "aether-plugin-runner unknown exception\n");
        return 1;
    }
}
