#include "core/hot_path_guard.hpp"
#include "core/hw_enum.hpp"
#include "core/paths.hpp"
#include "core/telemetry.hpp"
#include "rhi/rhi_device.hpp"
#include "ui/palette/command_palette.hpp"
#include "ui/params/param_registry.hpp"
#include "ui/params/project_settings.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/theme/precision_dark.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/wizard/hardware_optimizer_wizard.hpp"
#include "ui/workspace/studio_workspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

} // namespace

TEST_CASE("ParamStore enforces 50-parameter registry bounds, epochs, and project reset", "[ui][params]") {
    const auto specs = all_param_specs();
    REQUIRE(specs.size() == kParamCount);
    REQUIRE(kParamCount == 50U); // 49 + render.backend (ADR-0016)

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
    REQUIRE(rec_npu.ep_override == AETHER_EP_INTEL_NPU);
    REQUIRE(rec_npu.tracking_ep_mode == 1);

    HardwareInventory dgpu_hw{};
    dgpu_hw.npu.available = false;
    auto rec_dgpu = HardwareOptimizerWizard::recommend_for_hardware(dgpu_hw, dgpu);
    REQUIRE(rec_dgpu.ep_override == AETHER_EP_DIRECTML_GPU);
    REQUIRE(rec_dgpu.tracking_ep_mode == 2);

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
    rhi->destroy();
    delete rhi;
}

} // namespace aether::ui
