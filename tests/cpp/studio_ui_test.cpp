#include "core/hot_path_guard.hpp"
#include "core/hw_enum.hpp"
#include "core/paths.hpp"
#include "core/telemetry.hpp"
#include "core/viewport_aspect.hpp"
#include "rhi/rhi_device.hpp"
#include "ui/palette/command_palette.hpp"
#include "ui/panels/viewport/camera_presets.hpp"
#include "ui/params/param_registry.hpp"
#include "ui/params/project_settings.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/theme/precision_dark.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/widgets/property_row.hpp"
#include "ui/wizard/hardware_optimizer_wizard.hpp"
#include "ui/workspace/studio_workspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace aether::ui {
namespace {

struct ScopedTempDir {
    std::filesystem::path dir{};

    explicit ScopedTempDir(const char* tag) {
        std::error_code ec;
        const auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        dir = std::filesystem::temp_directory_path(ec) /
              ("aether_ui_test_" + std::string(tag) + "_" + std::to_string(now_ns));
        std::filesystem::create_directories(dir, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

struct ScopedUiRig {
    rhi::RhiDevice* rhi{nullptr};
    StudioWorkspace ws{};
    ImGuiArenaAllocator arena{};
    bool ws_init{false};
    bool imgui_init{false};
    bool implot_init{false};

    ~ScopedUiRig() {
        if (ws_init) {
            ws.shutdown();
        }
        if (implot_init) {
            ImPlot::DestroyContext();
        }
        if (imgui_init) {
            ImGui::DestroyContext();
        }
        arena.shutdown();
        restore_default_imgui_allocator();
        if (rhi != nullptr) {
            rhi->destroy();
            delete rhi;
        }
    }
};

} // namespace

TEST_CASE("ParamStore enforces parameter registry bounds, epochs, and project reset", "[ui][params]") {
    const auto specs = all_param_specs();
    REQUIRE(specs.size() == kParamCount);
    REQUIRE(kParamCount == 111U); // 97 + Phase 1 Studio Fixture & Optical Modifiers/Satsuei/Holo-Grading (14)

    for (const auto& sp : specs) {
        REQUIRE(sp.id != nullptr);
        REQUIRE(sp.label != nullptr);
        REQUIRE(find_param_by_id(sp.id) == &sp);
    }

    ParamStore store;
    const uint64_t e0 = store.epoch();
    REQUIRE(store.set(ParamId::AppUiScalePct, 125.0));
    REQUIRE(store.get_int(ParamId::AppUiScalePct) == 125);
    REQUIRE(store.epoch() > e0);

    // Photometric fixture & [TESTING] crease disguise parameter bounds
    // Photometric fixture, catchlight & [TESTING] crease disguise defaults + bounds
    REQUIRE_FALSE(store.get_bool(ParamId::RenderCreaseDisguiseEnabled));
    (void)store.set(ParamId::RenderEyeCatchlightIntensity, 9.0);
    REQUIRE(store.get_float(ParamId::RenderEyeCatchlightIntensity) <= 2.0f);
    (void)store.set(ParamId::RenderLightingFixture, 99.0);
    REQUIRE(store.get_int(ParamId::RenderLightingFixture) <= 5);

    // Out-of-bounds + NaN clamping
    (void)store.set(ParamId::RenderFlatness, 99.0);
    REQUIRE(store.get_float(ParamId::RenderFlatness) <= 1.0f);
    (void)store.set(ParamId::RenderFlatness, std::numeric_limits<double>::quiet_NaN());
    REQUIRE(std::isfinite(store.get_float(ParamId::RenderFlatness)));

    REQUIRE(store.set_by_id("render.flatness", 0.35));
    REQUIRE(std::fabs(store.get_float(ParamId::RenderFlatness) - 0.35f) < 1e-4f);

    // Project reset restores project params while preserving App-scoped scale
    store.reset_project_to_defaults();
    REQUIRE(std::fabs(store.get_float(ParamId::RenderFlatness) -
                      static_cast<float>(param_spec(ParamId::RenderFlatness).default_val)) < 1e-4f);
    REQUIRE(store.get_int(ParamId::AppUiScalePct) == 125);
}

TEST_CASE("UndoStack enforces 200-entry eviction, drag coalescing, and graph round-trip", "[ui][undo]") {
    ParamStore store;
    UndoStack undo;

    std::string applied_graph{};
    UndoContext ctx{
        &store,
        [&](const std::string& g) { applied_graph = g; },
    };

    for (int i = 0; i < 220; ++i) {
        const double prev = static_cast<double>(i) * 0.5;
        const double next = static_cast<double>(i + 1) * 0.5;
        undo.push_param(ParamId::RenderKeyAzimuthDeg, prev, next);
    }
    REQUIRE(undo.undo_count() == UndoStack::kMaxEntries);
    REQUIRE(UndoStack::kMaxEntries == 200U);
    REQUIRE(undo.is_dirty());

    REQUIRE(undo.undo(ctx));
    REQUIRE(std::fabs(store.get(ParamId::RenderKeyAzimuthDeg) - 109.5) < 1e-6);
    REQUIRE(undo.redo(ctx));
    REQUIRE(std::fabs(store.get(ParamId::RenderKeyAzimuthDeg) - 110.0) < 1e-6);

    // Drag coalescing produces exactly 1 undo entry
    const size_t before_drag_cnt = undo.undo_count();
    undo.begin_drag(ParamId::RenderKeyIntensity, 1.0);
    (void)store.set(ParamId::RenderKeyIntensity, 1.4);
    (void)store.set(ParamId::RenderKeyIntensity, 2.2);
    undo.commit_drag(ParamId::RenderKeyIntensity, 2.2);
    REQUIRE(undo.undo_count() == before_drag_cnt); // capped at 200 if already full, so test fresh stack too

    UndoStack fresh;
    fresh.begin_drag(ParamId::RenderKeyIntensity, 1.0);
    fresh.commit_drag(ParamId::RenderKeyIntensity, 2.5);
    REQUIRE(fresh.undo_count() == 1U);
    fresh.push_graph_edit("Connect Pin", "{\"nodes\":[]}", "{\"nodes\":[{\"id\":1}]}");
    REQUIRE(fresh.undo_count() == 2U);
    REQUIRE(fresh.undo(ctx));
    REQUIRE(applied_graph == "{\"nodes\":[]}");
    REQUIRE(fresh.redo(ctx));
    REQUIRE(applied_graph == "{\"nodes\":[{\"id\":1}]}");
    fresh.mark_saved();
    REQUIRE_FALSE(fresh.is_dirty());
}

TEST_CASE("KeymapRegistry detects conflicts cleanly and persists TOML atomically", "[ui][settings]") {
    KeymapRegistry km;
    REQUIRE(km.detect_conflicts().empty());

    // Intentional collision between Ctrl+K (OpenPalette) and ModeNodes
    km.set_chord(ActionId::ModeNodes, "ctrl + k");
    REQUIRE(km.chord(ActionId::ModeNodes) == "Ctrl+K");
    const auto conflicts = km.detect_conflicts();
    REQUIRE_FALSE(conflicts.empty());

    ScopedTempDir tmp("toml");
    const auto km_path = tmp.dir / "keymap.toml";
    const auto st_path = tmp.dir / "settings.toml";

    km.set_global_hotkey_enabled(ActionId::Expression1, true);
    REQUIRE(km.save_toml(km_path));

    KeymapRegistry km2;
    km2.reset_defaults();
    REQUIRE(km2.load_toml(km_path));
    REQUIRE(km2.chord(ActionId::ModeNodes) == "Ctrl+K");
    REQUIRE(km2.global_hotkey_enabled(ActionId::Expression1));

    ParamStore ps;
    (void)ps.set(ParamId::AppUiScalePct, 150.0);
    AppSettings s1{};
    s1.wizard_completed = true;
    s1.ws_auth_token = "local_test_tok_987";
    s1.record_recent_project("C:/Projects/demo.aether");
    REQUIRE(save_app_settings_toml(st_path, s1, ps));

    ParamStore ps2;
    AppSettings s2{};
    REQUIRE(load_app_settings_toml(st_path, s2, ps2));
    REQUIRE(s2.wizard_completed);
    REQUIRE(s2.ws_auth_token == "local_test_tok_987");
    REQUIRE(ps2.get_int(ParamId::AppUiScalePct) == 150);
    REQUIRE(s2.recent_projects.size() == 1U);
}

TEST_CASE("HardwareOptimizerWizard tier selection & budget downshift follow §12.13", "[ui][wizard]") {
    HardwareInventory npu_hw{};
    npu_hw.npu.available = true;
    npu_hw.npu.device_name = "Intel AI Boost NPU";
    rhi::SelectedGpuInfo dgpu{};
    dgpu.is_discrete = true;
    dgpu.rt_enabled = true;
    dgpu.vendor_id = 0x10DE;
    dgpu.dedicated_vram_mb = 8192;

    auto rec_npu = HardwareOptimizerWizard::recommend_for_hardware(npu_hw, dgpu);
    REQUIRE(rec_npu.ep_override == -1);
    REQUIRE(rec_npu.tracking_ep_mode == 0);

    HardwareInventory dgpu_hw{};
    dgpu_hw.npu.available = false;
    auto rec_dgpu = HardwareOptimizerWizard::recommend_for_hardware(dgpu_hw, dgpu);
    REQUIRE(rec_dgpu.ep_override == -1);
    REQUIRE(rec_dgpu.tracking_ep_mode == 0);

    HardwareInventory cpu_hw{};
    rhi::SelectedGpuInfo soft_gpu{};
    auto rec_cpu = HardwareOptimizerWizard::recommend_for_hardware(cpu_hw, soft_gpu);
    REQUIRE(rec_cpu.ep_override == AETHER_EP_CPU);
    REQUIRE(rec_cpu.tracking_ep_mode == 4);

    // Budget downshift when > 20.0 ms M2P
    auto high_rec = rec_npu;
    REQUIRE_FALSE(HardwareOptimizerWizard::adjust_for_budget(high_rec, 16.5f));
    REQUIRE(HardwareOptimizerWizard::adjust_for_budget(high_rec, 25.0f));
    REQUIRE(high_rec.downshifted_by_budget);
}

TEST_CASE("CommandPalette fuzzy matcher prioritizes exact subsequence and word starts", "[ui][palette]") {
    std::array<uint8_t, 48> pos{};
    uint8_t cnt = 0;
    const int32_t s_exact = CommandPalette::fuzzy_score("save", "Save Project", pos, cnt);
    const int32_t s_sub = CommandPalette::fuzzy_score("tgonair", "Toggle ON AIR", pos, cnt);
    const int32_t s_miss = CommandPalette::fuzzy_score("zzz", "Toggle ON AIR", pos, cnt);
    REQUIRE(s_exact > 0);
    REQUIRE(s_sub > 0);
    REQUIRE(s_miss < 0);
}

TEST_CASE("Inspector, Outliner and Viewport text fits at 100/125/150/175% DPI without truncation",
          "[ui][layout][dpi]") {
    const RuntimePaths paths = discover_runtime_paths();
    for (const float dpi : {1.0f, 1.25f, 1.5f, 1.75f}) {
        struct ScopedCtxGuard {
            bool in_frame{false};
            ~ScopedCtxGuard() {
                if (ImGui::GetCurrentContext() != nullptr) {
                    if (in_frame) {
                        ImGui::EndFrame();
                    }
                    ImGui::DestroyContext();
                }
            }
        } dpi_ctx_guard{};
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920.0f, 1080.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        const UiFontSet fonts = load_precision_dark_fonts(paths, dpi);
        apply_precision_dark_style(ImGui::GetStyle(), dpi, false);
        unsigned char* px = nullptr;
        int tw = 0;
        int th = 0;
        io.Fonts->GetTexDataAsAlpha8(&px, &tw, &th);
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));
        ImGui::NewFrame();
        dpi_ctx_guard.in_frame = true;

        const PrecisionDarkTokens& t = tokens();
        const ImGuiStyle& st = ImGui::GetStyle();

        // Inspector (right dock, default width): every label fits the label column next to its reset dot,
        // and every combo value fits the control column next to the combo arrow.
        const float insp_w = t.right_dock_w_px * dpi - st.WindowPadding.x * 2.0f - st.ScrollbarSize;
        const float label_col = property_label_column_width(insp_w);
        const float control_col = insp_w - label_col - st.CellPadding.x * 4.0f;
        float widest_label = 0.0f;
        float widest_enum = 0.0f;
        for (const ParamSpec& spec : kParamSpecs) {
            widest_label = std::max(widest_label, ImGui::CalcTextSize(spec.label).x);
            if (spec.enum_labels != nullptr) {
                const char* p = spec.enum_labels;
                while (*p != '\0') {
                    const char* bar = std::strchr(p, '|');
                    const char* end = bar != nullptr ? bar : p + std::strlen(p);
                    widest_enum = std::max(widest_enum, ImGui::CalcTextSize(p, end).x);
                    if (bar == nullptr) {
                        break;
                    }
                    p = bar + 1;
                }
            }
        }
        const float fscale = ImGui::GetFontSize() / t.type_body_px; // actual UI scale (whole-px fonts)
        const float reset_dot_w = 12.0f * fscale;
        const float combo_text_room = control_col - ImGui::GetFrameHeight() - st.FramePadding.x * 2.0f;
        std::printf("[UI_DPI] dpi=%.2f inspector=%.1f label_col=%.1f widest_label=%.1f control=%.1f "
                    "combo_room=%.1f widest_enum=%.1f\n",
                    static_cast<double>(dpi), static_cast<double>(insp_w), static_cast<double>(label_col),
                    static_cast<double>(widest_label), static_cast<double>(control_col),
                    static_cast<double>(combo_text_room), static_cast<double>(widest_enum));
        REQUIRE(widest_label + reset_dot_w <= label_col + 0.5f);
        REQUIRE(widest_enum <= combo_text_room + 0.5f);

        // Outliner (left dock, default width): search placeholder + 2-column expression key-badge buttons.
        const float outl_w = t.left_dock_w_px * dpi - st.WindowPadding.x * 2.0f - st.ScrollbarSize;
        REQUIRE(ImGui::CalcTextSize("Search scene, bones, outputs").x + st.FramePadding.x * 2.0f <= outl_w);
        const float cell_w = (outl_w - st.CellPadding.x * 4.0f) * 0.5f;
        ImGui::PushFont(fonts.mono);
        const float badge_w = ImGui::CalcTextSize("F8").x + 8.0f * fscale;
        ImGui::PopFont();
        for (const char* name :
             {"Smile", "Wink L", "Surprised", "Angry", "Sad", "Sparkle Eyes", "Blush", "Reset Neutral"}) {
            const float need = 5.0f * fscale + badge_w + 6.0f * fscale + ImGui::CalcTextSize(name).x + 5.0f * fscale;
            REQUIRE(need <= cell_w + 0.5f);
        }
        // Expression buttons are frame-height tall: text + 2 * FramePadding.y keeps descenders inside.
        REQUIRE(ImGui::GetFrameHeight() >= ImGui::GetTextLineHeight() + 2.0f * st.FramePadding.y - 0.5f);

        ImGui::EndFrame();
        dpi_ctx_guard.in_frame = false;
        ImGui::DestroyContext();
    }
}

TEST_CASE("NodeCanvas default graph.json schema uses 'type' discriminator and M2P rejects epoch-0 deltas",
          "[ui][nodegraph][telemetry]") {
    NodeCanvas canvas;
    canvas.reset_default_graph();
    REQUIRE(canvas.node_count() == 6U);

    const std::string json_str = canvas.serialize_graph_json();
    REQUIRE(json_str.find("\"type\":\"aether.source.npu_pose@1\"") != std::string::npos);
    REQUIRE(json_str.find("\"type_id\"") == std::string::npos);

    NodeCanvas roundtrip;
    roundtrip.set_graph_json(json_str);
    REQUIRE(roundtrip.node_count() == 6U);
    REQUIRE(roundtrip.serialize_graph_json() == json_str);

    // M2P epoch/uptime validation: reject capture_ts == 0, tracking_valid == false, or > 5000 ms delta.
    RollingLatencyStats m2p{};
    REQUIRE_FALSE(m2p.add_m2p_sample(0ULL, 114'398'368'000ULL, true));
    REQUIRE_FALSE(m2p.add_m2p_sample(16'666ULL, 114'398'368'000ULL, true));
    REQUIRE_FALSE(m2p.add_m2p_sample(100'000ULL, 114'500ULL, false));
    REQUIRE_FALSE(m2p.add_m2p_sample(100'000ULL, 90'000ULL, true));
    REQUIRE(m2p.compute_summary().sample_count == 0U);

    REQUIRE(m2p.add_m2p_sample(100'000ULL, 114'500ULL, true));
    const LatencySummary sum = m2p.compute_summary();
    REQUIRE(sum.sample_count == 1U);
    REQUIRE(std::fabs(sum.p50_ms - 14.5f) < 1e-3f);
}

TEST_CASE("StudioWorkspace meets <= 0.40 ms CPU budget, zero steady-state allocs, and <= 5% accent pixels",
          "[ui][perf][budget]") {
    HotPathGuard::install();

    rhi::RhiDeviceConfig rhi_cfg{};
    rhi_cfg.enable_validation = false;
    rhi_cfg.require_validation = false;
    rhi_cfg.headless = true;
    auto rhi_res = rhi::RhiDevice::create(rhi_cfg);
    REQUIRE(rhi_res.has_value());
    rhi::RhiDevice* rhi = rhi_res.value();

    ImGuiArenaAllocator arena;
    install_imgui_arena(arena);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    const RuntimePaths paths = discover_runtime_paths();
    HardwareInventory hw{};
    StudioWorkspace ws;
    ws.init(paths, hw, 1.0f, nullptr, false);

    // Upload headless font atlas texture handle cleanly
    unsigned char* tex_pixels = nullptr;
    int tex_w = 0;
    int tex_h = 0;
    io.Fonts->GetTexDataAsAlpha8(&tex_pixels, &tex_w, &tex_h);
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));

    ws.node_canvas().load_sample_64_nodes();
    REQUIRE(ws.node_canvas().node_count() >= 64U);

    TelemetrySnapshot telem{};
    telem.render_fps = 60.0f;
    telem.last_frame.t1_capture_us = 1000ULL;
    telem.last_frame.t4_output_us = 16800ULL;
    DevShellInputEcho echo{};
    DevShellAvatarView avatar{};
    DevShellTrackingView tracking{};
    DevShellRenderView render_view{};
    DevShellOutputView output_view{};
    StudioActions actions{};

    // Warm up all 3 studio modes + DockBuilder layouts before measuring steady state
    for (int f = 0; f < 60; ++f) {
        if (f == 0) {
            ws.switch_mode(StudioMode::LiveStudio);
        } else if (f == 20) {
            ws.switch_mode(StudioMode::Rigging);
        } else if (f == 40) {
            ws.switch_mode(StudioMode::Nodes);
        }
        ImGui::NewFrame();
        ws.render(static_cast<double>(f) * (1.0 / 60.0), *rhi, telem, echo, arena, true, avatar, tracking, render_view,
                  output_view, actions);
        ImGui::Render();
    }
    // Keep Nodes mode (64-node stress graph visible!) across final warm frames so DockBuilder finishes before steady
    // state
    for (int f = 60; f < 80; ++f) {
        ImGui::NewFrame();
        ws.render(static_cast<double>(f) * (1.0 / 60.0), *rhi, telem, echo, arena, true, avatar, tracking, render_view,
                  output_view, actions);
        ImGui::Render();
    }
    arena.mark_warmup_complete();

    std::vector<double> frame_ms;
    frame_ms.reserve(120);
    const uint64_t violations_before = HotPathGuard::total_violations();

    for (int f = 80; f < 200; ++f) {
        if (f == 100) {
            (void)ws.params().set(ParamId::RenderKeyAzimuthDeg, 48.0);
            (void)ws.params().set(ParamId::AppSkeletonHeatmap, 1.0);
        }

        const auto t0 = std::chrono::steady_clock::now();
        {
            AETHER_HOT_PATH_SCOPE;
            ImGui::NewFrame();
            ws.render(static_cast<double>(f) * (1.0 / 60.0), *rhi, telem, echo, arena, true, avatar, tracking,
                      render_view, output_view, actions);
            ImGui::Render();
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        frame_ms.push_back(ms);
    }

    const uint64_t violations_after = HotPathGuard::total_violations();
    REQUIRE(violations_after == violations_before);
    REQUIRE(arena.os_fallback_allocs() == 0ULL);
    REQUIRE(arena.steady_bump_growth_bytes() == 0ULL);

    // Verify immediate parameter-to-action coupling (§12.16)
    REQUIRE(actions.viewport.lighting_dirty);
    REQUIRE(std::fabs(actions.viewport.key_azimuth_deg - 48.0f) < 1e-3f);
    REQUIRE(actions.viewport.show_skeleton_heatmap);

    std::sort(frame_ms.begin(), frame_ms.end());
    const double p95_ms = frame_ms[static_cast<size_t>(std::floor(0.95 * static_cast<double>(frame_ms.size() - 1U)))];
    std::printf(
        "[STUDIO_UI_PERF] nodes=%zu p95_ms=%.4f mean_ms=%.4f steady_growth=%zu os_fallbacks=%llu hp_viol=%llu\n",
        ws.node_canvas().node_count(), p95_ms,
        [&]() {
            double s = 0.0;
            for (double v : frame_ms) {
                s += v;
            }
            return s / static_cast<double>(frame_ms.size());
        }(),
        arena.steady_bump_growth_bytes(), static_cast<unsigned long long>(arena.os_fallback_allocs()),
        static_cast<unsigned long long>(violations_after - violations_before));
    std::fflush(stdout);

#if !defined(_DEBUG)
    // REQ-UI-020: UI CPU budget <= 0.40 ms p95 under RelWithDebInfo / Release optimization.
    REQUIRE(p95_ms <= 0.40);
#else
    REQUIRE(p95_ms <= 2.50);
#endif

    // Quantitative check: emitted accent-colored triangle coverage <= 5.0% of viewport area (§12.2)
    const ImDrawData* dd = ImGui::GetDrawData();
    REQUIRE(dd != nullptr);
    const ImU32 accent_rgb = tokens().col32_accent & 0x00FFFFFFU;
    double accent_area_px2 = 0.0;
    for (int l = 0; l < dd->CmdListsCount; ++l) {
        const ImDrawList* dl = dd->CmdLists[l];
        for (int i = 0; i + 2 < dl->IdxBuffer.Size; i += 3) {
            const ImDrawVert& v0 = dl->VtxBuffer[dl->IdxBuffer[i]];
            const ImDrawVert& v1 = dl->VtxBuffer[dl->IdxBuffer[i + 1]];
            const ImDrawVert& v2 = dl->VtxBuffer[dl->IdxBuffer[i + 2]];
            if ((v0.col & 0x00FFFFFFU) == accent_rgb && (v1.col & 0x00FFFFFFU) == accent_rgb &&
                (v2.col & 0x00FFFFFFU) == accent_rgb) {
                const double tri_area =
                    std::fabs(static_cast<double>(v1.pos.x - v0.pos.x) * static_cast<double>(v2.pos.y - v0.pos.y) -
                              static_cast<double>(v2.pos.x - v0.pos.x) * static_cast<double>(v1.pos.y - v0.pos.y)) *
                    0.5;
                accent_area_px2 += tri_area;
            }
        }
    }
    const double accent_pct = 100.0 * accent_area_px2 / (1600.0 * 900.0);
    std::printf("[STUDIO_UI_ACCENT] accent_area_pct=%.4f%%\n", accent_pct);
    REQUIRE(accent_pct <= 5.0);

    ws.shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    arena.shutdown();
    restore_default_imgui_allocator();
    rhi->destroy();
    delete rhi;
}

TEST_CASE("Viewport Q/W/E/R gizmo tools, shortcuts, 3D ray drag, and atomic Undo/Redo", "[ui][viewport][gizmo]") {
    rhi::RhiDeviceConfig rhi_cfg{};
    rhi_cfg.enable_validation = false;
    rhi_cfg.require_validation = false;
    rhi_cfg.headless = true;
    auto rhi_res = rhi::RhiDevice::create(rhi_cfg);
    REQUIRE(rhi_res.has_value());

    ScopedUiRig rig{};
    rig.rhi = rhi_res.value();
    REQUIRE(rig.arena.init(16ULL * 1024ULL * 1024ULL));
    REQUIRE(install_imgui_arena(rig.arena));
    ImGui::CreateContext();
    rig.imgui_init = true;
    ImPlot::CreateContext();
    rig.implot_init = true;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigInputTrickleEventQueue = false;

    const RuntimePaths paths = discover_runtime_paths();
    HardwareInventory hw{};
    rig.ws.init(paths, hw, 1.0f, nullptr, false);
    rig.ws_init = true;
    StudioWorkspace& ws = rig.ws;

    unsigned char* tex_pixels = nullptr;
    int tex_w = 0;
    int tex_h = 0;
    io.Fonts->GetTexDataAsAlpha8(&tex_pixels, &tex_w, &tex_h);
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));

