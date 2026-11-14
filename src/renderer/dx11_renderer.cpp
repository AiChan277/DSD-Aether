#include "dx11_renderer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <cstring>

namespace aether::renderer {

namespace {

static_assert(sizeof(AetherVertex25D) == 64, "AetherVertex25D must be exactly 64 bytes (HR-03)");

struct alignas(16) HlslNprConstants {
    std::array<float, 16> view_proj{};
    std::array<float, 4> light_dir_flatness{};
    std::array<float, 4> lit_color{};
    std::array<float, 4> shadow_color{};
    std::array<float, 4> rim_color_strength{};
};

static_assert(sizeof(HlslNprConstants) == 128, "HlslNprConstants must be a multiple of 16 bytes for D3D11 cbuffers");

constexpr const char kHlsl25dNprSource[] = R"(
cbuffer NprFrameConstants : register(b0) {
    row_major float4x4 u_view_proj;
    float4 u_light_dir_flatness;
    float4 u_lit_color;
    float4 u_shadow_color;
    float4 u_rim_color_strength;
};

struct VSInput {
    float4 pos_outline : POSITION0;
    float4 true_norm_flat : NORMAL0;
    float4 proxy_norm_sdf : NORMAL1;
    float4 smooth_tangent : TANGENT0;
};

struct PSInput {
    float4 sv_pos : SV_POSITION;
    float3 true_norm : TEXCOORD0;
    float3 proxy_norm : TEXCOORD1;
    float sdf_mask : TEXCOORD2;
};

PSInput vs_main(VSInput vin) {
    PSInput vout;
    float flatten = clamp(vin.true_norm_flat.w * u_light_dir_flatness.w, 0.0, 0.85);
    float3 pos = vin.pos_outline.xyz;
    pos.z *= (1.0 - flatten);
    vout.sv_pos = mul(float4(pos, 1.0), u_view_proj);
    vout.true_norm = normalize(vin.true_norm_flat.xyz + float3(0.0, 0.0, 1e-5));
    vout.proxy_norm = normalize(vin.proxy_norm_sdf.xyz + float3(0.0, 0.0, 1e-5));
    vout.sdf_mask = vin.proxy_norm_sdf.w;
    return vout;
}

float4 ps_main(PSInput pin) : SV_Target0 {
    float3 L = normalize(-u_light_dir_flatness.xyz + float3(0.0, 0.0, 1e-5));
    float3 N = normalize(lerp(pin.true_norm, pin.proxy_norm, 0.65));
    float ndl = dot(N, L) * 0.5 + 0.5;
    float toon_ramp = smoothstep(0.42, 0.55, ndl);
    float3 base_rgb = lerp(u_shadow_color.rgb, u_lit_color.rgb, toon_ramp);

    float3 V = float3(0.0, 0.0, -1.0);
    float rim = pow( saturate(1.0 - abs(dot(pin.true_norm, V))), 3.0 ) * u_rim_color_strength.w;
    float3 final_rgb = saturate(base_rgb + u_rim_color_strength.rgb * rim);
    float alpha = saturate(u_lit_color.a);
    return float4(final_rgb * alpha, alpha);
}
)";

template <typename T> void safe_release(T*& ptr) noexcept {
    if (ptr != nullptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

} // namespace

Dx11NprRenderer::~Dx11NprRenderer() noexcept {
    shutdown();
}

Expected<void> Dx11NprRenderer::init(const Dx11RendererConfig& cfg) {
    shutdown();
    if (cfg.width == 0U || cfg.height == 0U || cfg.max_vertices == 0U || cfg.max_indices == 0U) {
        return make_unexpected(
            Error::make(ErrorCode::InvalidArg, "Dx11NprRenderer::init received zero dimensions or capacity"));
    }
    cfg_ = cfg;

    constexpr std::array<D3D_FEATURE_LEVEL, 2> kFeatureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL achieved_level = D3D_FEATURE_LEVEL_11_0;
    constexpr UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, create_flags, kFeatureLevels.data(),
                                   static_cast<UINT>(kFeatureLevels.size()), D3D11_SDK_VERSION, &device_,
                                   &achieved_level, &context_);
    is_warp_ = false;
    if (FAILED(hr) && cfg_.allow_warp_fallback) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, create_flags, kFeatureLevels.data(),
                               static_cast<UINT>(kFeatureLevels.size()), D3D11_SDK_VERSION, &device_, &achieved_level,
                               &context_);
        is_warp_ = SUCCEEDED(hr);
    }
    if (FAILED(hr) || device_ == nullptr || context_ == nullptr) {
        shutdown();
        return make_unexpected(Error::make(ErrorCode::Internal, "D3D11CreateDevice failed for Dx11NprRenderer"));
    }

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    ID3DBlob* err_blob = nullptr;

    hr = D3DCompile(kHlsl25dNprSource, sizeof(kHlsl25dNprSource) - 1U, "aether_25d_npr.hlsl", nullptr, nullptr,
                    "vs_main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_blob, &err_blob);
    safe_release(err_blob);
    if (FAILED(hr) || vs_blob == nullptr) {
        safe_release(vs_blob);
        shutdown();
        return make_unexpected(Error::make(ErrorCode::Internal, "D3DCompile(vs_5_0) failed for Dx11NprRenderer"));
    }

    hr = D3DCompile(kHlsl25dNprSource, sizeof(kHlsl25dNprSource) - 1U, "aether_25d_npr.hlsl", nullptr, nullptr,
                    "ps_main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_blob, &err_blob);
    safe_release(err_blob);
    if (FAILED(hr) || ps_blob == nullptr) {
        safe_release(vs_blob);
        safe_release(ps_blob);
        shutdown();
        return make_unexpected(Error::make(ErrorCode::Internal, "D3DCompile(ps_5_0) failed for Dx11NprRenderer"));
    }

    hr = device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_);
    if (SUCCEEDED(hr)) {
        hr = device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &ps_);
    }

    constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 4> kLayoutDesc{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
    }};
    if (SUCCEEDED(hr)) {
        hr = device_->CreateInputLayout(kLayoutDesc.data(), static_cast<UINT>(kLayoutDesc.size()),
                                        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &input_layout_);
    }
    safe_release(vs_blob);
    safe_release(ps_blob);
    if (FAILED(hr)) {
        shutdown();
        return make_unexpected(
            Error::make(ErrorCode::Internal, "Failed to create D3D11 shaders or 64-byte AetherVertex25D layout"));
    }

    D3D11_BUFFER_DESC vb_desc{};
    vb_desc.ByteWidth = static_cast<UINT>(cfg_.max_vertices * sizeof(AetherVertex25D));
    vb_desc.Usage = D3D11_USAGE_DYNAMIC;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&vb_desc, nullptr, &vertex_buffer_);
    if (FAILED(hr)) {
        shutdown();
        return make_unexpected(Error::make(ErrorCode::Internal, "Failed to create D3D11 dynamic vertex buffer"));
    }

    D3D11_BUFFER_DESC ib_desc{};
    ib_desc.ByteWidth = static_cast<UINT>(cfg_.max_indices * sizeof(uint32_t));
    ib_desc.Usage = D3D11_USAGE_DYNAMIC;
    ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ib_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&ib_desc, nullptr, &index_buffer_);
    if (FAILED(hr)) {
        shutdown();
        return make_unexpected(Error::make(ErrorCode::Internal, "Failed to create D3D11 dynamic index buffer"));
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = sizeof(HlslNprConstants);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&cb_desc, nullptr, &constant_buffer_);
    if (FAILED(hr)) {
        shutdown();
        return make_unexpected(Error::make(ErrorCode::Internal, "Failed to create D3D11 NPR constant buffer"));
    }

    D3D11_RASTERIZER_DESC rs_desc{};
    rs_desc.FillMode = D3D11_FILL_SOLID;
    rs_desc.CullMode = D3D11_CULL_NONE;
    rs_desc.FrontCounterClockwise = TRUE;
    rs_desc.DepthClipEnable = TRUE;
    hr = device_->CreateRasterizerState(&rs_desc, &raster_state_);
    if (FAILED(hr)) {
        shutdown();
        return make_unexpected(Error::make(ErrorCode::Internal, "Failed to create D3D11 rasterizer state"));
    }

    D3D11_BLEND_DESC bs_desc{};
    bs_desc.RenderTarget[0].BlendEnable = TRUE;
    bs_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bs_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bs_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bs_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bs_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bs_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bs_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device_->CreateBlendState(&bs_desc, &blend_state_);
    if (FAILED(hr)) {
        shutdown();
        return make_unexpected(
            Error::make(ErrorCode::Internal, "Failed to create D3D11 premultiplied alpha blend state"));
    }

    D3D11_DEPTH_STENCIL_DESC ds_desc{};
    ds_desc.DepthEnable = TRUE;
    ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    hr = device_->CreateDepthStencilState(&ds_desc, &depth_state_);
    if (FAILED(hr)) {
        shutdown();
        return make_unexpected(Error::make(ErrorCode::Internal, "Failed to create D3D11 depth stencil state"));
    }

    auto rt_res = create_render_targets(cfg_.width, cfg_.height);
    if (!rt_res.has_value()) {
        shutdown();
        return rt_res;
    }

    initialized_ = true;
    return {};
}

