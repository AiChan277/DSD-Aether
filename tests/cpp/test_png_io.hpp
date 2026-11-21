#pragma once

#include "aether_ffi_contract.h"
#include "core/clock.hpp"
#include "core/expected.hpp"
#include "renderer/asset_upload/asset_job.hpp"
#include "renderer/asset_upload/gpu_avatar.hpp"
#include "renderer/lighting_rig.hpp"
#include "renderer/preview_pass.hpp"
#include "rhi/rhi_device.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <objbase.h>
#include <wincodec.h>
#include <windows.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace aether::test {

struct LoadedPng {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> rgba{};
};

inline void init_com_once() noexcept {
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}

[[nodiscard]] inline bool save_rgba8_png(const std::filesystem::path& path, uint32_t width, uint32_t height,
                                         const uint8_t* rgba) {
    if (width == 0 || height == 0 || rgba == nullptr) {
        return false;
    }
    init_com_once();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return false;
    }
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;

    bool ok = false;
    do {
        if (FAILED(factory->CreateStream(&stream))) {
            break;
        }
        if (FAILED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE))) {
            break;
        }
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) {
            break;
        }
        if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
            break;
        }
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) {
            break;
        }
        if (FAILED(frame->Initialize(nullptr))) {
            break;
        }
        if (FAILED(frame->SetSize(width, height))) {
            break;
        }
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&fmt)) || !IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) {
            break;
        }
        const UINT stride = width * 4U;
        const UINT size = stride * height;
        std::vector<uint8_t> bgra(size);
        for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
            bgra[i * 4 + 0] = rgba[i * 4 + 2];
            bgra[i * 4 + 1] = rgba[i * 4 + 1];
            bgra[i * 4 + 2] = rgba[i * 4 + 0];
            bgra[i * 4 + 3] = rgba[i * 4 + 3];
        }
        if (FAILED(frame->WritePixels(height, stride, size, bgra.data()))) {
            break;
        }
        if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
            break;
        }
        ok = true;
    } while (false);

    if (frame != nullptr) {
        frame->Release();
    }
    if (encoder != nullptr) {
        encoder->Release();
    }
    if (stream != nullptr) {
        stream->Release();
    }
    factory->Release();
    return ok;
}

[[nodiscard]] inline Expected<LoadedPng> load_rgba8_png(const std::filesystem::path& path) {
    init_com_once();
    if (!std::filesystem::is_regular_file(path)) {
        return Unexpected<Error>{Error::make(ErrorCode::IoError, "PNG file does not exist")};
    }
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return Unexpected<Error>{Error::make(ErrorCode::Internal, "CoCreateInstance(WIC) failed")};
    }

    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    LoadedPng out{};
    bool ok = false;

    do {
        if (FAILED(factory->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &decoder))) {
            break;
        }
        if (FAILED(decoder->GetFrame(0, &frame))) {
            break;
        }
        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) {
            break;
        }
        if (FAILED(factory->CreateFormatConverter(&converter))) {
            break;
        }
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom))) {
            break;
        }
        out.width = w;
        out.height = h;
        out.rgba.resize(static_cast<size_t>(w) * h * 4U);
        if (FAILED(converter->CopyPixels(nullptr, w * 4U, static_cast<UINT>(out.rgba.size()), out.rgba.data()))) {
            break;
        }
        ok = true;
    } while (false);

    if (converter != nullptr) {
        converter->Release();
    }
    if (frame != nullptr) {
        frame->Release();
    }
    if (decoder != nullptr) {
        decoder->Release();
    }
    factory->Release();

    if (!ok) {
        return Unexpected<Error>{Error::make(ErrorCode::IoError, "WIC PNG decode failed")};
    }
    return out;
}