    TelemetrySnapshot telem{};
    telem.render_fps = 60.0f;
    DevShellInputEcho echo{};
    DevShellAvatarView avatar{};
    avatar.has_avatar = true;
    avatar.wireframe_supported = true;
    avatar.has_camera = true;
    avatar.camera_eye = {{0.716f, 1.110f, 1.961f}};
    avatar.camera_target = {{0.0f, 0.90f, 0.0f}};
    avatar.camera_fovy_rad = 0.62f;
    avatar.gizmo_pivot_y = 0.90f;
    DevShellTrackingView tracking{};
    DevShellRenderView render_view{};
    DevShellOutputView output_view{};
    StudioActions actions{};

    auto step = [&](double t_s) {
        actions.reset_per_frame();
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ws.render(t_s, *rig.rhi, telem, echo, rig.arena, true, avatar, tracking, render_view, output_view, actions);
        ImGui::Render();
    };

    ws.switch_mode(StudioMode::LiveStudio);
    for (int f = 0; f < 16; ++f) {
        step(static_cast<double>(f) / 60.0);
    }

    ImGuiWindow* vp_win = ImGui::FindWindowByName("Viewport");
    REQUIRE(vp_win != nullptr);
    const ImVec2 vp_center = vp_win->InnerRect.GetCenter();

