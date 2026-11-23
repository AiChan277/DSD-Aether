// Lighting workspace tab end-to-end verification (Vulkan + DirectX 12 bridged), 8590256991748008892.vrm.
//
// GPU half replays the application's exact per-frame sequence (application.cpp):
//   Broadcast request (+ LIVE CAM PiP encode) -> LightingStage request (copy of the Broadcast request with
//   skip_skin_and_blas = true and the external DX12 image cleared).
// and asserts:
//   - the Stage canvas is not blank (avatar pixels in the canvas centre), on Vulkan AND with a DX12 Broadcast frame
//     (where the Vulkan graph never skins during the Broadcast pass),
//   - Grid + Schematic overlay pixels land INSIDE a wide (w != h) canvas (regression: viewport y = w),
//   - the PiP equals the Live Studio framing (full, uncropped 16:9 frame, head present, zero top crop),
//   - 1:1 pixel aspect across 1600x900 / 1600x360 / 450x900 stage canvases.
// UI half drives the real StudioWorkspace Lighting tab headlessly and asserts single-row toolbar (<= 36 px, no
// widget past the right edge), canvas bounds, PiP / ViewCube separation, no window scroll on wheel-zoom, PiP UVs and
// balanced ImGui stacks in every mode. It then rasterizes the tab WITH the GPU stage + PiP readbacks bound as
// textures and writes <build>/tests/ui_snapshots/lighting_tab.png.

#include "core/garbage_collector.hpp"
#include "core/hw_enum.hpp"
#include "core/paths.hpp"
#include "core/telemetry.hpp"
#include "renderer/asset_upload/asset_job.hpp"
#include "renderer/dx12_bridge.hpp"
#include "renderer/npr_renderer.hpp"
#include "renderer/studio_lighting.hpp"
#include "rhi/rhi_device.hpp"
#include "rhi/rhi_swapchain.hpp"
#include "test_png_io.hpp"
#include "ui/theme/precision_dark.hpp"
#include "ui/workspace/studio_workspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace aether;

constexpr uint32_t kOutW = 1280u; // Broadcast output == Live Studio viewport (16:9, FitAuto du = dv = 1)
constexpr uint32_t kOutH = 720u;
constexpr ImTextureID kTexStage = static_cast<ImTextureID>(101);
constexpr ImTextureID kTexPip = static_cast<ImTextureID>(102);

struct Image {
    std::vector<uint8_t> px; // RGBA8
    uint32_t w{0};
    uint32_t h{0};
    [[nodiscard]] const uint8_t* at(uint32_t x, uint32_t y) const noexcept {
        return px.data() + (static_cast<size_t>(y) * w + x) * 4u;
    }
};

[[nodiscard]] int luma(const uint8_t* p) noexcept {
    return (54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8;
}

struct Rect {
    uint32_t x0, y0, x1, y1;
};

[[nodiscard]] uint32_t count_bright(const Image& im, Rect r, int thresh) {
    uint32_t n = 0;
    for (uint32_t y = r.y0; y < std::min(r.y1, im.h); ++y) {
        for (uint32_t x = r.x0; x < std::min(r.x1, im.w); ++x) {
            n += (luma(im.at(x, y)) > thresh) ? 1u : 0u;
        }
    }
    return n;
}

struct Bbox {
    uint32_t x0{UINT32_MAX}, y0{UINT32_MAX}, x1{0}, y1{0};
    [[nodiscard]] bool valid() const noexcept { return x1 >= x0 && y1 >= y0 && x0 != UINT32_MAX; }
    [[nodiscard]] uint32_t w() const noexcept { return x1 - x0 + 1u; }
    [[nodiscard]] uint32_t h() const noexcept { return y1 - y0 + 1u; }
};

[[nodiscard]] Bbox bbox_bright(const Image& im, int thresh) {
    Bbox b{};
    for (uint32_t y = 0; y < im.h; ++y) {
        for (uint32_t x = 0; x < im.w; ++x) {
            if (luma(im.at(x, y)) > thresh) {
                b.x0 = std::min(b.x0, x);
                b.y0 = std::min(b.y0, y);
                b.x1 = std::max(b.x1, x);
                b.y1 = std::max(b.y1, y);
            }
        }
    }
    return b;
}

[[nodiscard]] Image crop(const std::vector<uint8_t>& rt_px, uint32_t rt_w, uint32_t w, uint32_t h) {
    Image out{std::vector<uint8_t>(static_cast<size_t>(w) * h * 4u), w, h};
    for (uint32_t y = 0; y < h; ++y) {
        std::copy_n(rt_px.data() + (static_cast<size_t>(y) * rt_w) * 4u, static_cast<size_t>(w) * 4u,
                    out.px.data() + static_cast<size_t>(y) * w * 4u);
    }
    return out;
}

[[nodiscard]] Image box_downsample_2x(const Image& im) {
    Image out{std::vector<uint8_t>(static_cast<size_t>(im.w / 2u) * (im.h / 2u) * 4u), im.w / 2u, im.h / 2u};
    for (uint32_t y = 0; y < out.h; ++y) {
        for (uint32_t x = 0; x < out.w; ++x) {
            for (uint32_t c = 0; c < 4u; ++c) {
                const int s = im.at(2 * x, 2 * y)[c] + im.at(2 * x + 1, 2 * y)[c] + im.at(2 * x, 2 * y + 1)[c] +
                              im.at(2 * x + 1, 2 * y + 1)[c];
                out.px[(static_cast<size_t>(y) * out.w + x) * 4u + c] = static_cast<uint8_t>((s + 2) / 4);
            }
        }
    }
    return out;
}

[[nodiscard]] double mean_abs_diff_rgb(const Image& a, const Image& b) {
    REQUIRE(a.w == b.w);
    REQUIRE(a.h == b.h);
    double s = 0.0;
    for (size_t i = 0; i < a.px.size(); i += 4u) {
        for (size_t c = 0; c < 3u; ++c) {
            s += std::abs(static_cast<int>(a.px[i + c]) - static_cast<int>(b.px[i + c]));
        }
    }
    return s / (static_cast<double>(a.w) * a.h * 3.0);
}

std::string read_env_var(const char* name) {
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr) {
        std::string out(buf);
        std::free(buf);
        return out;
    }
    return {};
}

