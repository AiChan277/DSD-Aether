#include "rhi/d3d12_device.hpp"

#include "core/log.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

#include <algorithm>
#include <cstring>
#include <memory>

namespace aether::rhi {

namespace {

template <typename T>
void safe_release(T*& ptr) noexcept {
    if (ptr != nullptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

[[nodiscard]] Expected<void> fail_d3d12(ErrorCode code, const char* msg) noexcept {
    return make_unexpected(Error::make(code, msg));
}

#if defined(_WIN32)
[[nodiscard]] std::string wide_to_utf8(const wchar_t* wstr) {
    if (wstr == nullptr || wstr[0] == L'\0') {
        return "Unknown GPU";
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return "Unknown GPU";
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

[[nodiscard]] uint32_t score_d3d12_adapter(const D3D12AdapterDescriptor& d) noexcept {
    if (d.is_software) {
        return 1u;
    }
    uint32_t score = d.is_discrete ? 1000u : (d.is_uma ? 250u : 100u);
    if (d.supports_dxr_1_1) {
        score += 500u;
    }
    if (d.supports_sm_6_6) {
        score += 200u;
    }
    if (d.supports_fp16) {
        score += 50u;
    }
    score += static_cast<uint32_t>(std::min<uint64_t>(d.dedicated_vram_mb / 512ULL, 200ULL));
    if (d.vendor_id == 0x10DEu) {
        score += 40u;
    } else if (d.vendor_id == 0x1002u) {
        score += 30u;
    } else if (d.vendor_id == 0x8086u) {
        score += 20u;
    }
    return score;
}

[[nodiscard]] bool probe_adapter_descriptor(IDXGIAdapter4* adapter, uint32_t idx,
                                            D3D12AdapterDescriptor& out_desc) noexcept {
    if (adapter == nullptr) {
        return false;
    }
    DXGI_ADAPTER_DESC3 desc{};
    if (FAILED(adapter->GetDesc3(&desc))) {
        return false;
    }

    ID3D12Device* probe_raw = nullptr;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&probe_raw)))) {
        return false;
    }

    out_desc = D3D12AdapterDescriptor{};
    out_desc.device_name = wide_to_utf8(desc.Description);
    out_desc.vendor_id = desc.VendorId;
    out_desc.device_id = desc.DeviceId;
    out_desc.adapter_index = idx;
    std::memcpy(out_desc.luid.data(), &desc.AdapterLuid, sizeof(desc.AdapterLuid));
    out_desc.luid_valid = true;
    out_desc.dedicated_vram_mb = static_cast<uint64_t>(desc.DedicatedVideoMemory) / (1024ULL * 1024ULL);
    out_desc.shared_system_mb = static_cast<uint64_t>(desc.SharedSystemMemory) / (1024ULL * 1024ULL);
    out_desc.is_software = (desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0;

    D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
    if (SUCCEEDED(probe_raw->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch)))) {
        out_desc.is_uma = (arch.UMA == TRUE);
        out_desc.cache_coherent_uma = (arch.CacheCoherentUMA == TRUE);
    }
    out_desc.is_discrete = !out_desc.is_software && !out_desc.is_uma && (out_desc.dedicated_vram_mb >= 512ULL);
    out_desc.is_intel_igpu = !out_desc.is_software && (out_desc.vendor_id == 0x8086u) && out_desc.is_uma;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
    if (SUCCEEDED(probe_raw->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5)))) {
        out_desc.supports_dxr_1_1 = (opts5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 opts4{};
    if (SUCCEEDED(probe_raw->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &opts4, sizeof(opts4)))) {
        out_desc.supports_fp16 = (opts4.Native16BitShaderOpsSupported == TRUE);
    }
    D3D12_FEATURE_DATA_SHADER_MODEL sm{};
    sm.HighestShaderModel = D3D_SHADER_MODEL_6_6;
    if (SUCCEEDED(probe_raw->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
        out_desc.supports_sm_6_6 = (sm.HighestShaderModel >= D3D_SHADER_MODEL_6_6);
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT bgra_sup{DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_FORMAT_SUPPORT1_NONE,
                                             D3D12_FORMAT_SUPPORT2_NONE};
    if (SUCCEEDED(probe_raw->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &bgra_sup, sizeof(bgra_sup)))) {
        out_desc.supports_bgra8_uav = (bgra_sup.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0;
    }

    out_desc.estimated_bandwidth_gbps =
        out_desc.is_uma ? 80u : (out_desc.is_discrete ? (out_desc.dedicated_vram_mb >= 8192u ? 320u : 224u) : 64u);
    // Safe tier auto-selection: Intel iGPU / UMA / non-DXR adapters default to Mode 0 (Traditional Lighting).
    out_desc.recommended_lighting_mode =
        (out_desc.is_uma || out_desc.is_intel_igpu || out_desc.is_software || !out_desc.supports_dxr_1_1) ? uint8_t{0}
                                                                                                         : uint8_t{1};
    out_desc.score = score_d3d12_adapter(out_desc);

    probe_raw->Release();
    return true;
}

[[nodiscard]] bool is_depth_dxgi_format(DXGI_FORMAT fmt) noexcept {
    return fmt == DXGI_FORMAT_D32_FLOAT || fmt == DXGI_FORMAT_D24_UNORM_S8_UINT || fmt == DXGI_FORMAT_D16_UNORM;
}

[[nodiscard]] DXGI_FORMAT typeless_for_dsv(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    default:
        return fmt;
    }
}

[[nodiscard]] DXGI_FORMAT srv_format_for_dsv(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default:
        return fmt;
    }
}

[[nodiscard]] bool is_bc7_dxgi(DXGI_FORMAT fmt) noexcept {
    return fmt == DXGI_FORMAT_BC7_UNORM || fmt == DXGI_FORMAT_BC7_UNORM_SRGB || fmt == DXGI_FORMAT_BC7_TYPELESS;
}

[[nodiscard]] D3D12_HEAP_PROPERTIES heap_props_for(D3D12BufferHeap heap) noexcept {
    D3D12_HEAP_PROPERTIES hp{};
    switch (heap) {
    case D3D12BufferHeap::UploadCpu:
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        break;
    case D3D12BufferHeap::ReadbackCpu:
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        break;
    default:
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        break;
    }
    return hp;
}

[[nodiscard]] bool luid_matches(const std::array<uint8_t, 8>& a, const std::array<uint8_t, 8>& b) noexcept {
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}
#endif

} // namespace

D3D12Device::~D3D12Device() {
    destroy();
}

Expected<std::vector<D3D12AdapterDescriptor>> D3D12Device::enumerate_adapters() {
#if !defined(_WIN32)
    return fail_d3d12(ErrorCode::Unsupported, "D3D12 is Windows-only");
#else
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        return make_unexpected(Error::make(ErrorCode::VulkanError, "CreateDXGIFactory2 failed"));
    }

    std::vector<D3D12AdapterDescriptor> out;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* raw_ad = nullptr;
        if (factory->EnumAdapters1(i, &raw_ad) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        IDXGIAdapter4* ad4 = nullptr;
        if (SUCCEEDED(raw_ad->QueryInterface(IID_PPV_ARGS(&ad4)))) {
            D3D12AdapterDescriptor d{};
            if (probe_adapter_descriptor(ad4, i, d)) {
                out.push_back(std::move(d));
            }
            ad4->Release();
        }
        raw_ad->Release();
    }

    std::sort(out.begin(), out.end(),
              [](const D3D12AdapterDescriptor& a, const D3D12AdapterDescriptor& b) { return a.score > b.score; });

    factory->Release();
    return out;
