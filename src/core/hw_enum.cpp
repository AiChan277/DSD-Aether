#include "core/hw_enum.hpp"

#include "aether_ffi_contract.h"
#include "core/log.hpp"

#if defined(_WIN32)
// clang-format off
#include <Windows.h>
// clang-format on
#include <SetupAPI.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>

namespace aether {

namespace {

#if defined(_WIN32)
std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) {
        return {};
    }
    const int needed =
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

uint64_t fnv1a_64(const void* data, size_t len) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ULL;
    }
    return h;
}

bool contains_ci(std::string_view hay, std::string_view needle) noexcept {
    if (needle.empty() || hay.size() < needle.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            auto to_low = [](char c) noexcept -> char {
                return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            };
            if (to_low(hay[i + j]) != to_low(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

void enum_dxgi_gpus(std::vector<GpuAdapterInfo>& out) {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory6)))) {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory1;
        if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory1)))) {
            return;
        }
        for (UINT idx = 0;; ++idx) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (factory1->EnumAdapters1(idx, &adapter) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                GpuAdapterInfo info{};
                info.name = wide_to_utf8(desc.Description);
                info.vendor_id = desc.VendorId;
                info.device_id = desc.DeviceId;
                info.dedicated_vram_mb = desc.DedicatedVideoMemory / (1024ULL * 1024ULL);
                info.shared_vram_mb = desc.SharedSystemMemory / (1024ULL * 1024ULL);
                std::memcpy(info.luid.data(), &desc.AdapterLuid, 8);
                info.is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                out.push_back(std::move(info));
            }
        }
        return;
    }

    for (UINT idx = 0;; ++idx) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory6->EnumAdapterByGpuPreference(idx, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) ==
            DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            GpuAdapterInfo info{};
            info.name = wide_to_utf8(desc.Description);
            info.vendor_id = desc.VendorId;
            info.device_id = desc.DeviceId;
            info.dedicated_vram_mb = desc.DedicatedVideoMemory / (1024ULL * 1024ULL);
            info.shared_vram_mb = desc.SharedSystemMemory / (1024ULL * 1024ULL);
            std::memcpy(info.luid.data(), &desc.AdapterLuid, 8);
            info.is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

            LARGE_INTEGER driver_ver{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver_ver))) {
                info.has_dxgi_driver = true;
                info.driver_version_parts[0] = HIWORD(driver_ver.HighPart);
                info.driver_version_parts[1] = LOWORD(driver_ver.HighPart);
                info.driver_version_parts[2] = HIWORD(driver_ver.LowPart);
                info.driver_version_parts[3] = LOWORD(driver_ver.LowPart);
            }
            out.push_back(std::move(info));
        }
    }
}

void enum_mf_cameras(std::vector<CameraDeviceInfo>& out) {
    const HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = SUCCEEDED(com_hr);

    if (SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        Microsoft::WRL::ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(::MFCreateAttributes(&attrs, 1)) &&
            SUCCEEDED(
                attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
            IMFActivate** devices = nullptr;
            UINT32 count = 0;
            if (SUCCEEDED(::MFEnumDeviceSources(attrs.Get(), &devices, &count)) && devices != nullptr) {
                for (UINT32 i = 0; i < count; ++i) {
                    CameraDeviceInfo cam{};
                    WCHAR* friendly = nullptr;
                    UINT32 friendly_len = 0;
                    if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendly,
                                                                 &friendly_len)) &&
                        friendly != nullptr) {
                        cam.friendly_name = wide_to_utf8(std::wstring_view(friendly, friendly_len));
                        ::CoTaskMemFree(friendly);
                    } else {
                        cam.friendly_name = "Video Capture Device";
                    }

                    WCHAR* symlink = nullptr;
                    UINT32 symlink_len = 0;
                    if (SUCCEEDED(devices[i]->GetAllocatedString(
                            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symlink, &symlink_len)) &&
                        symlink != nullptr) {
                        cam.symbolic_link = wide_to_utf8(std::wstring_view(symlink, symlink_len));
                        cam.symbolic_id_hash = fnv1a_64(symlink, symlink_len * sizeof(WCHAR));
                        ::CoTaskMemFree(symlink);
                    }
                    static constexpr const char* kVirtualHints[] = {
                        "smart connect", "obs virtual",  "obs-camera", "broadcast", "snap camera",
                        "virtual",       "root\\camera", "manycam",    "iriun",     "droidcam",
                        "epoccam",       "youcam",       "vcam",
                    };
                    for (const char* hint : kVirtualHints) {
                        if (contains_ci(cam.friendly_name, hint) || contains_ci(cam.symbolic_link, hint)) {
                            cam.likely_virtual = true;
                            break;
                        }
                    }
                    // HR-12: NEVER open/activate stream or capture/persist camera frames here.
                    devices[i]->Release();
                    out.push_back(std::move(cam));
                }
                ::CoTaskMemFree(static_cast<void*>(devices));
                std::stable_partition(out.begin(), out.end(),
                                      [](const CameraDeviceInfo& c) { return !c.likely_virtual; });
            }
        }
        ::MFShutdown();
    }

    if (com_owned) {
        ::CoUninitialize();
    }
}

