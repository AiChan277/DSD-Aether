#pragma once

#include "core/expected.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct IDXGIFactory6;
struct IDXGIAdapter4;
struct ID3D12Device5;
struct ID3D12CommandQueue;
struct ID3D12CommandAllocator;
struct ID3D12GraphicsCommandList4;
struct ID3D12Fence;
struct ID3D12DescriptorHeap;
struct ID3D12RootSignature;
struct ID3D12PipelineState;
struct ID3D12Resource;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11On12Device;
struct ID3D11Resource;
struct ID3D11Texture2D;

namespace aether::rhi {

inline constexpr uint32_t kD3D12FrameCount = 3u;
inline constexpr uint32_t kD3D12InvalidDescIndex = 0xFFFFFFFFu;

/** Encodes a 64-bit buffer handle matching `aether_common.slang` `__TARGET_DXIL__`. */
[[nodiscard]] constexpr uint64_t encode_dx12_buffer_handle(uint32_t desc_index, bool is_uav,
                                                           uint32_t byte_offset = 0u) noexcept {
    if (desc_index == kD3D12InvalidDescIndex || desc_index == 0u) {
        return 0ULL;
    }
    const uint64_t low = static_cast<uint64_t>(desc_index & 0xFFFFFu) | (is_uav ? 0x100000ULL : 0ULL);
    return low | (static_cast<uint64_t>(byte_offset) << 32);
}

enum class D3D12BufferHeap : uint8_t {
    DefaultGpu = 0, // DEVICE_LOCAL VRAM (discrete) or shared default heap
    UploadCpu = 1,  // Persistent-mapped UPLOAD heap (zero-copy directly readable on UMA!)
    ReadbackCpu = 2 // Persistent-mapped READBACK heap for VCam/screenshot/parity tests
};

enum class D3D12BufferUsage : uint32_t {
    None = 0u,
    ShaderResource = 1u << 0,
    UnorderedAccess = 1u << 1,
    VertexBuffer = 1u << 2,
    IndexBuffer = 1u << 3,
    AccelStructStorage = 1u << 4,
    AccelStructInput = 1u << 5,
    CopySrc = 1u << 6,
    CopyDst = 1u << 7
};

[[nodiscard]] constexpr D3D12BufferUsage operator|(D3D12BufferUsage a, D3D12BufferUsage b) noexcept {
    return static_cast<D3D12BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

[[nodiscard]] constexpr bool has_flag(D3D12BufferUsage mask, D3D12BufferUsage bit) noexcept {
    return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(bit)) != 0u;
}

struct D3D12AdapterDescriptor {
    std::string device_name{};
    uint32_t vendor_id{0};
    uint32_t device_id{0};
    uint32_t adapter_index{0};
    std::array<uint8_t, 8> luid{};
    bool luid_valid{false};
    uint64_t dedicated_vram_mb{0};
    uint64_t shared_system_mb{0};
    bool is_software{false};
    bool is_discrete{false};
    bool is_uma{false};
    bool cache_coherent_uma{false};
    bool is_intel_igpu{false};
    bool supports_dxr_1_1{false};
    bool supports_fp16{false};
    bool supports_sm_6_6{false};
    bool supports_bgra8_uav{false};
    uint32_t estimated_bandwidth_gbps{0};
    uint8_t recommended_lighting_mode{0}; // 0=Traditional, 1=Hybrid RT, 2=Path Tracing
    uint32_t score{0};
};

struct D3D12DeviceConfig {
    bool enable_debug_layer{true};
    bool prefer_integrated{false};
    bool allow_warp_fallback{true};
    bool use_preferred_luid{false};
    std::array<uint8_t, 8> preferred_luid{};
    bool enable_d3d11on12{true}; // Required for DX12 -> Spout2 D3D11/KMT sharing
};

struct D3D12Buffer {
    ID3D12Resource* resource{nullptr};
    uint64_t size{0};
    uint64_t gpu_va{0};
    void* mapped_ptr{nullptr};
    uint32_t srv_index{kD3D12InvalidDescIndex};
    uint32_t uav_index{kD3D12InvalidDescIndex};
    int current_state{0}; // D3D12_RESOURCE_STATES
    D3D12BufferHeap heap_kind{D3D12BufferHeap::DefaultGpu};
    bool is_uma_zero_copy{false};