std::filesystem::path find_user_vrm_path() {
    const std::string up = read_env_var("USERPROFILE");
    if (!up.empty()) {
        const auto cand = std::filesystem::path(up) / "Downloads" / "8590256991748008892.vrm";
        if (std::filesystem::is_regular_file(cand)) {
            return cand;
        }
    }
    return {};
}

std::filesystem::path snapshot_out_dir() {
#ifdef AETHER_VRM_SNAPSHOT_DIR
    return std::filesystem::path(AETHER_VRM_SNAPSHOT_DIR);
#else
    return std::filesystem::current_path() / "ui_snapshots";
#endif
}

std::unique_ptr<renderer::GpuAvatar> load_avatar_file(rhi::RhiDevice& rhi, const std::filesystem::path& path) {
    renderer::AssetJob job;
    job.start_load(rhi, path.string());
    HighResTimer timer;
    const uint64_t deadline = now_us() + 30'000'000ULL;
    while (job.state() == renderer::AssetJobState::Loading && now_us() < deadline) {
        timer.sleep_us(500);
    }
    if (job.state() != renderer::AssetJobState::ReadyForGpu) {
        return nullptr;
    }
    auto res = job.finalize_gpu_upload(rhi);
    return res ? std::move(res.value()) : nullptr;
}

/** Default Lighting-tab Stage camera: target (0, 1.15, 0), 2.8 m (matches LightingStagePanels). */
renderer::PreviewCamera default_stage_camera(bool front) {
    auto cam = renderer::make_stage_camera_preset(renderer::StageCameraView::FreePerspective, {0.0f, 1.15f, 0.0f});
    cam.distance = 2.8f;
    if (front) {
        cam.yaw_rad = 0.0f;
        cam.pitch_rad = 0.0f;
    }
    return cam;
}

struct AppFrameResult {
    Image stage;     // active stage canvas region (stage_w x stage_h)
    Image stage_rt;  // full padded stage RT (what ImGui samples with uv1 = ren / rt)
    Image pip;       // LIVE CAM PiP target
    Image live_view; // Live Studio viewport (FitAuto at 16:9 == full Broadcast frame)
    bool used_external{false};
};

/** Exact replay of application.cpp's Broadcast -> Stage sequence for one frame. */
struct AppSequence {
    rhi::RhiDevice& rhi;
    renderer::NprRenderer& npr;
    renderer::GpuAvatar& avatar;
    renderer::LightingRig& rig;
    renderer::PreviewCamera bcast_cam;
    renderer::Dx12Bridge* bridge{nullptr};

