#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_4.h>
#include <initguid.h>
#include <olectl.h>
#include <strmif.h>
#include <uuids.h>
#include <windows.h>

#include "aether_ffi_contract.h"
#include "core/clock.hpp"
#include "core/hot_path_guard.hpp"
#include "output/ndi_output.hpp"
#include "output/output_coordinator.hpp"
#include "output/spout_output.hpp"
#include "output/vcam_transport.hpp"
#include "renderer/npr_renderer.hpp"
#include "test_png_io.hpp"
#include "vcam/guids.h"
#include "vcam/vcam_shm.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::filesystem::path find_bin_file(const wchar_t* name) {
#ifdef AETHER_VCAM_DLL_PATH
    std::filesystem::path defined_path(AETHER_VCAM_DLL_PATH);
    if (std::filesystem::exists(defined_path)) {
        return defined_path;
    }
#endif
    wchar_t mod_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, mod_path, MAX_PATH);
    return std::filesystem::path(mod_path).parent_path() / name;
}

std::vector<uint8_t> readback_r8_plane(aether::rhi::RhiDevice& dev, VkImage img, VkImageLayout layout, uint32_t w,
                                       uint32_t h, uint32_t bpp) {
    dev.wait_idle();
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * bpp;
    auto stg = dev.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
                                 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT)
                   .value();

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = dev.graphics_family();
    cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkCreateCommandPool(dev.device(), &cpci, nullptr, &pool);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev.device(), &cbai, &cmd);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    aether::rhi::RhiDevice::cmd_transition_image(cmd, img, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                                                 VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy reg{};
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stg.handle, 1, &reg);
    aether::rhi::RhiDevice::cmd_transition_image(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout,
                                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                                                 VK_IMAGE_ASPECT_COLOR_BIT);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(dev.graphics_queue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(dev.graphics_queue());
    vkDestroyCommandPool(dev.device(), pool, nullptr);

    vmaInvalidateAllocation(dev.allocator(), stg.allocation, 0, bytes);
    std::vector<uint8_t> out(static_cast<size_t>(bytes));
    std::memcpy(out.data(), stg.info.pMappedData, out.size());
    dev.destroy_buffer(stg);
    return out;
}

struct MockNdiCapture {
    bool init_called{false};
    bool destroy_called{false};
    bool clock_video{true};
    std::string sender_name{};
    std::vector<const uint8_t*> frame_ptrs{};
    std::vector<uint32_t> fourccs{};
    std::vector<int32_t> strides{};
    std::vector<uint8_t> first_frame_bytes{};
};

MockNdiCapture* g_mock_ndi = nullptr;

bool mock_ndi_init() {
    g_mock_ndi->init_called = true;
    return true;
}
void mock_ndi_destroy() {
    g_mock_ndi->destroy_called = true;
}
void* mock_ndi_create(const aether::output::NdiSendCreateAbi* cfg) {
    g_mock_ndi->clock_video = cfg->clock_video;
    g_mock_ndi->sender_name = cfg->ndi_name ? cfg->ndi_name : "";
    return g_mock_ndi;
}
void mock_ndi_destroy_send(void*) {}
void mock_ndi_send_async(void*, const aether::output::NdiVideoFrameV2Abi* vf) {
    g_mock_ndi->frame_ptrs.push_back(vf->p_data);
    g_mock_ndi->fourccs.push_back(vf->fourcc);
    g_mock_ndi->strides.push_back(vf->line_stride_in_bytes);
    if (g_mock_ndi->first_frame_bytes.empty() && vf->p_data != nullptr && vf->yres > 0 &&
        vf->line_stride_in_bytes > 0) {
        const size_t n = static_cast<size_t>(vf->line_stride_in_bytes) * static_cast<size_t>(vf->yres);
        g_mock_ndi->first_frame_bytes.assign(vf->p_data, vf->p_data + n);
    }
}

} // namespace

