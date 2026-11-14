#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "aether_ffi_contract.h"
#include "core/expected.hpp"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11InputLayout;
struct ID3D11Buffer;
struct ID3D11RasterizerState;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11Query;

namespace aether::renderer {

/** Configuration for the DirectX 11 2.5D NPR Renderer & DXGI Interop Backend (ADR-0015). */
struct Dx11RendererConfig {
    uint32_t width{1920};
    uint32_t height{1080};
    uint32_t max_vertices{16384};
    uint32_t max_indices{49152};
    bool allow_warp_fallback{true};
    bool enable_shared_texture{true};
    // DXGI adapter LUID (HighPart << 32 | LowPart); 0 = system default adapter.
    uint64_t adapter_luid{0};
    // true: pixel shader converts authored sRGB colours to linear light (Vulkan composite contract).
    bool linear_output{false};
};

/** One indexed draw with its own cel colours (one per avatar sub-mesh / material). */
struct Dx11DrawRange {
    uint32_t index_offset{0};
    uint32_t index_count{0};
    int32_t base_vertex{0};
    std::array<float, 4> lit_color{0.94f, 0.86f, 0.84f, 1.0f};
    std::array<float, 4> shadow_color{0.62f, 0.52f, 0.58f, 1.0f};
};

/** Per-frame 2.5D NPR cel/toon shading parameters (16-byte aligned HLSL cbuffer layout). */
struct alignas(16) Dx11NprFrameParams {
    std::array<float, 16> view_proj{
        1.0f, 0.0f, 0.0f, 0.0f, // col 0
        0.0f, 1.0f, 0.0f, 0.0f, // col 1
        0.0f, 0.0f, 1.0f, 0.0f, // col 2
        0.0f, 0.0f, 0.0f, 1.0f, // col 3
    };
    // xyz = normalized key light direction, w = global z-flatten factor [0, 0.85]
    std::array<float, 4> light_dir_flatness{0.35f, 0.65f, -0.67f, 0.50f};
    // Lit cel color (RGBA linear/sRGB)
    std::array<float, 4> lit_color{0.94f, 0.86f, 0.84f, 1.0f};
    // Shade cel color (RGBA)
    std::array<float, 4> shadow_color{0.62f, 0.52f, 0.58f, 1.0f};
    // xyz = rim color, w = rim intensity
    std::array<float, 4> rim_color_strength{0.38f, 0.72f, 0.96f, 0.45f};
    // Clear background color (premultiplied RGBA)
    std::array<float, 4> clear_color{0.0f, 0.0f, 0.0f, 0.0f};
};

/**
 * DirectX 11 2.5D NPR Renderer & DXGI Shared-Texture Backend (ADR-0015).
 *
 * Provides native Direct3D 11 2.5D cel/toon rendering over the frozen 64-byte `AetherVertex25D`
 * contract (`HR-03`) with zero per-frame heap allocations (`HR-01`) and zero blocking locks
 * (`HR-02`), plus DXGI shared-resource export for hybrid Vulkan/DirectX compositing.
 */
class Dx11NprRenderer {
  public:
    Dx11NprRenderer() noexcept = default;
    ~Dx11NprRenderer() noexcept;

    Dx11NprRenderer(const Dx11NprRenderer&) = delete;
    Dx11NprRenderer& operator=(const Dx11NprRenderer&) = delete;

    [[nodiscard]] Expected<void> init(const Dx11RendererConfig& cfg);
    void shutdown() noexcept;

    [[nodiscard]] Expected<void> resize(uint32_t width, uint32_t height);

    /**
     * Renders an indexed 2.5D NPR mesh (`AetherVertex25D` 64 B stride) into the premultiplied-alpha
     * D3D11 render target. Allocation-free and lock-free (`HR-01`, `HR-02`).
     */
    [[nodiscard]] bool render_npr_mesh(std::span<const AetherVertex25D> vertices, std::span<const uint32_t> indices,
                                       const Dx11NprFrameParams& params) noexcept;

    /**
     * Uploads the mesh once and issues one draw per range, each with its own lit/shade colours
     * (`params.lit_color` / `params.shadow_color` are ignored). Allocation-free (`HR-01`).
     */
    [[nodiscard]] bool render_npr_draws(std::span<const AetherVertex25D> vertices, std::span<const uint32_t> indices,
                                        std::span<const Dx11DrawRange> draws,
                                        const Dx11NprFrameParams& params) noexcept;

    /** Blocks until all submitted D3D11 work has completed (event query). Returns false on timeout. */
    [[nodiscard]] bool wait_for_gpu(uint32_t timeout_ms = 100) noexcept;

    /**
     * Copies the rendered RGBA8 target into `out_rgba` (`width * height * 4` bytes) via the
     * pre-allocated staging texture. Allocation-free (`HR-01`).
     */
    [[nodiscard]] bool readback_rgba8(std::span<uint8_t> out_rgba) noexcept;

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }
    [[nodiscard]] bool is_warp_device() const noexcept { return is_warp_; }
    [[nodiscard]] uint32_t width() const noexcept { return cfg_.width; }
    [[nodiscard]] uint32_t height() const noexcept { return cfg_.height; }
    [[nodiscard]] uint64_t rendered_frames() const noexcept { return rendered_frames_; }
    [[nodiscard]] uint32_t max_vertices() const noexcept { return cfg_.max_vertices; }
    [[nodiscard]] uint32_t max_indices() const noexcept { return cfg_.max_indices; }
    [[nodiscard]] void* shared_dxgi_handle() const noexcept { return shared_handle_; }
    [[nodiscard]] ID3D11Device* device() const noexcept { return device_; }
    [[nodiscard]] ID3D11Texture2D* color_target() const noexcept { return color_tex_; }

  private:
    [[nodiscard]] Expected<void> create_render_targets(uint32_t width, uint32_t height);
    void release_render_targets() noexcept;
    [[nodiscard]] bool write_constants(const Dx11NprFrameParams& params, const std::array<float, 4>& lit,
                                       const std::array<float, 4>& shade) noexcept;

    Dx11RendererConfig cfg_{};
    bool initialized_{false};
    bool is_warp_{false};
    uint64_t rendered_frames_{0};
    void* shared_handle_{nullptr};

    ID3D11Device* device_{nullptr};
    ID3D11DeviceContext* context_{nullptr};
    ID3D11Texture2D* color_tex_{nullptr};
    ID3D11RenderTargetView* rtv_{nullptr};
    ID3D11Texture2D* depth_tex_{nullptr};
    ID3D11DepthStencilView* dsv_{nullptr};
    ID3D11Texture2D* staging_tex_{nullptr};

    ID3D11VertexShader* vs_{nullptr};
    ID3D11PixelShader* ps_{nullptr};
    ID3D11InputLayout* input_layout_{nullptr};
    ID3D11Buffer* vertex_buffer_{nullptr};
    ID3D11Buffer* index_buffer_{nullptr};
    ID3D11Buffer* constant_buffer_{nullptr};

    ID3D11RasterizerState* raster_state_{nullptr};
    ID3D11BlendState* blend_state_{nullptr};
    ID3D11DepthStencilState* depth_state_{nullptr};
    ID3D11Query* idle_query_{nullptr};
};

} // namespace aether::renderer