    // Focus the Viewport window via an empty-space click (far left inside canvas away from gizmo)
    const ImVec2 focus_pt(vp_win->InnerRect.Min.x + 28.0f, vp_center.y);
    io.AddMousePosEvent(focus_pt.x, focus_pt.y);
    step(0.30);
    io.AddMouseButtonEvent(0, true);
    step(0.32);
    io.AddMouseButtonEvent(0, false);
    step(0.34);
    REQUIRE(ws.viewport_panel().is_focused());

    // Q / W / E / R / X keyboard shortcuts while Viewport is focused
    auto tap_key = [&](ImGuiKey k, double& t_s) {
        io.AddKeyEvent(k, true);
        step(t_s);
        t_s += 0.02;
        io.AddKeyEvent(k, false);
        step(t_s);
        t_s += 0.02;
    };
    double t_s = 0.40;
    tap_key(ImGuiKey_W, t_s);
    REQUIRE(ws.viewport_panel().gizmo_tool() == GizmoTool::Move);
    tap_key(ImGuiKey_E, t_s);
    REQUIRE(ws.viewport_panel().gizmo_tool() == GizmoTool::Rotate);
    tap_key(ImGuiKey_R, t_s);
    REQUIRE(ws.viewport_panel().gizmo_tool() == GizmoTool::Scale);
    const bool prev_local = ws.viewport_panel().gizmo_local();
    tap_key(ImGuiKey_X, t_s);
    REQUIRE(ws.viewport_panel().gizmo_local() != prev_local);
    tap_key(ImGuiKey_Q, t_s);
    REQUIRE(ws.viewport_panel().gizmo_tool() == GizmoTool::Select);

