#include "tracking/ort_router.hpp"
#include "core/log.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <Windows.h>
// clang-format on
#include <DirectML.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::tracking {

namespace {

using Microsoft::WRL::ComPtr;

[[nodiscard]] Ort::Env& shared_ort_env() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "AetherTracking");
    return env;
}

[[nodiscard]] const char* ep_display_name(EpTier ep) noexcept {
    switch (ep) {
    case EpTier::TensorRt:
        return "Tier 0: TensorRT";
    case EpTier::NpuOpenVino:
        return "Tier 1: OpenVINO NPU";
    case EpTier::GpuOpenVino:
        return "Tier 2A: OpenVINO iGPU";
    case EpTier::DirectMlIgpu:
        return "Tier 2: DirectML iGPU (HiPri Compute)";
    case EpTier::DirectMlDgpu:
        return "Tier 3: DirectML dGPU (HiPri Compute)";
    case EpTier::Cpu:
    default:
        return "Tier 4: CPU EP";
    }
}

[[nodiscard]] bool has_intel_igpu(const HardwareInventory& hw) noexcept {
    for (const auto& gpu : hw.gpus) {
        if (!gpu.is_software && gpu.vendor_id == 0x8086U && gpu.dedicated_vram_mb <= 1024ULL) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool find_gpu_luid(const HardwareInventory& hw, bool integrated, uint64_t& out_luid) noexcept {
    const GpuAdapterInfo* best = nullptr;
    for (const auto& gpu : hw.gpus) {
        if (gpu.is_software) {
            continue;
        }
        const bool is_igpu = (gpu.dedicated_vram_mb <= 1024ULL);
        if (is_igpu != integrated) {
            continue;
        }
        if (best == nullptr || (!integrated && gpu.dedicated_vram_mb > best->dedicated_vram_mb)) {
            best = &gpu;
        }
    }
    if (best == nullptr) {
        return false;
    }
    out_luid = 0;
    for (size_t i = 0; i < 8U; ++i) {
        out_luid |= (static_cast<uint64_t>(best->luid[i]) << (i * 8U));
    }
    return true;
}

[[nodiscard]] size_t shape_element_count(const std::vector<int64_t>& shape) noexcept {
    size_t prod = 1;
    for (const int64_t d : shape) {
        prod *= static_cast<size_t>(d > 0 ? d : 1);
    }
    return prod;
}

[[nodiscard]] bool write_npu_fast_compile_config(const std::filesystem::path& cache_dir, std::string& out_config_json) {
    // Request LATENCY hint with full NPU runtime graph optimization (cached in ov_cache).
    static constexpr const char* kFastCompileJson = "{\n"
                                                    "  \"NPU\": {\n"
                                                    "    \"NPU_COMPILATION_MODE_PARAMS\": \"optimization-level=2\",\n"
                                                    "    \"PERFORMANCE_HINT\": \"LATENCY\"\n"
                                                    "  }\n"
                                                    "}";
    if (!cache_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cache_dir, ec);
        const std::filesystem::path cfg_file = cache_dir / "npu_fast_compile_config.json";
        std::ofstream ofs(cfg_file, std::ios::trunc);
        if (ofs.is_open()) {
            ofs << kFastCompileJson << "\n";
        }
    }
    out_config_json = kFastCompileJson;
    return true;
}

[[nodiscard]] bool create_d3d12_hipri_dml_for_luid(uint64_t target_luid_packed, ComPtr<ID3D12Device>& out_d3d,
                                                   ComPtr<ID3D12CommandQueue>& out_queue, ComPtr<IDMLDevice>& out_dml,
                                                   std::string* out_err) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        if (out_err != nullptr) {
            *out_err = "CreateDXGIFactory1 failed";
        }
        return false;
    }

    ComPtr<IDXGIAdapter1> chosen;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U) {
                adapter.Reset();
                continue;
            }
            const uint64_t packed = (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32U) |
                                    static_cast<uint64_t>(desc.AdapterLuid.LowPart);
            if (target_luid_packed == 0ULL || packed == target_luid_packed) {
                chosen = adapter;
                break;
            }
        }
        adapter.Reset();
    }
    if (!chosen) {
        if (out_err != nullptr) {
            *out_err = "Matching DXGI adapter LUID not found";
        }
        return false;
    }

    if (FAILED(::D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&out_d3d)))) {
        if (out_err != nullptr) {
            *out_err = "D3D12CreateDevice failed";
        }
        return false;
    }

    // §8.4: Isolated high-priority async compute queue avoids contending with T3 render queue
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    qdesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qdesc.NodeMask = 0;
    if (FAILED(out_d3d->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&out_queue)))) {
        // Retry with normal priority if driver denies global high priority without elevated privileges
        qdesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        if (FAILED(out_d3d->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&out_queue)))) {
            if (out_err != nullptr) {
                *out_err = "CreateCommandQueue(COMPUTE) failed";
            }
            return false;
        }
    }

    using PfnDmlCreateDevice = HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**);
    HMODULE h_dml = ::LoadLibraryW(L"DirectML.dll");
    if (h_dml == nullptr) {
        if (out_err != nullptr) {
            *out_err = "DirectML.dll not available";
        }
        return false;
    }
    const FARPROC raw_proc = ::GetProcAddress(h_dml, "DMLCreateDevice");
    PfnDmlCreateDevice pfn_create = nullptr;
    if (raw_proc != nullptr) {
        static_assert(sizeof(pfn_create) == sizeof(raw_proc));
        std::memcpy(static_cast<void*>(&pfn_create), static_cast<const void*>(&raw_proc), sizeof(pfn_create));
    }
    if (pfn_create == nullptr ||
        FAILED(pfn_create(out_d3d.Get(), DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&out_dml)))) {
        if (out_err != nullptr) {
            *out_err = "DMLCreateDevice failed";
        }
        return false;
    }
    return true;
}

