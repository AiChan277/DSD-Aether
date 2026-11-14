// Headless visual snapshots of the Studio workspace (broadcast overhaul §5.3 layout review).
// Renders the real StudioWorkspace through ImGui, rasterizes the draw lists on the CPU (font atlas
// sampled; the 3D viewport texture is absent headless) and writes PNGs to
// <build>/tests/ui_snapshots/ so every zone can be reviewed at 100% and 150% DPI without a GPU swapchain.

#include "core/hw_enum.hpp"
#include "core/paths.hpp"
#include "core/telemetry.hpp"
#include "rhi/rhi_device.hpp"
#include "test_png_io.hpp"
#include "ui/theme/precision_dark.hpp"
#include "ui/workspace/studio_workspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace aether::ui {
namespace {

struct Rgba {
    float r, g, b, a;
};

Rgba unpack(ImU32 c) noexcept {
    return Rgba{static_cast<float>(c & 0xFFU) / 255.0f, static_cast<float>((c >> 8) & 0xFFU) / 255.0f,
                static_cast<float>((c >> 16) & 0xFFU) / 255.0f, static_cast<float>((c >> 24) & 0xFFU) / 255.0f};
}

/** Minimal CPU rasterizer for ImDrawData: per-pixel barycentric fill, alpha-8 font atlas, src-over blend. */
std::vector<uint8_t> rasterize(const ImDrawData* dd, int w, int h, const uint8_t* atlas, int atlas_w, int atlas_h) {
    std::vector<float> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3U, 0.0f);
    for (int l = 0; l < dd->CmdListsCount; ++l) {
        const ImDrawList* dl = dd->CmdLists[l];
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback != nullptr) {
                continue;
            }
            const bool font_tex = cmd.GetTexID() == static_cast<ImTextureID>(1);
            const int cx0 = std::max(0, static_cast<int>(cmd.ClipRect.x));
            const int cy0 = std::max(0, static_cast<int>(cmd.ClipRect.y));
            const int cx1 = std::min(w, static_cast<int>(std::ceil(cmd.ClipRect.z)));
            const int cy1 = std::min(h, static_cast<int>(std::ceil(cmd.ClipRect.w)));
            for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const int idx0 = static_cast<int>(cmd.VtxOffset + dl->IdxBuffer[static_cast<int>(cmd.IdxOffset + i)]);
                const int idx1 =
                    static_cast<int>(cmd.VtxOffset + dl->IdxBuffer[static_cast<int>(cmd.IdxOffset + i + 1U)]);
                const int idx2 =
                    static_cast<int>(cmd.VtxOffset + dl->IdxBuffer[static_cast<int>(cmd.IdxOffset + i + 2U)]);
                const ImDrawVert& v0 = dl->VtxBuffer[idx0];
                const ImDrawVert& v1 = dl->VtxBuffer[idx1];
                const ImDrawVert& v2 = dl->VtxBuffer[idx2];
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
                        if (font_tex) {
                            const float u = v0.uv.x * b0 + v1.uv.x * b1 + v2.uv.x * b2;
                            const float v = v0.uv.y * b0 + v1.uv.y * b1 + v2.uv.y * b2;
                            const int tx =
                                std::clamp(static_cast<int>(u * static_cast<float>(atlas_w)), 0, atlas_w - 1);
                            const int ty =
                                std::clamp(static_cast<int>(v * static_cast<float>(atlas_h)), 0, atlas_h - 1);
                            a *= static_cast<float>(atlas[static_cast<size_t>(ty) * static_cast<size_t>(atlas_w) +
                                                          static_cast<size_t>(tx)]) /
                                 255.0f;
                        }
                        if (a <= 0.0f) {
                            continue;
                        }
                        const size_t o =
                            (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3U;
                        rgb[o + 0] = rgb[o + 0] * (1.0f - a) + (c0.r * b0 + c1.r * b1 + c2.r * b2) * a;
                        rgb[o + 1] = rgb[o + 1] * (1.0f - a) + (c0.g * b0 + c1.g * b1 + c2.g * b2) * a;
                        rgb[o + 2] = rgb[o + 2] * (1.0f - a) + (c0.b * b0 + c1.b * b1 + c2.b * b2) * a;
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

struct SnapshotRig {
    rhi::RhiDevice* rhi{nullptr};
    TelemetrySnapshot telem{};
    DevShellInputEcho echo{};
    DevShellAvatarView avatar{};
    DevShellTrackingView tracking{};
    DevShellRenderView render_view{};
    DevShellOutputView output_view{};
    StudioActions actions{};
    double t{0.0};

    void frame(StudioWorkspace& ws) {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ws.render(t, *rhi, telem, echo, ImGuiArenaAllocator{}, true, avatar, tracking, render_view, output_view,
                  actions);
        ImGui::Render();
        t += 1.0 / 60.0;
    }
};

std::filesystem::path snapshot_dir() {
    return std::filesystem::current_path() / "ui_snapshots";
}

void save_snapshot(const char* name, int w, int h) {
    unsigned char* atlas = nullptr;
    int aw = 0;
    int ah = 0;
    ImGui::GetIO().Fonts->GetTexDataAsAlpha8(&atlas, &aw, &ah);
    const std::vector<uint8_t> px = rasterize(ImGui::GetDrawData(), w, h, atlas, aw, ah);
    const auto path = snapshot_dir() / name;
    REQUIRE(test::save_rgba8_png(path, static_cast<uint32_t>(w), static_cast<uint32_t>(h), px.data()));
    std::printf("[UI_SNAPSHOT] %s\n", path.string().c_str());
}

} // namespace

TEST_CASE("Studio workspace headless snapshots (Live / Rigging / Nodes, 100% and 150% DPI)", "[ui][snapshot]") {
    rhi::RhiDeviceConfig rhi_cfg{};
    rhi_cfg.enable_validation = false;
    rhi_cfg.require_validation = false;
    rhi_cfg.headless = true;
    auto rhi_res = rhi::RhiDevice::create(rhi_cfg);
    REQUIRE(rhi_res.has_value());

    for (const float dpi : {1.0f, 1.5f}) {
        ImGui::CreateContext();
        ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        const int w = static_cast<int>(1600.0f * dpi);
        const int h = static_cast<int>(900.0f * dpi);
        io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigInputTrickleEventQueue = false;

        const RuntimePaths paths = discover_runtime_paths();
        HardwareInventory hw{};
        StudioWorkspace ws;
        ws.init(paths, hw, dpi, nullptr, false);
        unsigned char* px = nullptr;
        int tw = 0;
        int th = 0;
        io.Fonts->GetTexDataAsAlpha8(&px, &tw, &th);
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));