std::string query_devnode_string_prop(HDEVINFO devs, SP_DEVINFO_DATA& dev_info, DWORD prop) {
    std::array<wchar_t, 512> buf{};
    DWORD reg_type = 0;
    DWORD needed = 0;
    if (::SetupDiGetDeviceRegistryPropertyW(devs, &dev_info, prop, &reg_type, reinterpret_cast<PBYTE>(buf.data()),
                                            static_cast<DWORD>((buf.size() - 1U) * sizeof(wchar_t)),
                                            &needed) != FALSE) {
        buf.back() = L'\0';
        return wide_to_utf8(buf.data());
    }
    return {};
}

bool check_openvino_ep_dll_present() {
    std::array<wchar_t, MAX_PATH> exe_path{};
    const DWORD n = ::GetModuleFileNameW(nullptr, exe_path.data(), static_cast<DWORD>(exe_path.size()));
    if (n > 0 && n < exe_path.size()) {
        std::wstring dir(exe_path.data(), n);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            const std::wstring candidate = dir.substr(0, slash + 1U) + L"onnxruntime_providers_openvino.dll";
            if (::GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return true;
            }
        }
    }
    std::array<wchar_t, MAX_PATH> local_app{};
    const DWORD env_n =
        ::GetEnvironmentVariableW(L"LOCALAPPDATA", local_app.data(), static_cast<DWORD>(local_app.size()));
    if (env_n > 0 && env_n < local_app.size()) {
        const std::wstring sdk_dll = std::wstring(local_app.data(), env_n) +
                                     L"\\Aether\\toolchains\\ort-1.24.1\\bin\\onnxruntime_providers_openvino.dll";
        if (::GetFileAttributesW(sdk_dll.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return true;
        }
    }
    return ::SearchPathW(nullptr, L"onnxruntime_providers_openvino.dll", nullptr, 0, nullptr, nullptr) > 0;
}

void enum_wasapi_audio_inputs(std::vector<AudioInputDeviceInfo>& out) {
    out.clear();
    const HRESULT co_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = (co_hr == S_OK || co_hr == S_FALSE);

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (SUCCEEDED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
        enumerator) {
        std::string default_id;
        Microsoft::WRL::ComPtr<IMMDevice> def_dev;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &def_dev)) && def_dev) {
            LPWSTR wid = nullptr;
            if (SUCCEEDED(def_dev->GetId(&wid)) && wid != nullptr) {
                default_id = wide_to_utf8(wid);
                ::CoTaskMemFree(wid);
            }
        }

        Microsoft::WRL::ComPtr<IMMDeviceCollection> coll;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll)) && coll) {
            UINT count = 0;
            if (SUCCEEDED(coll->GetCount(&count))) {
                for (UINT i = 0; i < count; ++i) {
                    Microsoft::WRL::ComPtr<IMMDevice> dev;
                    if (FAILED(coll->Item(i, &dev)) || !dev) {
                        continue;
                    }
                    AudioInputDeviceInfo info{};
                    LPWSTR wid = nullptr;
                    if (SUCCEEDED(dev->GetId(&wid)) && wid != nullptr) {
                        info.device_id = wide_to_utf8(wid);
                        ::CoTaskMemFree(wid);
                    }
                    info.is_default = (!default_id.empty() && info.device_id == default_id);

                    Microsoft::WRL::ComPtr<IPropertyStore> props;
                    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                        static constexpr PROPERTYKEY kFriendlyNameKey = {
                            {0xa45c254eU, 0xdf1cU, 0x4efdU, {0x80U, 0x20U, 0x67U, 0xd1U, 0x46U, 0xa8U, 0x50U, 0xe0U}},
                            14};
                        PROPVARIANT var{};
                        PropVariantInit(&var);
                        if (SUCCEEDED(props->GetValue(kFriendlyNameKey, &var)) && var.vt == VT_LPWSTR &&
                            var.pwszVal != nullptr) {
                            info.friendly_name = wide_to_utf8(var.pwszVal);
                        }
                        ::PropVariantClear(&var);
                    }
                    if (info.friendly_name.empty()) {
                        info.friendly_name = "Audio Capture Device";
                    }
                    out.push_back(std::move(info));
                }
            }
        }
    }

    std::stable_partition(out.begin(), out.end(), [](const AudioInputDeviceInfo& d) { return d.is_default; });

    if (com_owned) {
        ::CoUninitialize();
    }
}