#endif
}

Expected<D3D12Device*> D3D12Device::create(const D3D12DeviceConfig& cfg) {
#if !defined(_WIN32)
    (void)cfg;
    return fail_d3d12(ErrorCode::Unsupported, "D3D12 is Windows-only");
#else
    auto dev = std::unique_ptr<D3D12Device>(new D3D12Device());
    if (auto rc = dev->init_internal(cfg); !rc) {
        dev->destroy();
        return make_unexpected(rc.error());
    }
    return dev.release();
#endif
}

Expected<void> D3D12Device::init_internal(const D3D12DeviceConfig& cfg) {
#if !defined(_WIN32)
    (void)cfg;
    return fail_d3d12(ErrorCode::Unsupported, "D3D12 is Windows-only");
#else
    UINT dxgi_flags = 0;
#if !defined(NDEBUG)
    if (cfg.enable_debug_layer) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            dbg->Release();
            debug_layer_active_ = true;
            dxgi_flags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    if (FAILED(CreateDXGIFactory2(dxgi_flags, IID_PPV_ARGS(&factory_)))) {
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) {
            return fail_d3d12(ErrorCode::VulkanError, "CreateDXGIFactory2 failed");
        }
    }

    IDXGIAdapter4* best_ad = nullptr;
    D3D12AdapterDescriptor best_desc{};
    uint32_t best_effective_score = 0;

    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* raw = nullptr;
        if (factory_->EnumAdapters1(i, &raw) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        IDXGIAdapter4* ad4 = nullptr;
        if (SUCCEEDED(raw->QueryInterface(IID_PPV_ARGS(&ad4)))) {
            D3D12AdapterDescriptor d{};
            if (probe_adapter_descriptor(ad4, i, d)) {
                if (d.is_software && !cfg.allow_warp_fallback) {
                    ad4->Release();
                    raw->Release();
                    continue;
                }
                uint32_t eff = d.score;
                if (cfg.use_preferred_luid && d.luid_valid && luid_matches(d.luid, cfg.preferred_luid)) {
                    eff += 100000u;
                } else if (cfg.prefer_integrated && d.is_uma && !d.is_software) {
                    eff += 50000u;
                }
                if (best_ad == nullptr || eff > best_effective_score) {
                    safe_release(best_ad);
                    best_ad = ad4;
                    ad4 = nullptr;
                    best_desc = d;
                    best_effective_score = eff;
                }
            }
            safe_release(ad4);
        }
        raw->Release();
    }

    if (best_ad == nullptr) {
        return fail_d3d12(ErrorCode::Unsupported, "No suitable D3D12 adapter found");
    }

    adapter_ = best_ad;
    gpu_info_ = best_desc;

    ID3D12Device* dev_raw = nullptr;
    if (FAILED(D3D12CreateDevice(adapter_, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev_raw)))) {
        return fail_d3d12(ErrorCode::VulkanError, "D3D12CreateDevice failed");
    }
    if (FAILED(dev_raw->QueryInterface(IID_PPV_ARGS(&device_)))) {
        dev_raw->Release();
        return fail_d3d12(ErrorCode::Unsupported, "ID3D12Device5 interface unavailable");
    }
    dev_raw->Release();

    if (debug_layer_active_) {
        ID3D12InfoQueue* iq = nullptr;
        if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&iq)))) {
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            iq->Release();
        }
    }

    if (auto r = create_command_objects(); !r) {
        return r;
    }
    if (auto r = create_descriptor_heaps_and_samplers(); !r) {
        return r;
    }
    if (auto r = create_root_signatures(); !r) {
        return r;
    }
    if (cfg.enable_d3d11on12) {
        if (auto r = create_d3d11on12_bridge(); !r) {
            AETHER_LOG_WARN("rhi", "D3D11On12 bridge unavailable: %s", r.error().message.data());
        }
    }

    AETHER_LOG_INFO("rhi",
                    "D3D12 initialized on '%s' (UMA=%d IntelIGPU=%d DXR1.1=%d FP16=%d SM6.6=%d rec_mode=%u d3d11on12=%d)",
                    gpu_info_.device_name.c_str(), gpu_info_.is_uma ? 1 : 0, gpu_info_.is_intel_igpu ? 1 : 0,
                    gpu_info_.supports_dxr_1_1 ? 1 : 0, gpu_info_.supports_fp16 ? 1 : 0,
                    gpu_info_.supports_sm_6_6 ? 1 : 0, static_cast<unsigned>(gpu_info_.recommended_lighting_mode),
                    d3d11on12_device_ != nullptr ? 1 : 0);
    return {};
#endif
}

Expected<void> D3D12Device::create_command_objects() {
#if !defined(_WIN32)
    return fail_d3d12(ErrorCode::Unsupported, "D3D12 is Windows-only");
#else
    auto make_queue = [&](D3D12_COMMAND_LIST_TYPE type, ID3D12CommandQueue*& out_q) -> Expected<void> {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = type;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&out_q)))) {
            return fail_d3d12(ErrorCode::VulkanError, "CreateCommandQueue failed");
        }
        return {};
    };
    if (auto r = make_queue(D3D12_COMMAND_LIST_TYPE_DIRECT, direct_queue_); !r) {
        return r;
    }
    if (auto r = make_queue(D3D12_COMMAND_LIST_TYPE_COMPUTE, compute_queue_); !r) {
        return r;
    }
    if (auto r = make_queue(D3D12_COMMAND_LIST_TYPE_COPY, copy_queue_); !r) {
        return r;
    }

    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&direct_fence_)))) {
        return fail_d3d12(ErrorCode::VulkanError, "CreateFence(direct) failed");
    }
    direct_fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (direct_fence_event_ == nullptr) {
        return fail_d3d12(ErrorCode::VulkanError, "CreateEventW(direct_fence) failed");
    }

    for (uint32_t i = 0; i < kD3D12FrameCount; ++i) {
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame_allocators_[i])))) {
            return fail_d3d12(ErrorCode::VulkanError, "CreateCommandAllocator(frame) failed");
        }
    }
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame_allocators_[0], nullptr,
                                          IID_PPV_ARGS(&direct_cmd_)))) {
        return fail_d3d12(ErrorCode::VulkanError, "CreateCommandList(direct) failed");
    }
    direct_cmd_->Close();

    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&imm_alloc_)))) {
        return fail_d3d12(ErrorCode::VulkanError, "CreateCommandAllocator(imm) failed");
    }
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, imm_alloc_, nullptr,
                                          IID_PPV_ARGS(&imm_list_)))) {
        return fail_d3d12(ErrorCode::VulkanError, "CreateCommandList(imm) failed");
    }
    imm_list_->Close();
    return {};
#endif
}