    AppFrameResult run(uint64_t fidx, const renderer::PreviewCamera& stage_cam, uint32_t stage_w, uint32_t stage_h,
                       bool schem, bool grid) {
        bool rebuilt = false;
        REQUIRE(npr.graph().ensure_stage_target_sizes(stage_w, stage_h, stage_w / 2u, stage_h / 2u, stage_w / 2u,
                                                      stage_h / 2u, rebuilt));

        renderer::FrameRenderRequest req{};
        req.avatar = &avatar;
        req.rig = &rig;
        req.camera = bcast_cam;
        req.frame_index = fidx;
        req.completed_frame_index = (fidx >= 2u) ? fidx - 2u : 0u;
        req.frame_slot = static_cast<uint32_t>(fidx % rhi::RhiSwapchain::kFramesInFlight);
        req.lighting_mode = 0u;
        req.rt_enabled = false;
        req.show_checkerboard = false;
        req.render_csm_shadow = true;
        req.encode_stage_broadcast_pip = true;
        AppFrameResult out{};
        if (bridge != nullptr && bridge->render(req)) {
            bridge->fill_request(req);
        }
        out.used_external = (req.external_color_image != VK_NULL_HANDLE);
        REQUIRE(npr.render_headless_frame(req));
        if (out.used_external) {
            bridge->on_submitted(npr.frame_timeline_value());
        }

        renderer::FrameRenderRequest sreq = req;
        sreq.view_kind = renderer::RenderViewKind::LightingStage;
        sreq.camera = stage_cam;
        sreq.stage_viewport_w = stage_w;
        sreq.stage_viewport_h = stage_h;
        sreq.encode_stage_broadcast_pip = false;
        sreq.stage_diag_mode = renderer::StageDiagnosticMode::Lit;
        sreq.show_stage_schematic = schem;
        sreq.show_stage_grid = grid;
        sreq.preview_25d_flatten = false;
        sreq.has_studio_frustum = true;
        sreq.studio_camera = bcast_cam;
        sreq.studio_frustum_aspect = 16.0f / 9.0f;
        sreq.skip_skin_and_blas = true;
        sreq.stage_output_slot = renderer::FrameRenderRequest::StageOutputSlot::MainStage;
        sreq.viewport_aspect_mode = core::ViewportAspectMode::FitAuto;
        sreq.encode_output_bgra = false;
        sreq.encode_output_nv12 = false;
        sreq.external_color_image = VK_NULL_HANDLE;
        REQUIRE(npr.render_headless_frame(sreq));

        const auto o = npr.graph().outputs();
        REQUIRE(o.stage_viewport_width == stage_w);
        REQUIRE(o.stage_viewport_height == stage_h);
        auto st = npr.graph().readback_image_rgba8(o.stage_viewport_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                   o.stage_viewport_rt_width, o.stage_viewport_rt_height);
        auto pp =
            npr.graph().readback_image_rgba8(o.stage_broadcast_pip_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                             o.stage_broadcast_pip_width, o.stage_broadcast_pip_height);
        auto lv = npr.graph().readback_image_rgba8(o.viewport_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, o.width,
                                                   o.height);
        REQUIRE(st.has_value());
        REQUIRE(pp.has_value());
        REQUIRE(lv.has_value());
        out.stage_rt = Image{st.value(), o.stage_viewport_rt_width, o.stage_viewport_rt_height};
        out.stage = crop(st.value(), o.stage_viewport_rt_width, stage_w, stage_h);
        out.pip = Image{std::move(pp.value()), o.stage_broadcast_pip_width, o.stage_broadcast_pip_height};
        out.live_view = Image{std::move(lv.value()), o.width, o.height};
        return out;
    }
};

void check_stage_not_blank(const Image& st, const char* tag) {
    const Rect centre{st.w * 35u / 100u, st.h * 15u / 100u, st.w * 65u / 100u, st.h * 85u / 100u};
    const uint32_t lit = count_bright(st, centre, 40);
    const uint32_t area = (centre.x1 - centre.x0) * (centre.y1 - centre.y0);
    std::printf("[LIGHTING_TAB] %s stage %ux%u centre avatar px=%u / %u\n", tag, st.w, st.h, lit, area);
    INFO(tag << " centre avatar pixels " << lit << " / " << area);
    CHECK(lit * 50u > area); // > 2 % of the centre box is lit avatar
}

void check_pip_matches_live(const AppFrameResult& r, const char* tag) {
    REQUIRE(r.pip.w * 2u == r.live_view.w);
    REQUIRE(r.pip.h * 2u == r.live_view.h);
    const Image live_half = box_downsample_2x(r.live_view);
    const double mad = mean_abs_diff_rgb(live_half, r.pip);
    const Bbox bp = bbox_bright(r.pip, 28);
    const Bbox bl = bbox_bright(live_half, 28);
    REQUIRE(bp.valid());
    REQUIRE(bl.valid());
    const Rect upper_mid{r.pip.w * 35u / 100u, r.pip.h * 5u / 100u, r.pip.w * 65u / 100u, r.pip.h * 45u / 100u};
    const uint32_t head_px = count_bright(r.pip, upper_mid, 28);
    const uint32_t um_area = (upper_mid.x1 - upper_mid.x0) * (upper_mid.y1 - upper_mid.y0);
    std::printf("[LIGHTING_TAB] %s PiP %ux%u vs LiveStudio/2: mad=%.2f bbox_top pip=%u live=%u head_px=%u/%u\n", tag,
                r.pip.w, r.pip.h, mad, bp.y0, bl.y0, head_px, um_area);
    INFO(tag << " PiP vs Live Studio mean abs diff " << mad);
    CHECK(mad < 6.0);
    CHECK(std::abs(static_cast<int>(bp.y0) - static_cast<int>(bl.y0)) <= static_cast<int>(r.pip.h / 50u));
    CHECK(head_px * 10u > um_area); // head / face occupies the upper-middle of the frame
}