    [[nodiscard]] uint64_t srv_handle(uint32_t byte_offset = 0u) const noexcept {
        return encode_dx12_buffer_handle(srv_index, false, byte_offset);
    }
    [[nodiscard]] uint64_t uav_handle(uint32_t byte_offset = 0u) const noexcept {
        return encode_dx12_buffer_handle(uav_index, true, byte_offset);
    }
};

struct D3D12Texture {
    ID3D12Resource* resource{nullptr};
    uint32_t width{0};
    uint32_t height{0};
    int format{0}; // DXGI_FORMAT
    uint32_t srv_index{kD3D12InvalidDescIndex};
    uint32_t uav_index{kD3D12InvalidDescIndex};
    uint32_t rtv_index{kD3D12InvalidDescIndex};
    uint32_t dsv_index{kD3D12InvalidDescIndex};
    uint32_t dsv_readonly_index{kD3D12InvalidDescIndex};
    uint32_t stencil_srv_index{kD3D12InvalidDescIndex};
    int current_state{0}; // D3D12_RESOURCE_STATES
    void* shared_nt_handle{nullptr};
    ID3D11Resource* d3d11_wrapped{nullptr};
    ID3D11Texture2D* d3d11_tex{nullptr};
};

struct D3D12GraphicsPipelineDesc {
    std::span<const uint8_t> vs_dxil{};
    std::span<const uint8_t> ps_dxil{};
    int rtv_formats[4]{}; // DXGI_FORMAT_UNKNOWN (0) ends active MRTs unless num_render_targets is explicit
    uint32_t num_render_targets{0};
    int dsv_format{0}; // DXGI_FORMAT
    bool depth_enable{false};
    bool depth_write{false};
    int depth_func{4}; // D3D12_COMPARISON_FUNC_LESS_EQUAL (4) or LESS (2)
    bool stencil_enable{false};
    uint8_t stencil_read_mask{0xFFu};
    uint8_t stencil_write_mask{0xFFu};
    int stencil_pass_op{3}; // D3D12_STENCIL_OP_REPLACE (3)
    int cull_mode{3};  // D3D12_CULL_MODE_BACK (3), FRONT (2), NONE (1)
    bool front_counter_clockwise{true};
    bool wireframe{false};
    bool alpha_blend{false};
    bool premultiplied_alpha{false};
    bool use_vertex_input_vertex25d{false};
    bool use_vertex_input_pos3{false};
};

/**
 * Fixed-capacity, allocation-free descriptor heap slot pool (`HR-01`, `HR-02`).
 * Slot 0 on CBV_SRV_UAV is permanently reserved for the safe null buffer SRV.
 */
template <size_t Capacity>
class D3D12DescriptorPool {
  public:
    void reset(size_t reserve_low = 0) noexcept {
        next_free_ = reserve_low;
        free_count_ = 0;
    }

    [[nodiscard]] uint32_t allocate() noexcept {
        if (free_count_ > 0) {
            return free_stack_[--free_count_];
        }
        if (next_free_ < Capacity) {
            return static_cast<uint32_t>(next_free_++);
        }
        return kD3D12InvalidDescIndex;
    }

    void free(uint32_t idx) noexcept {
        if (idx < next_free_ && free_count_ < Capacity) {
            free_stack_[free_count_++] = idx;
        }
    }