Expected<void> D3D12Device::create_descriptor_heaps_and_samplers() {
#if !defined(_WIN32)
    return fail_d3d12(ErrorCode::Unsupported, "D3D12 is Windows-only");
#else
    auto make_heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool shader_vis,
                         ID3D12DescriptorHeap*& out_h) -> Expected<void> {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = type;
        d.NumDescriptors = count;
        d.Flags = shader_vis ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device_->CreateDescriptorHeap(&d, IID_PPV_ARGS(&out_h)))) {
            return fail_d3d12(ErrorCode::VulkanError, "CreateDescriptorHeap failed");
        }
        return {};
    };
    if (auto r = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kCbvSrvUavHeapCapacity, true, cbv_srv_uav_heap_); !r) {
        return r;
    }
    if (auto r = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kCbvSrvUavHeapCapacity, false,
                           cbv_srv_uav_cpu_staging_);
        !r) {
        return r;
    }
    if (auto r = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapCapacity, true, sampler_heap_); !r) {
        return r;
    }
    if (auto r = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapCapacity, false, rtv_heap_); !r) {
        return r;
    }
    if (auto r = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapCapacity, false, dsv_heap_); !r) {
        return r;
    }

    cbv_srv_uav_inc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    sampler_inc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    rtv_inc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsv_inc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // Slot 0 on CBV_SRV_UAV reserved for null safe SRV.
    srv_pool_.reset(1u);
    rtv_pool_.reset(0u);
    dsv_pool_.reset(0u);

    // Populate samplers matching aether_common.slang indices:
    // 0=LinearClamp, 1=NearestClamp, 2=LinearRepeat, 3=NearestRepeat
    D3D12_SAMPLER_DESC samp_descs[4]{};
    for (int i = 0; i < 4; ++i) {
        samp_descs[i].MinLOD = 0.0f;
        samp_descs[i].MaxLOD = D3D12_FLOAT32_MAX;
        samp_descs[i].MaxAnisotropy = 1u;
        samp_descs[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    }
    samp_descs[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp_descs[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp_descs[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp_descs[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    samp_descs[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp_descs[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp_descs[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp_descs[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    samp_descs[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp_descs[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp_descs[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp_descs[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

    samp_descs[3].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp_descs[3].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp_descs[3].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp_descs[3].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

    const D3D12_CPU_DESCRIPTOR_HANDLE samp_base = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 4u; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE dst{samp_base.ptr + static_cast<SIZE_T>(i) * sampler_inc_};
        device_->CreateSampler(&samp_descs[i], dst);
    }

    // Populate slot 0 with a valid null ByteAddressBuffer SRV so out-of-range fallback never faults.
    auto dummy_res = create_buffer(64u, D3D12BufferUsage::ShaderResource, D3D12BufferHeap::DefaultGpu, false, false);
    if (!dummy_res) {
        return make_unexpected(dummy_res.error());
    }
    null_dummy_buf_ = dummy_res.value();
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv{};
    null_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    null_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null_srv.Format = DXGI_FORMAT_R32_TYPELESS;
    null_srv.Buffer.FirstElement = 0;
    null_srv.Buffer.NumElements = 16;
    null_srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device_->CreateShaderResourceView(null_dummy_buf_.resource, &null_srv,
                                      cbv_srv_uav_heap_->GetCPUDescriptorHandleForHeapStart());
    return {};
#endif
}

Expected<void> D3D12Device::create_root_signatures() {
#if !defined(_WIN32)
    return fail_d3d12(ErrorCode::Unsupported, "D3D12 is Windows-only");
#else
    D3D12_ROOT_PARAMETER1 params[2]{};
    // Param 0: 16 DWORD root constants matching GpuPushConstants (64 bytes) at b0, space0.
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Param 1 (RT variant only): TLAS Root SRV at t0, space1.
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.RegisterSpace = 1;
    params[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    const D3D12_ROOT_SIGNATURE_FLAGS flags =
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    auto compile_rs = [&](UINT num_params, ID3D12RootSignature*& out_rs) -> Expected<void> {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
        rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rsd.Desc_1_1.NumParameters = num_params;
        rsd.Desc_1_1.pParameters = params;
        rsd.Desc_1_1.Flags = flags;

        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(D3D12SerializeVersionedRootSignature(&rsd, &blob, &err))) {
            if (err != nullptr) {
                AETHER_LOG_ERROR("rhi", "RootSignature serialize failed: %s",
                                 static_cast<const char*>(err->GetBufferPointer()));
                err->Release();
            }
            safe_release(blob);
            return fail_d3d12(ErrorCode::VulkanError, "D3D12SerializeVersionedRootSignature failed");
        }
        safe_release(err);
        const HRESULT hr =
            device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out_rs));
        blob->Release();
        if (FAILED(hr)) {
            return fail_d3d12(ErrorCode::VulkanError, "CreateRootSignature failed");
        }
        return {};
    };

    if (auto r = compile_rs(1u, bindless_root_sig_); !r) {
        return r;
    }
    if (auto r = compile_rs(2u, rt_bindless_root_sig_); !r) {
        return r;
    }
    return {};
#endif
}

Expected<void> D3D12Device::create_d3d11on12_bridge() {
#if !defined(_WIN32)
    return fail_d3d12(ErrorCode::Unsupported, "D3D11On12 is Windows-only");
#else
    if (device_ == nullptr || direct_queue_ == nullptr) {
        return fail_d3d12(ErrorCode::InvalidArg, "D3D12Device not ready for D3D11On12");
    }
    IUnknown* queues[1] = {direct_queue_};
    const D3D_FEATURE_LEVEL fls[1] = {D3D_FEATURE_LEVEL_11_1};
    const HRESULT hr = D3D11On12CreateDevice(device_, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 1, queues, 1, 0,
                                             &d3d11_device_, &d3d11_context_, nullptr);
    if (FAILED(hr) || d3d11_device_ == nullptr) {
        return fail_d3d12(ErrorCode::Unsupported, "D3D11On12CreateDevice failed");
    }
    if (FAILED(d3d11_device_->QueryInterface(IID_PPV_ARGS(&d3d11on12_device_)))) {
        safe_release(d3d11_context_);
        safe_release(d3d11_device_);
        return fail_d3d12(ErrorCode::Unsupported, "QueryInterface(ID3D11On12Device) failed");
    }
    return {};
#endif
}

void D3D12Device::destroy() noexcept {
#if defined(_WIN32)
    wait_idle();
    destroy_buffer(null_dummy_buf_);

    safe_release(d3d11on12_device_);
    safe_release(d3d11_context_);
    safe_release(d3d11_device_);

    safe_release(rt_bindless_root_sig_);
    safe_release(bindless_root_sig_);

    safe_release(imm_list_);
    safe_release(imm_alloc_);
    safe_release(direct_cmd_);
    for (auto*& alloc : frame_allocators_) {
        safe_release(alloc);
    }

    safe_release(dsv_heap_);
    safe_release(rtv_heap_);
    safe_release(sampler_heap_);
    safe_release(cbv_srv_uav_cpu_staging_);
    safe_release(cbv_srv_uav_heap_);

    if (direct_fence_event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(direct_fence_event_));
        direct_fence_event_ = nullptr;
    }
    safe_release(direct_fence_);
    safe_release(copy_queue_);
    safe_release(compute_queue_);
    safe_release(direct_queue_);
    safe_release(device_);
    safe_release(adapter_);
    safe_release(factory_);
#endif
    next_direct_fence_ = 1;
    frame_cursor_ = 0;
    frame_fence_values_.fill(0);
    debug_layer_active_ = false;
    device_lost_.store(false, std::memory_order_release);
}

Expected<ID3D12GraphicsCommandList4*> D3D12Device::begin_direct_frame(uint32_t& out_frame_slot) noexcept {
#if !defined(_WIN32)
    (void)out_frame_slot;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "D3D12 is Windows-only"));
#else
    if (device_ == nullptr || direct_cmd_ == nullptr) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "D3D12Device not initialized"));
    }
    out_frame_slot = frame_cursor_ % kD3D12FrameCount;
    const uint64_t pending_fence = frame_fence_values_[out_frame_slot];
    if (pending_fence > 0 && !wait_direct_fence(pending_fence, 5000u)) {
        return make_unexpected(Error::make(ErrorCode::Timeout, "D3D12 frame fence wait timed out"));
    }
    ID3D12CommandAllocator* alloc = frame_allocators_[out_frame_slot];
    if (FAILED(alloc->Reset())) {
        return make_unexpected(Error::make(ErrorCode::VulkanError, "ID3D12CommandAllocator::Reset failed"));
    }
    if (FAILED(direct_cmd_->Reset(alloc, nullptr))) {
        return make_unexpected(Error::make(ErrorCode::VulkanError, "ID3D12GraphicsCommandList4::Reset failed"));
    }
    return direct_cmd_;
#endif
}

Expected<uint64_t> D3D12Device::submit_direct_frame(uint32_t frame_slot) noexcept {
#if !defined(_WIN32)
    (void)frame_slot;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "D3D12 is Windows-only"));
#else
    if (direct_queue_ == nullptr || direct_fence_ == nullptr || direct_cmd_ == nullptr) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "D3D12Device not initialized"));
    }
    if (FAILED(direct_cmd_->Close())) {
        return make_unexpected(Error::make(ErrorCode::VulkanError, "CommandList Close failed"));
    }
    ID3D12CommandList* lists[1] = {direct_cmd_};
    direct_queue_->ExecuteCommandLists(1, lists);
    const uint64_t sig = next_direct_fence_++;
    if (FAILED(direct_queue_->Signal(direct_fence_, sig))) {
        if (device_ != nullptr && device_->GetDeviceRemovedReason() != S_OK) {
            device_lost_.store(true, std::memory_order_release);
        }
        return make_unexpected(Error::make(ErrorCode::VulkanError, "CommandQueue Signal failed"));
    }
    frame_fence_values_[frame_slot % kD3D12FrameCount] = sig;
    ++frame_cursor_;
    return sig;