// ---------------------------------------------------------------- UI rasterizer (font + bound textures)
struct Rgba {
    float r, g, b, a;
};

Rgba unpack(ImU32 c) noexcept {
    return Rgba{static_cast<float>(c & 0xFFU) / 255.0f, static_cast<float>((c >> 8) & 0xFFU) / 255.0f,
                static_cast<float>((c >> 16) & 0xFFU) / 255.0f, static_cast<float>((c >> 24) & 0xFFU) / 255.0f};
}

std::vector<uint8_t> rasterize(const ImDrawData* dd, int w, int h, const uint8_t* atlas, int atlas_w, int atlas_h,
                               const Image* stage_tex, const Image* pip_tex) {
    std::vector<float> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3U, 0.0f);
    for (int l = 0; l < dd->CmdListsCount; ++l) {
        const ImDrawList* dl = dd->CmdLists[l];
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback != nullptr) {
                continue;
            }
            const ImTextureID tid = cmd.GetTexID();
            const bool font_tex = tid == static_cast<ImTextureID>(1);
            const Image* img = (tid == kTexStage) ? stage_tex : ((tid == kTexPip) ? pip_tex : nullptr);
            const int cx0 = std::max(0, static_cast<int>(cmd.ClipRect.x));
            const int cy0 = std::max(0, static_cast<int>(cmd.ClipRect.y));
            const int cx1 = std::min(w, static_cast<int>(std::ceil(cmd.ClipRect.z)));
            const int cy1 = std::min(h, static_cast<int>(std::ceil(cmd.ClipRect.w)));
            for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawVert& v0 =
                    dl->VtxBuffer[static_cast<int>(cmd.VtxOffset + dl->IdxBuffer[static_cast<int>(cmd.IdxOffset + i)])];
                const ImDrawVert& v1 = dl->VtxBuffer[static_cast<int>(
                    cmd.VtxOffset + dl->IdxBuffer[static_cast<int>(cmd.IdxOffset + i + 1U)])];
                const ImDrawVert& v2 = dl->VtxBuffer[static_cast<int>(
                    cmd.VtxOffset + dl->IdxBuffer[static_cast<int>(cmd.IdxOffset + i + 2U)])];
                const float area =
                    (v1.pos.x - v0.pos.x) * (v2.pos.y - v0.pos.y) - (v2.pos.x - v0.pos.x) * (v1.pos.y - v0.pos.y);
                if (std::fabs(area) < 1e-6f) {
                    continue;
                }
                const int x0 = std::max(cx0, static_cast<int>(std::floor(std::min({v0.pos.x, v1.pos.x, v2.pos.x}))));
                const int y0 = std::max(cy0, static_cast<int>(std::floor(std::min({v0.pos.y, v1.pos.y, v2.pos.y}))));
                const int x1 = std::min(cx1, static_cast<int>(std::ceil(std::max({v0.pos.x, v1.pos.x, v2.pos.x}))));
                const int y1 = std::min(cy1, static_cast<int>(std::ceil(std::max({v0.pos.y, v1.pos.y, v2.pos.y}))));
                const Rgba c0 = unpack(v0.col);
                const Rgba c1 = unpack(v1.col);
                const Rgba c2 = unpack(v2.col);
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        const float px = static_cast<float>(x) + 0.5f;
                        const float py = static_cast<float>(y) + 0.5f;
                        const float b0 = ((v1.pos.x - px) * (v2.pos.y - py) - (v2.pos.x - px) * (v1.pos.y - py)) / area;
                        const float b1 = ((v2.pos.x - px) * (v0.pos.y - py) - (v0.pos.x - px) * (v2.pos.y - py)) / area;
                        const float b2 = 1.0f - b0 - b1;
                        if (b0 < -1e-4f || b1 < -1e-4f || b2 < -1e-4f) {
                            continue;
                        }
                        float a = c0.a * b0 + c1.a * b1 + c2.a * b2;
                        float cr = c0.r * b0 + c1.r * b1 + c2.r * b2;
                        float cg = c0.g * b0 + c1.g * b1 + c2.g * b2;
                        float cb = c0.b * b0 + c1.b * b1 + c2.b * b2;
                        const float u = v0.uv.x * b0 + v1.uv.x * b1 + v2.uv.x * b2;
                        const float v = v0.uv.y * b0 + v1.uv.y * b1 + v2.uv.y * b2;
                        if (font_tex) {
                            const int tx =
                                std::clamp(static_cast<int>(u * static_cast<float>(atlas_w)), 0, atlas_w - 1);
                            const int ty =
                                std::clamp(static_cast<int>(v * static_cast<float>(atlas_h)), 0, atlas_h - 1);
                            a *= static_cast<float>(atlas[static_cast<size_t>(ty) * static_cast<size_t>(atlas_w) +
                                                          static_cast<size_t>(tx)]) /
                                 255.0f;
                        } else if (img != nullptr && img->w > 0u) {
                            const uint32_t tx =
                                std::min(img->w - 1u, static_cast<uint32_t>(std::max(0.0f, u * img->w)));
                            const uint32_t ty =
                                std::min(img->h - 1u, static_cast<uint32_t>(std::max(0.0f, v * img->h)));
                            const uint8_t* s = img->at(tx, ty);
                            cr *= s[0] / 255.0f;
                            cg *= s[1] / 255.0f;
                            cb *= s[2] / 255.0f;
                        }
                        if (a <= 0.0f) {
                            continue;
                        }
                        const size_t o =
                            (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3U;
                        rgb[o + 0] = rgb[o + 0] * (1.0f - a) + cr * a;
                        rgb[o + 1] = rgb[o + 1] * (1.0f - a) + cg * a;
                        rgb[o + 2] = rgb[o + 2] * (1.0f - a) + cb * a;
                    }
                }
            }
        }
    }
    std::vector<uint8_t> out(static_cast<size_t>(w) * static_cast<size_t>(h) * 4U, 255);
    for (size_t p = 0; p < static_cast<size_t>(w) * static_cast<size_t>(h); ++p) {
        for (size_t k = 0; k < 3; ++k) {
            out[p * 4 + k] = static_cast<uint8_t>(std::lround(std::clamp(rgb[p * 3 + k], 0.0f, 1.0f) * 255.0f));
        }
    }
    return out;
}