  private:
    std::array<uint32_t, Capacity> free_stack_{};
    size_t free_count_{0};
    size_t next_free_{0};
};

/**
 * Production DirectX 12 (`ID3D12Device5` + DXR Tier 1.1 + D3D11On12) RHI backend (`Task 1 & Task 2`).
 * - Supports triple-buffered Direct & Compute queues + fences with Waitable Events (zero polling sleeps).
 * - SM 6.6 bindless `CBV_SRV_UAV` & `Sampler` heaps matching Slang `__TARGET_DXIL__`.
 * - First-class UMA / Intel iGPU zero-copy heap management + capability tier auto-selection.
 * - Zero mutexes (`HR-02`) and zero runtime heap allocations during frame submission (`HR-01`).
 */
class D3D12Device {
  public:
    static constexpr uint32_t kCbvSrvUavHeapCapacity = 4096u;
    static constexpr uint32_t kSamplerHeapCapacity = 64u;
    static constexpr uint32_t kRtvHeapCapacity = 128u;
    static constexpr uint32_t kDsvHeapCapacity = 32u;

    D3D12Device() = default;
    ~D3D12Device();

    D3D12Device(const D3D12Device&) = delete;
    D3D12Device& operator=(const D3D12Device&) = delete;

    [[nodiscard]] static Expected<std::vector<D3D12AdapterDescriptor>> enumerate_adapters();

    [[nodiscard]] static Expected<D3D12Device*> create(const D3D12DeviceConfig& cfg = {});
    void destroy() noexcept;

    [[nodiscard]] bool is_valid() const noexcept { return device_ != nullptr; }
    [[nodiscard]] const D3D12AdapterDescriptor& gpu_info() const noexcept { return gpu_info_; }
    [[nodiscard]] bool is_uma() const noexcept { return gpu_info_.is_uma; }
    [[nodiscard]] bool is_intel_igpu() const noexcept { return gpu_info_.is_intel_igpu; }
    [[nodiscard]] bool dxr_supported() const noexcept { return gpu_info_.supports_dxr_1_1; }
    [[nodiscard]] bool fp16_supported() const noexcept { return gpu_info_.supports_fp16; }
    [[nodiscard]] uint8_t recommended_lighting_mode() const noexcept { return gpu_info_.recommended_lighting_mode; }

    [[nodiscard]] ID3D12Device5* device() const noexcept { return device_; }
    [[nodiscard]] ID3D12CommandQueue* direct_queue() const noexcept { return direct_queue_; }
    [[nodiscard]] ID3D12CommandQueue* compute_queue() const noexcept { return compute_queue_; }
    [[nodiscard]] ID3D12CommandQueue* copy_queue() const noexcept { return copy_queue_; }
    [[nodiscard]] ID3D12DescriptorHeap* cbv_srv_uav_heap() const noexcept { return cbv_srv_uav_heap_; }
    [[nodiscard]] ID3D12DescriptorHeap* sampler_heap() const noexcept { return sampler_heap_; }
    [[nodiscard]] ID3D12RootSignature* bindless_root_sig() const noexcept { return bindless_root_sig_; }
    [[nodiscard]] ID3D12RootSignature* rt_bindless_root_sig() const noexcept { return rt_bindless_root_sig_; }

    [[nodiscard]] bool d3d11on12_active() const noexcept { return d3d11on12_device_ != nullptr; }
    [[nodiscard]] ID3D11Device* d3d11_device() const noexcept { return d3d11_device_; }
    [[nodiscard]] ID3D11DeviceContext* d3d11_context() const noexcept { return d3d11_context_; }
    [[nodiscard]] ID3D11On12Device* d3d11on12_device() const noexcept { return d3d11on12_device_; }

    /** Triple-buffered frame lifecycle (zero allocations). */
    [[nodiscard]] Expected<ID3D12GraphicsCommandList4*> begin_direct_frame(uint32_t& out_frame_slot) noexcept;
    [[nodiscard]] Expected<uint64_t> submit_direct_frame(uint32_t frame_slot) noexcept;
    [[nodiscard]] bool wait_direct_fence(uint64_t fence_val, uint32_t timeout_ms = 5000u) noexcept;
    void wait_idle() noexcept;