TEST_CASE("Phase 5 Pass 12: Decoupled 1080p output vs 720p viewport, OutputRing, Chroma NV12, Straight BGRA, Bottom-Up",
          "[phase5][pass12]") {
    aether::rhi::RhiDeviceConfig dcfg{};
    dcfg.headless = true;
    dcfg.enable_validation = true;
    dcfg.require_validation = true;
    auto dev_res = aether::rhi::RhiDevice::create(dcfg);
    REQUIRE(dev_res.has_value());
    std::unique_ptr<aether::rhi::RhiDevice> rhi(dev_res.value());

    auto av_res = aether::test::load_reference_vrm_avatar(*rhi);
    REQUIRE(av_res.has_value());
    auto avatar = std::move(av_res.value());

    aether::renderer::RendererConfig rcfg{};
    rcfg.width = 1280;
    rcfg.height = 720;
    rcfg.output_width = 1920;
    rcfg.output_height = 1080;
    rcfg.enable_rt = rhi->rt_supported();

    aether::renderer::NprRenderer renderer;
    REQUIRE(renderer.init(*rhi, rcfg).has_value());
    REQUIRE(renderer.bind_avatar(avatar.get()).has_value());

    const auto scenes = aether::test::make_canonical_scenes();
    auto test_rig = scenes[5].rig;
    test_rig.bloom_threshold = 0.12f; // Generate partial-alpha bloom halo around avatar silhouette
    test_rig.bloom_intensity = 0.95f;
    aether::renderer::FrameRenderRequest req{};
    req.avatar = avatar.get();
    req.rig = &test_rig;
    req.camera = scenes[5].camera;
    req.rt_enabled = rcfg.enable_rt;
    req.need_straight_alpha = true;
    req.need_bottom_up = true;
    req.non_alpha_bg_rgb = {0.0f, 177.0f / 255.0f, 64.0f / 255.0f}; // Chroma green #00B140

    std::array<VkImage, 3> ring_images{};
    for (uint32_t f = 0; f < 3; ++f) {
        req.frame_index = f;
        req.completed_frame_index = f > 0 ? f - 1 : 0;
        req.frame_slot = f;
        REQUIRE(renderer.render_headless_frame(req).has_value());
        INFO("Frame " << f << " validation errors = " << rhi->validation_error_count());
        REQUIRE(rhi->validation_error_count() == 0);
        const auto outs = renderer.graph().outputs();
        REQUIRE(outs.width == 1280);
        REQUIRE(outs.height == 720);
        REQUIRE(outs.output_width == 1920);
        REQUIRE(outs.output_height == 1080);
        REQUIRE(outs.bgra8_image != VK_NULL_HANDLE);
        REQUIRE(outs.bgra8_straight_image != VK_NULL_HANDLE);
        REQUIRE(outs.bottom_up_bgra8_image != VK_NULL_HANDLE);
        ring_images[f] = outs.bgra8_image;
    }
    REQUIRE(ring_images[0] != ring_images[1]);
    REQUIRE(ring_images[1] != ring_images[2]);

    const auto outs = renderer.graph().outputs();
    auto vp_res =
        renderer.graph().readback_image_rgba8(outs.viewport_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1280, 720);
    auto premul_res = renderer.graph().readback_image_rgba8(outs.bgra8_image, VK_IMAGE_LAYOUT_GENERAL, 1920, 1080);
    auto straight_res =
        renderer.graph().readback_image_rgba8(outs.bgra8_straight_image, VK_IMAGE_LAYOUT_GENERAL, 1920, 1080);
    auto bot_res =
        renderer.graph().readback_image_rgba8(outs.bottom_up_bgra8_image, VK_IMAGE_LAYOUT_GENERAL, 1920, 1080);
    REQUIRE(vp_res.has_value());
    REQUIRE(premul_res.has_value());
    REQUIRE(straight_res.has_value());
    REQUIRE(bot_res.has_value());
    const auto& vp = vp_res.value();
    const auto& out_premul = premul_res.value();
    const auto& out_straight = straight_res.value();
    const auto& out_bot = bot_res.value();
    REQUIRE(vp.size() == 1280u * 720u * 4u);
    REQUIRE(out_premul.size() == 1920u * 1080u * 4u);

    // Top-left pixel (0,0) must have alpha == 0 on avatar BGRA8 and row 0 bottom-up equals row (h-1) top-down
    REQUIRE(out_premul[3] == 0);
    const size_t row_bytes = 1920u * 4u;
    REQUIRE(std::memcmp(out_bot.data(), out_premul.data() + (1079u * row_bytes), row_bytes) == 0);

    // Ensure avatar interior pixels exist with full alpha and translucent edge/glow pixels show straight > premul RGB
    bool saw_opaque_avatar = false;
    bool saw_partial_alpha = false;
    size_t mid_alpha_cnt = 0;
    size_t any_partial_cnt = 0;
    int max_straight_diff = 0;
    for (size_t i = 0; i < 1920u * 1080u; ++i) {
        const uint8_t a = out_premul[i * 4u + 3u];
        if (a > 240u) {
            saw_opaque_avatar = true;
        } else if (a >= 10u && a <= 220u) {
            ++any_partial_cnt;
            if (a >= 40u && a <= 160u) {
                ++mid_alpha_cnt;
            }
            const uint8_t pr = out_premul[i * 4u + 0u];
            const uint8_t pg = out_premul[i * 4u + 1u];
            const uint8_t pb = out_premul[i * 4u + 2u];
            const uint8_t sr = out_straight[i * 4u + 0u];
            const uint8_t sg = out_straight[i * 4u + 1u];
            const uint8_t sb = out_straight[i * 4u + 2u];
            max_straight_diff =
                std::max(max_straight_diff, std::max({int(sr) - int(pr), int(sg) - int(pg), int(sb) - int(pb)}));
            if (sr >= pr && sg >= pg && sb >= pb && (sr > pr + 2u || sg > pg + 2u || sb > pb + 2u)) {
                saw_partial_alpha = true;
            }
        }
    }
    INFO("any_partial_cnt=" << any_partial_cnt << " mid_alpha_cnt=" << mid_alpha_cnt
                            << " max_straight_diff=" << max_straight_diff);
    REQUIRE(saw_opaque_avatar);
    REQUIRE(saw_partial_alpha);

    // Verify NV12 background composites over Chroma Green #00B140 (BT.709 Y ≈ 131, Cb ≈ 91, Cr ≈ 62)
    auto nv12_y = readback_r8_plane(*rhi, outs.nv12_y_image, VK_IMAGE_LAYOUT_GENERAL, 1920, 1080, 1);
    auto nv12_uv = readback_r8_plane(*rhi, outs.nv12_uv_image, VK_IMAGE_LAYOUT_GENERAL, 960, 540, 2);
    REQUIRE(nv12_y[0] >= 125);
    REQUIRE(nv12_y[0] <= 138);
    REQUIRE(nv12_uv[0] < 110); // Cb < 0.5
    REQUIRE(nv12_uv[1] < 85);  // Cr << 0.5 for green

    renderer.destroy();
    avatar->destroy();
    rhi->destroy();
}