struct UiRig {
    rhi::RhiDevice* rhi{nullptr};
    TelemetrySnapshot telem{};
    ui::DevShellInputEcho echo{};
    ui::DevShellAvatarView avatar{};
    ui::DevShellTrackingView tracking{};
    ui::DevShellRenderView render_view{};
    ui::DevShellOutputView output_view{};
    ui::StudioActions actions{};
    double t{0.0};

    void frame(ui::StudioWorkspace& ws) {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ws.render(t, *rhi, telem, echo, ui::ImGuiArenaAllocator{}, true, avatar, tracking, render_view, output_view,
                  actions);
        ImGui::Render();
        t += 1.0 / 60.0;
        const ImGuiContext& g = *GImGui;
        CHECK(g.ColorStack.Size == 0);
        CHECK(g.StyleVarStack.Size == 0);
        CHECK(g.FontStack.Size == 0);
        CHECK(g.CurrentWindowStack.Size == 0);
        CHECK(g.GroupStack.Size == 0);
        CHECK(g.ItemFlagsStack.Size <= 1);
        CHECK(g.FocusScopeStack.Size == 0);
    }
};

[[nodiscard]] bool inside(const std::array<float, 4>& in, const std::array<float, 4>& out, float eps = 0.5f) {
    return in[0] >= out[0] - eps && in[1] >= out[1] - eps && in[2] <= out[2] + eps && in[3] <= out[3] + eps;
}

[[nodiscard]] bool overlaps(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
}

} // namespace