#endif
}

bool D3D12Device::wait_direct_fence(uint64_t fence_val, uint32_t timeout_ms) noexcept {
#if !defined(_WIN32)
    (void)fence_val;
    (void)timeout_ms;
    return false;
#else
    if (direct_fence_ == nullptr || fence_val == 0) {
        return true;
    }
    if (direct_fence_->GetCompletedValue() >= fence_val) {
        return true;
    }
    if (FAILED(direct_fence_->SetEventOnCompletion(fence_val, static_cast<HANDLE>(direct_fence_event_)))) {
        return false;
    }
    return WaitForSingleObject(static_cast<HANDLE>(direct_fence_event_), timeout_ms) == WAIT_OBJECT_0;
#endif
}

void D3D12Device::wait_idle() noexcept {
#if defined(_WIN32)
    if (direct_queue_ == nullptr || direct_fence_ == nullptr) {
        return;
    }
    const uint64_t sig = next_direct_fence_++;
    if (SUCCEEDED(direct_queue_->Signal(direct_fence_, sig))) {
        (void)wait_direct_fence(sig, 5000u);
    }
#endif
}

Expected<void> D3D12Device::reset_immediate_list() noexcept {
#if !defined(_WIN32)
    return fail_d3d12(ErrorCode::Unsupported, "D3D12 is Windows-only");
#else
    if (FAILED(imm_alloc_->Reset()) || FAILED(imm_list_->Reset(imm_alloc_, nullptr))) {
        return fail_d3d12(ErrorCode::VulkanError, "Immediate command list reset failed");
    }
    return {};
#endif
}

Expected<void> D3D12Device::submit_and_wait_immediate() noexcept {
#if !defined(_WIN32)
    return fail_d3d12(ErrorCode::Unsupported, "D3D12 is Windows-only");
#else
    if (FAILED(imm_list_->Close())) {
        return fail_d3d12(ErrorCode::VulkanError, "Immediate command list Close failed");
    }
    ID3D12CommandList* lists[1] = {imm_list_};
    direct_queue_->ExecuteCommandLists(1, lists);
    const uint64_t sig = next_direct_fence_++;
    if (FAILED(direct_queue_->Signal(direct_fence_, sig))) {
        return fail_d3d12(ErrorCode::VulkanError, "Immediate fence Signal failed");
    }
    if (!wait_direct_fence(sig, 5000u)) {
        return fail_d3d12(ErrorCode::Timeout, "Immediate fence wait timed out");
    }
    return {};
#endif
}

Expected<D3D12Buffer> D3D12Device::create_buffer(uint64_t size, D3D12BufferUsage usage, D3D12BufferHeap heap,
                                                 bool create_srv, bool create_uav) noexcept {
#if !defined(_WIN32)
    (void)size;
    (void)usage;
    (void)heap;
    (void)create_srv;
    (void)create_uav;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "D3D12 is Windows-only"));