    // Move back into Move (W) for ray-intersection drag along the +Y stem
    tap_key(ImGuiKey_W, t_s);
    REQUIRE(ws.viewport_panel().gizmo_tool() == GizmoTool::Move);

    // Project (0, 0.90, 0) lands at vp_center; +Y axis projects straight up (~68 px).
    // Hover on (vp_center.x, vp_center.y - 36.0f), press LMB, drag upward 48 px, release.
    const ImVec2 y_handle(vp_center.x, vp_center.y - 36.0f);
    io.AddMousePosEvent(y_handle.x, y_handle.y);
    step(t_s);
    t_s += 0.02;
    step(t_s);
    t_s += 0.02;

    io.AddMouseButtonEvent(0, true);
    step(t_s);
    t_s += 0.02;

    rig.arena.mark_warmup_complete();
    const uint64_t hp_before = HotPathGuard::total_violations();
    for (int s = 1; s <= 4; ++s) {
        io.AddMousePosEvent(y_handle.x, y_handle.y - static_cast<float>(s) * 12.0f);
        {
            AETHER_HOT_PATH_SCOPE;
            step(t_s);
        }
        t_s += 0.02;
        REQUIRE(std::fabs(actions.viewport.orbit_yaw_delta) < 1e-6f);
        REQUIRE(std::fabs(actions.viewport.orbit_pitch_delta) < 1e-6f);
    }
    REQUIRE(HotPathGuard::total_violations() == hp_before);
    REQUIRE(rig.arena.os_fallback_allocs() == 0ULL);
    REQUIRE(rig.arena.steady_bump_growth_bytes() == 0ULL);

