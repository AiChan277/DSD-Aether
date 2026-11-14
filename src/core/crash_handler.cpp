#include "core/crash_handler.hpp"

#include "core/log.hpp"

#if defined(_WIN32)
// clang-format off
#include <Windows.h>
#include <DbgHelp.h>
// clang-format on
#endif

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace aether {

namespace {

#if defined(_WIN32)
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

std::atomic<bool> g_installed{false};
std::atomic<bool> g_fault_claimed{false};

std::array<wchar_t, 512> g_crash_dir_w{};
HMODULE g_dbghelp_mod = nullptr;
MiniDumpWriteDumpFn g_minidump_fn = nullptr;

HANDLE g_crash_event = nullptr;
HANDLE g_done_event = nullptr;
HANDLE g_stop_event = nullptr;
HANDLE g_watcher_thread = nullptr;

LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;
EXCEPTION_POINTERS* g_fault_pointers = nullptr;
DWORD g_fault_tid = 0;

DWORD WINAPI crash_watcher_proc(LPVOID) noexcept {
    ::SetThreadDescription(::GetCurrentThread(), L"CrashMinidumpWatcher");
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    HANDLE wait_handles[2] = {g_crash_event, g_stop_event};
    for (;;) {
        const DWORD res = ::WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
        if (res == WAIT_OBJECT_0 + 1) {
            break;
        }
        if (res != WAIT_OBJECT_0) {
            break;
        }

        if (g_minidump_fn != nullptr && g_crash_dir_w[0] != L'\0') {
            std::array<wchar_t, 640> dmp_path{};
            const ULONGLONG ticks = ::GetTickCount64();
            std::swprintf(dmp_path.data(), dmp_path.size(), L"%s\\aether_crash_%llu.dmp", g_crash_dir_w.data(), ticks);

            HANDLE file = ::CreateFileW(dmp_path.data(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION exc_info{};
                exc_info.ThreadId = g_fault_tid;
                exc_info.ExceptionPointers = g_fault_pointers;
                exc_info.ClientPointers = FALSE;

                const MINIDUMP_TYPE dump_type =
                    static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
                g_minidump_fn(::GetCurrentProcess(), ::GetCurrentProcessId(), file, dump_type,
                              g_fault_pointers != nullptr ? &exc_info : nullptr, nullptr, nullptr);
                ::CloseHandle(file);
            }
        }

        if (g_done_event != nullptr) {
            ::SetEvent(g_done_event);
        }
    }
    return 0;
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* exc) noexcept {
    if (!g_fault_claimed.exchange(true, std::memory_order_acq_rel)) {
        g_fault_pointers = exc;
        g_fault_tid = ::GetCurrentThreadId();
        if (g_crash_event != nullptr) {
            ::SetEvent(g_crash_event);
        }
        if (g_done_event != nullptr) {
            ::WaitForSingleObject(g_done_event, 5000);
        }
    }
    if (g_prev_filter != nullptr) {
        return g_prev_filter(exc);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

} // namespace

bool CrashHandler::install(std::string_view crash_dir_utf8) noexcept {
#if defined(_WIN32)
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }

    if (!crash_dir_utf8.empty()) {
        const int needed =
            ::MultiByteToWideChar(CP_UTF8, 0, crash_dir_utf8.data(), static_cast<int>(crash_dir_utf8.size()),
                                  g_crash_dir_w.data(), static_cast<int>(g_crash_dir_w.size() - 1));
        if (needed > 0) {
            g_crash_dir_w[static_cast<size_t>(needed)] = L'\0';
        }
    }

    g_dbghelp_mod = ::LoadLibraryW(L"DbgHelp.dll");
    if (g_dbghelp_mod != nullptr) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
#endif
        g_minidump_fn = reinterpret_cast<MiniDumpWriteDumpFn>(::GetProcAddress(g_dbghelp_mod, "MiniDumpWriteDump"));
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    }

    g_crash_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_done_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_crash_event == nullptr || g_done_event == nullptr || g_stop_event == nullptr) {
        shutdown();
        return false;
    }

    g_watcher_thread = ::CreateThread(nullptr, 0, &crash_watcher_proc, nullptr, 0, nullptr);
    if (g_watcher_thread == nullptr) {
        shutdown();
        return false;
    }

    g_prev_filter = ::SetUnhandledExceptionFilter(&unhandled_exception_filter);
    g_installed.store(true, std::memory_order_release);
    AETHER_LOG_INFO("crash", "Pre-spawned crash minidump watcher active");
    return true;
#else
    (void)crash_dir_utf8;
    return false;
#endif
}

void CrashHandler::shutdown() noexcept {
#if defined(_WIN32)
    if (g_installed.exchange(false, std::memory_order_acq_rel)) {
        ::SetUnhandledExceptionFilter(g_prev_filter);
        g_prev_filter = nullptr;
    }
    if (g_stop_event != nullptr) {
        ::SetEvent(g_stop_event);
    }
    if (g_watcher_thread != nullptr) {
        ::WaitForSingleObject(g_watcher_thread, 2000);
        ::CloseHandle(g_watcher_thread);
        g_watcher_thread = nullptr;
    }
    if (g_crash_event != nullptr) {
        ::CloseHandle(g_crash_event);
        g_crash_event = nullptr;
    }
    if (g_done_event != nullptr) {
        ::CloseHandle(g_done_event);
        g_done_event = nullptr;
    }
    if (g_stop_event != nullptr) {
        ::CloseHandle(g_stop_event);
        g_stop_event = nullptr;
    }
    if (g_dbghelp_mod != nullptr) {
        ::FreeLibrary(g_dbghelp_mod);
        g_dbghelp_mod = nullptr;
        g_minidump_fn = nullptr;
    }
#endif
}

bool CrashHandler::is_installed() noexcept {
#if defined(_WIN32)
    return g_installed.load(std::memory_order_acquire);
#else
    return false;
#endif
}

} // namespace aether
