#include "core/cpu_topology.hpp"

#include "core/log.hpp"

#if defined(_WIN32)
#include <Windows.h>
#include <avrt.h>
#include <intrin.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

namespace aether {

namespace {

std::string query_cpu_brand() {
#if defined(_WIN32)
    int info[4]{};
    ::__cpuid(info, static_cast<int>(0x80000000U));
    const unsigned int max_ext = static_cast<unsigned int>(info[0]);
    if (max_ext < 0x80000004U) {
        return "Unknown x64 CPU";
    }
    std::array<char, 49> brand{};
    for (unsigned int i = 0; i < 3; ++i) {
        int regs[4]{};
        ::__cpuid(regs, static_cast<int>(0x80000002U + i));
        std::memcpy(brand.data() + (static_cast<size_t>(i) * 16U), regs, 16);
    }
    brand.back() = '\0';
    std::string result(brand.data());
    while (!result.empty() && result.front() == ' ') {
        result.erase(result.begin());
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result.empty() ? "Unknown x64 CPU" : result;
#else
    return "x64 CPU";
#endif
}

uint32_t popcount64(uint64_t x) noexcept {
    uint32_t c = 0;
    while (x != 0) {
        x &= (x - 1ULL);
        ++c;
    }
    return c;
}

} // namespace

CpuTopology detect_cpu_topology() {
    CpuTopology topo{};
    topo.brand_string = query_cpu_brand();

#if defined(_WIN32)
    DWORD len = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len > 0) {
        auto buffer = std::make_unique<uint8_t[]>(len);
        if (::GetLogicalProcessorInformationEx(RelationProcessorCore,
                                               reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.get()),
                                               &len) != FALSE) {
            uint8_t max_eff = 0;
            uint8_t min_eff = 255;

            DWORD offset = 0;
            uint32_t idx = 0;
            while (offset < len) {
                const auto* entry =
                    reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.get() + offset);
                if (entry->Relationship == RelationProcessorCore && entry->Processor.GroupCount > 0) {
                    CpuCoreInfo ci{};
                    ci.physical_index = idx++;
                    ci.efficiency_class = entry->Processor.EfficiencyClass;
                    ci.affinity_mask = static_cast<uint64_t>(entry->Processor.GroupMask[0].Mask);
                    ci.logical_threads = popcount64(ci.affinity_mask);
                    max_eff = std::max(max_eff, ci.efficiency_class);
                    min_eff = std::min(min_eff, ci.efficiency_class);
                    topo.logical_processors += ci.logical_threads;
                    topo.cores.push_back(ci);
                }
                offset += entry->Size;
            }

            topo.physical_cores = static_cast<uint32_t>(topo.cores.size());
            topo.is_hybrid = (max_eff > min_eff);

            for (auto& ci : topo.cores) {
                ci.is_p_core = !topo.is_hybrid || (ci.efficiency_class == max_eff);
                if (ci.is_p_core) {
                    ++topo.p_core_count;
                    topo.p_logical_count += ci.logical_threads;
                } else {
                    ++topo.e_core_count;
                    topo.e_logical_count += ci.logical_threads;
                }
            }
        }
    }

    // Collect Win32 CPU Set IDs split by EfficiencyClass for `SetThreadSelectedCpuSets`.
    ULONG set_bytes = 0;
    ::GetSystemCpuSetInformation(nullptr, 0, &set_bytes, ::GetCurrentProcess(), 0);
    if (set_bytes > 0) {
        auto set_buf = std::make_unique<uint8_t[]>(set_bytes);
        if (::GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(set_buf.get()), set_bytes,
                                         &set_bytes, ::GetCurrentProcess(), 0) != FALSE) {
            uint8_t max_set_eff = 0;
            uint8_t min_set_eff = 255;
            ULONG off = 0;
            while (off < set_bytes) {
                const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(set_buf.get() + off);
                if (info->Type == CpuSetInformation) {
                    max_set_eff = std::max(max_set_eff, info->CpuSet.EfficiencyClass);
                    min_set_eff = std::min(min_set_eff, info->CpuSet.EfficiencyClass);
                }
                off += info->Size;
            }
            off = 0;
            while (off < set_bytes) {
                const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(set_buf.get() + off);
                if (info->Type == CpuSetInformation) {
                    if (max_set_eff > min_set_eff && info->CpuSet.EfficiencyClass == min_set_eff) {
                        topo.e_cpu_set_ids.push_back(info->CpuSet.Id);
                    } else {
                        topo.p_cpu_set_ids.push_back(info->CpuSet.Id);
                    }
                }
                off += info->Size;
            }
        }
    }
