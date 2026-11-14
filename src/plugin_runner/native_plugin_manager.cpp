#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <WinSafer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "native_plugin_manager.hpp"

namespace aether::native_plugin {
namespace {

uint64_t now_qpc_us() noexcept {
    static LARGE_INTEGER freq = []() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    if (freq.QuadPart <= 0) {
        return 0;
    }
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
    const uint64_t hz = static_cast<uint64_t>(freq.QuadPart);
    return (ticks / hz) * 1000000ULL + ((ticks % hz) * 1000000ULL) / hz;
}

std::string wide_to_utf8(const std::wstring& src) {
    if (src.empty()) {
        return {};
    }
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, src.c_str(), static_cast<int>(src.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, src.c_str(), static_cast<int>(src.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::filesystem::path resolve_runner_exe(const std::filesystem::path& preferred) {
    std::error_code ec{};
    if (!preferred.empty() && std::filesystem::exists(preferred, ec)) {
        return preferred;
    }
    std::array<wchar_t, 1024> module_buf{};
    const DWORD len = GetModuleFileNameW(nullptr, module_buf.data(), static_cast<DWORD>(module_buf.size()));
    if (len > 0 && len < module_buf.size()) {
        const std::filesystem::path exe_dir = std::filesystem::path(module_buf.data()).parent_path();
        const std::array<std::filesystem::path, 4> candidates{
            exe_dir / L"aether-plugin-runner.exe",
            exe_dir / L".." / L"src" / L"plugin_runner" / L"aether-plugin-runner.exe",
            exe_dir / L".." / L".." / L"src" / L"plugin_runner" / L"aether-plugin-runner.exe",
            exe_dir / L"plugin_runner" / L"aether-plugin-runner.exe",
        };
        for (const auto& c : candidates) {
            if (std::filesystem::exists(c, ec)) {
                return std::filesystem::weakly_canonical(c, ec);
            }
        }
    }
    return {};
}

struct SlotRuntime {
    std::atomic<uint8_t> occupied{0};
    std::atomic<bool> start_requested{false};
    std::atomic<bool> unload_requested{false};
    std::atomic<bool> reset_requested{false};

    std::atomic<PluginState> state{PluginState::Stopped};
    std::atomic<bool> permanently_disabled{false};
    std::atomic<bool> restricted_token_active{false};
    std::atomic<bool> job_object_active{false};
    std::atomic<uint32_t> restart_count{0};
    std::atomic<uint32_t> crash_count{0};
    std::atomic<uint32_t> hang_kill_count{0};
    std::atomic<uint32_t> last_exit_code{0};
    std::atomic<uint32_t> seq_counter{1};
    std::atomic<uint32_t> shm_submit_in_flight{0};

    std::atomic<uint32_t> error_write_idx{0};
    std::array<std::array<char, 256>, 4> error_ring{};
    std::atomic<uint32_t> desc_write_idx{0};
    std::array<AetherNativePluginDesc, 2> desc_buf{};

    NativePluginConfig cfg{};
    std::filesystem::path resolved_runner{};
    std::wstring shm_name{};
    std::wstring req_event_name{};
    std::wstring rsp_event_name{};
    std::wstring stop_event_name{};

    HANDLE shm_handle{nullptr};
    ShmBlock* shm{nullptr};
    HANDLE req_event{nullptr};
    HANDLE rsp_event{nullptr};
    HANDLE stop_event{nullptr};
    HANDLE ready_event{nullptr};
    HANDLE unloaded_event{nullptr};

    HANDLE job_handle{nullptr};
    HANDLE proc_handle{nullptr};
    uint64_t spawn_qpc_us{0};
    uint64_t next_restart_qpc_us{0};
    std::array<uint64_t, 8> restart_timestamps_us{};
    size_t restart_history_count{0};

    void set_error(const char* msg) noexcept {
        const uint32_t next = (error_write_idx.load(std::memory_order_relaxed) + 1u) & 3u;
        auto& dst = error_ring[next];
        if (msg != nullptr) {
            std::snprintf(dst.data(), dst.size(), "%s", msg);
        } else {
            dst[0] = '\0';
        }
        error_write_idx.store(next, std::memory_order_release);
    }

    [[nodiscard]] std::string read_error() const {
        const uint32_t idx = error_write_idx.load(std::memory_order_acquire) & 3u;
        return std::string(error_ring[idx].data());
    }

    void publish_desc(const AetherNativePluginDesc& d) noexcept {
        const uint32_t next = desc_write_idx.load(std::memory_order_relaxed) ^ 1u;
        desc_buf[next] = d;
        desc_write_idx.store(next, std::memory_order_release);
    }

    [[nodiscard]] AetherNativePluginDesc read_desc() const noexcept {
        const uint32_t idx = desc_write_idx.load(std::memory_order_acquire) & 1u;
        return desc_buf[idx];
    }

    void close_process_and_job(bool graceful) noexcept {
        if (proc_handle != nullptr) {
            if (graceful && stop_event != nullptr) {
                SetEvent(stop_event);
                (void)WaitForSingleObject(proc_handle, 120);
            }
            if (job_handle != nullptr) {
                TerminateJobObject(job_handle, graceful ? 0u : 1u);
            }
            (void)WaitForSingleObject(proc_handle, 300);
            CloseHandle(proc_handle);
            proc_handle = nullptr;
        }
        if (job_handle != nullptr) {
            CloseHandle(job_handle);
            job_handle = nullptr;
        }
        job_object_active.store(false, std::memory_order_relaxed);
    }

    void cleanup_ipc() noexcept {
        while (shm_submit_in_flight.load(std::memory_order_acquire) != 0) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        if (shm != nullptr) {
            UnmapViewOfFile(shm);
            shm = nullptr;
        }
        if (shm_handle != nullptr) {
            CloseHandle(shm_handle);
            shm_handle = nullptr;
        }
        if (req_event != nullptr) {
            CloseHandle(req_event);
            req_event = nullptr;
        }
        if (rsp_event != nullptr) {
            CloseHandle(rsp_event);
            rsp_event = nullptr;
        }
        if (stop_event != nullptr) {
            CloseHandle(stop_event);
            stop_event = nullptr;
        }
    }

    void reset_shm_before_launch() {
        if (shm == nullptr) {
            return;
        }
        while (shm_submit_in_flight.load(std::memory_order_acquire) != 0) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        shm->magic = AETHER_NATIVE_SHM_MAGIC;
        shm->abi_version = AETHER_NATIVE_PLUGIN_ABI_VERSION;
        shm->heartbeat_qpc_us.store(now_qpc_us(), std::memory_order_relaxed);
        shm->active_flag.store(0u, std::memory_order_relaxed);
        shm->init_status.store(0, std::memory_order_relaxed);

        const std::string dll_utf8 = wide_to_utf8(cfg.dll_path.wstring());
        const uint32_t dll_len =
            static_cast<uint32_t>(std::min<size_t>(dll_utf8.size(), sizeof(shm->dll_path_utf8) - 1));
        std::memcpy(shm->dll_path_utf8, dll_utf8.data(), dll_len);
        shm->dll_path_utf8[dll_len] = '\0';
        shm->dll_path_len = dll_len;

        const uint32_t cfg_len =
            static_cast<uint32_t>(std::min<size_t>(cfg.config_json.size(), sizeof(shm->config_json_utf8) - 1));
        if (cfg_len > 0) {
            std::memcpy(shm->config_json_utf8, cfg.config_json.data(), cfg_len);
        }
        shm->config_json_utf8[cfg_len] = '\0';
        shm->config_json_len = cfg_len;

        shm->plugin_desc = AetherNativePluginDesc{};
        shm->requests.head.value.store(0, std::memory_order_relaxed);
        shm->requests.tail.value.store(0, std::memory_order_relaxed);
        shm->responses.head.value.store(0, std::memory_order_relaxed);
        shm->responses.tail.value.store(0, std::memory_order_relaxed);

        if (req_event != nullptr) {
            ResetEvent(req_event);
        }
        if (rsp_event != nullptr) {
            ResetEvent(rsp_event);
        }
        if (stop_event != nullptr) {
            ResetEvent(stop_event);
        }
    }
};

} // namespace

struct NativePluginManager::Impl {
    std::array<SlotRuntime, kMaxSlots> slots{};
    HANDLE wake_event{nullptr};
    std::atomic<bool> stop_supervisor{false};
    std::thread supervisor{};
    std::atomic<uint64_t> generation_counter{1};

    Impl() {
        wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (auto& s : slots) {
            s.ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            s.unloaded_event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        }
        supervisor = std::thread([this]() { run_supervisor(); });
    }

    ~Impl() {
        for (uint32_t i = 0; i < static_cast<uint32_t>(slots.size()); ++i) {
            if (slots[i].occupied.load(std::memory_order_acquire) == 2u) {
                slots[i].unload_requested.store(true, std::memory_order_release);
            }
        }
        stop_supervisor.store(true, std::memory_order_release);
        if (wake_event != nullptr) {
            SetEvent(wake_event);
        }
        if (supervisor.joinable()) {
            supervisor.join();
        }
        for (auto& s : slots) {
            s.close_process_and_job(true);
            s.cleanup_ipc();
            if (s.ready_event != nullptr) {
                CloseHandle(s.ready_event);
                s.ready_event = nullptr;
            }
            if (s.unloaded_event != nullptr) {
                CloseHandle(s.unloaded_event);
                s.unloaded_event = nullptr;
            }
        }
        if (wake_event != nullptr) {
            CloseHandle(wake_event);
            wake_event = nullptr;
        }
    }

    void spawn_runner(SlotRuntime& s) {
        s.close_process_and_job(false);
        if (s.resolved_runner.empty() || !std::filesystem::exists(s.resolved_runner)) {
            s.set_error("aether-plugin-runner.exe not found");
            s.permanently_disabled.store(true, std::memory_order_release);
            s.state.store(PluginState::Disabled, std::memory_order_release);
            SetEvent(s.ready_event);
            return;
        }

        s.reset_shm_before_launch();

        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job == nullptr) {
            s.set_error("CreateJobObjectW failed");
            schedule_restart_or_disable(s);
            return;
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext{};
        ext.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY |
                                               JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                                               JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        ext.BasicLimitInformation.ActiveProcessLimit = 1;
        ext.ProcessMemoryLimit = static_cast<SIZE_T>(s.cfg.memory_limit_bytes);
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &ext, sizeof(ext))) {
            CloseHandle(job);
            s.set_error("SetInformationJobObject(ExtendedLimit) failed");
            schedule_restart_or_disable(s);
            return;
        }

        JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
        ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                                 JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
                                 JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                                 JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
        (void)SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui));