    /** Allocates a DX12 buffer. On UMA, `UploadCpu` is directly zero-copy GPU-readable! */
    [[nodiscard]] Expected<D3D12Buffer> create_buffer(uint64_t size, D3D12BufferUsage usage,
                                                      D3D12BufferHeap heap = D3D12BufferHeap::DefaultGpu,
                                                      bool create_srv = true, bool create_uav = false) noexcept;
    void destroy_buffer(D3D12Buffer& buf) noexcept;

    /** Allocates a 2D texture + RTV/DSV/SRV/UAV views + optional NT shared handle & D3D11On12 wrap. */
    [[nodiscard]] Expected<D3D12Texture> create_texture_2d(uint32_t width, uint32_t height, int dxgi_format,
                                                           bool allow_rtv, bool allow_dsv, bool allow_uav,
                                                           bool shared_nt = false, bool wrap_d3d11 = false,
                                                           const float* clear_rgba = nullptr) noexcept;
    /** Creates `D32_FLOAT_S8X24_UINT` with separate depth SRV (`srv_index`) and stencil `.x` SRV (`stencil_srv_index`). */
    [[nodiscard]] Expected<D3D12Texture> create_depth_stencil_s8(uint32_t width, uint32_t height) noexcept;
    void destroy_texture(D3D12Texture& tex) noexcept;

    /** Creates an SM 6.6 Compute or Graphics PSO sharing `bindless_root_sig_` (or `rt_bindless_root_sig_`). */
    [[nodiscard]] Expected<ID3D12PipelineState*> create_compute_pso(std::span<const uint8_t> cs_dxil,
                                                                    bool uses_rt_tlas = false) noexcept;
    [[nodiscard]] Expected<ID3D12PipelineState*> create_graphics_pso(const D3D12GraphicsPipelineDesc& desc) noexcept;
    static void destroy_pso(ID3D12PipelineState*& pso) noexcept;

    /** Creates an SM 6.6 static texture uploaded synchronously from CPU staging bytes (init/avatar upload). */
    [[nodiscard]] Expected<D3D12Texture> create_texture_2d_from_data(uint32_t width, uint32_t height, int dxgi_format,
                                                                     std::span<const uint8_t> pixels,
                                                                     uint32_t bytes_per_pixel_or_block_row) noexcept;

    /** Binds shader-visible CBV_SRV_UAV & Sampler descriptor heaps + bindless root signature on `cmd`. */
    void cmd_bind_bindless_compute(ID3D12GraphicsCommandList4* cmd, bool uses_rt_tlas = false,
                                   uint64_t tlas_gpu_va = 0ULL) const noexcept;
    void cmd_bind_bindless_graphics(ID3D12GraphicsCommandList4* cmd) const noexcept;
    void cmd_transition_buffer(ID3D12GraphicsCommandList4* cmd, D3D12Buffer& buf, int target_state) noexcept;
    void cmd_transition_texture(ID3D12GraphicsCommandList4* cmd, D3D12Texture& tex, int target_state) noexcept;
    void cmd_uav_barrier(ID3D12GraphicsCommandList4* cmd, ID3D12Resource* res = nullptr) noexcept;

    /** Executes one synchronous upload/transfer command list cleanly (used only during asset upload/init). */
    template <typename Fn>
    [[nodiscard]] Expected<void> execute_immediate(Fn&& record_fn) noexcept {
        if (device_ == nullptr || direct_queue_ == nullptr || imm_alloc_ == nullptr || imm_list_ == nullptr) {
            return make_unexpected(Error::make(ErrorCode::InvalidArg, "D3D12Device::execute_immediate not initialized"));
        }
        if (auto r = reset_immediate_list(); !r) {
            return r;
        }
        record_fn(imm_list_);
        return submit_and_wait_immediate();
    }