void probe_npu_hardware(NpuStatusInfo& out) {
    out.probed = true;
    out.available = false;
    out.ffi_status = AETHER_OK;
    out.device_name = "None";

    // Windows 11 ComputeAccelerator PnP class GUID: {F01A9D53-3FF6-48D2-9F97-C8A7004BE10C}
    constexpr GUID kComputeAcceleratorClass = {
        0xf01a9d53U, 0x3ff6U, 0x48d2U, {0x9fU, 0x97U, 0xc8U, 0xa7U, 0x00U, 0x4bU, 0xe1U, 0x0cU}};

    auto scan_devset = [&](HDEVINFO devs, bool filter_keywords) -> bool {
        if (devs == INVALID_HANDLE_VALUE) {
            return false;
        }
        bool found = false;
        SP_DEVINFO_DATA dev_info{};
        dev_info.cbSize = sizeof(dev_info);
        for (DWORD idx = 0; ::SetupDiEnumDeviceInfo(devs, idx, &dev_info) != FALSE; ++idx) {
            std::string name = query_devnode_string_prop(devs, dev_info, SPDRP_FRIENDLYNAME);
            if (name.empty()) {
                name = query_devnode_string_prop(devs, dev_info, SPDRP_DEVICEDESC);
            }
            const std::string hwid = query_devnode_string_prop(devs, dev_info, SPDRP_HARDWAREID);
            if (filter_keywords) {
                const bool is_npu = contains_ci(name, "AI Boost") || contains_ci(name, "NPU") ||
                                    contains_ci(name, "Neural") || contains_ci(hwid, "DEV_AD1D") ||
                                    contains_ci(hwid, "DEV_7D1D") || contains_ci(hwid, "DEV_643E");
                if (!is_npu) {
                    continue;
                }
            }
            if (!name.empty()) {
                out.available = true;
                out.device_name = name;
                found = true;
                break;
            }
        }
        ::SetupDiDestroyDeviceInfoList(devs);
        return found;
    };

    HDEVINFO ca_devs = ::SetupDiGetClassDevsW(&kComputeAcceleratorClass, nullptr, nullptr, DIGCF_PRESENT);
    if (!scan_devset(ca_devs, false)) {
        HDEVINFO pci_devs = ::SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
        (void)scan_devset(pci_devs, true);
    }

    const bool ov_dll = check_openvino_ep_dll_present();
    if (out.available) {
        out.note = ov_dll ? "ComputeAccelerator PnP verified; OpenVINO EP runtime available"
                          : "ComputeAccelerator PnP verified; OpenVINO EP DLL not in probe search path";
    } else {
        out.note = ov_dll ? "No dedicated ComputeAccelerator/NPU PnP device detected; OpenVINO EP DLL present"
                          : "No dedicated NPU PnP device detected (tier fallback handles iGPU/dGPU/CPU)";
    }
}
#endif

} // namespace

HardwareInventory enumerate_hardware() {
    HardwareInventory inv{};
    inv.cpu = detect_cpu_topology();

#if defined(_WIN32)
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (::GlobalMemoryStatusEx(&mem) != FALSE) {
        inv.total_ram_mb = mem.ullTotalPhys / (1024ULL * 1024ULL);
    }
    enum_dxgi_gpus(inv.gpus);
    enum_mf_cameras(inv.cameras);
    enum_wasapi_audio_inputs(inv.audio_inputs);
    probe_npu_hardware(inv.npu);
#else
    inv.npu.probed = true;
    inv.npu.available = false;
    inv.npu.ffi_status = AETHER_OK;
    inv.npu.device_name = "None";
    inv.npu.note = "Non-Windows platform";
#endif

    AETHER_LOG_INFO("hw", "CPU: %s (%uP/%uE, %uT), RAM: %llu MiB, GPUs: %zu, Cameras: %zu, Mics: %zu, NPU: %s (%s)",
                    inv.cpu.brand_string.c_str(), inv.cpu.p_core_count, inv.cpu.e_core_count,
                    inv.cpu.logical_processors, static_cast<unsigned long long>(inv.total_ram_mb), inv.gpus.size(),
                    inv.cameras.size(), inv.audio_inputs.size(), inv.npu.device_name.c_str(), inv.npu.note.c_str());
    return inv;
}

std::vector<AudioInputDeviceInfo> enumerate_audio_input_devices() {
    std::vector<AudioInputDeviceInfo> out;
#if defined(_WIN32)
    enum_wasapi_audio_inputs(out);
#endif
    return out;
}

} // namespace aether