        std::wstring cmd = L"\"" + s.resolved_runner.wstring() + L"\"" + L" --shm " + s.shm_name + L" --req-event " +
                           s.req_event_name + L" --rsp-event " + s.rsp_event_name + L" --stop-event " +
                           s.stop_event_name;

        std::vector<wchar_t> cmd_mut(cmd.begin(), cmd.end());
        cmd_mut.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        bool created = false;
        bool restricted_ok = false;

        HANDLE proc_token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY, &proc_token)) {
            HANDLE restricted_token = nullptr;
            if (CreateRestrictedToken(proc_token, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr,
                                      &restricted_token)) {
                if (CreateProcessAsUserW(restricted_token, s.resolved_runner.c_str(), cmd_mut.data(), nullptr, nullptr,
                                         FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                         nullptr, nullptr, &si, &pi)) {
                    created = true;
                    restricted_ok = true;
                }
                CloseHandle(restricted_token);
            }
            CloseHandle(proc_token);
        }

        if (!created) {
            SAFER_LEVEL_HANDLE safer_level = nullptr;
            if (SaferCreateLevel(SAFER_SCOPEID_USER, SAFER_LEVELID_CONSTRAINED, SAFER_LEVEL_OPEN, &safer_level,
                                 nullptr)) {
                HANDLE safer_token = nullptr;
                if (SaferComputeTokenFromLevel(safer_level, nullptr, &safer_token, 0, nullptr)) {
                    std::vector<wchar_t> cmd_retry(cmd.begin(), cmd.end());
                    cmd_retry.push_back(L'\0');
                    if (CreateProcessAsUserW(safer_token, s.resolved_runner.c_str(), cmd_retry.data(), nullptr, nullptr,
                                             FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                             nullptr, nullptr, &si, &pi)) {
                        created = true;
                        restricted_ok = true;
                    }
                    CloseHandle(safer_token);
                }
                SaferCloseLevel(safer_level);
            }
        }

        if (!created) {
            std::vector<wchar_t> cmd_fallback(cmd.begin(), cmd.end());
            cmd_fallback.push_back(L'\0');
            created = CreateProcessW(s.resolved_runner.c_str(), cmd_fallback.data(), nullptr, nullptr, FALSE,
                                     CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                                     &si, &pi) != FALSE;
        }

        if (!created) {
            CloseHandle(job);
            s.set_error("Failed to launch aether-plugin-runner.exe");
            schedule_restart_or_disable(s);
            return;
        }

        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(job);
            s.set_error("AssignProcessToJobObject failed");
            schedule_restart_or_disable(s);
            return;
        }

        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        s.job_handle = job;
        s.proc_handle = pi.hProcess;
        s.spawn_qpc_us = now_qpc_us();
        s.restricted_token_active.store(restricted_ok, std::memory_order_relaxed);
        s.job_object_active.store(true, std::memory_order_relaxed);
        s.state.store(PluginState::Starting, std::memory_order_release);
    }

    void schedule_restart_or_disable(SlotRuntime& s) {
        s.close_process_and_job(false);
        const uint64_t now_us = now_qpc_us();
        const uint64_t window_us = static_cast<uint64_t>(s.cfg.restart_window_sec) * 1000000ULL;

        size_t kept = 0;
        for (size_t i = 0; i < s.restart_history_count; ++i) {
            if (now_us >= s.restart_timestamps_us[i] && (now_us - s.restart_timestamps_us[i]) <= window_us) {
                s.restart_timestamps_us[kept++] = s.restart_timestamps_us[i];
            }
        }
        s.restart_history_count = kept;

        if (s.restart_history_count >= s.cfg.max_restarts_in_window) {
            s.permanently_disabled.store(true, std::memory_order_release);
            s.state.store(PluginState::Disabled, std::memory_order_release);
            s.set_error("Native plugin disabled: exceeded restart budget");
            SetEvent(s.ready_event);
            return;
        }

        const size_t backoff_idx = std::min<size_t>(s.restart_history_count, s.cfg.backoff_ms.size() - 1);
        const uint32_t delay_ms = s.cfg.backoff_ms[backoff_idx];
        if (s.restart_history_count < s.restart_timestamps_us.size()) {
            s.restart_timestamps_us[s.restart_history_count++] = now_us;
        }
        s.restart_count.fetch_add(1, std::memory_order_relaxed);
        s.next_restart_qpc_us = now_us + static_cast<uint64_t>(delay_ms) * 1000ULL;
        s.state.store(PluginState::Restarting, std::memory_order_release);
    }

    void tick_slot(SlotRuntime& s) {
        if (s.occupied.load(std::memory_order_acquire) != 2u) {
            return;
        }

        if (s.unload_requested.exchange(false, std::memory_order_acq_rel)) {
            s.state.store(PluginState::Stopped, std::memory_order_release);
            s.close_process_and_job(true);
            s.cleanup_ipc();
            s.restart_history_count = 0;
            SetEvent(s.ready_event);
            s.occupied.store(0u, std::memory_order_release);
            SetEvent(s.unloaded_event);
            return;
        }

        if (s.reset_requested.exchange(false, std::memory_order_acq_rel)) {
            s.permanently_disabled.store(false, std::memory_order_release);
            s.restart_history_count = 0;
            s.set_error("");
            ResetEvent(s.ready_event);
            s.start_requested.store(true, std::memory_order_release);
        }

        if (s.start_requested.exchange(false, std::memory_order_acq_rel)) {
            ResetEvent(s.ready_event);
            spawn_runner(s);
            return;
        }

        const PluginState cur = s.state.load(std::memory_order_acquire);
        const uint64_t now_us = now_qpc_us();

        if (cur == PluginState::Restarting) {
            if (now_us >= s.next_restart_qpc_us) {
                spawn_runner(s);
            }
            return;
        }

        if (cur != PluginState::Starting && cur != PluginState::Running) {
            return;
        }

        if (s.proc_handle == nullptr || s.shm == nullptr) {
            s.set_error("Runner handle missing unexpectedly");
            schedule_restart_or_disable(s);
            return;
        }

        if (WaitForSingleObject(s.proc_handle, 0) == WAIT_OBJECT_0) {
            DWORD exit_code = 0;
            (void)GetExitCodeProcess(s.proc_handle, &exit_code);
            s.last_exit_code.store(exit_code, std::memory_order_relaxed);
            s.crash_count.fetch_add(1, std::memory_order_relaxed);
            char msg[128]{};
            std::snprintf(msg, sizeof(msg), "Native runner exited unexpectedly (code 0x%08lX)", exit_code);
            s.set_error(msg);
            schedule_restart_or_disable(s);
            return;
        }

        if (cur == PluginState::Starting) {
            const uint32_t flag = s.shm->active_flag.load(std::memory_order_acquire);
            if (flag == 1u) {
                s.publish_desc(s.shm->plugin_desc);
                s.set_error("");
                s.state.store(PluginState::Running, std::memory_order_release);
                SetEvent(s.ready_event);
                return;
            }
            if (flag == 2u) {
                s.crash_count.fetch_add(1, std::memory_order_relaxed);
                s.set_error("Native plugin initialization failed inside runner");
                schedule_restart_or_disable(s);
                return;
            }
            if ((now_us - s.spawn_qpc_us) > 5000000ULL) {
                s.hang_kill_count.fetch_add(1, std::memory_order_relaxed);
                s.set_error("Native runner startup timed out (>5000 ms)");
                schedule_restart_or_disable(s);
            }
            return;
        }

        // cur == PluginState::Running
        const uint64_t hb = s.shm->heartbeat_qpc_us.load(std::memory_order_acquire);
        const uint64_t timeout_us = static_cast<uint64_t>(s.cfg.heartbeat_timeout_ms) * 1000ULL;
        if (now_us >= hb && (now_us - hb) > timeout_us) {
            s.hang_kill_count.fetch_add(1, std::memory_order_relaxed);
            s.last_exit_code.store(0xDEAD0500u, std::memory_order_relaxed);
            s.set_error("Native runner heartbeat stall (>500 ms); killed by watchdog");
            schedule_restart_or_disable(s);
        }
    }

    void run_supervisor() {
        while (!stop_supervisor.load(std::memory_order_acquire)) {
            for (auto& s : slots) {
                tick_slot(s);
            }
            (void)WaitForSingleObject(wake_event, 20);
        }
        for (auto& s : slots) {
            tick_slot(s);
        }
    }
};