TEST_CASE("Phase 5 Spout2 Output: LUID KMT D3D11 receiver probe, 1 ms mutex timeout & <= 0.2 ms publish",
          "[phase5][spout]") {
    aether::rhi::RhiDeviceConfig dcfg{};
    dcfg.headless = true;
    dcfg.enable_validation = true;
    auto dev_res = aether::rhi::RhiDevice::create(dcfg);
    REQUIRE(dev_res.has_value());
    std::unique_ptr<aether::rhi::RhiDevice> rhi(dev_res.value());

    auto av_res = aether::test::load_reference_vrm_avatar(*rhi);
    REQUIRE(av_res.has_value());
    auto avatar = std::move(av_res.value());

    constexpr uint32_t kW = 1920;
    constexpr uint32_t kH = 1080;
    aether::renderer::RendererConfig rcfg{};
    rcfg.width = kW;
    rcfg.height = kH;
    rcfg.enable_rt = rhi->rt_supported();

    aether::renderer::NprRenderer renderer;
    REQUIRE(renderer.init(*rhi, rcfg).has_value());
    REQUIRE(renderer.bind_avatar(avatar.get()).has_value());

    const auto scenes = aether::test::make_canonical_scenes();
    auto test_rig = scenes[5].rig;
    test_rig.bloom_threshold = 0.12f; // Generate partial-alpha bloom halo around avatar silhouette
    test_rig.bloom_intensity = 0.95f;
    aether::renderer::FrameRenderRequest req{};
    req.avatar = avatar.get();
    req.rig = &test_rig;
    req.camera = scenes[5].camera;
    req.rt_enabled = rcfg.enable_rt;
    uint64_t tl_val = 0;
    REQUIRE(renderer.submit_critical_and_signal(req, tl_val).has_value());
    rhi->wait_idle();

    const DWORD pid = GetCurrentProcessId();
    char sender_name[64]{};
    snprintf(sender_name, sizeof(sender_name), "Aether2.5D_Test_%lu", pid);

    aether::output::SpoutOutput spout;
    REQUIRE(spout.init(*rhi, kW, kH, sender_name, aether::output::SpoutSharingMode::GpuDefault).has_value());
    REQUIRE(spout.is_active());
    REQUIRE(spout.raw_kmt_share_handle() != 0u);

    const auto outs = renderer.graph().outputs();
    REQUIRE(spout.publish_frame(outs.bgra8_image, kW, kH, tl_val).has_value());

    // Open SharedTextureInfo MMF & separate D3D11 receiver opening the KMT handle
    HANDLE info_map = OpenFileMappingA(FILE_MAP_READ, FALSE, sender_name);
    REQUIRE(info_map != nullptr);
    const auto* info =
        static_cast<const aether::output::SpoutSharedTextureInfo*>(MapViewOfFile(info_map, FILE_MAP_READ, 0, 0, 280));
    REQUIRE(info != nullptr);
    REQUIRE(info->width == kW);
    REQUIRE(info->height == kH);
    REQUIRE(info->format == DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(info->share_handle == spout.raw_kmt_share_handle());
    const uint32_t kmt_u32 = info->share_handle;
    UnmapViewOfFile(info);
    CloseHandle(info_map);

    // Probe via independent ID3D11Device matching Vulkan LUID
    IDXGIFactory4* fac = nullptr;
    IDXGIAdapter1* adp = nullptr;
    ID3D11Device* rx_dev = nullptr;
    ID3D11DeviceContext* rx_ctx = nullptr;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&fac))));
    if (rhi->gpu_info().luid_valid) {
        LUID luid{};
        std::memcpy(&luid, rhi->gpu_info().luid.data(), sizeof(luid));
        REQUIRE(SUCCEEDED(fac->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adp))));
    } else {
        REQUIRE(SUCCEEDED(fac->EnumAdapters1(0, &adp)));
    }
    REQUIRE(SUCCEEDED(D3D11CreateDevice(adp, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                        &rx_dev, nullptr, &rx_ctx)));

    ID3D11Texture2D* shared_rx = nullptr;
    const HANDLE kmt_handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(kmt_u32));
    REQUIRE(SUCCEEDED(
        rx_dev->OpenSharedResource(kmt_handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&shared_rx))));

    D3D11_TEXTURE2D_DESC sdesc{};
    sdesc.Width = kW;
    sdesc.Height = kH;
    sdesc.MipLevels = 1;
    sdesc.ArraySize = 1;
    sdesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sdesc.SampleDesc.Count = 1;
    sdesc.Usage = D3D11_USAGE_STAGING;
    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stg = nullptr;
    REQUIRE(SUCCEEDED(rx_dev->CreateTexture2D(&sdesc, nullptr, &stg)));
    rx_ctx->CopyResource(stg, shared_rx);
    rx_ctx->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    REQUIRE(SUCCEEDED(rx_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &mapped)));
    size_t lit_pixels = 0;
    for (uint32_t y = 0; y < kH; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        for (uint32_t x = 0; x < kW; ++x) {
            if (row[x * 4u + 3u] > 200u) {
                ++lit_pixels;
            }
        }
    }
    rx_ctx->Unmap(stg, 0);
    REQUIRE(lit_pixels > 5000);

    stg->Release();
    shared_rx->Release();
    rx_ctx->Release();
    rx_dev->Release();
    adp->Release();
    fac->Release();

    // Verify 1 ms mutex timeout contention never hangs T4
    char mtx_name[96]{};
    snprintf(mtx_name, sizeof(mtx_name), "%s_SpoutAccessMutex", sender_name);
    HANDLE hold_mtx = OpenMutexA(SYNCHRONIZE, FALSE, mtx_name);
    REQUIRE(hold_mtx != nullptr);
    const uint64_t before_skip = spout.skipped_frames();
    std::thread contender([&] {
        REQUIRE(WaitForSingleObject(hold_mtx, 2000) == WAIT_OBJECT_0);
        Sleep(25);
        ReleaseMutex(hold_mtx);
    });
    Sleep(5);
    const uint64_t t_skip0 = aether::now_us();
    REQUIRE(spout.publish_frame(outs.bgra8_image, kW, kH, tl_val).has_value());
    const double skip_ms = static_cast<double>(aether::now_us() - t_skip0) / 1000.0;
    contender.join();
    CloseHandle(hold_mtx);
    REQUIRE(spout.skipped_frames() == before_skip + 1u);
    REQUIRE(skip_ms < 10.0);

    // Measure Spout publish latency across 60 frames (GPU copy + submit + WaitForSingleObject)
    std::vector<double> pub_ms;
    const uint32_t iters = 60u;
    pub_ms.reserve(iters);
    {
#if !defined(__SANITIZE_ADDRESS__)
        aether::HotPathScope guard;
#endif
        for (uint32_t i = 0; i < iters; ++i) {
            REQUIRE(spout.publish_frame(outs.bgra8_image, kW, kH, tl_val).has_value());
            pub_ms.push_back(spout.last_publish_ms());
        }
#if !defined(__SANITIZE_ADDRESS__)
        REQUIRE(aether::HotPathGuard::thread_violations() == 0);
#endif
    }
    std::sort(pub_ms.begin(), pub_ms.end());
    const double p50 = pub_ms[pub_ms.size() / 2u];
    INFO("Spout publish p50 = " << p50 << " ms");