TEST_CASE("Lighting tab: non-blank Stage, in-canvas Grid/Schematic, uncropped PiP, 1:1 aspect, single-row UI "
          "(Vulkan + DX12) on 8590256991748008892.vrm",
          "[lighting_tab][vrm][snapshot][dx12][vulkan]") {
    const auto vrm_path = find_user_vrm_path();
    REQUIRE_FALSE(vrm_path.empty());

    rhi::RhiDeviceConfig vk_cfg{};
    vk_cfg.headless = true;
    vk_cfg.enable_validation = true;
    vk_cfg.require_validation = false;
    auto vk_dev_res = rhi::RhiDevice::create(vk_cfg);
    REQUIRE(vk_dev_res.has_value());
    std::unique_ptr<rhi::RhiDevice> rhi(vk_dev_res.value());

    auto avatar = load_avatar_file(*rhi, vrm_path);
    REQUIRE(avatar != nullptr);

    renderer::NprRenderer npr;
    renderer::RendererConfig rcfg{};
    rcfg.width = kOutW;
    rcfg.height = kOutH;
    rcfg.enable_rt = false;
    REQUIRE(npr.init(*rhi, rcfg).has_value());
    REQUIRE(npr.bind_avatar(avatar.get()).has_value());

    renderer::LightingRig rig = renderer::LightingRig::default_three_point();
    renderer::ensure_studio_lights_initialized(rig);
    const renderer::PreviewCamera bcast_cam = renderer::fit_camera_to_avatar(*avatar);

    AppSequence seq{*rhi, npr, *avatar, rig, bcast_cam, nullptr};
    uint64_t fidx = 1u;
    const auto stage_cam = default_stage_camera(false);

    // ---------------------------------------------------------------- DirectX 12 Broadcast (external frame)
    // Runs FIRST on the fresh renderer: its Vulkan skinned-vertex buffer has never been written by a Broadcast pass,
    // which is exactly the live-app state when DX12 renders every Broadcast frame.
    GarbageCollector gc;
    renderer::Dx12Bridge bridge;
    bridge.attach(*rhi, gc);
    const bool dx12_ok =
        bridge.configure(kOutW, kOutH, avatar->requirements.vertex_count, &rhi->gpu_info()).has_value() &&
        bridge.bind_avatar(avatar.get()).has_value();
    if (!dx12_ok) {
        WARN("DirectX 12 unavailable: DX12 half of the Lighting tab verification skipped");
    } else {
        AppSequence dseq{*rhi, npr, *avatar, rig, bcast_cam, &bridge};
        constexpr uint32_t kSw = 1280u;
        constexpr uint32_t kSh = 480u;
        (void)dseq.run(fidx++, stage_cam, kSw, kSh, true, true);
        (void)dseq.run(fidx++, stage_cam, kSw, kSh, true, true);
        const AppFrameResult dx_schem = dseq.run(fidx++, stage_cam, kSw, kSh, true, true);
        const AppFrameResult dx_plain = dseq.run(fidx++, stage_cam, kSw, kSh, false, false);
        CHECK(dx_schem.used_external);
        check_stage_not_blank(dx_plain.stage, "dx12");
        check_pip_matches_live(dx_schem, "dx12");
        const auto snap = snapshot_out_dir();
        CHECK(test::save_rgba8_png(snap / "lighting_stage_dx12_1280x480.png", kSw, kSh, dx_schem.stage.px.data()));
        CHECK(test::save_rgba8_png(snap / "lighting_pip_dx12.png", dx_schem.pip.w, dx_schem.pip.h,
                                   dx_schem.pip.px.data()));
        rhi->wait_idle();
        bridge.release();
        (void)gc.flush_all_epochs();
    }

    // ---------------------------------------------------------------- Vulkan
    {
        constexpr uint32_t kSw = 1280u; // wide (w != h): the old `y = w` schematic viewport pushed the overlay out
        constexpr uint32_t kSh = 480u;
        (void)seq.run(fidx++, stage_cam, kSw, kSh, true, true); // warm-up (history, layouts)
        const AppFrameResult with_schem = seq.run(fidx++, stage_cam, kSw, kSh, true, true);
        const AppFrameResult no_schem = seq.run(fidx++, stage_cam, kSw, kSh, false, false);
        CHECK_FALSE(with_schem.used_external);
        check_stage_not_blank(no_schem.stage, "vulkan");

        uint32_t diff_px = 0u, diff_bottom = 0u;
        double sum_y = 0.0;
        for (uint32_t y = 0; y < kSh; ++y) {
            for (uint32_t x = 0; x < kSw; ++x) {
                const uint8_t* a = with_schem.stage.at(x, y);
                const uint8_t* b = no_schem.stage.at(x, y);
                if (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) > 36) {
                    ++diff_px;
                    diff_bottom += (y > kSh / 2u) ? 1u : 0u;
                    sum_y += y;
                }
            }
        }
        const double cy = diff_px > 0u ? sum_y / diff_px / kSh : 0.0;
        std::printf("[LIGHTING_TAB] vulkan grid+schem px inside %ux%u canvas=%u bottom_half=%u centroid_y=%.3f\n", kSw,
                    kSh, diff_px, diff_bottom, cy);
        CHECK(diff_px > 2000u);    // grid + 4 schematic fixtures + frustum are drawn inside the canvas
        CHECK(diff_bottom > 400u); // floor grid below the horizon
        CHECK(cy > 0.15);
        CHECK(cy < 0.90);

        check_pip_matches_live(with_schem, "vulkan");

        // Second frame after a Stage pass: PiP must still be the clean Broadcast (no Stage contamination).
        const AppFrameResult again = seq.run(fidx++, stage_cam, kSw, kSh, true, true);
        check_pip_matches_live(again, "vulkan_after_stage");

        const auto snap = snapshot_out_dir();
        CHECK(test::save_rgba8_png(snap / "lighting_stage_vk_1280x480.png", kSw, kSh, with_schem.stage.px.data()));
        CHECK(test::save_rgba8_png(snap / "lighting_pip_vk.png", with_schem.pip.w, with_schem.pip.h,
                                   with_schem.pip.px.data()));
    }

    // ---------------------------------------------------------------- 1:1 pixel aspect (front cam, no overlay)
    {
        const auto front = default_stage_camera(true);
        const Image s169 = seq.run(fidx++, front, 1600u, 900u, false, false).stage;
        const Image swide = seq.run(fidx++, front, 1600u, 360u, false, false).stage;
        const Image stall = seq.run(fidx++, front, 450u, 900u, false, false).stage;
        const Bbox b169 = bbox_bright(s169, 30);
        const Bbox bwide = bbox_bright(swide, 30);
        const Bbox btall = bbox_bright(stall, 30);
        REQUIRE(b169.valid());
        REQUIRE(bwide.valid());
        REQUIRE(btall.valid());
        std::printf("[LIGHTING_TAB] aspect bbox 1600x900=%ux%u 1600x360=%ux%u 450x900=%ux%u\n", b169.w(), b169.h(),
                    bwide.w(), bwide.h(), btall.w(), btall.h());
        // Same vertical FOV at equal height -> identical avatar pixel height regardless of canvas width.
        CHECK(std::abs(static_cast<int>(b169.h()) - static_cast<int>(btall.h())) <= 3);
        // Height scales by 360/900 and width by the same factor -> width/height ratio preserved (1:1 pixels).
        const double r169 = static_cast<double>(b169.w()) / b169.h();
        const double rwide = static_cast<double>(bwide.w()) / bwide.h();
        CHECK(std::fabs(rwide / r169 - 1.0) < 0.04);
        CHECK(std::fabs(static_cast<double>(bwide.h()) / b169.h() - 0.4) < 0.02);
    }

    // ---------------------------------------------------------------- Real StudioWorkspace Lighting tab UI
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigInputTrickleEventQueue = false;
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    const RuntimePaths paths = discover_runtime_paths();
    HardwareInventory hw{};
    ui::StudioWorkspace ws;
    ws.init(paths, hw, 1.0f, nullptr, false);
    unsigned char* atlas = nullptr;
    int aw = 0;
    int ah = 0;
    io.Fonts->GetTexDataAsAlpha8(&atlas, &aw, &ah);
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));

    UiRig ui_rig{};
    ui_rig.rhi = rhi.get();
    ui_rig.avatar.has_avatar = true;
    ui_rig.avatar.has_camera = true;
    ui_rig.avatar.bone_head_y = 1.42f;
    ui_rig.avatar.bone_chest_y = 1.18f;
    ui_rig.avatar.camera_eye = {{0.0f, 1.30f, 2.0f}};
    ui_rig.avatar.camera_target = {{0.0f, 1.30f, 0.0f}};
    ui_rig.avatar.broadcast_width = kOutW;
    ui_rig.avatar.broadcast_height = kOutH;
    ui_rig.avatar.stage_viewport_texture = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(kTexStage));
    ui_rig.avatar.stage_broadcast_pip_texture = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(kTexPip));

    // Balanced ImGui stacks in every mode (UiRig::frame asserts after each frame).
    for (const auto m :
         {ui::StudioMode::LiveStudio, ui::StudioMode::Rigging, ui::StudioMode::Nodes, ui::StudioMode::Lighting}) {
        ws.switch_mode(m);
        for (int f = 0; f < 6; ++f) {
            ui_rig.frame(ws);
        }
    }

    auto& ls = ws.lighting_stage();
    struct Size {
        float w, h;
    };
    for (const Size sz : {Size{1280.0f, 720.0f}, Size{1920.0f, 1080.0f}, Size{1600.0f, 900.0f}, Size{1600.0f, 360.0f},
                          Size{450.0f, 900.0f}}) {
        io.DisplaySize = ImVec2(sz.w, sz.h);
        for (int f = 0; f < 8; ++f) {
            ui_rig.frame(ws);
        }
        const ImGuiWindow* vpw = ImGui::FindWindowByName("Viewport");
        REQUIRE(vpw != nullptr);
        const std::array<float, 4> win{vpw->InnerRect.Min.x, vpw->InnerRect.Min.y, vpw->InnerRect.Max.x,
                                       vpw->InnerRect.Max.y};
        const auto canvas = ls.last_canvas_rect();
        INFO("display " << sz.w << "x" << sz.h << " toolbar_h=" << ls.last_toolbar_height() << " toolbar_right="
                        << ls.last_toolbar_right() << " limit=" << ls.last_toolbar_limit() << " canvas=(" << canvas[0]
                        << "," << canvas[1] << ")-(" << canvas[2] << "," << canvas[3] << ")");
        std::printf("[LIGHTING_TAB] ui %gx%g toolbar_h=%.1f right=%.1f/%.1f canvas=(%.0f,%.0f)-(%.0f,%.0f)\n", sz.w,
                    sz.h, ls.last_toolbar_height(), ls.last_toolbar_right(), ls.last_toolbar_limit(), canvas[0],
                    canvas[1], canvas[2], canvas[3]);
        CHECK(ls.last_toolbar_height() > 0.0f);
        CHECK(ls.last_toolbar_height() <= 36.0f);
        CHECK(ls.last_toolbar_right() <= ls.last_toolbar_limit() + 0.5f);
        CHECK(canvas[2] - canvas[0] >= 32.0f);
        CHECK(canvas[3] - canvas[1] >= 32.0f);
        CHECK(inside(canvas, win));
        CHECK(vpw->ScrollMax.y == 0.0f);
        CHECK(vpw->Scroll.y == 0.0f);
        CHECK(ui_rig.actions.viewport.desired_stage_w == static_cast<uint32_t>(canvas[2] - canvas[0]));
        CHECK(ui_rig.actions.viewport.desired_stage_h == static_cast<uint32_t>(canvas[3] - canvas[1]));
        if (ls.pip_monitor_open()) {
            const auto pip = ls.last_pip_outer_rect();
            if (pip[2] > pip[0]) {
                CHECK(inside(pip, canvas));
                CHECK_FALSE(overlaps(pip, ls.last_viewcube_rect()));
                const auto uv = ls.last_pip_uv();
                CHECK(uv == std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f});
            }
        }
    }

    // Wheel-zoom over the canvas never scrolls the Viewport window (canvas stays put).
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    for (int f = 0; f < 4; ++f) {
        ui_rig.frame(ws);
    }
    const auto canvas_before = ls.last_canvas_rect();
    const float dist_before = ls.stage_camera().distance;
    io.AddMousePosEvent(canvas_before[0] + (canvas_before[2] - canvas_before[0]) * 0.3f,
                        (canvas_before[1] + canvas_before[3]) * 0.5f);
    for (int f = 0; f < 6; ++f) {
        io.AddMouseWheelEvent(0.0f, -1.0f);
        ui_rig.frame(ws);
    }
    const ImGuiWindow* vpw = ImGui::FindWindowByName("Viewport");
    REQUIRE(vpw != nullptr);
    CHECK(vpw->Scroll.y == 0.0f);
    CHECK(ls.last_canvas_rect() == canvas_before);
    CHECK(ls.stage_camera().distance > dist_before); // the wheel went to the Stage camera zoom instead
    io.AddMousePosEvent(-1000.0f, -1000.0f);
    // Restore the default framing for the snapshot.
    ls.set_stage_view(renderer::StageCameraView::FreePerspective, {0.0f, 1.15f, 0.0f});
    for (int f = 0; f < 2; ++f) {
        ui_rig.frame(ws);
    }

    // Full lighting_tab.png: render the Stage on the GPU at the UI's exact canvas size and bind it + the PiP.
    {
        const uint32_t cw = ui_rig.actions.viewport.desired_stage_w;
        const uint32_t ch = ui_rig.actions.viewport.desired_stage_h;
        REQUIRE(cw >= 64u);
        REQUIRE(ch >= 64u);
        AppSequence useq{*rhi, npr, *avatar, rig, bcast_cam, nullptr};
        (void)useq.run(fidx++, ui_rig.actions.viewport.stage_camera, cw, ch, true, true);
        const AppFrameResult r = useq.run(fidx++, ui_rig.actions.viewport.stage_camera, cw, ch, true, true);
        check_stage_not_blank(r.stage, "ui_canvas");
        const auto o = npr.graph().outputs();
        ui_rig.avatar.stage_viewport_w = o.stage_viewport_width;
        ui_rig.avatar.stage_viewport_h = o.stage_viewport_height;
        ui_rig.avatar.stage_viewport_rt_w = o.stage_viewport_rt_width;
        ui_rig.avatar.stage_viewport_rt_h = o.stage_viewport_rt_height;
        ui_rig.avatar.stage_broadcast_pip_w = o.stage_broadcast_pip_width;
        ui_rig.avatar.stage_broadcast_pip_h = o.stage_broadcast_pip_height;
        for (int f = 0; f < 3; ++f) {
            ui_rig.frame(ws);
        }
        const std::vector<uint8_t> px = rasterize(ImGui::GetDrawData(), 1600, 900, atlas, aw, ah, &r.stage_rt, &r.pip);
        const Image tab{px, 1600u, 900u};
        const auto canvas = ls.last_canvas_rect();
        const Rect centre{static_cast<uint32_t>(canvas[0] + (canvas[2] - canvas[0]) * 0.35f),
                          static_cast<uint32_t>(canvas[1] + (canvas[3] - canvas[1]) * 0.15f),
                          static_cast<uint32_t>(canvas[0] + (canvas[2] - canvas[0]) * 0.65f),
                          static_cast<uint32_t>(canvas[1] + (canvas[3] - canvas[1]) * 0.85f)};
        const uint32_t lit = count_bright(tab, centre, 40);
        const uint32_t area = (centre.x1 - centre.x0) * (centre.y1 - centre.y0);
        std::printf("[LIGHTING_TAB] lighting_tab.png canvas centre avatar px=%u / %u\n", lit, area);
        CHECK(lit * 50u > area);
        const auto path = snapshot_out_dir() / "lighting_tab.png";
        CHECK(test::save_rgba8_png(path, 1600u, 900u, px.data()));
        std::printf("[UI_SNAPSHOT] %s\n", path.string().c_str());
    }

    ws.shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    rhi->wait_idle();
    npr.destroy();
    avatar->destroy();
    CHECK(rhi->validation_error_count() == 0u);
    rhi->destroy();
}