NativePluginManager::NativePluginManager() : impl_(std::make_unique<Impl>()) {}
NativePluginManager::~NativePluginManager() = default;

int32_t NativePluginManager::load_plugin(const NativePluginConfig& cfg) {
    if (cfg.dll_path.empty()) {
        return -1;
    }

    int32_t chosen = -1;
    for (uint32_t i = 0; i < static_cast<uint32_t>(kMaxSlots); ++i) {
        uint8_t expected = 0;
        if (impl_->slots[i].occupied.compare_exchange_strong(expected, 1u, std::memory_order_acq_rel)) {
            chosen = static_cast<int32_t>(i);
            break;
        }
    }
    if (chosen < 0) {
        return -2;
    }

    SlotRuntime& s = impl_->slots[static_cast<size_t>(chosen)];
    s.cfg = cfg;
    s.resolved_runner = resolve_runner_exe(cfg.runner_exe_path);
    s.start_requested.store(false, std::memory_order_relaxed);
    s.unload_requested.store(false, std::memory_order_relaxed);
    s.reset_requested.store(false, std::memory_order_relaxed);
    s.permanently_disabled.store(false, std::memory_order_relaxed);
    s.restricted_token_active.store(false, std::memory_order_relaxed);
    s.job_object_active.store(false, std::memory_order_relaxed);
    s.restart_count.store(0, std::memory_order_relaxed);
    s.crash_count.store(0, std::memory_order_relaxed);
    s.hang_kill_count.store(0, std::memory_order_relaxed);
    s.last_exit_code.store(0, std::memory_order_relaxed);
    s.restart_history_count = 0;
    s.set_error("");
    s.publish_desc(AetherNativePluginDesc{});
    s.state.store(PluginState::Stopped, std::memory_order_relaxed);

    const DWORD pid = GetCurrentProcessId();
    const uint64_t gen = impl_->generation_counter.fetch_add(1, std::memory_order_relaxed);
    const std::wstring suffix = std::to_wstring(pid) + L"_" + std::to_wstring(chosen) + L"_" + std::to_wstring(gen);
    s.shm_name = L"Local\\AetherNativePlugin_" + suffix;
    s.req_event_name = L"Local\\AetherNativeReq_" + suffix;
    s.rsp_event_name = L"Local\\AetherNativeRsp_" + suffix;
    s.stop_event_name = L"Local\\AetherNativeStop_" + suffix;

    const size_t shm_bytes = sizeof(ShmBlock);
    s.shm_handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      static_cast<DWORD>((shm_bytes >> 32) & 0xFFFFFFFFULL),
                                      static_cast<DWORD>(shm_bytes & 0xFFFFFFFFULL), s.shm_name.c_str());
    if (s.shm_handle == nullptr) {
        s.occupied.store(0u, std::memory_order_release);
        return -3;
    }
    void* view = MapViewOfFile(s.shm_handle, FILE_MAP_ALL_ACCESS, 0, 0, shm_bytes);
    if (view == nullptr) {
        s.cleanup_ipc();
        s.occupied.store(0u, std::memory_order_release);
        return -4;
    }
    std::memset(view, 0, shm_bytes);
    s.shm = new (view) ShmBlock();

    s.req_event = CreateEventW(nullptr, FALSE, FALSE, s.req_event_name.c_str());
    s.rsp_event = CreateEventW(nullptr, FALSE, FALSE, s.rsp_event_name.c_str());
    s.stop_event = CreateEventW(nullptr, TRUE, FALSE, s.stop_event_name.c_str());
    if (s.req_event == nullptr || s.rsp_event == nullptr || s.stop_event == nullptr) {
        s.cleanup_ipc();
        s.occupied.store(0u, std::memory_order_release);
        return -5;
    }

    ResetEvent(s.unloaded_event);
    ResetEvent(s.ready_event);
    s.state.store(PluginState::Starting, std::memory_order_release);
    s.start_requested.store(true, std::memory_order_release);
    s.occupied.store(2u, std::memory_order_release);
    SetEvent(impl_->wake_event);
    return chosen;
}