#else
    if (device_ == nullptr || size == 0) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Invalid D3D12 buffer args"));
    }
    const uint64_t aligned_size = (size + 15ULL) & ~15ULL;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = aligned_size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    const bool req_uav = (heap == D3D12BufferHeap::DefaultGpu) &&
                         (has_flag(usage, D3D12BufferUsage::UnorderedAccess) ||
                          has_flag(usage, D3D12BufferUsage::AccelStructStorage));
    if (req_uav) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    D3D12_RESOURCE_STATES init_state = D3D12_RESOURCE_STATE_COMMON;
    if (heap == D3D12BufferHeap::UploadCpu) {
        init_state = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else if (heap == D3D12BufferHeap::ReadbackCpu) {
        init_state = D3D12_RESOURCE_STATE_COPY_DEST;
    } else if (has_flag(usage, D3D12BufferUsage::AccelStructStorage)) {
        init_state = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    }

    const D3D12_HEAP_PROPERTIES hp = heap_props_for(heap);
    D3D12Buffer out{};
    out.size = aligned_size;
    out.heap_kind = heap;
    out.current_state = static_cast<int>(init_state);
    out.is_uma_zero_copy = gpu_info_.is_uma && (heap != D3D12BufferHeap::DefaultGpu);

    if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr,
                                                IID_PPV_ARGS(&out.resource)))) {
        return make_unexpected(Error::make(ErrorCode::OutOfMemory, "D3D12 buffer allocation failed"));
    }
    out.gpu_va = out.resource->GetGPUVirtualAddress();

    if (heap == D3D12BufferHeap::UploadCpu || heap == D3D12BufferHeap::ReadbackCpu) {
        D3D12_RANGE read_rng{0, (heap == D3D12BufferHeap::ReadbackCpu) ? static_cast<SIZE_T>(aligned_size) : 0};
        if (FAILED(out.resource->Map(0, &read_rng, &out.mapped_ptr))) {
            destroy_buffer(out);
            return make_unexpected(Error::make(ErrorCode::VulkanError, "D3D12 buffer Map failed"));
        }
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE heap_start = cbv_srv_uav_heap_->GetCPUDescriptorHandleForHeapStart();
    if (create_srv && heap != D3D12BufferHeap::ReadbackCpu) {
        const uint32_t slot = srv_pool_.allocate();
        if (slot == kD3D12InvalidDescIndex) {
            destroy_buffer(out);
            return make_unexpected(Error::make(ErrorCode::OutOfMemory, "CBV_SRV_UAV descriptor heap exhausted"));
        }
        out.srv_index = slot;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = static_cast<UINT>(aligned_size / 4ULL);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE dst{heap_start.ptr + static_cast<SIZE_T>(slot) * cbv_srv_uav_inc_};
        device_->CreateShaderResourceView(out.resource, &srv, dst);
    }

    if (create_uav && req_uav) {
        const uint32_t slot = srv_pool_.allocate();
        if (slot == kD3D12InvalidDescIndex) {
            destroy_buffer(out);
            return make_unexpected(Error::make(ErrorCode::OutOfMemory, "CBV_SRV_UAV descriptor heap exhausted"));
        }
        out.uav_index = slot;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.Buffer.FirstElement = 0;
        uav.Buffer.NumElements = static_cast<UINT>(aligned_size / 4ULL);
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE dst{heap_start.ptr + static_cast<SIZE_T>(slot) * cbv_srv_uav_inc_};
        device_->CreateUnorderedAccessView(out.resource, nullptr, &uav, dst);
    }

    return out;
#endif
}

void D3D12Device::destroy_buffer(D3D12Buffer& buf) noexcept {
#if defined(_WIN32)
    if (buf.resource != nullptr && buf.mapped_ptr != nullptr) {
        D3D12_RANGE zero{0, 0};
        buf.resource->Unmap(0, &zero);
        buf.mapped_ptr = nullptr;
    }
    safe_release(buf.resource);
#endif
    if (buf.srv_index != kD3D12InvalidDescIndex) {
        srv_pool_.free(buf.srv_index);
    }
    if (buf.uav_index != kD3D12InvalidDescIndex) {
        srv_pool_.free(buf.uav_index);
    }
    buf = D3D12Buffer{};
}

Expected<D3D12Texture> D3D12Device::create_texture_2d(uint32_t width, uint32_t height, int dxgi_format, bool allow_rtv,
                                                      bool allow_dsv, bool allow_uav, bool shared_nt, bool wrap_d3d11,
                                                      const float* clear_rgba) noexcept {
#if !defined(_WIN32)
    (void)width;
    (void)height;
    (void)dxgi_format;
    (void)allow_rtv;
    (void)allow_dsv;
    (void)allow_uav;
    (void)shared_nt;
    (void)wrap_d3d11;
    (void)clear_rgba;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "D3D12 is Windows-only"));
#else
    if (device_ == nullptr || width == 0 || height == 0) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Invalid D3D12 texture args"));
    }

    const auto fmt = static_cast<DXGI_FORMAT>(dxgi_format);
    const bool is_dsv_fmt = allow_dsv && is_depth_dxgi_format(fmt);

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = is_dsv_fmt ? typeless_for_dsv(fmt) : fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (allow_rtv) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (allow_dsv) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    if (allow_uav) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (shared_nt) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    }

    D3D12_CLEAR_VALUE cv{};
    const D3D12_CLEAR_VALUE* cv_ptr = nullptr;
    if (allow_dsv) {
        cv.Format = fmt;
        cv.DepthStencil.Depth = 1.0f;
        cv.DepthStencil.Stencil = 0;
        cv_ptr = &cv;
    } else if (allow_rtv) {
        cv.Format = fmt;
        if (clear_rgba != nullptr) {
            cv.Color[0] = clear_rgba[0];
            cv.Color[1] = clear_rgba[1];
            cv.Color[2] = clear_rgba[2];
            cv.Color[3] = clear_rgba[3];
        }
        cv_ptr = &cv;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_HEAP_FLAGS hflags = shared_nt ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE;
    const D3D12_RESOURCE_STATES init_state =
        allow_dsv ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COMMON;

    D3D12Texture out{};
    out.width = width;
    out.height = height;
    out.format = dxgi_format;
    out.current_state = static_cast<int>(init_state);

    if (FAILED(device_->CreateCommittedResource(&hp, hflags, &rd, init_state, cv_ptr, IID_PPV_ARGS(&out.resource)))) {
        return make_unexpected(Error::make(ErrorCode::OutOfMemory, "D3D12 CreateCommittedResource(Tex2D) failed"));
    }

    // SRV
    {
        const uint32_t slot = srv_pool_.allocate();
        if (slot == kD3D12InvalidDescIndex) {
            destroy_texture(out);
            return make_unexpected(Error::make(ErrorCode::OutOfMemory, "SRV descriptor heap exhausted"));
        }
        out.srv_index = slot;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = is_dsv_fmt ? srv_format_for_dsv(fmt) : fmt;
        srv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE dst{cbv_srv_uav_heap_->GetCPUDescriptorHandleForHeapStart().ptr +
                                        static_cast<SIZE_T>(slot) * cbv_srv_uav_inc_};
        device_->CreateShaderResourceView(out.resource, &srv, dst);
    }

    // UAV
    if (allow_uav) {
        const uint32_t slot = srv_pool_.allocate();
        if (slot == kD3D12InvalidDescIndex) {
            destroy_texture(out);
            return make_unexpected(Error::make(ErrorCode::OutOfMemory, "UAV descriptor heap exhausted"));
        }
        out.uav_index = slot;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = fmt;
        D3D12_CPU_DESCRIPTOR_HANDLE dst{cbv_srv_uav_heap_->GetCPUDescriptorHandleForHeapStart().ptr +
                                        static_cast<SIZE_T>(slot) * cbv_srv_uav_inc_};
        device_->CreateUnorderedAccessView(out.resource, nullptr, &uav, dst);
    }

    // RTV
    if (allow_rtv) {
        const uint32_t slot = rtv_pool_.allocate();
        if (slot == kD3D12InvalidDescIndex) {
            destroy_texture(out);
            return make_unexpected(Error::make(ErrorCode::OutOfMemory, "RTV descriptor heap exhausted"));
        }
        out.rtv_index = slot;
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtv.Format = fmt;
        D3D12_CPU_DESCRIPTOR_HANDLE dst{rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr +
                                        static_cast<SIZE_T>(slot) * rtv_inc_};
        device_->CreateRenderTargetView(out.resource, &rtv, dst);
    }

    // DSV
    if (allow_dsv) {
        const uint32_t slot = dsv_pool_.allocate();
        if (slot == kD3D12InvalidDescIndex) {
            destroy_texture(out);
            return make_unexpected(Error::make(ErrorCode::OutOfMemory, "DSV descriptor heap exhausted"));
        }
        out.dsv_index = slot;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Format = fmt;
        D3D12_CPU_DESCRIPTOR_HANDLE dst{dsv_heap_->GetCPUDescriptorHandleForHeapStart().ptr +
                                        static_cast<SIZE_T>(slot) * dsv_inc_};
        device_->CreateDepthStencilView(out.resource, &dsv, dst);
    }

    if (shared_nt) {
        HANDLE h = nullptr;
        if (SUCCEEDED(device_->CreateSharedHandle(out.resource, nullptr, GENERIC_ALL, nullptr, &h))) {
            out.shared_nt_handle = static_cast<void*>(h);
        }
    }

    if (wrap_d3d11 && d3d11on12_device_ != nullptr) {
        D3D11_RESOURCE_FLAGS rf{};
        rf.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (allow_rtv) {
            rf.BindFlags |= D3D11_BIND_RENDER_TARGET;
        }
        const HRESULT hr =
            d3d11on12_device_->CreateWrappedResource(out.resource, &rf, D3D12_RESOURCE_STATE_COMMON,
                                                     D3D12_RESOURCE_STATE_COMMON, IID_PPV_ARGS(&out.d3d11_wrapped));
        if (SUCCEEDED(hr) && out.d3d11_wrapped != nullptr) {
            (void)out.d3d11_wrapped->QueryInterface(IID_PPV_ARGS(&out.d3d11_tex));
        }
    }

    return out;
#endif
}