[[nodiscard]] inline Expected<std::unique_ptr<renderer::GpuAvatar>> load_reference_vrm_avatar(rhi::RhiDevice& rhi) {
    const auto ref_vrm = std::filesystem::path(AETHER_REPO_ROOT) / "tests" / "data" / "reference_avatar.vrm";
    renderer::AssetJob job;
    job.start_load(rhi, ref_vrm.string());
    const uint64_t deadline = aether::now_us() + 15000000ULL;
    HighResTimer timer;
    while (job.state() == renderer::AssetJobState::Loading && aether::now_us() < deadline) {
        timer.sleep_us(500);
    }
    if (job.state() != renderer::AssetJobState::ReadyForGpu) {
        return Unexpected<Error>{Error::make(ErrorCode::IoError, job.last_error())};
    }
    return job.finalize_gpu_upload(rhi);
}

struct CanonicalSceneSpec {
    const char* name{nullptr};
    renderer::PreviewCamera camera{};
    renderer::LightingRig rig{};
    bool checkerboard{false};
    bool boost_emission_bloom{false};
};

[[nodiscard]] inline std::vector<CanonicalSceneSpec> make_canonical_scenes() {
    std::vector<CanonicalSceneSpec> scenes;

    {
        CanonicalSceneSpec s{};
        s.name = "front_key";
        s.camera = renderer::PreviewCamera{0.0f, 1.38f, 0.0f, 0.0f, 0.05f, 1.25f, 0.55f};
        s.rig = renderer::LightingRig::default_three_point();
        scenes.push_back(s);
    }
    {
        CanonicalSceneSpec s{};
        s.name = "three_quarter_left";
        s.camera = renderer::PreviewCamera{0.0f, 1.35f, 0.0f, -0.55f, 0.08f, 1.30f, 0.55f};
        s.rig = renderer::LightingRig::default_three_point();
        s.rig.studio_lights[0].orbit_azimuth_deg = -45.0f;
        scenes.push_back(s);
    }
    {
        CanonicalSceneSpec s{};
        s.name = "rim_backlight";
        s.camera = renderer::PreviewCamera{0.0f, 1.32f, 0.0f, 0.25f, 0.04f, 1.35f, 0.55f};
        s.rig = renderer::LightingRig::default_three_point();
        s.rig.studio_lights[0].intensity_ev100 = 0.45f;
        s.rig.studio_lights[2].intensity_ev100 = 3.10f;
        s.rig.studio_lights[2].orbit_azimuth_deg = 175.0f;
        scenes.push_back(s);
    }
    {
        CanonicalSceneSpec s{};
        s.name = "closeup_zflatten";
        s.camera = renderer::PreviewCamera{0.0f, 1.48f, 0.0f, 0.35f, 0.03f, 0.62f, 0.60f};
        s.rig = renderer::LightingRig::default_three_point();
        s.rig.global_live2d_flatness = 0.75f;
        scenes.push_back(s);
    }
    {
        CanonicalSceneSpec s{};
        s.name = "hair_shadow_sweep";
        s.camera = renderer::PreviewCamera{0.0f, 1.46f, 0.0f, 0.08f, 0.05f, 0.72f, 0.52f};
        s.rig = renderer::LightingRig::default_three_point();
        s.rig.studio_lights[0].orbit_azimuth_deg = 60.0f;
        s.rig.hair_shadow_offset_px = 6.0f;
        s.rig.hair_shadow_strength = 0.65f;
        scenes.push_back(s);
    }
    {
        CanonicalSceneSpec s{};
        s.name = "transparent_glow_checkerboard";
        s.camera = renderer::PreviewCamera{0.0f, 1.25f, 0.0f, 0.18f, 0.08f, 1.55f, 0.58f};
        s.rig = renderer::LightingRig::default_three_point();
        s.rig.bloom_threshold = 0.85f;
        s.rig.bloom_intensity = 0.45f;
        s.checkerboard = true;
        s.boost_emission_bloom = true;
        scenes.push_back(s);
    }

    return scenes;
}

} // namespace aether::test