        SnapshotRig rig{};
        rig.rhi = rhi_res.value();
        rig.avatar.has_avatar = true;
        rig.avatar.wireframe_supported = true;
        rig.avatar.has_camera = true;
        rig.avatar.camera_eye = {{0.716f, 1.110f, 1.961f}};
        rig.avatar.camera_target = {{0.0f, 0.90f, 0.0f}};
        rig.avatar.camera_fovy_rad = 0.62f;
        rig.avatar.gizmo_pivot_y = 0.90f;
        rig.tracking.ready = true;
        rig.tracking.active_ep_label = "OpenVINO NPU";
        rig.tracking.inference_ms = 5.4f;
        rig.tracking.face_confidence = 0.93f;
        rig.tracking.camera_active = true;
        rig.tracking.camera_label = "Integrated Camera";
        rig.tracking.camera_width = 1280U;
        rig.tracking.camera_height = 720U;
        rig.tracking.camera_fps = 30U;
        rig.tracking.neutral_calibrated = true;
        rig.tracking.live_preview_valid = true;
        rig.tracking.live_preview_texture_id = 2ULL;
        rig.tracking.live_preview_width = 320U;
        rig.tracking.live_preview_height = 180U;
        rig.tracking.live_preview_face_locked = true;
        rig.tracking.live_preview_face_roi = {0.50f, 0.48f, 0.44f, 0.62f};
        rig.tracking.live_preview_keypoint_count = 68U;
        rig.tracking.live_preview_keypoint_valid.fill(1U);
        for (uint32_t i = 0; i <= 16U; ++i) {
            const float t = static_cast<float>(i) / 16.0f;
            rig.tracking.live_preview_keypoints[i] = {0.36f + t * 0.28f,
                                                      0.45f + 0.24f * std::sin(t * 3.14159265f)};
        }
        for (uint32_t i = 0; i < 5U; ++i) {
            const float t = static_cast<float>(i) / 4.0f;
            rig.tracking.live_preview_keypoints[17U + i] = {0.39f + t * 0.08f,
                                                            0.35f - 0.025f * std::sin(t * 3.14159265f)};
            rig.tracking.live_preview_keypoints[22U + i] = {0.53f + t * 0.08f,
                                                            0.35f - 0.025f * std::sin(t * 3.14159265f)};
        }
        for (uint32_t i = 0; i < 4U; ++i) {
            rig.tracking.live_preview_keypoints[27U + i] = {0.50f, 0.39f + 0.04f * static_cast<float>(i)};
        }
        for (uint32_t i = 0; i < 5U; ++i) {
            const float t = static_cast<float>(i) / 4.0f;
            rig.tracking.live_preview_keypoints[31U + i] = {0.46f + t * 0.08f,
                                                            0.53f - 0.015f * std::sin(t * 3.14159265f)};
        }
        for (uint32_t i = 0; i < 6U; ++i) {
            const float a = static_cast<float>(i) * 6.2831853f / 6.0f;
            rig.tracking.live_preview_keypoints[36U + i] = {0.43f + 0.032f * std::cos(a),
                                                            0.41f + 0.018f * std::sin(a)};
            rig.tracking.live_preview_keypoints[42U + i] = {0.57f + 0.032f * std::cos(a),
                                                            0.41f + 0.018f * std::sin(a)};
        }
        rig.tracking.live_preview_iris_valid = true;
        rig.tracking.live_preview_iris_centers[0] = {0.43f, 0.41f};
        rig.tracking.live_preview_iris_centers[1] = {0.57f, 0.41f};
        for (uint32_t i = 0; i < 12U; ++i) {
            const float a = static_cast<float>(i) * 6.2831853f / 12.0f;
            rig.tracking.live_preview_keypoints[48U + i] = {0.50f + 0.055f * std::cos(a),
                                                            0.60f + 0.030f * std::sin(a)};
        }
        for (uint32_t i = 0; i < 8U; ++i) {
            const float a = static_cast<float>(i) * 6.2831853f / 8.0f;
            rig.tracking.live_preview_keypoints[60U + i] = {0.50f + 0.038f * std::cos(a),
                                                            0.60f + 0.018f * std::sin(a)};
        }
        rig.tracking.live2d.param_angle_x = 4.2f;
        rig.tracking.live2d.param_angle_y = -1.8f;
        rig.tracking.live2d.param_angle_z = 0.9f;
        rig.tracking.live2d.param_eye_l_open = 0.95f;
        rig.tracking.live2d.param_eye_r_open = 0.95f;
        rig.tracking.live2d.param_brow_l_y = 0.42f;
        rig.tracking.live2d.param_brow_r_y = 0.39f;
        rig.tracking.live2d.param_mouth_open_y = 0.58f;
        rig.tracking.live2d.param_mouth_form = 0.25f;
        rig.tracking.live2d.vowel_a = 0.52f;
        rig.tracking.live2d.vowel_e = 0.22f;
        rig.render_view.total_gpu_ms = 2.9f;
        rig.render_view.denoiser_active_name = "NVIDIA NRD SIGMA (Composite)";
        rig.output_view.spout_active = true;
        rig.output_view.osc_active = false;
        rig.telem.render_fps = 332.0f;