struct SessionBundle {
    std::unique_ptr<Ort::Session> session;
    Ort::RunOptions run_opts{nullptr};
    std::vector<std::string> in_name_strings;
    std::vector<std::string> out_name_strings;
    std::vector<const char*> in_name_ptrs;
    std::vector<const char*> out_name_ptrs;
    std::vector<Ort::Value> in_tensors;
    std::vector<Ort::Value> out_tensors;
    std::vector<std::vector<float>> extra_storage;
    EpTier actual_ep{EpTier::Cpu};
};

} // namespace

struct AetherTrackingRouter::Impl {
    Ort::MemoryInfo cpu_mem_info{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};
    ComPtr<ID3D12Device> dml_d3d_device;
    ComPtr<ID3D12CommandQueue> dml_compute_queue;
    ComPtr<IDMLDevice> dml_device;
    std::array<SessionBundle, static_cast<size_t>(ModelId::Count)> sessions{};
    static constexpr size_t kMaxHealthWindow = 512U;
    std::array<double, kMaxHealthWindow> health_window{};
    size_t health_head{0U};
    size_t health_count{0U};
    double health_sum_ms{0.0};
    bool health_demoted{false};

    void clear_sessions() noexcept {
        try {
            for (auto& s : sessions) {
                s.in_tensors.clear();
                s.out_tensors.clear();
                s.in_name_ptrs.clear();
                s.out_name_ptrs.clear();
                s.in_name_strings.clear();
                s.out_name_strings.clear();
                s.extra_storage.clear();
                s.run_opts = Ort::RunOptions{nullptr};
                s.session.reset();
            }
            dml_device.Reset();
            dml_compute_queue.Reset();
            dml_d3d_device.Reset();
        } catch (...) {
            AETHER_LOG_WARN("tracking", "Exception caught while clearing ORT sessions");
        }
    }
};

AetherTrackingRouter::AetherTrackingRouter() : impl_(std::make_unique<Impl>()) {}

AetherTrackingRouter::~AetherTrackingRouter() {
    shutdown();
}