#endif

    return topo;
}

ThreadPlacementResult apply_thread_placement(ThreadRole role, const CpuTopology& topology) noexcept {
    ThreadPlacementResult res{};
    res.role = role;

#if defined(_WIN32)
    HANDLE thread = ::GetCurrentThread();

    // Name thread for Visual Studio / Tracy / crash dump diagnostics.
    const wchar_t* wname = L"AetherWorker";
    switch (role) {
    case ThreadRole::MainT0:
        wname = L"T0_Main";
        break;
    case ThreadRole::TrackingT1:
        wname = L"T1_Tracking";
        break;
    case ThreadRole::SimT2:
        wname = L"T2_Sim";
        break;
    case ThreadRole::RenderT3:
        wname = L"T3_RenderUI";
        break;
    case ThreadRole::OutputT4:
        wname = L"T4_Output";
        break;
    case ThreadRole::AssetLoader:
        wname = L"Loader_Async";
        break;
    case ThreadRole::Background:
        wname = L"Background";
        break;
    }
    ::SetThreadDescription(thread, wname);

    LogSystem::set_thread_role(static_cast<uint8_t>(role));

    if (role == ThreadRole::SimT2 || role == ThreadRole::RenderT3 || role == ThreadRole::TrackingT1) {
        if (topology.is_hybrid && !topology.p_cpu_set_ids.empty()) {
            static_assert(sizeof(ULONG) == sizeof(uint32_t) && alignof(ULONG) == alignof(uint32_t));
            res.cpu_set_applied =
                ::SetThreadSelectedCpuSets(thread, reinterpret_cast<const ULONG*>(topology.p_cpu_set_ids.data()),
                                           static_cast<ULONG>(topology.p_cpu_set_ids.size())) != FALSE;
            res.assigned_cpu_sets = static_cast<uint32_t>(topology.p_cpu_set_ids.size());
        }
        const int prio = (role == ThreadRole::SimT2 || role == ThreadRole::RenderT3) ? THREAD_PRIORITY_ABOVE_NORMAL
                                                                                     : THREAD_PRIORITY_NORMAL;
        res.priority_applied = (::SetThreadPriority(thread, prio) != FALSE);

        if (role == ThreadRole::SimT2 || role == ThreadRole::RenderT3) {
            DWORD task_idx = 0;
            HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Games", &task_idx);
            if (mmcss != nullptr) {
                res.mmcss_applied = true;
                // Keep association alive via intentional process-lifetime handle or clean detach on exit?
                // Actually AvSetMmThreadCharacteristicsW boosts thread until AvRevertMmThreadCharacteristics or thread
                // exit.
            }
        }
        res.policy_summary = topology.is_hybrid ? "P-core CPU Set + Boost" : "Homogeneous Boost";
    } else if (role == ThreadRole::AssetLoader || role == ThreadRole::Background) {
        if (topology.is_hybrid && !topology.e_cpu_set_ids.empty()) {
            static_assert(sizeof(ULONG) == sizeof(uint32_t) && alignof(ULONG) == alignof(uint32_t));
            res.cpu_set_applied =
                ::SetThreadSelectedCpuSets(thread, reinterpret_cast<const ULONG*>(topology.e_cpu_set_ids.data()),
                                           static_cast<ULONG>(topology.e_cpu_set_ids.size())) != FALSE;
            res.assigned_cpu_sets = static_cast<uint32_t>(topology.e_cpu_set_ids.size());
        }
        res.priority_applied = (::SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL) != FALSE);
        res.policy_summary = topology.is_hybrid ? "E-core CPU Set + LowPrio" : "LowPrio";
    } else {
        res.policy_summary = "OS Default";
    }
#else
    (void)topology;
#endif

    AETHER_LOG_INFO("topo", "Thread %s placed (%s, sets=%u, mmcss=%d)", thread_role_name(role), res.policy_summary,
                    res.assigned_cpu_sets, res.mmcss_applied ? 1 : 0);
    return res;
}

const char* thread_role_name(ThreadRole role) noexcept {
    switch (role) {
    case ThreadRole::MainT0:
        return "T0_Main";
    case ThreadRole::TrackingT1:
        return "T1_Tracking";
    case ThreadRole::SimT2:
        return "T2_Sim";
    case ThreadRole::RenderT3:
        return "T3_RenderUI";
    case ThreadRole::OutputT4:
        return "T4_Output";
    case ThreadRole::AssetLoader:
        return "Loader_Async";
    case ThreadRole::Background:
        return "Background";
    }
    return "Unknown";
}

} // namespace aether