    io.AddMouseButtonEvent(0, false);
    step(t_s);
    t_s += 0.02;

    const float moved_y = ws.params().get_float(ParamId::AvatarPosY);
    REQUIRE(moved_y > 0.05f);
    REQUIRE(std::fabs(actions.viewport.avatar_pos[1] - moved_y) < 1e-5f);
    REQUIRE(ws.undo_stack().undo_count() == 1U);
    REQUIRE(std::string(ws.undo_stack().undo_label()) == "Transform Avatar");

    // Ctrl+Z undoes the atomic transform, Ctrl+Y redoes it cleanly
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_Z, true);
    step(t_s);
    t_s += 0.02;
    io.AddKeyEvent(ImGuiKey_Z, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    step(t_s);
    t_s += 0.02;
    REQUIRE(std::fabs(ws.params().get_float(ParamId::AvatarPosY)) < 1e-4f);
    REQUIRE(ws.undo_stack().can_redo());

    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_Y, true);
    step(t_s);
    t_s += 0.02;
    io.AddKeyEvent(ImGuiKey_Y, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    step(t_s);
    t_s += 0.02;
    REQUIRE(std::fabs(ws.params().get_float(ParamId::AvatarPosY) - moved_y) < 1e-4f);

    // Rigging mode forces show_rig_colliders on Viewport actions
    ws.switch_mode(StudioMode::Rigging);
    step(t_s);
    t_s += 0.02;
    REQUIRE(actions.viewport.show_rig_colliders);
}

TEST_CASE("StudioWorkspace meets <= 0.45 ms CPU budget at 1080p and 1440p with zero steady-state heap allocs",
          "[ui][perf][release]") {
    rhi::RhiDeviceConfig rhi_cfg{};
    rhi_cfg.enable_validation = false;
    rhi_cfg.require_validation = false;
    rhi_cfg.headless = true;
    auto rhi_res = rhi::RhiDevice::create(rhi_cfg);
    REQUIRE(rhi_res.has_value());
    rhi::RhiDevice* rhi = rhi_res.value();

    struct ResolutionCase {
        int w;
        int h;
        const char* label;
    };
    const std::array<ResolutionCase, 2> cases{{{1920, 1080, "1080P"}, {2560, 1440, "1440P"}}};

    for (const auto& rc : cases) {
        ScopedUiRig rig{};
        REQUIRE(rig.arena.init(16ULL * 1024ULL * 1024ULL));
        REQUIRE(install_imgui_arena(rig.arena));
        ImGui::CreateContext();
        rig.imgui_init = true;
        ImPlot::CreateContext();
        rig.implot_init = true;
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(rc.w), static_cast<float>(rc.h));
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigInputTrickleEventQueue = false;

        const RuntimePaths paths = discover_runtime_paths();
        HardwareInventory hw{};
        rig.ws.init(paths, hw, 1.0f, nullptr, false);
        rig.ws_init = true;
        StudioWorkspace& ws = rig.ws;

        unsigned char* tex_pixels = nullptr;
        int tex_w = 0;
        int tex_h = 0;
        io.Fonts->GetTexDataAsAlpha8(&tex_pixels, &tex_w, &tex_h);
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));

        // Realistic active telemetry & watched pins + Move gizmo active
        StudioHostStatus host{};
        host.host_alive = true;
        host.compiled_node_count = static_cast<uint32_t>(ws.node_canvas().nodes().size());
        host.last_compile_ms = 0.75f;
        host.last_eval_us = 16.2f;
        host.telemetry.struct_size = static_cast<uint32_t>(sizeof(AetherGraphTelemetryReadout));
        host.telemetry.valid = 1u;
        const auto& nodes = ws.node_canvas().nodes();
        uint32_t ni = 0u;
        uint32_t pi = 0u;
        for (const auto& n : nodes) {
            if (ni < AETHER_GRAPH_MAX_DEBUG_NODES) {
                host.telemetry.node_ids[ni] = n.id;
                host.telemetry.node_eval_ns[ni] = 1500u + ni * 420u;
                ++ni;
            }
            if (pi < AETHER_GRAPH_MAX_DEBUG_PINS && n.num_outputs > 0u) {
                host.telemetry.pin_node_ids[pi] = n.id;
                host.telemetry.pin_indices[pi] = 0u;
                host.telemetry.pin_kinds[pi] = AETHER_PIN_VALUE_FLOAT;
                host.telemetry.pin_values[pi][0] = 0.42f + static_cast<float>(pi) * 0.05f;
                ++pi;
            }
        }
        host.telemetry.active_node_count = ni;
        host.telemetry.pin_readout_count = pi;
        ws.set_host_status(host);
        if (!nodes.empty()) {
            (void)ws.node_canvas().set_pin_watched(PinRef{nodes[0].id, 0u, false}, true);
        }

        TelemetrySnapshot telem{};
        telem.render_fps = 60.0f;
        DevShellInputEcho echo{};
        DevShellAvatarView avatar{};
        avatar.has_avatar = true;
        avatar.wireframe_supported = true;
        avatar.has_camera = true;
        avatar.camera_eye = {{0.716f, 1.110f, 1.961f}};
        avatar.camera_target = {{0.0f, 0.90f, 0.0f}};
        avatar.camera_fovy_rad = 0.62f;
        avatar.gizmo_pivot_y = 0.90f;
        DevShellTrackingView tracking{};
        DevShellRenderView render_view{};
        DevShellOutputView output_view{};
        StudioActions actions{};

        ws.viewport_panel().set_gizmo_tool(GizmoTool::Move);
        for (int f = 0; f < 30; ++f) {
            actions.reset_per_frame();
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ws.render(static_cast<double>(f) / 60.0, *rhi, telem, echo, rig.arena, true, avatar, tracking, render_view,
                      output_view, actions);
            ImGui::Render();
        }
        rig.arena.mark_warmup_complete();

        std::vector<double> frame_ms;
        frame_ms.reserve(120);
        const uint64_t violations_before = HotPathGuard::total_violations();

        for (int f = 30; f < 150; ++f) {
            const auto t0 = std::chrono::steady_clock::now();
            {
                AETHER_HOT_PATH_SCOPE;
                actions.reset_per_frame();
                io.DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                ws.render(static_cast<double>(f) / 60.0, *rhi, telem, echo, rig.arena, true, avatar, tracking,
                          render_view, output_view, actions);
                ImGui::Render();
            }
            const auto t1 = std::chrono::steady_clock::now();
            frame_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }

        const uint64_t violations_after = HotPathGuard::total_violations();
        REQUIRE(violations_after == violations_before);
        REQUIRE(rig.arena.os_fallback_allocs() == 0ULL);
        REQUIRE(rig.arena.steady_bump_growth_bytes() == 0ULL);

        std::sort(frame_ms.begin(), frame_ms.end());
        const double p95_ms =
            frame_ms[static_cast<size_t>(std::floor(0.95 * static_cast<double>(frame_ms.size() - 1U)))];
        double sum_ms = 0.0;
        for (double v : frame_ms) {
            sum_ms += v;
        }
        const double mean_ms = sum_ms / static_cast<double>(frame_ms.size());
        std::printf(
            "[STUDIO_UI_PERF_%s] res=%dx%d p95_ms=%.4f mean_ms=%.4f steady_growth=%zu os_fallbacks=%llu hp_viol=%llu\n",
            rc.label, rc.w, rc.h, p95_ms, mean_ms, rig.arena.steady_bump_growth_bytes(),
            static_cast<unsigned long long>(rig.arena.os_fallback_allocs()),
            static_cast<unsigned long long>(violations_after - violations_before));
        std::fflush(stdout);