    /** Descriptor CPU handle lookups for OMSetRenderTargets / ClearRTV / ClearDSV. */
    void cpu_rtv_handle(uint32_t index, void* out_d3d12_cpu_handle) const noexcept;
    void cpu_dsv_handle(uint32_t index, void* out_d3d12_cpu_handle) const noexcept;
    void gpu_srv_handle(uint32_t index, void* out_d3d12_gpu_handle) const noexcept;
    void cpu_srv_staging_handle(uint32_t index, void* out_d3d12_cpu_handle) const noexcept;

    /** D3D11On12 Spout2 synchronization wrappers around `d3d11_wrapped`. */
    bool acquire_d3d11on12_resource(const D3D12Texture& tex) noexcept;
    bool release_and_flush_d3d11on12_resource(const D3D12Texture& tex) noexcept;

    /** Queries current validation error count (debug builds) and updates device-lost status. */
    [[nodiscard]] uint64_t poll_debug_errors() noexcept;
    [[nodiscard]] bool is_device_lost() const noexcept { return device_lost_.load(std::memory_order_acquire); }

  private:
    Expected<void> init_internal(const D3D12DeviceConfig& cfg);
    Expected<void> create_descriptor_heaps_and_samplers();
    Expected<void> create_root_signatures();
    Expected<void> create_command_objects();
    Expected<void> create_d3d11on12_bridge();
    Expected<void> reset_immediate_list() noexcept;
    Expected<void> submit_and_wait_immediate() noexcept;

    D3D12AdapterDescriptor gpu_info_{};
    bool debug_layer_active_{false};
    std::atomic<bool> device_lost_{false};
    std::atomic<uint64_t> validation_errors_{0};

    IDXGIFactory6* factory_{nullptr};
    IDXGIAdapter4* adapter_{nullptr};
    ID3D12Device5* device_{nullptr};

    ID3D12CommandQueue* direct_queue_{nullptr};
    ID3D12CommandQueue* compute_queue_{nullptr};
    ID3D12CommandQueue* copy_queue_{nullptr};

    ID3D12Fence* direct_fence_{nullptr};
    void* direct_fence_event_{nullptr};
    uint64_t next_direct_fence_{1};
    uint32_t frame_cursor_{0};
    std::array<uint64_t, kD3D12FrameCount> frame_fence_values_{};
    std::array<ID3D12CommandAllocator*, kD3D12FrameCount> frame_allocators_{};
    ID3D12GraphicsCommandList4* direct_cmd_{nullptr};

    ID3D12CommandAllocator* imm_alloc_{nullptr};
    ID3D12GraphicsCommandList4* imm_list_{nullptr};

    ID3D12DescriptorHeap* cbv_srv_uav_heap_{nullptr};
    ID3D12DescriptorHeap* cbv_srv_uav_cpu_staging_{nullptr};
    ID3D12DescriptorHeap* sampler_heap_{nullptr};
    ID3D12DescriptorHeap* rtv_heap_{nullptr};
    ID3D12DescriptorHeap* dsv_heap_{nullptr};
    uint32_t cbv_srv_uav_inc_{0};
    uint32_t sampler_inc_{0};
    uint32_t rtv_inc_{0};
    uint32_t dsv_inc_{0};

    D3D12DescriptorPool<kCbvSrvUavHeapCapacity> srv_pool_{};
    D3D12DescriptorPool<kRtvHeapCapacity> rtv_pool_{};
    D3D12DescriptorPool<kDsvHeapCapacity> dsv_pool_{};

    D3D12Buffer null_dummy_buf_{};

    ID3D12RootSignature* bindless_root_sig_{nullptr};
    ID3D12RootSignature* rt_bindless_root_sig_{nullptr};

    ID3D11Device* d3d11_device_{nullptr};
    ID3D11DeviceContext* d3d11_context_{nullptr};
    ID3D11On12Device* d3d11on12_device_{nullptr};
};

} // namespace aether::rhi