Expected<D3D12Texture> D3D12Device::create_texture_2d_from_data(uint32_t width, uint32_t height, int dxgi_format,
                                                                std::span<const uint8_t> pixels,
                                                                uint32_t bytes_per_pixel_or_block_row) noexcept {
#if !defined(_WIN32)
    (void)width;
    (void)height;
    (void)dxgi_format;
    (void)pixels;
    (void)bytes_per_pixel_or_block_row;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "D3D12 is Windows-only"));
#else
    auto tex_res = create_texture_2d(width, height, dxgi_format, false, false, false, false, false, nullptr);
    if (!tex_res) {
        return tex_res;
    }
    D3D12Texture tex = tex_res.value();

    const auto fmt = static_cast<DXGI_FORMAT>(dxgi_format);
    const bool bc7 = is_bc7_dxgi(fmt);
    const uint32_t src_row_pitch = (bytes_per_pixel_or_block_row > 0u)
                                       ? bytes_per_pixel_or_block_row
                                       : (bc7 ? ((std::max(1u, width) + 3u) / 4u) * 16u : std::max(1u, width) * 4u);
    const uint32_t num_rows = bc7 ? ((std::max(1u, height) + 3u) / 4u) : std::max(1u, height);
    const uint32_t aligned_pitch =
        (src_row_pitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    const uint64_t upload_bytes = static_cast<uint64_t>(aligned_pitch) * num_rows;

    auto stg_res = create_buffer(upload_bytes, D3D12BufferUsage::CopySrc, D3D12BufferHeap::UploadCpu, false, false);
    if (!stg_res) {
        destroy_texture(tex);
        return make_unexpected(stg_res.error());
    }
    D3D12Buffer stg = stg_res.value();
    auto* dst_ptr = static_cast<uint8_t*>(stg.mapped_ptr);
    for (uint32_t r = 0; r < num_rows; ++r) {
        const size_t src_off = static_cast<size_t>(r) * src_row_pitch;
        const size_t copy_len =
            (src_off < pixels.size()) ? std::min<size_t>(src_row_pitch, pixels.size() - src_off) : 0u;
        if (copy_len > 0) {
            std::memcpy(dst_ptr + static_cast<size_t>(r) * aligned_pitch, pixels.data() + src_off, copy_len);
        }
    }

    auto rc = execute_immediate([&](ID3D12GraphicsCommandList4* cmd) {
        cmd_transition_texture(cmd, tex, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = stg.resource;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint.Footprint.Format = fmt;
        src_loc.PlacedFootprint.Footprint.Width = std::max(1u, width);
        src_loc.PlacedFootprint.Footprint.Height = std::max(1u, height);
        src_loc.PlacedFootprint.Footprint.Depth = 1;
        src_loc.PlacedFootprint.Footprint.RowPitch = aligned_pitch;

        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = tex.resource;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = 0;
        cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        cmd_transition_texture(cmd, tex, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    });
    destroy_buffer(stg);
    if (!rc) {
        destroy_texture(tex);
        return make_unexpected(rc.error());
    }
    return tex;
#endif
}

Expected<D3D12Texture> D3D12Device::create_depth_stencil_s8(uint32_t width, uint32_t height) noexcept {
#if !defined(_WIN32)
    (void)width;
    (void)height;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "D3D12 is Windows-only"));
#else
    if (device_ == nullptr || width == 0 || height == 0) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Invalid D3D12 D32S8 texture args"));
    }

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    cv.DepthStencil.Depth = 1.0f;
    cv.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12Texture out{};
    out.width = width;
    out.height = height;
    out.format = static_cast<int>(DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    out.current_state = static_cast<int>(D3D12_RESOURCE_STATE_DEPTH_WRITE);

    if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                IID_PPV_ARGS(&out.resource)))) {
        return make_unexpected(Error::make(ErrorCode::OutOfMemory, "D3D12 CreateCommittedResource(D32S8) failed"));
    }

    // Writable DSV + Read-Only DSV (allows simultaneous depth test + depth/stencil SRV reads in Pass 6/7)
    {
        const uint32_t slot = dsv_pool_.allocate();
        const uint32_t ro_slot = dsv_pool_.allocate();
        if (slot == kD3D12InvalidDescIndex || ro_slot == kD3D12InvalidDescIndex) {
            if (slot != kD3D12InvalidDescIndex) {
                dsv_pool_.free(slot);
            }
            if (ro_slot != kD3D12InvalidDescIndex) {
                dsv_pool_.free(ro_slot);
            }
            destroy_texture(out);
            return make_unexpected(Error::make(ErrorCode::OutOfMemory, "DSV heap exhausted"));
        }
        out.dsv_index = slot;
        out.dsv_readonly_index = ro_slot;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        D3D12_CPU_DESCRIPTOR_HANDLE dst{dsv_heap_->GetCPUDescriptorHandleForHeapStart().ptr +
                                        static_cast<SIZE_T>(slot) * dsv_inc_};
        device_->CreateDepthStencilView(out.resource, &dsv, dst);

        dsv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        D3D12_CPU_DESCRIPTOR_HANDLE dst_ro{dsv_heap_->GetCPUDescriptorHandleForHeapStart().ptr +
                                           static_cast<SIZE_T>(ro_slot) * dsv_inc_};
        device_->CreateDepthStencilView(out.resource, &dsv, dst_ro);
    }

    // Depth SRV (`R32_FLOAT_X8X24_TYPELESS`)
    {
        const uint32_t slot = srv_pool_.allocate();
        if (slot == kD3D12InvalidDescIndex) {
            destroy_texture(out);
            return make_unexpected(Error::make(ErrorCode::OutOfMemory, "SRV heap exhausted"));
        }
        out.srv_index = slot;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        srv.Texture2D.MipLevels = 1;
        srv.Texture2D.PlaneSlice = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE dst{cbv_srv_uav_heap_->GetCPUDescriptorHandleForHeapStart().ptr +
                                        static_cast<SIZE_T>(slot) * cbv_srv_uav_inc_};
        device_->CreateShaderResourceView(out.resource, &srv, dst);
    }

    // Stencil SRV (`X32_TYPELESS_G8X24_UINT`, swizzled component 1 -> .x matching `Texture2D<uint4>.Load().x`)
    {
        const uint32_t slot = srv_pool_.allocate();
        if (slot == kD3D12InvalidDescIndex) {
            destroy_texture(out);
            return make_unexpected(Error::make(ErrorCode::OutOfMemory, "Stencil SRV heap exhausted"));
        }
        out.stencil_srv_index = slot;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1, 4, 4, 5);
        srv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        srv.Texture2D.MipLevels = 1;
        srv.Texture2D.PlaneSlice = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE dst{cbv_srv_uav_heap_->GetCPUDescriptorHandleForHeapStart().ptr +
                                        static_cast<SIZE_T>(slot) * cbv_srv_uav_inc_};
        device_->CreateShaderResourceView(out.resource, &srv, dst);
    }

    return out;