Expected<void> Dx11NprRenderer::create_render_targets(uint32_t width, uint32_t height) {
    release_render_targets();

    D3D11_TEXTURE2D_DESC color_desc{};
    color_desc.Width = width;
    color_desc.Height = height;
    color_desc.MipLevels = 1;
    color_desc.ArraySize = 1;
    color_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color_desc.SampleDesc.Count = 1;
    color_desc.Usage = D3D11_USAGE_DEFAULT;
    color_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    color_desc.MiscFlags = (cfg_.enable_shared_texture && !is_warp_) ? D3D11_RESOURCE_MISC_SHARED : 0U;

    HRESULT hr = device_->CreateTexture2D(&color_desc, nullptr, &color_tex_);
    if (FAILED(hr) && color_desc.MiscFlags != 0U) {
        color_desc.MiscFlags = 0U;
        hr = device_->CreateTexture2D(&color_desc, nullptr, &color_tex_);
    }
    if (FAILED(hr) || color_tex_ == nullptr) {
        return make_unexpected(Error::make(ErrorCode::Internal, "Failed to create D3D11 RGBA8 color target"));
    }

    hr = device_->CreateRenderTargetView(color_tex_, nullptr, &rtv_);
    if (FAILED(hr) || rtv_ == nullptr) {
        release_render_targets();
        return make_unexpected(Error::make(ErrorCode::Internal, "Failed to create D3D11 RenderTargetView"));
    }

    if ((color_desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED) != 0U) {
        IDXGIResource* dxgi_res = nullptr;
        void* raw_res = nullptr;
        if (SUCCEEDED(color_tex_->QueryInterface(__uuidof(IDXGIResource), &raw_res)) && raw_res != nullptr) {
            dxgi_res = static_cast<IDXGIResource*>(raw_res);
            HANDLE h_shared = nullptr;
            if (SUCCEEDED(dxgi_res->GetSharedHandle(&h_shared))) {
                shared_handle_ = h_shared;
            }
            dxgi_res->Release();
        }
    }

    D3D11_TEXTURE2D_DESC depth_desc{};
    depth_desc.Width = width;
    depth_desc.Height = height;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = device_->CreateTexture2D(&depth_desc, nullptr, &depth_tex_);
    if (SUCCEEDED(hr) && depth_tex_ != nullptr) {
        hr = device_->CreateDepthStencilView(depth_tex_, nullptr, &dsv_);
    }
    if (FAILED(hr) || dsv_ == nullptr) {
        release_render_targets();
        return make_unexpected(Error::make(ErrorCode::Internal, "Failed to create D3D11 depth-stencil target"));
    }

    D3D11_TEXTURE2D_DESC stage_desc{};
    stage_desc.Width = width;
    stage_desc.Height = height;
    stage_desc.MipLevels = 1;
    stage_desc.ArraySize = 1;
    stage_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    stage_desc.SampleDesc.Count = 1;
    stage_desc.Usage = D3D11_USAGE_STAGING;
    stage_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device_->CreateTexture2D(&stage_desc, nullptr, &staging_tex_);
    if (FAILED(hr) || staging_tex_ == nullptr) {
        release_render_targets();
        return make_unexpected(Error::make(ErrorCode::Internal, "Failed to create D3D11 staging readback target"));
    }

    cfg_.width = width;
    cfg_.height = height;
    return {};
}