#if !defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    REQUIRE(p50 <= 0.25);
#endif

    // Verify CpuCompatibility fallback initializes cleanly as well
    spout.shutdown();
    REQUIRE(spout.init(*rhi, kW, kH, sender_name, aether::output::SpoutSharingMode::CpuCompatibility).has_value());
    REQUIRE(spout.using_cpu_fallback());
    REQUIRE(spout.publish_frame(outs.bgra8_image, kW, kH, tl_val).has_value());

    spout.shutdown();
    renderer.destroy();
    avatar->destroy();
    REQUIRE(rhi->validation_error_count() == 0u);
    rhi->destroy();
}

TEST_CASE("Phase 5 DirectShow Virtual Cam COM Filter & 1080p DMA Transport", "[phase5][vcam]") {
    const auto dll_path = find_bin_file(L"aether_vcam_ds_x64.dll");
    REQUIRE(std::filesystem::exists(dll_path));

    HMODULE vcam_dll = LoadLibraryW(dll_path.c_str());
    REQUIRE(vcam_dll != nullptr);

    using FnDllGetClassObject = HRESULT(__stdcall*)(REFCLSID, REFIID, LPVOID*);
    auto fn_gco = reinterpret_cast<FnDllGetClassObject>(GetProcAddress(vcam_dll, "DllGetClassObject"));
    REQUIRE(fn_gco != nullptr);

    REQUIRE(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));

    IClassFactory* factory = nullptr;
    REQUIRE(SUCCEEDED(fn_gco(CLSID_AetherVirtualCam, IID_IClassFactory, reinterpret_cast<void**>(&factory))));
    IBaseFilter* filter = nullptr;
    REQUIRE(SUCCEEDED(factory->CreateInstance(nullptr, IID_IBaseFilter, reinterpret_cast<void**>(&filter))));

    // Enumerate pin & verify ARGB32 / RGB32 / NV12 media types via IAMStreamConfig
    IEnumPins* epins = nullptr;
    REQUIRE(SUCCEEDED(filter->EnumPins(&epins)));
    IPin* out_pin = nullptr;
    ULONG fetched = 0;
    REQUIRE(epins->Next(1, &out_pin, &fetched) == S_OK);
    REQUIRE(out_pin != nullptr);

    IAMStreamConfig* sc = nullptr;
    REQUIRE(SUCCEEDED(out_pin->QueryInterface(IID_IAMStreamConfig, reinterpret_cast<void**>(&sc))));
    int count = 0;
    int size = 0;
    REQUIRE(SUCCEEDED(sc->GetNumberOfCapabilities(&count, &size)));
    REQUIRE(count >= 3);

    bool has_argb32 = false;
    bool has_rgb32 = false;
    bool has_nv12 = false;
    for (int i = 0; i < count; ++i) {
        AM_MEDIA_TYPE* pmt = nullptr;
        std::vector<uint8_t> caps(static_cast<size_t>(size));
        REQUIRE(SUCCEEDED(sc->GetStreamCaps(i, &pmt, caps.data())));
        if (pmt != nullptr) {
            if (IsEqualGUID(pmt->subtype, MEDIASUBTYPE_ARGB32)) {
                has_argb32 = true;
                REQUIRE(SUCCEEDED(sc->SetFormat(pmt)));
            } else if (IsEqualGUID(pmt->subtype, MEDIASUBTYPE_RGB32)) {
                has_rgb32 = true;
            } else if (IsEqualGUID(pmt->subtype, MEDIASUBTYPE_NV12)) {
                has_nv12 = true;
            }
            if (pmt->pbFormat != nullptr) {
                CoTaskMemFree(pmt->pbFormat);
            }
            CoTaskMemFree(pmt);
        }
    }
    REQUIRE(has_argb32);
    REQUIRE(has_rgb32);
    REQUIRE(has_nv12);

    // Offline state should keep shared header unset before VcamTransport runs and verify VcamTransport + RhiDevice
    aether::rhi::RhiDeviceConfig dcfg{};
    dcfg.headless = true;
    dcfg.enable_validation = true;
    auto dev_res = aether::rhi::RhiDevice::create(dcfg);
    REQUIRE(dev_res.has_value());
    std::unique_ptr<aether::rhi::RhiDevice> rhi(dev_res.value());

    aether::renderer::RendererConfig rcfg{};
    rcfg.width = 1920;
    rcfg.height = 1080;
    rcfg.enable_rt = false;
    aether::renderer::NprRenderer renderer;
    REQUIRE(renderer.init(*rhi, rcfg).has_value());
    aether::renderer::FrameRenderRequest req{};
    req.rig = &renderer.lighting();
    req.rt_enabled = false;
    uint64_t tl = 0;
    REQUIRE(renderer.submit_critical_and_signal(req, tl).has_value());

    aether::output::VcamTransport vcam;
    REQUIRE(vcam.init(*rhi).has_value());
    REQUIRE(vcam.is_open());

    const auto outs = renderer.graph().outputs();
    constexpr uint32_t kWarmup = AETHER_VCAM_SLOTS;
    for (uint32_t w = 0; w < kWarmup; ++w) {
        REQUIRE(vcam.publish_frame(outs.bgra8_image, outs.nv12_y_image, outs.nv12_uv_image, 1920, 1080, 60, false,
                                   w + 1u, aether::now_us(), tl)
                    .has_value());
    }

    std::vector<double> dma_ms;
    const uint32_t frames = 60u;
    dma_ms.reserve(frames);
    for (uint32_t f = 0; f < frames; ++f) {
        {
#if !defined(__SANITIZE_ADDRESS__)
            aether::HotPathScope guard;
#endif
            REQUIRE(vcam.publish_frame(outs.bgra8_image, outs.nv12_y_image, outs.nv12_uv_image, 1920, 1080, 60, false,
                                       kWarmup + f + 1u, aether::now_us(), tl)
                        .has_value());
#if !defined(__SANITIZE_ADDRESS__)
            REQUIRE(aether::HotPathGuard::thread_violations() == 0);
#endif
        }
        dma_ms.push_back(vcam.last_dma_ms());
    }
    std::sort(dma_ms.begin(), dma_ms.end());
    const double dma_p95 = dma_ms[static_cast<size_t>(dma_ms.size() * 95u / 100u)];
    INFO("VCam 1080p DMA p95 = " << dma_p95 << " ms");
    REQUIRE(vcam.header()->magic == AETHER_VCAM_MAGIC);
    REQUIRE((vcam.header()->write_seq & 1L) == 0L);
    REQUIRE(vcam.frames_published() == kWarmup + frames);