        // Seed realistic per-node FFI telemetry and pin watch values for Nodes snapshots
        StudioHostStatus host{};
        host.host_alive = true;
        host.compiled_node_count = static_cast<uint32_t>(ws.node_canvas().nodes().size());
        host.last_compile_ms = 0.82f;
        host.last_eval_us = 18.4f;
        host.telemetry.struct_size = static_cast<uint32_t>(sizeof(AetherGraphTelemetryReadout));
        host.telemetry.valid = 1u;
        host.telemetry.eval_sequence = 120u;
        host.telemetry.total_eval_ns = 18400ULL;
        {
            const auto& nodes = ws.node_canvas().nodes();
            uint32_t ni = 0u;
            uint32_t pi = 0u;
            for (const auto& n : nodes) {
                if (ni < AETHER_GRAPH_MAX_DEBUG_NODES) {
                    host.telemetry.node_ids[ni] = n.id;
                    host.telemetry.node_eval_ns[ni] = 1800u + ni * 950u;
                    ++ni;
                }
                for (uint8_t oi = 0; oi < n.num_outputs; ++oi) {
                    if (pi >= AETHER_GRAPH_MAX_DEBUG_PINS) {
                        break;
                    }
                    host.telemetry.pin_node_ids[pi] = n.id;
                    host.telemetry.pin_indices[pi] = oi;
                    host.telemetry.pin_kinds[pi] =
                        (std::string(n.kind.data()) == "aether.source.dsp_audio@1") ? AETHER_PIN_VALUE_VEC3
                                                                                    : AETHER_PIN_VALUE_FLOAT;
                    host.telemetry.pin_values[pi][0] = 0.72f + static_cast<float>(oi) * 0.14f;
                    host.telemetry.pin_values[pi][1] = 0.58f;
                    host.telemetry.pin_values[pi][2] = 0.91f;
                    ++pi;
                }
            }
            host.telemetry.active_node_count = ni;
            host.telemetry.pin_readout_count = pi;
            // Pin watches on DSP Audio In (AudioFrame Purple) & Formant Lip-Sync (Viseme Amber)
            for (const auto& n : nodes) {
                if (std::string(n.kind.data()) == "aether.source.dsp_audio@1" ||
                    std::string(n.kind.data()) == "aether.expr.formant_lipsync@1") {
                    (void)ws.node_canvas().set_pin_watched(PinRef{n.id, 0u, false}, true);
                }
            }
        }
        ws.set_host_status(host);