void NativePluginManager::unload_plugin(uint32_t slot_id) {
    if (slot_id >= kMaxSlots) {
        return;
    }
    SlotRuntime& s = impl_->slots[slot_id];
    if (s.occupied.load(std::memory_order_acquire) != 2u) {
        return;
    }
    s.unload_requested.store(true, std::memory_order_release);
    SetEvent(impl_->wake_event);
    (void)WaitForSingleObject(s.unloaded_event, 2000);
}

bool NativePluginManager::wait_until_ready(uint32_t slot_id, uint32_t timeout_ms) {
    if (slot_id >= kMaxSlots) {
        return false;
    }
    SlotRuntime& s = impl_->slots[slot_id];
    const PluginState cur = s.state.load(std::memory_order_acquire);
    if (cur == PluginState::Running) {
        return true;
    }
    if (cur == PluginState::Disabled || s.occupied.load(std::memory_order_acquire) != 2u) {
        return false;
    }
    const DWORD wait_rc = WaitForSingleObject(s.ready_event, timeout_ms);
    if (wait_rc != WAIT_OBJECT_0) {
        return s.state.load(std::memory_order_acquire) == PluginState::Running;
    }
    return s.state.load(std::memory_order_acquire) == PluginState::Running;
}

bool NativePluginManager::try_submit_process(uint32_t slot_id, uint64_t frame_index, double time_sec,
                                             const float* inputs, uint32_t input_count, uint32_t output_count,
                                             uint32_t* out_seq) noexcept {
    if (slot_id >= kMaxSlots) {
        return false;
    }
    SlotRuntime& s = impl_->slots[slot_id];
    if (s.occupied.load(std::memory_order_acquire) != 2u) {
        return false;
    }

    s.shm_submit_in_flight.fetch_add(1, std::memory_order_acq_rel);
    bool pushed = false;
    if (s.state.load(std::memory_order_acquire) == PluginState::Running && s.shm != nullptr) {
        RequestSlot req{};
        const uint32_t seq = s.seq_counter.fetch_add(1, std::memory_order_relaxed);
        req.op = static_cast<uint32_t>(RequestOp::Process);
        req.seq = seq;
        req.frame_index = frame_index;
        req.time_sec = time_sec;
        req.input_count = std::min<uint32_t>(input_count, AETHER_NATIVE_PLUGIN_MAX_IO);
        req.output_count = std::min<uint32_t>(output_count, AETHER_NATIVE_PLUGIN_MAX_IO);
        if (inputs != nullptr && req.input_count > 0) {
            std::memcpy(req.inputs, inputs, static_cast<size_t>(req.input_count) * sizeof(float));
        }
        pushed = s.shm->requests.try_push(req);
        if (pushed) {
            if (out_seq != nullptr) {
                *out_seq = seq;
            }
            if (s.req_event != nullptr) {
                SetEvent(s.req_event);
            }
        }
    }
    s.shm_submit_in_flight.fetch_sub(1, std::memory_order_release);
    return pushed;
}