void Dx11NprRenderer::release_render_targets() noexcept {
    shared_handle_ = nullptr;
    safe_release(staging_tex_);
    safe_release(dsv_);
    safe_release(depth_tex_);
    safe_release(rtv_);
    safe_release(color_tex_);
}

Expected<void> Dx11NprRenderer::resize(uint32_t width, uint32_t height) {
    if (!initialized_ || device_ == nullptr) {
        return make_unexpected(Error::make(ErrorCode::Internal, "Dx11NprRenderer::resize called before init"));
    }
    if (width == 0U || height == 0U) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Dx11NprRenderer::resize dimensions must be > 0"));
    }
    if (width == cfg_.width && height == cfg_.height) {
        return {};
    }
    return create_render_targets(width, height);
}

void Dx11NprRenderer::shutdown() noexcept {
    release_render_targets();
    safe_release(depth_state_);
    safe_release(blend_state_);
    safe_release(raster_state_);
    safe_release(constant_buffer_);
    safe_release(index_buffer_);
    safe_release(vertex_buffer_);
    safe_release(input_layout_);
    safe_release(ps_);
    safe_release(vs_);
    safe_release(context_);
    safe_release(device_);
    initialized_ = false;
    is_warp_ = false;
    rendered_frames_ = 0;
}

bool Dx11NprRenderer::render_npr_mesh(std::span<const AetherVertex25D> vertices, std::span<const uint32_t> indices,
                                      const Dx11NprFrameParams& params) noexcept {
    if (!initialized_ || context_ == nullptr || rtv_ == nullptr || dsv_ == nullptr) {
        return false;
    }
    if (vertices.empty() || indices.empty() || vertices.size() > cfg_.max_vertices ||
        indices.size() > cfg_.max_indices) {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped_vb{};
    if (FAILED(context_->Map(vertex_buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_vb)) ||
        mapped_vb.pData == nullptr) {
        return false;
    }
    std::memcpy(mapped_vb.pData, vertices.data(), vertices.size() * sizeof(AetherVertex25D));
    context_->Unmap(vertex_buffer_, 0);

    D3D11_MAPPED_SUBRESOURCE mapped_ib{};
    if (FAILED(context_->Map(index_buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_ib)) || mapped_ib.pData == nullptr) {
        return false;
    }
    std::memcpy(mapped_ib.pData, indices.data(), indices.size() * sizeof(uint32_t));
    context_->Unmap(index_buffer_, 0);

    D3D11_MAPPED_SUBRESOURCE mapped_cb{};
    if (FAILED(context_->Map(constant_buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_cb)) ||
        mapped_cb.pData == nullptr) {
        return false;
    }
    HlslNprConstants cb_data{};
    cb_data.view_proj = params.view_proj;
    cb_data.light_dir_flatness = params.light_dir_flatness;
    cb_data.lit_color = params.lit_color;
    cb_data.shadow_color = params.shadow_color;
    cb_data.rim_color_strength = params.rim_color_strength;
    std::memcpy(mapped_cb.pData, &cb_data, sizeof(cb_data));
    context_->Unmap(constant_buffer_, 0);

    context_->ClearRenderTargetView(rtv_, params.clear_color.data());
    context_->ClearDepthStencilView(dsv_, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(cfg_.width);
    vp.Height = static_cast<float>(cfg_.height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(raster_state_);

    context_->OMSetRenderTargets(1, &rtv_, dsv_);
    constexpr std::array<float, 4> kBlendFactor{0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetBlendState(blend_state_, kBlendFactor.data(), 0xFFFFFFFFU);
    context_->OMSetDepthStencilState(depth_state_, 0);

    const UINT stride = sizeof(AetherVertex25D);
    const UINT offset = 0;
    context_->IASetInputLayout(input_layout_);
    context_->IASetVertexBuffers(0, 1, &vertex_buffer_, &stride, &offset);
    context_->IASetIndexBuffer(index_buffer_, DXGI_FORMAT_R32_UINT, 0);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    context_->VSSetShader(vs_, nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &constant_buffer_);
    context_->PSSetShader(ps_, nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &constant_buffer_);

    context_->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);
    context_->Flush();

    ++rendered_frames_;
    return true;
}

bool Dx11NprRenderer::readback_rgba8(std::span<uint8_t> out_rgba) noexcept {
    if (!initialized_ || context_ == nullptr || color_tex_ == nullptr || staging_tex_ == nullptr) {
        return false;
    }
    const size_t row_bytes = static_cast<size_t>(cfg_.width) * 4U;
    const size_t required_bytes = row_bytes * static_cast<size_t>(cfg_.height);
    if (out_rgba.size() < required_bytes) {
        return false;
    }

    context_->CopyResource(staging_tex_, color_tex_);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_tex_, 0, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr) {
        return false;
    }

    const auto* src_bytes = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t y = 0; y < cfg_.height; ++y) {
        const size_t src_off = static_cast<size_t>(y) * static_cast<size_t>(mapped.RowPitch);
        const size_t dst_off = static_cast<size_t>(y) * row_bytes;
        std::memcpy(out_rgba.data() + dst_off, src_bytes + src_off, row_bytes);
    }
    context_->Unmap(staging_tex_, 0);
    return true;
}

} // namespace aether::renderer