#endif
}

void D3D12Device::destroy_texture(D3D12Texture& tex) noexcept {
#if defined(_WIN32)
    if (d3d11on12_device_ != nullptr && tex.d3d11_wrapped != nullptr) {
        d3d11on12_device_->ReleaseWrappedResources(&tex.d3d11_wrapped, 1);
    }
    safe_release(tex.d3d11_tex);
    safe_release(tex.d3d11_wrapped);
    if (tex.shared_nt_handle != nullptr) {
        CloseHandle(static_cast<HANDLE>(tex.shared_nt_handle));
        tex.shared_nt_handle = nullptr;
    }
    safe_release(tex.resource);
#endif
    if (tex.srv_index != kD3D12InvalidDescIndex) {
        srv_pool_.free(tex.srv_index);
    }
    if (tex.stencil_srv_index != kD3D12InvalidDescIndex) {
        srv_pool_.free(tex.stencil_srv_index);
    }
    if (tex.uav_index != kD3D12InvalidDescIndex) {
        srv_pool_.free(tex.uav_index);
    }
    if (tex.rtv_index != kD3D12InvalidDescIndex) {
        rtv_pool_.free(tex.rtv_index);
    }
    if (tex.dsv_index != kD3D12InvalidDescIndex) {
        dsv_pool_.free(tex.dsv_index);
    }
    if (tex.dsv_readonly_index != kD3D12InvalidDescIndex) {
        dsv_pool_.free(tex.dsv_readonly_index);
    }
    tex = D3D12Texture{};
}

Expected<ID3D12PipelineState*> D3D12Device::create_compute_pso(std::span<const uint8_t> cs_dxil,
                                                               bool uses_rt_tlas) noexcept {
#if !defined(_WIN32)
    (void)cs_dxil;
    (void)uses_rt_tlas;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "D3D12 is Windows-only"));
#else
    if (device_ == nullptr || cs_dxil.empty()) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Empty DXIL compute shader"));
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = uses_rt_tlas ? rt_bindless_root_sig_ : bindless_root_sig_;
    desc.CS = {cs_dxil.data(), cs_dxil.size()};
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(device_->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)))) {
        return make_unexpected(Error::make(ErrorCode::VulkanError, "CreateComputePipelineState failed"));
    }
    return pso;
#endif
}

Expected<ID3D12PipelineState*> D3D12Device::create_graphics_pso(const D3D12GraphicsPipelineDesc& desc) noexcept {
#if !defined(_WIN32)
    (void)desc;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "D3D12 is Windows-only"));
#else
    if (device_ == nullptr || desc.vs_dxil.empty()) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Empty DXIL vertex shader"));
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = bindless_root_sig_;
    p.VS = {desc.vs_dxil.data(), desc.vs_dxil.size()};
    if (!desc.ps_dxil.empty()) {
        p.PS = {desc.ps_dxil.data(), desc.ps_dxil.size()};
    }

    p.BlendState.AlphaToCoverageEnable = FALSE;
    p.BlendState.IndependentBlendEnable = FALSE;
    auto& rt0 = p.BlendState.RenderTarget[0];
    rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (desc.alpha_blend || desc.premultiplied_alpha) {
        rt0.BlendEnable = TRUE;
        rt0.SrcBlend = desc.premultiplied_alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA;
        rt0.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt0.BlendOp = D3D12_BLEND_OP_ADD;
        rt0.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt0.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }

    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = desc.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = static_cast<D3D12_CULL_MODE>(desc.cull_mode);
    p.RasterizerState.FrontCounterClockwise = desc.front_counter_clockwise ? TRUE : FALSE;
    p.RasterizerState.DepthClipEnable = TRUE;

    p.DepthStencilState.DepthEnable = desc.depth_enable ? TRUE : FALSE;
    p.DepthStencilState.DepthWriteMask = desc.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    p.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(desc.depth_func);
    p.DepthStencilState.StencilEnable = desc.stencil_enable ? TRUE : FALSE;
    p.DepthStencilState.StencilReadMask = desc.stencil_read_mask;
    p.DepthStencilState.StencilWriteMask = desc.stencil_write_mask;
    p.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    p.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    p.DepthStencilState.FrontFace.StencilPassOp = static_cast<D3D12_STENCIL_OP>(desc.stencil_pass_op);
    p.DepthStencilState.FrontFace.StencilFunc = static_cast<D3D12_COMPARISON_FUNC>(desc.stencil_func);
    p.DepthStencilState.BackFace = p.DepthStencilState.FrontFace;
    if (desc.color_write_disable) {
        for (UINT i = 0; i < 4u; ++i) {
            p.BlendState.RenderTarget[i].RenderTargetWriteMask = 0u;
        }
    }
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    uint32_t rt_count = desc.num_render_targets;
    if (rt_count == 0u) {
        for (uint32_t i = 0; i < 4u; ++i) {
            if (desc.rtv_formats[i] != 0) {
                rt_count = i + 1u;
            }
        }
    }
    p.NumRenderTargets = rt_count;
    for (uint32_t i = 0; i < rt_count && i < 4u; ++i) {
        p.RTVFormats[i] = static_cast<DXGI_FORMAT>(desc.rtv_formats[i]);
    }
    p.DSVFormat = static_cast<DXGI_FORMAT>(desc.dsv_format);
    p.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = nullptr;
    if (FAILED(device_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso)))) {
        return make_unexpected(Error::make(ErrorCode::VulkanError, "CreateGraphicsPipelineState failed"));
    }
    return pso;
#endif
}

void D3D12Device::destroy_pso(ID3D12PipelineState*& pso) noexcept {
#if defined(_WIN32)
    safe_release(pso);
#else
    pso = nullptr;
#endif
}

void D3D12Device::cmd_bind_bindless_compute(ID3D12GraphicsCommandList4* cmd, bool uses_rt_tlas,
                                            uint64_t tlas_gpu_va) const noexcept {
#if defined(_WIN32)
    if (cmd == nullptr || cbv_srv_uav_heap_ == nullptr || sampler_heap_ == nullptr) {
        return;
    }
    ID3D12DescriptorHeap* heaps[2] = {cbv_srv_uav_heap_, sampler_heap_};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetComputeRootSignature(uses_rt_tlas ? rt_bindless_root_sig_ : bindless_root_sig_);
    if (uses_rt_tlas && tlas_gpu_va != 0ULL) {
        cmd->SetComputeRootShaderResourceView(1, tlas_gpu_va);
    }
#else
    (void)cmd;
    (void)uses_rt_tlas;
    (void)tlas_gpu_va;
#endif
}