bool NativePluginManager::try_pop_response(uint32_t slot_id, ResponseSlot& out) noexcept {
    if (slot_id >= kMaxSlots) {
        return false;
    }
    SlotRuntime& s = impl_->slots[slot_id];
    if (s.occupied.load(std::memory_order_acquire) != 2u) {
        return false;
    }
    s.shm_submit_in_flight.fetch_add(1, std::memory_order_acq_rel);
    bool popped = false;
    if (s.shm != nullptr && s.state.load(std::memory_order_acquire) == PluginState::Running) {
        popped = s.shm->responses.try_pop(out);
    }
    s.shm_submit_in_flight.fetch_sub(1, std::memory_order_release);
    return popped;
}

bool NativePluginManager::process_sync(uint32_t slot_id, uint64_t frame_index, double time_sec, const float* inputs,
                                       uint32_t input_count, float* outputs, uint32_t output_count, uint32_t timeout_ms,
                                       int32_t* out_plugin_status, std::string* out_error) {
    if (slot_id >= kMaxSlots) {
        return false;
    }
    SlotRuntime& s = impl_->slots[slot_id];
    uint32_t seq = 0;
    if (!try_submit_process(slot_id, frame_index, time_sec, inputs, input_count, output_count, &seq)) {
        return false;
    }

    const uint64_t deadline_us = now_qpc_us() + static_cast<uint64_t>(timeout_ms) * 1000ULL;
    for (;;) {
        ResponseSlot rsp{};
        while (try_pop_response(slot_id, rsp)) {
            if (rsp.seq == seq) {
                if (out_plugin_status != nullptr) {
                    *out_plugin_status = rsp.status;
                }
                if (out_error != nullptr) {
                    *out_error = rsp.error_msg;
                }
                if (outputs != nullptr) {
                    const uint32_t cnt =
                        std::min<uint32_t>({output_count, rsp.output_count, AETHER_NATIVE_PLUGIN_MAX_IO});
                    if (cnt > 0) {
                        std::memcpy(outputs, rsp.outputs, static_cast<size_t>(cnt) * sizeof(float));
                    }
                }
                return rsp.status == 0;
            }
        }

        if (s.state.load(std::memory_order_acquire) != PluginState::Running) {
            if (out_error != nullptr) {
                *out_error = s.read_error();
            }
            return false;
        }
        const uint64_t now_us = now_qpc_us();
        if (now_us >= deadline_us) {
            if (out_error != nullptr) {
                *out_error = "process_sync timed out";
            }
            return false;
        }
        const uint32_t rem_ms = std::max<uint32_t>(1u, static_cast<uint32_t>((deadline_us - now_us) / 1000ULL));
        (void)WaitForSingleObject(s.rsp_event, std::min<uint32_t>(rem_ms, 25u));
    }
}