        char name[64]{};
        const struct {
            StudioMode mode;
            const char* tag;
        } modes[] = {{StudioMode::LiveStudio, "live"}, {StudioMode::Rigging, "rigging"}, {StudioMode::Nodes, "nodes"}};
        for (const auto& m : modes) {
            ws.switch_mode(m.mode);
            for (int f = 0; f < 12; ++f) {
                rig.frame(ws);
            }
            std::snprintf(name, sizeof(name), "studio_%s_%d.png", m.tag, static_cast<int>(dpi * 100.0f));
            save_snapshot(name, w, h);
        }

        // 100% DPI: also capture interactive Move 3D Gizmo + Wireframe/Colliders in Live mode,
        // plus Nodes mode with selected node + hovered wire + watched pins & Eval telemetry.
        if (dpi == 1.0f) {
            ws.switch_mode(StudioMode::LiveStudio);
            ws.viewport_panel().set_gizmo_tool(GizmoTool::Move);
            for (int f = 0; f < 10; ++f) {
                rig.frame(ws);
            }
            save_snapshot("studio_gizmo_move_100.png", w, h);

            ws.switch_mode(StudioMode::Nodes);
            NodeCanvas& canvas = ws.node_canvas();
            for (int f = 0; f < 4; ++f) {
                rig.frame(ws);
            }
            const uint64_t pose_link = canvas.find_input_link(2, 0)->id;
            const ImVec2 mid = canvas.link_midpoint_screen(pose_link);
            io.AddMousePosEvent(mid.x, mid.y);
            rig.frame(ws);
            io.AddMouseButtonEvent(0, true);
            rig.frame(ws);
            io.AddMouseButtonEvent(0, false);
            rig.frame(ws);
            const ImVec2 pin = canvas.pin_screen_position(PinRef{5, 1, false});
            io.AddMousePosEvent(pin.x, pin.y);
            for (int f = 0; f < 3; ++f) {
                rig.frame(ws);
            }
            save_snapshot("studio_nodes_wire_selected_100.png", w, h);

            ws.switch_mode(StudioMode::LiveStudio);
            for (int f = 0; f < 4; ++f) {
                rig.frame(ws);
            }
            ImGui::SetWindowFocus("Expressions");
            for (int f = 0; f < 4; ++f) {
                rig.frame(ws);
            }
            save_snapshot("studio_expressions_100.png", w, h);
        }

        ws.shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }
    rhi_res.value()->destroy();
    delete rhi_res.value();
}

} // namespace aether::ui