void D3D12Device::cmd_bind_bindless_graphics(ID3D12GraphicsCommandList4* cmd) const noexcept {
#if defined(_WIN32)
    if (cmd == nullptr || cbv_srv_uav_heap_ == nullptr || sampler_heap_ == nullptr) {
        return;
    }
    ID3D12DescriptorHeap* heaps[2] = {cbv_srv_uav_heap_, sampler_heap_};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetGraphicsRootSignature(bindless_root_sig_);
#else
    (void)cmd;
#endif
}

void D3D12Device::cmd_transition_buffer(ID3D12GraphicsCommandList4* cmd, D3D12Buffer& buf, int target_state) noexcept {
#if defined(_WIN32)
    if (cmd == nullptr || buf.resource == nullptr || buf.heap_kind != D3D12BufferHeap::DefaultGpu) {
        return;
    }
    if (buf.current_state == target_state) {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = buf.resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(buf.current_state);
    b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(target_state);
    cmd->ResourceBarrier(1, &b);
    buf.current_state = target_state;
#else
    (void)cmd;
    (void)buf;
    (void)target_state;
#endif
}

void D3D12Device::cmd_transition_texture(ID3D12GraphicsCommandList4* cmd, D3D12Texture& tex, int target_state) noexcept {
#if defined(_WIN32)
    if (cmd == nullptr || tex.resource == nullptr || tex.current_state == target_state) {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex.resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(tex.current_state);
    b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(target_state);
    cmd->ResourceBarrier(1, &b);
    tex.current_state = target_state;
#else
    (void)cmd;
    (void)tex;
    (void)target_state;
#endif
}

void D3D12Device::cmd_uav_barrier(ID3D12GraphicsCommandList4* cmd, ID3D12Resource* res) noexcept {
#if defined(_WIN32)
    if (cmd == nullptr) {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    cmd->ResourceBarrier(1, &b);
#else
    (void)cmd;
    (void)res;
#endif
}

void D3D12Device::cpu_rtv_handle(uint32_t index, void* out_d3d12_cpu_handle) const noexcept {
#if defined(_WIN32)
    if (out_d3d12_cpu_handle == nullptr || rtv_heap_ == nullptr || index >= kRtvHeapCapacity) {
        return;
    }
    auto* dst = static_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(out_d3d12_cpu_handle);
    dst->ptr = rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(index) * rtv_inc_;
#else
    (void)index;
    (void)out_d3d12_cpu_handle;
#endif
}

void D3D12Device::cpu_dsv_handle(uint32_t index, void* out_d3d12_cpu_handle) const noexcept {
#if defined(_WIN32)
    if (out_d3d12_cpu_handle == nullptr || dsv_heap_ == nullptr || index >= kDsvHeapCapacity) {
        return;
    }
    auto* dst = static_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(out_d3d12_cpu_handle);
    dst->ptr = dsv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(index) * dsv_inc_;
#else
    (void)index;
    (void)out_d3d12_cpu_handle;
#endif
}

void D3D12Device::gpu_srv_handle(uint32_t index, void* out_d3d12_gpu_handle) const noexcept {
#if defined(_WIN32)
    if (out_d3d12_gpu_handle == nullptr || cbv_srv_uav_heap_ == nullptr || index >= kCbvSrvUavHeapCapacity) {
        return;
    }
    auto* dst = static_cast<D3D12_GPU_DESCRIPTOR_HANDLE*>(out_d3d12_gpu_handle);
    dst->ptr =
        cbv_srv_uav_heap_->GetGPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(index) * cbv_srv_uav_inc_;
#else
    (void)index;
    (void)out_d3d12_gpu_handle;
#endif
}

void D3D12Device::cpu_srv_staging_handle(uint32_t index, void* out_d3d12_cpu_handle) const noexcept {
#if defined(_WIN32)
    if (out_d3d12_cpu_handle == nullptr || cbv_srv_uav_cpu_staging_ == nullptr || index >= kCbvSrvUavHeapCapacity) {
        return;
    }
    auto* dst = static_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(out_d3d12_cpu_handle);
    dst->ptr = cbv_srv_uav_cpu_staging_->GetCPUDescriptorHandleForHeapStart().ptr +
               static_cast<SIZE_T>(index) * cbv_srv_uav_inc_;
#else
    (void)index;
    (void)out_d3d12_cpu_handle;
#endif
}

bool D3D12Device::acquire_d3d11on12_resource(const D3D12Texture& tex) noexcept {
#if defined(_WIN32)
    if (d3d11on12_device_ == nullptr || tex.d3d11_wrapped == nullptr) {
        return false;
    }
    ID3D11Resource* res[1] = {tex.d3d11_wrapped};
    d3d11on12_device_->AcquireWrappedResources(res, 1);
    return true;
#else
    (void)tex;
    return false;
#endif
}

bool D3D12Device::release_and_flush_d3d11on12_resource(const D3D12Texture& tex) noexcept {
#if defined(_WIN32)
    if (d3d11on12_device_ == nullptr || d3d11_context_ == nullptr || tex.d3d11_wrapped == nullptr) {
        return false;
    }
    ID3D11Resource* res[1] = {tex.d3d11_wrapped};
    d3d11on12_device_->ReleaseWrappedResources(res, 1);
    d3d11_context_->Flush();
    return true;
#else
    (void)tex;
    return false;
#endif
}

uint64_t D3D12Device::poll_debug_errors() noexcept {
#if defined(_WIN32)
    if (device_ == nullptr) {
        return validation_errors_.load(std::memory_order_acquire);
    }
    if (device_->GetDeviceRemovedReason() != S_OK) {
        device_lost_.store(true, std::memory_order_release);
    }
    if (debug_layer_active_) {
        ID3D12InfoQueue* iq = nullptr;
        if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&iq)))) {
            const UINT64 n = iq->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (UINT64 i = 0; i < n; ++i) {
                SIZE_T len = 0;
                if (SUCCEEDED(iq->GetMessage(i, nullptr, &len)) && len >= sizeof(D3D12_MESSAGE)) {
                    // Allocation-free fixed stack probe buffer for first 512 B of message
                    alignas(D3D12_MESSAGE) uint8_t buf[512]{};
                    SIZE_T cap = std::min<SIZE_T>(len, sizeof(buf));
                    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf);
                    if (SUCCEEDED(iq->GetMessage(i, msg, &cap)) &&
                        (msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                         msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
                        validation_errors_.fetch_add(1u, std::memory_order_acq_rel);
                        AETHER_LOG_ERROR("dx12-val", "%s", msg->pDescription != nullptr ? msg->pDescription : "D3D12 error");
                    }
                }
            }
            iq->ClearStoredMessages();
            iq->Release();
        }
    }
#endif
    return validation_errors_.load(std::memory_order_acquire);
}

} // namespace aether::rhi