AetherStatus AetherTrackingRouter::initialize(const TrackingRouterConfig& cfg, const HardwareInventory& hw,
                                              std::string* out_err) {
    shutdown();
    // Construct Ort::Env before any AppendExecutionProvider_* calls so DefaultLogger is registered.
    (void)shared_ort_env();
    cfg_ = cfg;
    hw_ = hw;
    impl_->health_window.fill(0.0);
    impl_->health_head = 0U;
    impl_->health_count = 0U;
    impl_->health_sum_ms = 0.0;
    impl_->health_demoted = false;
    demotion_count_ = 0;

    const AetherStatus mst = load_model_manifest(cfg.manifest_path, manifest_, out_err);
    if (mst != AETHER_OK) {
        return mst;
    }

    // Allocate persistent pinned buffers once before binding sessions (`HR-01`)
    buffers_.m0_in.assign(1ULL * 3ULL * 128ULL * 128ULL, 0.0f);
    buffers_.m0_regressors.assign(1ULL * 896ULL * 16ULL, 0.0f);
    buffers_.m0_classif.assign(1ULL * 896ULL * 1ULL, 0.0f);

    buffers_.m1_body_in.assign(1ULL * 3ULL * 256ULL * 256ULL, 0.0f);
    buffers_.m1_body_lm.assign(195ULL, 0.0f);
    buffers_.m1_body_flag.assign(1ULL, 0.0f);
    buffers_.m1_body_seg.assign(256ULL * 256ULL, 0.0f);
    buffers_.m1_body_heat.assign(64ULL * 64ULL * 39ULL, 0.0f);
    buffers_.m1_body_world.assign(117ULL, 0.0f);

    buffers_.m1_hands_in.assign(2ULL * 3ULL * 224ULL * 224ULL, 0.0f);
    buffers_.m1_hands_lm.assign(2ULL * 63ULL, 0.0f);
    buffers_.m1_hands_flag.assign(2ULL, 0.0f);
    buffers_.m1_hands_handed.assign(2ULL, 0.0f);
    buffers_.m1_hands_world.assign(2ULL * 63ULL, 0.0f);

    buffers_.m2_face_in.assign(1ULL * 3ULL * 256ULL * 256ULL, 0.0f);
    buffers_.m2_face_lm.assign(1434ULL, 0.0f);
    buffers_.m2_face_flag.assign(1ULL, 0.0f);

    buffers_.m3_in.assign(1ULL * 146ULL * 2ULL, 0.0f);
    buffers_.m3_blendshapes.assign(52ULL, 0.0f);

    // Ordered EP fallback cascade (`§8.4`: Tier 0 TensorRT -> Tier 1 NPU -> Tier 2A OV iGPU -> Tier 2B DML iGPU -> Tier
    // 3 dGPU -> Tier 4 CPU)
    static constexpr std::array<EpTier, 6> kAllTiers = {
        EpTier::TensorRt,     EpTier::NpuOpenVino,  EpTier::GpuOpenVino,
        EpTier::DirectMlIgpu, EpTier::DirectMlDgpu, EpTier::Cpu,
    };

    size_t start_idx = 0;
    for (size_t i = 0; i < kAllTiers.size(); ++i) {
        if (kAllTiers[i] == cfg.preferred_ep) {
            start_idx = i;
            break;
        }
    }

    std::string last_fail_reason;
    for (size_t t = start_idx; t < kAllTiers.size(); ++t) {
        const EpTier tier = kAllTiers[t];
        if ((cfg.forced_fail_ep_mask & (1U << static_cast<uint32_t>(tier))) != 0U) {
            last_fail_reason = std::string("Forced test fail bitmask for ") + ep_display_name(tier);
            continue;
        }

        // Verify hardware prerequisites for non-CPU tiers
        if (tier == EpTier::TensorRt) {
            HMODULE h_trt = ::LoadLibraryW(L"nvinfer_10.dll");
            if (h_trt == nullptr) {
                last_fail_reason = "TensorRT runtime DLL (nvinfer_10.dll) absent on host";
                continue;
            }
            ::FreeLibrary(h_trt);
        } else if (tier == EpTier::NpuOpenVino) {
            if (!hw.npu.available) {
                last_fail_reason = "NPU absent per hardware inventory";
                continue;
            }
        } else if (tier == EpTier::GpuOpenVino) {
            if (!has_intel_igpu(hw)) {
                last_fail_reason = "Intel integrated GPU absent per hardware inventory";
                continue;
            }
        } else if (tier == EpTier::DirectMlIgpu) {
            std::string dml_err;
            uint64_t luid = 0ULL;
            if (!find_gpu_luid(hw, true, luid) ||
                !create_d3d12_hipri_dml_for_luid(luid, impl_->dml_d3d_device, impl_->dml_compute_queue,
                                                 impl_->dml_device, &dml_err)) {
                last_fail_reason = "iGPU DML init failed: " + (dml_err.empty() ? std::string("absent") : dml_err);
                continue;
            }
        } else if (tier == EpTier::DirectMlDgpu) {
            std::string dml_err;
            uint64_t luid = 0ULL;
            if (!find_gpu_luid(hw, false, luid) ||
                !create_d3d12_hipri_dml_for_luid(luid, impl_->dml_d3d_device, impl_->dml_compute_queue,
                                                 impl_->dml_device, &dml_err)) {
                last_fail_reason = "dGPU DML init failed: " + (dml_err.empty() ? std::string("absent") : dml_err);
                continue;
            }
        }

        bool tier_ok = true;
        std::string npu_load_cfg_path;
        if (tier == EpTier::NpuOpenVino && cfg.enable_npu_fast_compile) {
            (void)write_npu_fast_compile_config(cfg.ov_cache_dir, npu_load_cfg_path);
        }

        for (size_t mi = 0; mi < static_cast<size_t>(ModelId::Count); ++mi) {
            const auto mid = static_cast<ModelId>(mi);
            EpTier model_ep = tier;
            std::filesystem::path onnx_path;
            std::string ver_err;
            if (verify_and_resolve_model_path(manifest_, mid, model_ep, onnx_path, &ver_err) != AETHER_OK) {
                last_fail_reason = ver_err;
                tier_ok = false;
                break;
            }

            try {
                Ort::SessionOptions so;
                so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
                const int intra_threads = (cfg.cpu_intra_op_threads > 0U) ? static_cast<int>(cfg.cpu_intra_op_threads)
                                                                          : ((model_ep == EpTier::Cpu) ? 4 : 1);
                so.SetIntraOpNumThreads(intra_threads);

                auto configure_ep_for_session = [&](Ort::SessionOptions& opts, EpTier ep_use) {
                    if (ep_use == EpTier::NpuOpenVino || ep_use == EpTier::GpuOpenVino) {
                        std::unordered_map<std::string, std::string> ov_opts;
                        ov_opts["device_type"] = (ep_use == EpTier::NpuOpenVino) ? "NPU" : "GPU.0";
                        ov_opts["disable_dynamic_shapes"] = "True";
                        ov_opts["num_streams"] = "1";
                        if (ep_use == EpTier::GpuOpenVino) {
                            ov_opts["precision"] = "FP16";
                            ov_opts["enable_opencl_throttling"] = "False";
                        }
                        if (!cfg.ov_cache_dir.empty()) {
                            std::error_code ec;
                            std::filesystem::create_directories(cfg.ov_cache_dir, ec);
                            ov_opts["cache_dir"] = cfg.ov_cache_dir.string();
                        }
                        if (ep_use == EpTier::NpuOpenVino && cfg.enable_npu_fast_compile &&
                            !npu_load_cfg_path.empty()) {
                            // ADR-0007: ORT 1.24.1 OpenVINO EP rejects deprecated enable_npu_fast_compile -> map via
                            // load_config
                            ov_opts["load_config"] = npu_load_cfg_path;
                        } else if (ep_use == EpTier::GpuOpenVino) {
                            ov_opts["load_config"] = "{\"GPU\":{\"PERFORMANCE_HINT\":\"LATENCY\"}}";
                        }
                        opts.AppendExecutionProvider_OpenVINO_V2(ov_opts);
                    } else if (ep_use == EpTier::DirectMlIgpu || ep_use == EpTier::DirectMlDgpu) {
                        opts.DisableMemPattern();
                        opts.SetExecutionMode(ORT_SEQUENTIAL);
                        const OrtDmlApi* dml_api = nullptr;
                        Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi(
                            "DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dml_api)));
                        if (dml_api == nullptr) {
                            throw std::runtime_error("OrtDmlApi unavailable");
                        }
                        Ort::ThrowOnError(dml_api->SessionOptionsAppendExecutionProvider_DML1(
                            opts, impl_->dml_device.Get(), impl_->dml_compute_queue.Get()));
                    }
                };

                try {
                    configure_ep_for_session(so, model_ep);
                    impl_->sessions[mi].session =
                        std::make_unique<Ort::Session>(shared_ort_env(), onnx_path.wstring().c_str(), so);
                } catch (const Ort::Exception& ex) {
                    if (mid == ModelId::M3ArkitBlendshapes && model_ep != EpTier::Cpu) {
                        AETHER_LOG_WARN("tracking",
                                        "M3 MLP rejected by %s (%s); hosting M3 on CPU cleanly alongside main EP",
                                        ep_display_name(tier), ex.what());
                        model_ep = EpTier::Cpu;
                        std::filesystem::path cpu_path;
                        std::string cpu_err;
                        if (verify_and_resolve_model_path(manifest_, mid, EpTier::Cpu, cpu_path, &cpu_err) !=
                            AETHER_OK) {
                            cpu_path = onnx_path;
                        }
                        Ort::SessionOptions cpu_so;
                        cpu_so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
                        cpu_so.SetIntraOpNumThreads(1);
                        impl_->sessions[mi].session =
                            std::make_unique<Ort::Session>(shared_ort_env(), cpu_path.wstring().c_str(), cpu_so);
                    } else {
                        throw;
                    }
                }
                impl_->sessions[mi].actual_ep = model_ep;

                // Construct zero-alloc pre-allocated Ort::Value arrays + name tables for direct Session::Run (`HR-01`)
                auto& bnd = impl_->sessions[mi];
                auto* sess = bnd.session.get();
                bnd.run_opts = Ort::RunOptions{};
                Ort::AllocatorWithDefaultOptions alloc;

                const size_t n_ins = sess->GetInputCount();
                if (n_ins != 1U) {
                    throw std::runtime_error("Unexpected model input count != 1");
                }
                auto in_name = sess->GetInputNameAllocated(0, alloc);
                bnd.in_name_strings.emplace_back(in_name.get());
                auto in_shape = sess->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
                for (auto& d : in_shape) {
                    if (d <= 0) {
                        d = 1;
                    }
                }
                float* in_ptr = nullptr;
                size_t in_count = 0;
                switch (mid) {
                case ModelId::M0FaceDetector:
                    in_ptr = buffers_.m0_in.data();
                    in_count = buffers_.m0_in.size();
                    break;
                case ModelId::M1BodyLandmarks:
                    in_ptr = buffers_.m1_body_in.data();
                    in_count = buffers_.m1_body_in.size();
                    break;
                case ModelId::M1HandsLandmarks:
                    in_ptr = buffers_.m1_hands_in.data();
                    in_count = buffers_.m1_hands_in.size();
                    break;
                case ModelId::M2FaceLandmarks:
                    in_ptr = buffers_.m2_face_in.data();
                    in_count = buffers_.m2_face_in.size();
                    break;
                case ModelId::M3ArkitBlendshapes:
                    in_ptr = buffers_.m3_in.data();
                    in_count = buffers_.m3_in.size();
                    break;
                default:
                    break;
                }
                const size_t needed_in = shape_element_count(in_shape);
                if (in_count < needed_in) {
                    throw std::runtime_error("Preallocated input buffer too small");
                }
                bnd.in_tensors.push_back(Ort::Value::CreateTensor<float>(impl_->cpu_mem_info, in_ptr, in_count,
                                                                         in_shape.data(), in_shape.size()));

                const size_t n_outs = sess->GetOutputCount();
                bnd.out_name_strings.reserve(n_outs);
                bnd.out_tensors.reserve(n_outs);
                for (size_t oi = 0; oi < n_outs; ++oi) {
                    auto out_name = sess->GetOutputNameAllocated(oi, alloc);
                    bnd.out_name_strings.emplace_back(out_name.get());
                    auto out_shape = sess->GetOutputTypeInfo(oi).GetTensorTypeAndShapeInfo().GetShape();
                    for (auto& d : out_shape) {
                        if (d <= 0) {
                            d = 1;
                        }
                    }
                    const size_t out_elems = shape_element_count(out_shape);
                    float* dst_ptr = nullptr;
                    size_t dst_cap = 0;

                    auto assign_dst = [&](std::vector<float>& vec) {
                        if (vec.size() >= out_elems) {
                            dst_ptr = vec.data();
                            dst_cap = vec.size();
                        }
                    };
                    if (mid == ModelId::M0FaceDetector) {
                        if (oi == 0)
                            assign_dst(buffers_.m0_regressors);
                        else if (oi == 1)
                            assign_dst(buffers_.m0_classif);
                    } else if (mid == ModelId::M1BodyLandmarks) {
                        if (oi == 0)
                            assign_dst(buffers_.m1_body_lm);
                        else if (oi == 1)
                            assign_dst(buffers_.m1_body_flag);
                        else if (oi == 2)
                            assign_dst(buffers_.m1_body_seg);
                        else if (oi == 3)
                            assign_dst(buffers_.m1_body_heat);
                        else if (oi == 4)
                            assign_dst(buffers_.m1_body_world);
                    } else if (mid == ModelId::M1HandsLandmarks) {
                        if (oi == 0)
                            assign_dst(buffers_.m1_hands_lm);
                        else if (oi == 1)
                            assign_dst(buffers_.m1_hands_flag);
                        else if (oi == 2)
                            assign_dst(buffers_.m1_hands_handed);
                        else if (oi == 3)
                            assign_dst(buffers_.m1_hands_world);
                    } else if (mid == ModelId::M2FaceLandmarks) {
                        if (oi == 0)
                            assign_dst(buffers_.m2_face_lm);
                        else if (oi == 1)
                            assign_dst(buffers_.m2_face_flag);
                    } else if (mid == ModelId::M3ArkitBlendshapes) {
                        if (oi == 0)
                            assign_dst(buffers_.m3_blendshapes);
                    }

                    if (dst_ptr == nullptr) {
                        bnd.extra_storage.emplace_back(out_elems, 0.0f);
                        dst_ptr = bnd.extra_storage.back().data();
                        dst_cap = bnd.extra_storage.back().size();
                    }
                    bnd.out_tensors.push_back(Ort::Value::CreateTensor<float>(impl_->cpu_mem_info, dst_ptr, dst_cap,
                                                                              out_shape.data(), out_shape.size()));
                }
                bnd.in_name_ptrs.reserve(bnd.in_name_strings.size());
                for (const auto& nm : bnd.in_name_strings) {
                    bnd.in_name_ptrs.push_back(nm.c_str());
                }
                bnd.out_name_ptrs.reserve(bnd.out_name_strings.size());
                for (const auto& nm : bnd.out_name_strings) {
                    bnd.out_name_ptrs.push_back(nm.c_str());
                }
            } catch (const std::exception& e) {
                last_fail_reason = std::string(ep_display_name(tier)) + " failed on model " +
                                   manifest_.models[mi].section_name + ": " + e.what();
                AETHER_LOG_WARN("tracking", "%s", last_fail_reason.c_str());
                impl_->clear_sessions();
                tier_ok = false;
                break;
            }
        }

        if (tier_ok) {
            active_ep_ = tier;
            active_ep_label_ = ep_display_name(tier);
            if (tier == EpTier::NpuOpenVino && cfg.enable_npu_fast_compile) {
                AETHER_LOG_INFO("tracking",
                                "Tracking router active: %s (fast-compile mapped via load_config, ADR-0007)",
                                active_ep_label_.c_str());
            } else {
                AETHER_LOG_INFO("tracking", "Tracking router active: %s", active_ep_label_.c_str());
            }
            initialized_ = true;
            return AETHER_OK;
        }
    }

    if (out_err != nullptr) {
        *out_err = last_fail_reason.empty() ? "All ONNX Execution Provider tiers failed" : last_fail_reason;
    }
    return AETHER_E_INTERNAL;
}

void AetherTrackingRouter::shutdown() noexcept {
    if (impl_) {
        impl_->clear_sessions();
        impl_->health_window.fill(0.0);
        impl_->health_head = 0U;
        impl_->health_count = 0U;
        impl_->health_sum_ms = 0.0;
    }
    initialized_ = false;
    active_ep_ = EpTier::Cpu;
    try {
        active_ep_label_ = "Uninitialized";
    } catch (...) {
        active_ep_label_.clear();
    }
}

namespace {

[[nodiscard]] AetherStatus run_bound_session(SessionBundle& bnd) noexcept {
    if (!bnd.session || bnd.in_tensors.empty() || bnd.out_tensors.empty()) {
        return AETHER_E_INVALID_ARG;
    }
    try {
        bnd.session->Run(bnd.run_opts, bnd.in_name_ptrs.data(), bnd.in_tensors.data(), bnd.in_tensors.size(),
                         bnd.out_name_ptrs.data(), bnd.out_tensors.data(), bnd.out_tensors.size());
        return AETHER_OK;
    } catch (const Ort::Exception& e) {
        AETHER_LOG_ERROR("tracking", "ORT session Run failed: %s", e.what());
        return AETHER_E_INTERNAL;
    } catch (...) {
        return AETHER_E_INTERNAL;
    }
}

} // namespace

AetherStatus AetherTrackingRouter::run_m0_face_detector() noexcept {
    return run_bound_session(impl_->sessions[static_cast<size_t>(ModelId::M0FaceDetector)]);
}

AetherStatus AetherTrackingRouter::run_m1_body() noexcept {
    return run_bound_session(impl_->sessions[static_cast<size_t>(ModelId::M1BodyLandmarks)]);
}

AetherStatus AetherTrackingRouter::run_m1_hands() noexcept {
    return run_bound_session(impl_->sessions[static_cast<size_t>(ModelId::M1HandsLandmarks)]);
}

AetherStatus AetherTrackingRouter::run_m2_face_landmarks() noexcept {
    return run_bound_session(impl_->sessions[static_cast<size_t>(ModelId::M2FaceLandmarks)]);
}

AetherStatus AetherTrackingRouter::run_m3_blendshapes() noexcept {
    return run_bound_session(impl_->sessions[static_cast<size_t>(ModelId::M3ArkitBlendshapes)]);
}

void AetherTrackingRouter::record_frame_latency(double total_infer_ms, std::string* out_demotion_msg) noexcept {
    if (!initialized_) {
        return;
    }
    const size_t cap = std::clamp<size_t>(cfg_.health_window_frames, 30U, Impl::kMaxHealthWindow);
    if (impl_->health_count < cap) {
        const size_t slot = (impl_->health_head + impl_->health_count) % cap;
        impl_->health_window[slot] = total_infer_ms;
        impl_->health_sum_ms += total_infer_ms;
        ++impl_->health_count;
    } else {
        impl_->health_sum_ms -= impl_->health_window[impl_->health_head];
        impl_->health_window[impl_->health_head] = total_infer_ms;
        impl_->health_sum_ms += total_infer_ms;
        impl_->health_head = (impl_->health_head + 1U) % cap;
    }
    rolling_avg_ms_ = impl_->health_sum_ms / static_cast<double>(impl_->health_count);

    if (impl_->health_count >= cap && rolling_avg_ms_ > cfg_.health_budget_ms && active_ep_ != EpTier::Cpu) {
        const EpTier prev = active_ep_;
        const AetherStatus st = force_demote_one_tier("Rolling 120-frame inference latency exceeded 9.0 ms");
        if (st == AETHER_OK && out_demotion_msg != nullptr) {
            try {
                *out_demotion_msg = std::string("Health monitor demoted EP from ") + ep_display_name(prev) + " to " +
                                    active_ep_label_ + " (avg=" + std::to_string(rolling_avg_ms_) + " ms)";
            } catch (...) {
                AETHER_LOG_WARN("tracking", "Exception formatting EP demotion message");
            }
        }
    }
}

AetherStatus AetherTrackingRouter::force_demote_one_tier(const char* reason) noexcept {
    if (active_ep_ == EpTier::Cpu) {
        return AETHER_E_UNSUPPORTED;
    }
    try {
        const uint32_t bit = 1U << static_cast<uint32_t>(active_ep_);
        TrackingRouterConfig next_cfg = cfg_;
        next_cfg.forced_fail_ep_mask |= bit;
        next_cfg.preferred_ep = active_ep_;
        const uint32_t prev_demotions = demotion_count_;

        AETHER_LOG_WARN("tracking", "Demoting tracking EP (%s): %s", active_ep_label_.c_str(),
                        (reason != nullptr) ? reason : "health monitor");
        std::string err;
        const AetherStatus st = initialize(next_cfg, hw_, &err);
        demotion_count_ = prev_demotions + (st == AETHER_OK ? 1U : 0U);
        return st;
    } catch (...) {
        return AETHER_E_INTERNAL;
    }
}

} // namespace aether::tracking