#if !defined(_DEBUG)
        REQUIRE(p95_ms <= 0.45);
#else
        REQUIRE(p95_ms <= 2.50);
#endif
    }

    rhi->destroy();
    delete rhi;
}

struct ScopedUiRig {
    rhi::RhiDevice* rhi{nullptr};
    StudioWorkspace ws{};
    ImGuiArenaAllocator arena{};
    bool ws_init{false};
    bool imgui_init{false};
    bool implot_init{false};

    ~ScopedUiRig() {
        if (ws_init) {
            ws.shutdown();
        }
        if (implot_init) {
            ImPlot::DestroyContext();
        }
        if (imgui_init) {
            ImGui::DestroyContext();
        }
        arena.shutdown();
        restore_default_imgui_allocator();
        if (rhi != nullptr) {
            rhi->destroy();
            delete rhi;
        }
    }
};

} // namespace aether::ui

TEST_CASE("Quick expressions F1-F8 never drive ARKit eye-look slots and F8 is a reset", "[ui][expressions]") {
    using aether::ui::StudioActions;
    // Regression: the quick slot index (0..7) used to be sent as the expression slot, so F1 "Smile" closed the left
    // eye (ARKit 0) and F3..F7 drove eye-look channels. Only F2 (Wink L) may target an ARKit slot, and only slot 0.
    for (size_t i = 0; i < StudioActions::kQuickExpressionSlots; ++i) {
        const int32_t s = StudioActions::kQuickExpressionGraphSlot[i];
        if (i == 1U) {
            CHECK(s == 0);
        } else if (i == StudioActions::kResetNeutralQuickSlot) {
            CHECK(s < 0);
        } else {
            CHECK(s >= 52);
            CHECK(s <= 58);
        }
    }
}

TEST_CASE("Task 3-4: Expressions tab Live2D parameter monitor and 68-pt HUD overlay execute with zero steady-state "
          "allocations",
          "[ui][live2d][perf]") {
    aether::rhi::RhiDeviceConfig rhi_cfg{};
    rhi_cfg.enable_validation = false;
    rhi_cfg.require_validation = false;
    rhi_cfg.headless = true;
    auto rhi_res = aether::rhi::RhiDevice::create(rhi_cfg);
    REQUIRE(rhi_res.has_value());

    aether::ui::ScopedUiRig rig{};
    rig.rhi = rhi_res.value();
    REQUIRE(rig.arena.init(16ULL * 1024ULL * 1024ULL));
    REQUIRE(aether::ui::install_imgui_arena(rig.arena));
    ImGui::CreateContext();
    rig.imgui_init = true;
    ImPlot::CreateContext();
    rig.implot_init = true;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigInputTrickleEventQueue = false;

    const aether::RuntimePaths paths = aether::discover_runtime_paths();
    aether::HardwareInventory hw{};
    rig.ws.init(paths, hw, 1.0f, nullptr, false);
    rig.ws_init = true;

    unsigned char* tex_pixels = nullptr;
    int tex_w = 0;
    int tex_h = 0;
    io.Fonts->GetTexDataAsAlpha8(&tex_pixels, &tex_w, &tex_h);
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));

    aether::TelemetrySnapshot telem{};
    telem.render_fps = 60.0f;
    aether::ui::DevShellInputEcho echo{};
    aether::ui::DevShellAvatarView avatar{};
    avatar.has_avatar = true;
    aether::ui::DevShellTrackingView tracking{};
    tracking.ready = true;
    tracking.face_confidence = 0.95f;
    tracking.live_preview_valid = true;
    tracking.live_preview_texture_id = 1ULL;
    tracking.live_preview_width = 320U;
    tracking.live_preview_height = 180U;
    tracking.live_preview_face_locked = true;
    tracking.live_preview_keypoint_count = 68U;
    for (uint32_t k = 0; k < 68U; ++k) {
        tracking.live_preview_keypoints[k] = {0.35f + 0.004f * static_cast<float>(k % 17U),
                                              0.25f + 0.08f * static_cast<float>(k / 17U)};
        tracking.live_preview_keypoint_valid[k] = 1U;
    }
    tracking.live_preview_iris_valid = true;
    tracking.live_preview_iris_centers[0] = {0.42f, 0.42f};
    tracking.live_preview_iris_centers[1] = {0.58f, 0.42f};
    tracking.live2d.param_angle_x = 6.5f;
    tracking.live2d.param_angle_y = -4.2f;
    tracking.live2d.param_angle_z = 2.1f;
    tracking.live2d.param_body_angle_x = 2.4f;
    tracking.live2d.param_eye_l_open = 0.96f;
    tracking.live2d.param_eye_r_open = 0.94f;
    tracking.live2d.param_brow_l_y = 0.55f;
    tracking.live2d.param_brow_r_y = 0.52f;
    tracking.live2d.param_mouth_open_y = 0.62f;
    tracking.live2d.param_mouth_form = 0.35f;
    tracking.live2d.vowel_a = 0.65f;
    tracking.live2d.vowel_e = 0.25f;

    aether::ui::DevShellRenderView render_view{};
    aether::ui::DevShellOutputView output_view{};
    aether::ui::StudioActions actions{};

    rig.ws.switch_mode(aether::ui::StudioMode::LiveStudio);
    for (int f = 0; f < 6; ++f) {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        rig.ws.render(static_cast<double>(f) / 60.0, *rig.rhi, telem, echo, rig.arena, true, avatar, tracking,
                      render_view, output_view, actions);
        ImGui::Render();
    }
    if (ImGuiWindow* expr_win = ImGui::FindWindowByName("Expressions"); expr_win != nullptr) {
        ImGui::FocusWindow(expr_win);
    }
    for (int f = 6; f < 10; ++f) {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        rig.ws.render(static_cast<double>(f) / 60.0, *rig.rhi, telem, echo, rig.arena, true, avatar, tracking,
                      render_view, output_view, actions);
        ImGui::Render();
    }

    rig.arena.mark_warmup_complete();
    aether::HotPathGuard::install();
    const uint64_t hp_before = aether::HotPathGuard::total_violations();
    for (int f = 10; f < 40; ++f) {
        actions.reset_per_frame();
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        {
            AETHER_HOT_PATH_SCOPE;
            rig.ws.render(static_cast<double>(f) / 60.0, *rig.rhi, telem, echo, rig.arena, true, avatar, tracking,
                          render_view, output_view, actions);
        }
        ImGui::Render();
    }
    CHECK(aether::HotPathGuard::total_violations() == hp_before);
    CHECK(rig.arena.os_fallback_allocs() == 0ULL);
    CHECK(rig.arena.steady_bump_growth_bytes() == 0ULL);
}

