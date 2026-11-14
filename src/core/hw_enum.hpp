#pragma once

#include "core/cpu_topology.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace aether {

struct GpuAdapterInfo {
    std::string name;
    uint32_t vendor_id{0};
    uint32_t device_id{0};
    uint64_t dedicated_vram_mb{0};
    uint64_t shared_vram_mb{0};
    std::array<uint8_t, 8> luid{};
    uint32_t driver_version_parts[4]{};
    bool is_software{false};
    bool has_dxgi_driver{false};
};

struct CameraDeviceInfo {
    std::string friendly_name;
    std::string symbolic_link;
    uint64_t symbolic_id_hash{0};
    bool likely_virtual{false};
};

struct NpuStatusInfo {
    bool probed{false};
    bool available{false};
    int32_t ffi_status{0}; // AETHER_E_UNSUPPORTED in Phase 1 (HR-09)
    std::string device_name;
    std::string note;
};

struct HardwareInventory {
    CpuTopology cpu;
    std::vector<GpuAdapterInfo> gpus;
    std::vector<CameraDeviceInfo> cameras;
    NpuStatusInfo npu;
    uint64_t total_ram_mb{0};
};

/**
 * Queries real system hardware inventory (`§4.9`): CPUID + P/E topology, total RAM,
 * DXGI GPU adapters with LUIDs, Media Foundation video capture device names (without
 * opening any camera stream, `HR-12`), and Phase 1 NPU stub status (`HR-09`).
 */
[[nodiscard]] HardwareInventory enumerate_hardware();

} // namespace aether