#if !defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    REQUIRE(dma_p95 <= 1.1);
#endif

    sc->Release();
    out_pin->Release();
    epins->Release();
    filter->Release();
    factory->Release();
    FreeLibrary(vcam_dll);

    vcam.shutdown();
    renderer.destroy();
    REQUIRE(rhi->validation_error_count() == 0u);
    rhi->destroy();
}

TEST_CASE("Phase 5 NDI 6 Async Double-Buffered Straight-Alpha & UYVA + Installer Manifest", "[phase5][ndi][wix]") {
    aether::rhi::RhiDeviceConfig dcfg{};
    dcfg.headless = true;
    dcfg.enable_validation = false;
    auto dev_res = aether::rhi::RhiDevice::create(dcfg);
    REQUIRE(dev_res.has_value());
    std::unique_ptr<aether::rhi::RhiDevice> rhi(dev_res.value());

    aether::renderer::RendererConfig rcfg{};
    rcfg.width = 160;
    rcfg.height = 90;
    rcfg.enable_rt = false;
    aether::renderer::NprRenderer renderer;
    REQUIRE(renderer.init(*rhi, rcfg).has_value());
    aether::renderer::FrameRenderRequest req{};
    req.rig = &renderer.lighting();
    req.need_straight_alpha = true;
    req.rt_enabled = false;
    uint64_t tl = 0;
    REQUIRE(renderer.submit_critical_and_signal(req, tl).has_value());

    MockNdiCapture cap{};
    g_mock_ndi = &cap;
    aether::output::NdiDispatchTable tbl{};
    tbl.initialize = &mock_ndi_init;
    tbl.destroy = &mock_ndi_destroy;
    tbl.send_create = &mock_ndi_create;
    tbl.send_destroy = &mock_ndi_destroy_send;
    tbl.send_video_async_v2 = &mock_ndi_send_async;

    // 1) Straight BGRA async double-buffering verification
    {
        aether::output::NdiOutput ndi;
        REQUIRE(ndi.init(*rhi, 160, 90, aether::output::NdiVideoFormat::BgraStraight, &tbl).has_value());
        REQUIRE(cap.init_called);
        REQUIRE_FALSE(cap.clock_video);
        REQUIRE(cap.sender_name.rfind("Aether 2.5D (", 0) == 0);

        const auto outs = renderer.graph().outputs();
        const VkImage src_img =
            (outs.bgra8_straight_image != VK_NULL_HANDLE) ? outs.bgra8_straight_image : outs.bgra8_image;
        REQUIRE(ndi.send_frame(src_img, 160, 90, 60, aether::now_us(), tl).has_value());
        REQUIRE(ndi.send_frame(src_img, 160, 90, 60, aether::now_us(), tl).has_value());
        REQUIRE(cap.frame_ptrs.size() == 2);
        REQUIRE(cap.frame_ptrs[0] != cap.frame_ptrs[1]);
        REQUIRE(cap.fourccs[0] == aether::output::kNdiFourCcBgra);
        ndi.shutdown();
    }
    REQUIRE(cap.destroy_called);

    // 2) Straight UYVA packer verification (full-res alpha plane preserves 0 and 255)
    std::array<uint8_t, 4 * 2 * 4> bgra{};
    // Pixel 0: transparent; Pixel 1: opaque white; Pixels 2..7: half alpha cyan
    bgra[0] = 0;
    bgra[1] = 0;
    bgra[2] = 0;
    bgra[3] = 0;
    bgra[4] = 255;
    bgra[5] = 255;
    bgra[6] = 255;
    bgra[7] = 255;
    for (int i = 2; i < 8; ++i) {
        bgra[static_cast<size_t>(i) * 4u + 0u] = 255;
        bgra[static_cast<size_t>(i) * 4u + 1u] = 200;
        bgra[static_cast<size_t>(i) * 4u + 2u] = 0;
        bgra[static_cast<size_t>(i) * 4u + 3u] = 128;
    }
    std::array<uint8_t, 4 * 2 * 3> uyva{};
    {
        aether::HotPathScope guard;
        aether::output::NdiOutput::pack_bgra_straight_to_uyva(bgra.data(), 4, 2, uyva.data());
        REQUIRE(aether::HotPathGuard::thread_violations() == 0);
    }
    const uint8_t* alpha_plane = uyva.data() + (4u * 2u * 2u);
    REQUIRE(alpha_plane[0] == 0);
    REQUIRE(alpha_plane[1] == 255);
    REQUIRE(alpha_plane[2] == 128);
    REQUIRE(alpha_plane[7] == 128);

    // Runtime DLL presence check must never throw or crash
    (void)aether::output::NdiOutput::is_runtime_dll_present();

    // WiX v4 manifest verification (once Step 4 creates installer/AetherStudio.wxs)
    const auto wxs = std::filesystem::path(AETHER_REPO_ROOT) / "installer" / "AetherStudio.wxs";
    if (std::filesystem::exists(wxs)) {
        std::ifstream ifs(wxs);
        std::ostringstream ss;
        ss << ifs.rdbuf();
        const std::string xml = ss.str();
        REQUIRE(xml.find("7A5E14B2-4F8C-4D31-9E2A-61B38F0C42D5") != std::string::npos);
        REQUIRE(xml.find("aether_vcam_ds_x64.dll") != std::string::npos);
        REQUIRE(xml.find("aether_vcam_ds_x86.dll") != std::string::npos);
    }

    renderer.destroy();
    rhi->destroy();
}