TEST_CASE("Viewport aspect lock preserves 1:1 pixel ratio and OBS 16:9 isolation", "[ui][aspect]") {
    using aether::core::compute_viewport_aspect_layout;
    using aether::core::ViewportAspectLayout;
    using aether::core::ViewportAspectMode;

    // Fit (Auto): zero distortion across extreme wide (1600x360), 16:9 (1600x900), and vertical (600x1200)
    for (const auto& sz : {std::pair{1600.0f, 360.0f}, std::pair{1600.0f, 900.0f}, std::pair{600.0f, 1200.0f}}) {
        const ViewportAspectLayout lay =
            compute_viewport_aspect_layout(sz.first, sz.second, 1920.0f, 1080.0f, ViewportAspectMode::FitAuto);
        CHECK_FALSE(lay.explicit_matte);
        CHECK(lay.uv_du <= 1.0001f);
        CHECK(lay.uv_dv <= 1.0001f);
        CHECK(std::fabs(lay.pixel_aspect_ratio(16.0f / 9.0f) - 1.0f) < 1e-3f);
        const float eff_fovy = lay.effective_fovy_rad(0.2805f);
        CHECK(eff_fovy > 0.01f);
        CHECK(eff_fovy <= 0.2806f);
    }

    // Explicit Aspect Modes (16:9, 9:16, 1:1): exact centered matte canvas & exact aspect ratio
    {
        const ViewportAspectLayout lay16 =
            compute_viewport_aspect_layout(1200.0f, 900.0f, 1920.0f, 1080.0f, ViewportAspectMode::Aspect16x9);
        CHECK(lay16.explicit_matte);
        CHECK(std::fabs(lay16.frame_aspect - 16.0f / 9.0f) < 1e-3f);
        CHECK(std::fabs(lay16.pixel_aspect_ratio(16.0f / 9.0f) - 1.0f) < 1e-3f);

        const ViewportAspectLayout lay9 =
            compute_viewport_aspect_layout(1200.0f, 900.0f, 1920.0f, 1080.0f, ViewportAspectMode::Aspect9x16);
        CHECK(lay9.explicit_matte);
        CHECK(std::fabs(lay9.frame_aspect - 9.0f / 16.0f) < 1e-3f);
        CHECK(lay9.norm_x0 > 0.25f);
        CHECK(lay9.norm_x1 < 0.75f);

        const ViewportAspectLayout lay1 =
            compute_viewport_aspect_layout(1200.0f, 900.0f, 1920.0f, 1080.0f, ViewportAspectMode::Aspect1x1);
        CHECK(lay1.explicit_matte);
        CHECK(std::fabs(lay1.frame_aspect - 1.0f) < 1e-3f);
    }
}