NativePluginHealth NativePluginManager::health(uint32_t slot_id) const {
    NativePluginHealth h{};
    if (slot_id >= kMaxSlots) {
        return h;
    }
    const SlotRuntime& s = impl_->slots[slot_id];
    if (s.occupied.load(std::memory_order_acquire) != 2u) {
        return h;
    }
    h.state = s.state.load(std::memory_order_acquire);
    h.permanently_disabled = s.permanently_disabled.load(std::memory_order_acquire);
    h.restricted_token_active = s.restricted_token_active.load(std::memory_order_relaxed);
    h.job_object_active = s.job_object_active.load(std::memory_order_relaxed);
    h.restart_count = s.restart_count.load(std::memory_order_relaxed);
    h.crash_count = s.crash_count.load(std::memory_order_relaxed);
    h.hang_kill_count = s.hang_kill_count.load(std::memory_order_relaxed);
    h.last_exit_code = s.last_exit_code.load(std::memory_order_relaxed);
    h.desc = s.read_desc();
    h.last_error = s.read_error();
    if (s.shm != nullptr) {
        const uint64_t hb = s.shm->heartbeat_qpc_us.load(std::memory_order_acquire);
        const uint64_t now_us = now_qpc_us();
        h.last_heartbeat_age_ms = (now_us >= hb && hb > 0) ? (now_us - hb) / 1000ULL : 0ULL;
    }
    return h;
}

bool NativePluginManager::reset_and_restart(uint32_t slot_id) {
    if (slot_id >= kMaxSlots) {
        return false;
    }
    SlotRuntime& s = impl_->slots[slot_id];
    if (s.occupied.load(std::memory_order_acquire) != 2u) {
        return false;
    }
    s.reset_requested.store(true, std::memory_order_release);
    SetEvent(impl_->wake_event);
    return true;
}

} // namespace aether::native_plugin