TEST_CASE("Viewport HUD collapse/close, Light Orb collision shift, Camera Presets, and Hand toggle",
          "[ui][presets][hud]") {
    using namespace aether::ui;
    aether::rhi::RhiDeviceConfig rhi_cfg{};
    rhi_cfg.enable_validation = false;
    rhi_cfg.require_validation = false;
    rhi_cfg.headless = true;
    auto rhi_res = aether::rhi::RhiDevice::create(rhi_cfg);
    REQUIRE(rhi_res.has_value());

    aether::ui::ScopedUiRig rig{};
    rig.rhi = rhi_res.value();
    REQUIRE(rig.arena.init(16ULL * 1024ULL * 1024ULL));
    REQUIRE(install_imgui_arena(rig.arena));
    ImGui::CreateContext();
    rig.imgui_init = true;
    ImPlot::CreateContext();
    rig.implot_init = true;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigInputTrickleEventQueue = false;

    const aether::RuntimePaths paths = aether::discover_runtime_paths();
    aether::HardwareInventory hw{};
    rig.ws.init(paths, hw, 1.0f, nullptr, false);
    rig.ws_init = true;
    rig.ws.viewport_panel().set_persist_presets(false);

    unsigned char* tex_pixels = nullptr;
    int tex_w = 0;
    int tex_h = 0;
    io.Fonts->GetTexDataAsAlpha8(&tex_pixels, &tex_w, &tex_h);
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));

    aether::TelemetrySnapshot telem{};
    telem.render_fps = 60.0f;
    aether::ui::DevShellInputEcho echo{};
    aether::ui::DevShellAvatarView avatar{};
    avatar.has_avatar = true;
    avatar.has_camera = true;
    avatar.bone_hips_y = 0.85f;
    avatar.bone_chest_y = 1.20f;
    avatar.bone_head_y = 1.46f;
    avatar.broadcast_width = 1920U;
    avatar.broadcast_height = 1080U;
    aether::ui::DevShellTrackingView tracking{};
    tracking.ready = true;
    tracking.camera_active = true;
    tracking.face_confidence = 0.96f;
    tracking.live_preview_valid = true;
    tracking.live_preview_texture_id = 1ULL;
    tracking.live_preview_width = 320U;
    tracking.live_preview_height = 180U;
    aether::ui::DevShellRenderView render_view{};
    aether::ui::DevShellOutputView output_view{};
    aether::ui::StudioActions actions{};

    // Hand tracking defaults to disabled (`false`)
    CHECK_FALSE(rig.ws.params().get_bool(ParamId::TrackingHandEnabled));

    // Render on tall viewport with expanded HUD: HUD drawn and Light Orb stays bottom-right without overlap
    for (int f = 0; f < 5; ++f) {
        actions.reset_per_frame();
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        rig.ws.render(static_cast<double>(f) / 60.0, *rig.rhi, telem, echo, rig.arena, true, avatar, tracking,
                      render_view, output_view, actions);
        ImGui::Render();
    }
    REQUIRE(rig.ws.viewport_panel().last_hud_drawn());
    {
        const ImVec2 hp0 = rig.ws.viewport_panel().last_hud_p0();
        const ImVec2 hp1 = rig.ws.viewport_panel().last_hud_p1();
        const ImVec2 op = rig.ws.viewport_panel().last_orb_pos();
        const float os = rig.ws.viewport_panel().last_orb_size();
        const bool overlaps = (op.x < hp1.x) && (op.x + os > hp0.x) && (op.y < hp1.y) && (op.y + os > hp0.y);
        CHECK_FALSE(overlaps);
    }

    // Shrink display height so bottom-right orb would collide with expanded HUD -> orb auto-shifts left
    io.DisplaySize = ImVec2(1280.0f, 380.0f);
    for (int f = 5; f < 9; ++f) {
        actions.reset_per_frame();
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        rig.ws.render(static_cast<double>(f) / 60.0, *rig.rhi, telem, echo, rig.arena, true, avatar, tracking,
                      render_view, output_view, actions);
        ImGui::Render();
    }
    if (rig.ws.viewport_panel().last_hud_drawn()) {
        const ImVec2 hp0 = rig.ws.viewport_panel().last_hud_p0();
        const ImVec2 hp1 = rig.ws.viewport_panel().last_hud_p1();
        const ImVec2 op = rig.ws.viewport_panel().last_orb_pos();
        const float os = rig.ws.viewport_panel().last_orb_size();
        const bool overlaps = (op.x < hp1.x) && (op.x + os > hp0.x) && (op.y < hp1.y) && (op.y + os > hp0.y);
        CHECK_FALSE(overlaps);
    }

    // Collapse HUD -> pill is shorter than expanded card
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    actions.reset_per_frame();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    rig.ws.render(0.2, *rig.rhi, telem, echo, rig.arena, true, avatar, tracking, render_view, output_view, actions);
    ImGui::Render();
    const float expanded_h = rig.ws.viewport_panel().last_hud_p1().y - rig.ws.viewport_panel().last_hud_p0().y;
    (void)rig.ws.params().set(ParamId::AppTrackingHudCollapsed, 1.0);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    rig.ws.render(0.22, *rig.rhi, telem, echo, rig.arena, true, avatar, tracking, render_view, output_view, actions);
    ImGui::Render();
    const float collapsed_h = rig.ws.viewport_panel().last_hud_p1().y - rig.ws.viewport_panel().last_hud_p0().y;
    CHECK(collapsed_h < expanded_h * 0.5f);

    // Close HUD -> overlay hidden while tracking controls/engine remain live; toggle back ON -> visible
    (void)rig.ws.params().set(ParamId::AppTrackingHudVisible, 0.0);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    rig.ws.render(0.24, *rig.rhi, telem, echo, rig.arena, true, avatar, tracking, render_view, output_view, actions);
    ImGui::Render();
    CHECK_FALSE(rig.ws.viewport_panel().last_hud_drawn());
    CHECK(tracking.camera_active);

    (void)rig.ws.params().set(ParamId::AppTrackingHudVisible, 1.0);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    rig.ws.render(0.26, *rig.rhi, telem, echo, rig.arena, true, avatar, tracking, render_view, output_view, actions);
    ImGui::Render();
    CHECK(rig.ws.viewport_panel().last_hud_drawn());

    // Aspect switches reflect cleanly in live ViewportPanel without disturbing broadcast 1920x1080 isolation
    (void)rig.ws.params().set(ParamId::AppViewportAspect, 2.0); // 9:16
    actions.reset_per_frame();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    rig.ws.render(0.28, *rig.rhi, telem, echo, rig.arena, true, avatar, tracking, render_view, output_view, actions);
    ImGui::Render();
    CHECK(rig.ws.viewport_panel().last_aspect_layout().explicit_matte);
    CHECK(std::fabs(rig.ws.viewport_panel().last_aspect_layout().frame_aspect - 9.0f / 16.0f) < 1e-2f);
    CHECK(avatar.broadcast_width == 1920U);
    CHECK(avatar.broadcast_height == 1080U);

    // Camera Presets Ctrl+1..6: bone-relative framing & custom preset JSON CRUD round-trip
    for (int p = 0; p < static_cast<int>(kBuiltinFramingPresetCount); ++p) {
        const CameraFramingPreset std_p = compute_builtin_framing_preset(p, 0.85f, 1.20f, 1.46f);
        const CameraFramingPreset short_p = compute_builtin_framing_preset(p, 0.65f, 0.92f, 1.12f);
        CHECK(std_p.target_y > short_p.target_y);
        CHECK(std_p.distance > 0.25f);
    }
    const CameraFramingPreset right_corner = compute_builtin_framing_preset(4, 0.85f, 1.20f, 1.46f);
    const CameraFramingPreset left_corner = compute_builtin_framing_preset(5, 0.85f, 1.20f, 1.46f);
    CHECK(right_corner.target_x < 0.0f); // Shifts camera target left -> avatar sits in bottom-right third
    CHECK(left_corner.target_x > 0.0f);  // Shifts camera target right -> avatar sits in bottom-left third
    const CameraFramingPreset face_closeup = compute_builtin_framing_preset(1, 0.85f, 1.20f, 1.46f);
    CHECK(std::fabs(face_closeup.pitch_rad) < 1e-4f);

    actions.reset_per_frame();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_2, true);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    rig.ws.render(0.30, *rig.rhi, telem, echo, rig.arena, true, avatar, tracking, render_view, output_view, actions);
    ImGui::Render();
    io.AddKeyEvent(ImGuiKey_2, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    CHECK(actions.viewport.request_apply_camera_preset);
    CHECK(actions.viewport.request_builtin_preset == 1);

    // Custom preset bank CRUD + JSON persistence
    {
        ScopedTempDir tmp("cam_presets");
        const auto json_path = (tmp.dir / "camera_presets.json").string();
        CameraPresetBank bank;
        bank.set_storage_path(json_path);
        CameraFramingPreset cp{};
        cp.name = "Desk CloseUp";
        cp.target_x = 0.12f;
        cp.target_y = 1.38f;
        cp.distance = 1.15f;
        cp.avatar_pos = {0.05f, 0.0f, -0.02f};
        cp.avatar_rot_y_deg = 12.0f;
        cp.avatar_scale = 1.05f;
        cp.has_avatar_xform = true;
        const size_t idx = bank.save_or_overwrite(cp, true);
        REQUIRE(idx == 0U);

        CameraPresetBank reloaded;
        REQUIRE(reloaded.load_from_file(json_path));
        REQUIRE(reloaded.custom_presets().size() == 1U);
        CHECK(reloaded.custom_presets()[0].name == "Desk CloseUp");
        CHECK(std::fabs(reloaded.custom_presets()[0].avatar_rot_y_deg - 12.0f) < 1e-4f);

        cp.distance = 0.95f;
        CHECK(reloaded.overwrite_at(0, cp, true));
        CHECK(reloaded.remove_at(0, true));
        CHECK(reloaded.custom_presets().empty());
    }

    // Enabling Hand Tracking dispatches pipeline toggle command cleanly
    actions.reset_per_frame();
    (void)rig.ws.params().set(ParamId::TrackingHandEnabled, 1.0);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    rig.ws.render(0.32, *rig.rhi, telem, echo, rig.arena, true, avatar, tracking, render_view, output_view, actions);
    ImGui::Render();
    CHECK(actions.viewport.request_set_hand_tracking);
    CHECK(actions.viewport.target_hand_tracking_enabled);
}
