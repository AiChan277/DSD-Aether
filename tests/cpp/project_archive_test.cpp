#include "core/action_registry.hpp"
#include "core/alfx_serializer.hpp"
#include "core/apr_serializer.hpp"
#include "core/clock.hpp"
#include "core/paths.hpp"
#include "core/session_manager.hpp"
#include "core/zip_archive.hpp"
#include "renderer/frame_globals_builder.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using Catch::Approx;

namespace {

struct ScopedTempDir {
    fs::path path;
    explicit ScopedTempDir(const char* prefix) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / (std::string(prefix) + "_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
};

struct ScopedThreadRoleReset {
    ~ScopedThreadRoleReset() { aether_set_thread_role(AETHER_THREAD_ROLE_ANY); }
};

[[nodiscard]] aether::renderer::StudioLightSpec make_test_light(const char* name,
                                                                aether::renderer::StudioFixtureProfile prof, float az,
                                                                float el, float ev, float cct, bool shadow,
                                                                aether::renderer::StudioGelPreset gel) {
    aether::renderer::StudioLightSpec s{};
    aether::renderer::configure_fixture_defaults(s, prof);
    s.name.fill('\0');
    const size_t n = std::min<size_t>(std::strlen(name), s.name.size() - 1u);
    std::memcpy(s.name.data(), name, n);
    s.use_orbit_angles = true;
    s.orbit_azimuth_deg = az;
    s.orbit_elevation_deg = el;
    s.orbit_distance_m = 2.15f;
    s.intensity_ev100 = ev;
    s.kelvin_cct = cct;
    s.cast_shadow = shadow;
    s.gel_enabled = (gel != aether::renderer::StudioGelPreset::None);
    s.gel = gel;
    s.grid_enabled = true;
    s.grid_angle_deg = 30.0f;
    s.barn_doors_enabled = true;
    s.barn_door_cuts_deg = {25.0f, 30.0f, 35.0f, 40.0f};
    s.softbox_enabled = true;
    s.diffusion_enabled = true;
    s.diffusion_density = 0.75f;
    return s;
}

[[nodiscard]] aether::lighting::StudioLightingStore make_rich_test_lighting_store() {
    aether::lighting::StudioLightingStore store;
    auto& rig = store.rig();
    rig.studio_light_count = 5u;
    rig.dominant_key_light_id = 1u;
    rig.circadian_sun_slot_id = 4;
    rig.circadian_enabled = true;
    rig.circadian_timezone = 9;
    rig.circadian_time_minutes = 1020;
    rig.ambient_floor = 0.19f;
    rig.environment_sky_strength = 0.33f;
    rig.ground_bounce_strength = 0.17f;
    rig.multi_light_soft_knee_threshold = 0.91f;
    rig.lighting_mode = 2u;

    rig.studio_lights[0] =
        make_test_light("Key SkyPanel Pro", aether::renderer::StudioFixtureProfile::ArriSkyPanel, 35.0f, 24.0f, 1.35f,
                        5200.0f, true, aether::renderer::StudioGelPreset::HalfCto);
    rig.studio_lights[1] =
        make_test_light("Primary Fresnel", aether::renderer::StudioFixtureProfile::ArriFresnel, -42.0f, 18.0f, 1.15f,
                        4800.0f, true, aether::renderer::StudioGelPreset::ShinkaiGoldenRose);
    rig.studio_lights[2] =
        make_test_light("Hair Kicker Strip", aether::renderer::StudioFixtureProfile::Stripbox, 155.0f, 44.0f, 0.88f,
                        6400.0f, false, aether::renderer::StudioGelPreset::CyberCyan);
    rig.studio_lights[3] = make_test_light("Neg Fill Flag", aether::renderer::StudioFixtureProfile::RectSoftbox, -85.0f,
                                           10.0f, 0.0f, 5600.0f, false, aether::renderer::StudioGelPreset::None);
    rig.studio_lights[3].passive_flag = aether::renderer::PassiveFlagMode::NegativeFillBlack;
    rig.studio_lights[4] =
        make_test_light("Solar CIE Dome", aether::renderer::StudioFixtureProfile::NaturalSunCieSky, 62.0f, 28.0f, 1.10f,
                        4400.0f, true, aether::renderer::StudioGelPreset::QuarterCto);

    rig.satsuei.light_wrap_strength = 0.37f;
    rig.satsuei.halation_strength = 0.29f;
    rig.satsuei.halation_threshold = 0.88f;
    rig.satsuei.backscatter_strength = 0.67f;

    rig.split_grading.avatar_contrast = 1.11f;
    rig.split_grading.avatar_saturation = 1.09f;
    rig.split_grading.avatar_warmth = 0.14f;
    rig.split_grading.avatar_ev_bias = 0.12f;
    rig.split_grading.bg_contrast = 0.94f;
    rig.split_grading.bg_saturation = 0.89f;
    rig.split_grading.bg_exposure = 1.08f;
    rig.split_grading.master_grade_strength = 0.92f;

    rig.bloom_threshold = 1.12f;
    rig.bloom_intensity = 0.14f;
    rig.tone_mode = 1u;
    rig.enable_2d_filter_pro = true;
    rig.painterly_flatten_strength = 0.63f;
    rig.dog_ink_boost = 0.72f;
    rig.enable_paper_warmth = true;
    rig.paper_warmth_strength = 0.24f;

    rig.vrm_mmd_parity.auto_toon_harmonizer_enabled = true;
    rig.vrm_mmd_parity.vrm_cel_crispness_boost = 0.91f;
    rig.vrm_mmd_parity.vrm_shadow_chroma_lift = 0.28f;
    rig.vrm_mmd_parity.vrm_auto_edge_scale_enabled = true;
    rig.vrm_mmd_parity.face_internal_edge_suppress_cm = 2.1f;
    rig.vrm_mmd_parity.unified_surface_group_crease = true;
    rig.vrm_mmd_parity.vrm_pre_cel_clarity_strength = 0.31f;
    rig.vrm_mmd_parity.vrm_hair_angel_ring_boost = 0.82f;
    rig.vrm_mmd_parity.pmx_alpha_bleed_padding = true;
    rig.vrm_mmd_parity.pmx_strict_material_draw_order = true;

    store.set_active_alfx_name("Studio_Test_Pro");
    return store;
}

[[nodiscard]] std::vector<uint8_t> make_test_rgba_frame(uint32_t w, uint32_t h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4u);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4u;
            px[idx + 0u] = static_cast<uint8_t>((x * 255u) / std::max(1u, w - 1u));
            px[idx + 1u] = static_cast<uint8_t>((y * 255u) / std::max(1u, h - 1u));
            px[idx + 2u] = 180u;
            px[idx + 3u] = 255u;
        }
    }
    return px;
}

[[nodiscard]] uint32_t read_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void write_text_file(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    REQUIRE(ofs.is_open());
    ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(ofs.good());
}

} // namespace

TEST_CASE("Hidden helper to generate presets/alfx/*.alfx", "[.gen_alfx]") {
    const fs::path dir = fs::path(AETHER_REPO_ROOT) / "presets" / "alfx";
    auto res = aether::core::AlfxSerializer::write_builtin_presets_dir(dir, true);
    REQUIRE(res.has_value());
}

TEST_CASE("ZipArchive RFC 1951 round-trip, BCrypt SHA-256, WIC 480x270 PNG, and atomic write rollback",
          "[rev31][archive]") {
    ScopedTempDir tmp("aether_zip_test");

    // 1. BCrypt SHA-256 known-vector check (empty input).
    auto empty_hash = aether::core::sha256_hex({});
    REQUIRE(empty_hash.has_value());
    REQUIRE(empty_hash.value() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // 2. WIC 480x270 PNG resize & encoding check.
    const auto rgba = make_test_rgba_frame(640u, 360u);
    auto png_res = aether::core::encode_thumbnail_png_480x270(rgba, 640u, 360u, false);
    REQUIRE(png_res.has_value());
    const auto& png = png_res.value();
    REQUIRE(png.size() > 32u);
    REQUIRE(png[0] == 0x89u);
    REQUIRE(png[1] == 0x50u);
    REQUIRE(png[2] == 0x4Eu);
    REQUIRE(png[3] == 0x47u);
    REQUIRE(std::memcmp(png.data() + 12, "IHDR", 4) == 0);
    REQUIRE(read_be32(png.data() + 16) == 480u);
    REQUIRE(read_be32(png.data() + 20) == 270u);

    // 3. RFC 1951 DEFLATE + STORE ZIP round-trip + CRC verification.
    const std::string big_text(4096u, 'A');
    std::vector<aether::core::ZipEntry> entries{
        {"manifest.json", std::vector<uint8_t>{'{', '"', 'k', '"', ':', '1', '}'}, true},
        {"big.txt", std::vector<uint8_t>(big_text.begin(), big_text.end()), true},
        {"thumbnail.png", png, false},
    };
    const fs::path zip_path = tmp.path / "test_bundle.zip";
    REQUIRE(aether::core::ZipWriter::write_atomic(zip_path, entries).has_value());

    auto zr = aether::core::ZipReader::open(zip_path);
    REQUIRE(zr.has_value());
    REQUIRE(zr.value().entries().size() == 3u);
    REQUIRE(zr.value().entries()[0].path == "manifest.json");
    REQUIRE(zr.value().verify_all().has_value());
    auto big_back = zr.value().read_text("big.txt");
    REQUIRE(big_back.has_value());
    REQUIRE(big_back.value() == big_text);

    // 4. Simulated write failure must preserve prior destination & .bak intact.
    const fs::path guarded_path = tmp.path / "guarded.dat";
    const std::vector<uint8_t> v1{'V', '1', '_', 'D', 'A', 'T', 'A'};
    const std::vector<uint8_t> v2{'V', '2', '_', 'D', 'A', 'T', 'A'};
    const std::vector<uint8_t> v3{'V', '3', '_', 'B', 'A', 'D'};
    REQUIRE(aether::core::write_bytes_atomic(guarded_path, v1).has_value());
    REQUIRE(aether::core::write_bytes_atomic(guarded_path, v2).has_value());

    auto fail_res = aether::core::write_bytes_atomic(guarded_path, v3, [](const fs::path&) -> aether::Expected<void> {
        return aether::fail(aether::ErrorCode::IoError, "Simulated write failure");
    });
    REQUIRE_FALSE(fail_res.has_value());

    auto dst_now = aether::core::read_file_bytes(guarded_path);
    auto bak_now = aether::core::read_file_bytes(fs::path(guarded_path.wstring() + L".bak"));
    REQUIRE(dst_now.has_value());
    REQUIRE(bak_now.has_value());
    REQUIRE(dst_now.value() == v2);
    REQUIRE(bak_now.value() == v1);
}

TEST_CASE("Empirical Test #6: .alfx v1.0.1 Cross-Model Portability, All 4 Import Modes, and 5 Builtin Presets",
          "[rev31][alfx]") {
    ScopedTempDir tmp("aether_alfx_test");

    // 1. Export studio_test.alfx from VRM store with full 5-light studio + parity + Satsuei config.
    const auto vrm_store = make_rich_test_lighting_store();
    aether::core::AlfxMetadata export_meta{};
    export_meta.name = "studio_test";
    export_meta.author = "Rev31_Test";
    export_meta.description = "Cross-model VRM -> PMX parity preset";
    export_meta.tags = {"test", "vrm", "pmx"};

    const fs::path alfx_path = tmp.path / "studio_test.alfx";
    REQUIRE(aether::core::AlfxSerializer::save_file(alfx_path, vrm_store, &export_meta).has_value());

    // Confirm human-readable indented UTF-8 JSON on disk.
    auto raw_bytes = aether::core::read_file_bytes(alfx_path);
    REQUIRE(raw_bytes.has_value());
    const std::string raw_str(reinterpret_cast<const char*>(raw_bytes.value().data()), raw_bytes.value().size());
    REQUIRE(raw_str.find("\"schema_version\": \"1.0.1\"") != std::string::npos);
    REQUIRE(raw_str.find("\"vrm_mmd_parity\"") != std::string::npos);

    auto loaded_doc = aether::core::AlfxSerializer::load_file(alfx_path);
    REQUIRE(loaded_doc.has_value());

    // 2. Import Mode 0: ReplaceAll (Apply Full) -> GPU buffers StudioLightGPU[16] + FrameGlobals match bit-exact.
    {
        aether::lighting::StudioLightingStore pmx_store;
        pmx_store.reset_to_zero_lights();
        aether::core::AlfxSerializer::apply_to_store(loaded_doc.value(), pmx_store,
                                                     aether::core::AlfxImportMode::ReplaceAll);

        std::array<aether::renderer::StudioLightGPU, aether::renderer::kMaxStudioLights> gpu_vrm{};
        std::array<aether::renderer::StudioLightGPU, aether::renderer::kMaxStudioLights> gpu_pmx{};
        const std::array<float, 3> pivot{0.0f, 1.32f, 0.0f};
        const std::array<float, 3> eye{0.0f, 1.45f, 2.2f};
        const uint32_t cnt_vrm = aether::renderer::pack_studio_lights_gpu(vrm_store.rig(), pivot, eye, gpu_vrm);
        const uint32_t cnt_pmx = aether::renderer::pack_studio_lights_gpu(pmx_store.rig(), pivot, eye, gpu_pmx);
        REQUIRE(cnt_vrm == cnt_pmx);
        REQUIRE(cnt_pmx == 5u);
        REQUIRE(std::memcmp(gpu_vrm.data(), gpu_pmx.data(), sizeof(gpu_vrm)) == 0);

        aether::renderer::GpuAvatar dummy_avatar{};
        dummy_avatar.face_skin_ref_linear = {0.82f, 0.71f, 0.66f, 1.0f};
        aether::renderer::GpuFrameGlobals fg_vrm{};
        aether::renderer::GpuFrameGlobals fg_pmx{};
        aether::renderer::GpuMat4 prev_vp{};
        aether::renderer::GpuMat4 next_vp_vrm{};
        aether::renderer::GpuMat4 next_vp_pmx{};
        aether::renderer::FrameRenderRequest req_vrm{};
        req_vrm.avatar = &dummy_avatar;
        req_vrm.rig = &vrm_store.rig();
        req_vrm.rt_enabled = true;
        aether::renderer::FrameRenderRequest req_pmx{};
        req_pmx.avatar = &dummy_avatar;
        req_pmx.rig = &pmx_store.rig();
        req_pmx.rt_enabled = true;
        (void)aether::renderer::detail::populate_shared_frame_globals(req_vrm, 1280u, 720u, true, true, false, prev_vp,
                                                                      fg_vrm, next_vp_vrm);
        (void)aether::renderer::detail::populate_shared_frame_globals(req_pmx, 1280u, 720u, true, true, false, prev_vp,
                                                                      fg_pmx, next_vp_pmx);
        REQUIRE(std::memcmp(&fg_vrm, &fg_pmx, sizeof(fg_vrm)) == 0);
    }

    // 3. Import Mode 1: LightsOnly -> replaces 16-slot lights cleanly, leaves Satsuei/SplitGrade/Parity untouched.
    {
        aether::lighting::StudioLightingStore target;
        target.reset_to_zero_lights();
        target.rig().satsuei.halation_strength = 0.55f;
        target.rig().split_grading.avatar_contrast = 1.27f;
        target.rig().vrm_mmd_parity.vrm_cel_crispness_boost = 0.61f;
        target.rig().bloom_intensity = 0.42f;

        aether::core::AlfxSerializer::apply_to_store(loaded_doc.value(), target,
                                                     aether::core::AlfxImportMode::LightsOnly);
        REQUIRE(target.rig().studio_light_count == 5u);
        REQUIRE(target.rig().dominant_key_light_id == 1u);
        REQUIRE(target.rig().satsuei.halation_strength == Approx(0.55f));
        REQUIRE(target.rig().split_grading.avatar_contrast == Approx(1.27f));
        REQUIRE(target.rig().vrm_mmd_parity.vrm_cel_crispness_boost == Approx(0.61f));
        REQUIRE(target.rig().bloom_intensity == Approx(0.42f));
    }

    // 4. Import Mode 2: ColorPostFxOnly -> replaces shading/Satsuei/parity, preserves existing lights untouched.
    {
        aether::lighting::StudioLightingStore target;
        target.rig().studio_light_count = 2u;
        target.rig().studio_lights[0] =
            make_test_light("Existing Key", aether::renderer::StudioFixtureProfile::Octabox, 15.0f, 30.0f, 2.10f,
                            5900.0f, true, aether::renderer::StudioGelPreset::None);
        target.rig().studio_lights[1] =
            make_test_light("Existing Rim", aether::renderer::StudioFixtureProfile::Stripbox, -150.0f, 20.0f, 0.90f,
                            6500.0f, false, aether::renderer::StudioGelPreset::None);

        aether::core::AlfxSerializer::apply_to_store(loaded_doc.value(), target,
                                                     aether::core::AlfxImportMode::ColorPostFxOnly);
        REQUIRE(target.rig().studio_light_count == 2u);
        REQUIRE(std::string(target.rig().studio_lights[0].name.data()) == "Existing Key");
        REQUIRE(target.rig().studio_lights[0].intensity_ev100 == Approx(2.10f));
        REQUIRE(target.rig().satsuei.halation_strength == Approx(0.29f));
        REQUIRE(target.rig().split_grading.avatar_contrast == Approx(1.11f));
        REQUIRE(target.rig().vrm_mmd_parity.vrm_cel_crispness_boost == Approx(0.91f));
        REQUIRE(target.rig().bloom_intensity == Approx(0.14f));
    }

    // 5. Import Mode 3: AdditiveAppendLights -> appends trailing lights cleanly & caps safely at 16 slots.
    {
        aether::lighting::StudioLightingStore target;
        target.rig().studio_light_count = 3u;
        target.rig().dominant_key_light_id = 0u;
        const float orig_key_ev = target.rig().studio_lights[0].intensity_ev100;

        aether::core::AlfxSerializer::apply_to_store(loaded_doc.value(), target,
                                                     aether::core::AlfxImportMode::AdditiveAppendLights);
        REQUIRE(target.rig().studio_light_count == 8u);
        REQUIRE(target.rig().studio_lights[0].intensity_ev100 == Approx(orig_key_ev));
        REQUIRE(std::string(target.rig().studio_lights[3].name.data()) == "Key SkyPanel Pro");

        target.rig().studio_light_count = 13u;
        aether::core::AlfxSerializer::apply_to_store(loaded_doc.value(), target,
                                                     aether::core::AlfxImportMode::AdditiveAppendLights);
        REQUIRE(target.rig().studio_light_count == aether::renderer::kMaxStudioLights);
    }

    // 6. All 5 shipped presets/alfx/*.alfx exist on disk and load cleanly.
    const fs::path presets_dir = fs::path(AETHER_REPO_ROOT) / "presets" / "alfx";
    const auto catalog = aether::core::AlfxSerializer::builtin_preset_catalog();
    for (size_t i = 0; i < catalog.size(); ++i) {
        const fs::path p = presets_dir / catalog[i].filename;
        REQUIRE(fs::exists(p));
        auto preset_doc = aether::core::AlfxSerializer::load_file(p);
        REQUIRE(preset_doc.has_value());
        REQUIRE(preset_doc.value().schema_version == "1.0.1");
        REQUIRE(preset_doc.value().rig.studio_light_count >= 3u);
        REQUIRE(preset_doc.value().meta.name == catalog[i].display_name);
    }
}

TEST_CASE(
    "Empirical Test #5: .apr v1.0.0 Full Round-Trip, Graph FFI, Tamper Rejection, Atomic Safety, & Legacy Migrator",
    "[rev31][apr]") {
    ScopedTempDir tmp("aether_apr_test");
    ScopedThreadRoleReset role_reset;

    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherHostConfig hcfg{};
    hcfg.struct_size = sizeof(hcfg);
    hcfg.worker_threads = 1u;
    AetherHostHandle host = nullptr;
    REQUIRE(aether_host_create(&hcfg, &host) == AETHER_OK);
    REQUIRE(host != nullptr);

    const char* graph_json = R"({
        "schema_version": 1,
        "nodes": [
            { "id": 1, "type": "aether.model.source@1", "position": [10.0, 20.0] },
            { "id": 2, "type": "aether.texture.asset@1", "params": { "path": "hair_glow.png" } },
            { "id": 3, "type": "aether.texture.grade@1", "params": { "saturation": 1.25 } },
            { "id": 4, "type": "aether.material.slot_override@1", "params": { "slot_name": "Hair_Mat" } },
            { "id": 5, "type": "aether.source.npu_pose@1" },
            { "id": 6, "type": "aether.output.pose@1" }
        ],
        "edges": [
            { "id": 1, "from_node": 1, "from_port": "material_table", "to_node": 4, "to_port": "material_table" },
            { "id": 2, "from_node": 2, "from_port": "texture", "to_node": 3, "to_port": "texture" },
            { "id": 3, "from_node": 3, "from_port": "texture", "to_node": 4, "to_port": "texture" },
            { "id": 4, "from_node": 5, "from_port": "pose", "to_node": 6, "to_port": "pose" }
        ]
    })";
    REQUIRE(aether_graph_deserialize_json(host, graph_json, std::strlen(graph_json)) == AETHER_OK);

    const fs::path rel_avatar = tmp.path / "models" / "kuroha.pmx";
    write_text_file(rel_avatar, "PMX-STUB");

    aether::StudioStateStore src_state{};
    src_state.meta.project_name = "Shinkai_Night_Stream";
    src_state.meta.avatar_display_name = "Kuroha_v2";
    src_state.meta.render_tier = "PathTracing_25D";
    src_state.avatar.format = "PMX";
    src_state.avatar.path_relative = "models/kuroha.pmx";
    src_state.avatar.path_absolute = "Z:/NonExistentMoveDrive/OldPath/kuroha.pmx";
    src_state.avatar.file_sha256 = "deadbeef";
    src_state.avatar.root_pos = {0.15f, 0.0f, -0.25f};
    src_state.avatar.root_rot_deg = {0.0f, 12.5f, 0.0f};
    src_state.avatar.uniform_scale = 1.07f;
    src_state.avatar.physics_enabled = true;
    src_state.avatar.pmx_rigidbody_damping = 0.91f;
    src_state.avatar.pmx_velocity_clamp = 6.2f;
    src_state.avatar.active_expression_slot = 3u;
    src_state.camera.active_preset_id = "user_01";
    src_state.camera.aspect_mode = "9:16";
    src_state.camera.live_camera_state.distance = 1.12f;
    src_state.camera.custom_presets.push_back(aether::ProjectCustomCameraPreset{"user_01",
                                                                                "Close Chatting",
                                                                                {0.0f, 1.44f, 0.0f},
                                                                                0.95f,
                                                                                6.0f,
                                                                                -1.5f,
                                                                                22.0f,
                                                                                {0.1f, 0.0f, 0.0f},
                                                                                5.0f,
                                                                                1.0f,
                                                                                true});
    src_state.lighting_stage_camera.view_mode = "Quad";
    src_state.lighting_stage_camera.quad_view_enabled = true;
    src_state.tracking_and_audio.hand_tracking_enabled = true;
    src_state.tracking_and_audio.npu_movement_bounds_min = {-0.40f, -0.30f, -0.35f};
    src_state.outputs.spout_sender_name = "AetherRev31Show";
    src_state.outputs.ndi_enabled = true;
    src_state.outputs.osc_port = 9123u;
    src_state.packed_assets.push_back(aether::PackedAssetEntry{"assets/overrides/hair_glow.bin", {1u, 2u, 3u, 4u}});

    const auto src_lighting = make_rich_test_lighting_store();
    const auto thumb_rgba = make_test_rgba_frame(640u, 360u);
    const fs::path apr_path = tmp.path / "Shinkai_Night_Stream.apr";

    aether::core::AprSaveInput save_in{};
    save_in.state = &src_state;
    save_in.lighting = &src_lighting;
    save_in.rust_host = host;
    save_in.thumbnail_pixels = thumb_rgba;
    save_in.thumb_width = 640u;
    save_in.thumb_height = 360u;

    auto saved_man = aether::core::AprSerializer::save_file(apr_path, save_in);
    REQUIRE(saved_man.has_value());
    REQUIRE(saved_man.value().summary.has_thumbnail);
    REQUIRE(saved_man.value().summary.active_light_count == 4u);

    // Fast manifest + thumbnail read (entry 0 must be manifest.json).
    {
        auto zr = aether::core::ZipReader::open(apr_path);
        REQUIRE(zr.has_value());
        REQUIRE_FALSE(zr.value().entries().empty());
        REQUIRE(zr.value().entries()[0].path == "manifest.json");
    }
    std::vector<uint8_t> fast_thumb;
    auto fast_man = aether::core::AprSerializer::read_manifest_fast(apr_path, &fast_thumb);
    REQUIRE(fast_man.has_value());
    REQUIRE(fast_man.value().project_name == "Shinkai_Night_Stream");
    REQUIRE(fast_thumb.size() > 32u);
    REQUIRE(std::memcmp(fast_thumb.data() + 12, "IHDR", 4) == 0);
    REQUIRE(read_be32(fast_thumb.data() + 16) == 480u);
    REQUIRE(read_be32(fast_thumb.data() + 20) == 270u);

    // Clear host graph, load .apr back, and assert 100% lossless restoration + host deserialization.
    const char* empty_graph = R"({"schema_version":1,"nodes":[],"edges":[]})";
    REQUIRE(aether_graph_deserialize_json(host, empty_graph, std::strlen(empty_graph)) == AETHER_OK);

    auto loaded_res = aether::core::AprSerializer::load_file(apr_path);
    REQUIRE(loaded_res.has_value());
    const auto& loaded = loaded_res.value();
    REQUIRE_FALSE(loaded.state.is_dirty());
    REQUIRE(loaded.state.meta.project_name == "Shinkai_Night_Stream");
    REQUIRE(loaded.state.avatar.root_pos[2] == Approx(-0.25f));
    REQUIRE(loaded.state.avatar.pmx_rigidbody_damping == Approx(0.91f));
    REQUIRE(loaded.state.camera.aspect_mode == "9:16");
    REQUIRE(loaded.state.camera.custom_presets.size() == 1u);
    REQUIRE(loaded.state.camera.custom_presets[0].name == "Close Chatting");
    REQUIRE(loaded.state.lighting_stage_camera.quad_view_enabled);
    REQUIRE(loaded.state.tracking_and_audio.hand_tracking_enabled);
    REQUIRE(loaded.state.outputs.spout_sender_name == "AetherRev31Show");
    REQUIRE(loaded.state.outputs.osc_port == 9123u);
    REQUIRE(loaded.state.packed_assets.size() == 1u);
    REQUIRE(loaded.lighting_rig.studio_light_count == 5u);
    REQUIRE(loaded.lighting_rig.satsuei.halation_strength == Approx(0.29f));

    // Relative avatar resolution works even when absolute path points to another drive.
    const fs::path resolved_av = aether::core::AprSerializer::resolve_avatar_path(apr_path, loaded.state.avatar);
    REQUIRE(fs::weakly_canonical(resolved_av) == fs::weakly_canonical(rel_avatar));

    // Apply loaded node graph back onto Rust host and verify non-empty graph round-trips.
    REQUIRE(aether::core::AprSerializer::apply_loaded_graph_to_host(loaded, host).has_value());
    char* host_json_out = nullptr;
    uintptr_t host_json_len = 0u;
    REQUIRE(aether_graph_serialize_json(host, &host_json_out, &host_json_len) == AETHER_OK);
    REQUIRE(host_json_out != nullptr);
    const std::string restored_graph(host_json_out, static_cast<size_t>(host_json_len));
    aether_string_free(host_json_out);
    REQUIRE(restored_graph.find("aether.material.slot_override@1") != std::string::npos);

    // Tamper check: alter lighting.alfx inside a copy while keeping manifest SHA-256 stale -> must reject!
    {
        auto zr = aether::core::ZipReader::open(apr_path);
        REQUIRE(zr.has_value());
        std::vector<aether::core::ZipEntry> tampered;
        for (const auto& info : zr.value().entries()) {
            auto bytes = zr.value().read_entry(info.path);
            REQUIRE(bytes.has_value());
            if (info.path == "lighting.alfx" && !bytes.value().empty()) {
                bytes.value()[1] = (bytes.value()[1] == ' ') ? '\n' : ' ';
            }
            tampered.push_back(aether::core::ZipEntry{info.path, std::move(bytes.value()), info.method != 0u});
        }
        const fs::path tampered_path = tmp.path / "tampered.apr";
        REQUIRE(aether::core::ZipWriter::write_atomic(tampered_path, tampered).has_value());
        auto tampered_res = aether::core::AprSerializer::load_file(tampered_path);
        REQUIRE_FALSE(tampered_res.has_value());
        REQUIRE(tampered_res.error().code == aether::ErrorCode::ParseError);
    }

    // Atomic failure check on AprSerializer::save_file -> existing .apr stays 100% readable & unchanged.
    {
        const auto bytes_before = aether::core::read_file_bytes(apr_path);
        REQUIRE(bytes_before.has_value());

        src_state.meta.project_name = "Should_Not_Overwrite";
        save_in.pre_commit_hook = [](const fs::path&) -> aether::Expected<void> {
            return aether::fail(aether::ErrorCode::IoError, "Simulated disk error");
        };
        auto fail_save = aether::core::AprSerializer::save_file(apr_path, save_in);
        REQUIRE_FALSE(fail_save.has_value());
        save_in.pre_commit_hook = {};

        const auto bytes_after = aether::core::read_file_bytes(apr_path);
        REQUIRE(bytes_after.has_value());
        REQUIRE(bytes_after.value() == bytes_before.value());

        auto still_valid = aether::core::AprSerializer::load_file(apr_path);
        REQUIRE(still_valid.has_value());
        REQUIRE(still_valid.value().state.meta.project_name == "Shinkai_Night_Stream");
    }

    // Legacy .aether/ bundle migration test: unsaved flag set, lights & graph & avatar preserved, source intact.
    {
        const fs::path legacy_dir = tmp.path / "MyOldShow.aether";
        const std::string legacy_toml = R"(schema_version = 2
app_version = "0.8.0"

[avatar]
path = "../models/kuroha.pmx"
sha256 = "abc123"

[avatar.import_settings]
uniform_scale = 1.15
physics_enabled = true

[scene.lights]
ambient_light = 0.21
key_ev = 1.45
key_az = 48.0
fill_ev = 0.42
rim_ev = 0.77

[scene.camera]
aspect_mode = "9:16"
fov_y_deg = 28.0

[outputs]
spout_enabled = true
spout_sender_name = "LegacyShow"
vcam_enabled = false
)";
        write_text_file(legacy_dir / "project.toml", legacy_toml);
        write_text_file(legacy_dir / "graph.json", graph_json);

        REQUIRE(aether::core::LegacyAetherMigrator::is_legacy_aether_path(legacy_dir));
        auto migrated_res = aether::core::LegacyAetherMigrator::migrate_to_bundle(legacy_dir);
        REQUIRE(migrated_res.has_value());
        const auto& migrated = migrated_res.value();
        REQUIRE(migrated.state.is_dirty());
        REQUIRE(migrated.state.avatar.uniform_scale == Approx(1.15f));
        REQUIRE(migrated.state.camera.aspect_mode == "9:16");
        REQUIRE(migrated.state.outputs.spout_sender_name == "LegacyShow");
        REQUIRE_FALSE(migrated.state.outputs.vcam_enabled);
        REQUIRE(migrated.lighting_rig.ambient_floor == Approx(0.21f));
        REQUIRE(migrated.lighting_rig.studio_lights[0].intensity_ev100 == Approx(1.45f));
        REQUIRE(migrated.lighting_rig.studio_lights[0].orbit_azimuth_deg == Approx(48.0f));
        REQUIRE(migrated.lighting_rig.studio_lights[1].intensity_ev100 == Approx(0.42f));
        REQUIRE(migrated.lighting_rig.studio_lights[2].intensity_ev100 == Approx(0.77f));

        const fs::path converted_apr = tmp.path / "MyConvertedShow.apr";
        auto converted = aether::core::LegacyAetherMigrator::migrate_to_apr_file(legacy_dir, converted_apr, host);
        REQUIRE(converted.has_value());
        REQUIRE(fs::exists(converted_apr));
        REQUIRE(fs::exists(legacy_dir / "project.toml"));
    }

    aether_host_destroy(host);
}

TEST_CASE("Rev 3.1 SessionManager portable.flag, 1-time settings.toml migration, and dead-PID crash recovery",
          "[session][migration][portable]") {
    ScopedTempDir tmp("rev31_session");

    // 1. portable.flag next to executable switches all roots to <exe_dir>/userdata cleanly.
    {
        const fs::path fake_exe_dir = tmp.path / "portable_bin";
        fs::create_directories(fake_exe_dir);
        write_text_file(fake_exe_dir / "portable.flag", "1\n");
        const aether::RuntimePaths pr = aether::discover_runtime_paths(&fake_exe_dir);
        REQUIRE(fs::path(pr.app_data_dir) == fake_exe_dir / "userdata");
        REQUIRE(fs::path(pr.cache_dir) == fake_exe_dir / "userdata" / "Cache");
        REQUIRE(fs::path(pr.default_projects_dir) == fake_exe_dir / "userdata" / "Projects");
    }

    // 2. Non-portable defaults point app_data_dir to %APPDATA%/AETHER-2.5D-Studio.
    {
        const fs::path std_exe_dir = tmp.path / "installed_bin";
        fs::create_directories(std_exe_dir);
        const aether::RuntimePaths sr = aether::discover_runtime_paths(&std_exe_dir);
        REQUIRE(fs::path(sr.app_data_dir).filename() == "AETHER-2.5D-Studio");
    }

    // 3. One-time settings.toml migration preserves legacy file + migrates hardware prefs, pin cap <= 30, & recovery.
    const fs::path app_dir = tmp.path / "session_app_data";
    const fs::path legacy_dir = tmp.path / "legacy_aether_studio";
    const fs::path proj_dir = tmp.path / "projects";
    fs::create_directories(proj_dir);
    write_text_file(legacy_dir / "settings.toml", "[app]\n"
                                                  "preferred_gpu_name = \"NVIDIA GeForce RTX 4060 Laptop GPU\"\n"
                                                  "preferred_gpu_luid = \"0102030405060708\"\n"
                                                  "preferred_mic_id = \"mic-usb-01\"\n"
                                                  "ws_auth_token = \"tok_mig_999\"\n"
                                                  "wizard_completed = true\n"
                                                  "[params]\n"
                                                  "render.backend = 2\n"
                                                  "render.lighting_mode = 1\n"
                                                  "app.ui_scale_pct = 125\n"
                                                  "tracking.ep_mode = 4\n");
    write_text_file(legacy_dir / "keymap.toml", "edit.undo = \"Ctrl+Z\"\n");

    aether::RuntimePaths rp{};
    rp.app_data_dir = app_dir.string();
    rp.legacy_app_data_dir = legacy_dir.string();
    rp.default_projects_dir = proj_dir.string();

    aether::core::SessionManager mgr{};
    REQUIRE(mgr.init(rp).has_value());
    REQUIRE(mgr.state().migrated_from_legacy_toml);
    REQUIRE(mgr.state().preferred_gpu_name == "NVIDIA GeForce RTX 4060 Laptop GPU");
    REQUIRE(mgr.state().preferred_gpu_luid == "0102030405060708");
    REQUIRE(mgr.state().preferred_mic_id == "mic-usb-01");
    REQUIRE(mgr.state().ws_auth_token == "tok_mig_999");
    REQUIRE(mgr.state().render_backend == "dx12");
    REQUIRE(mgr.state().lighting_tier == 1u);
    REQUIRE(mgr.state().wizard_completed);
    REQUIRE(mgr.state().app_params.at("app.ui_scale_pct") == Approx(125.0));
    REQUIRE(mgr.state().app_params.at("tracking.ep_mode") == Approx(0.0));
    REQUIRE(fs::exists(legacy_dir / "settings.toml")); // backup left intact
    REQUIRE(fs::exists(app_dir / "keymap.toml"));

    // Recent projects cap (30 max) & pin preservation.
    for (int i = 0; i < 35; ++i) {
        aether::core::AprManifest man{};
        man.project_uuid = "uuid-" + std::to_string(i);
        man.project_name = "Project " + std::to_string(i);
        man.modified_utc = "2026-11-23T10:00:00Z";
        const fs::path p = proj_dir / ("proj_" + std::to_string(i) + ".apr");
        mgr.record_recent_project(p, man);
        if (i == 0) {
            REQUIRE(mgr.set_project_pinned(p, true));
        }
    }
    REQUIRE(mgr.state().recent_projects.size() <= aether::core::SessionState::kMaxRecentProjects);
    REQUIRE(
        std::any_of(mgr.state().recent_projects.begin(), mgr.state().recent_projects.end(),
                    [](const aether::core::RecentProjectEntry& e) { return e.pinned && e.project_uuid == "uuid-0"; }));

    // Dead-PID session.lock + newer .apr.autosave triggers crash recovery banner info.
    const std::string rec_uuid = aether::core::AprSerializer::generate_uuid_v4();
    const fs::path orig_apr = proj_dir / "RecoverTarget.apr";
    const fs::path auto_apr = mgr.autosave_path_for(rec_uuid);

    aether::StudioStateStore st{};
    st.meta.project_uuid = rec_uuid;
    st.meta.project_name = "RecoverTarget";
    aether::lighting::StudioLightingStore lstore{};
    aether::core::AprSaveInput s_in{};
    s_in.state = &st;
    s_in.lighting = &lstore;
    REQUIRE(aether::core::AprSerializer::save_file(orig_apr, s_in).has_value());
    REQUIRE(aether::core::AprSerializer::save_file(auto_apr, s_in).has_value());
    REQUIRE(mgr.acquire_session_lock(rec_uuid, orig_apr).has_value());

    const auto recovery = mgr.inspect_crash_recovery(true);
    REQUIRE(recovery.detected);
    REQUIRE(recovery.project_uuid == rec_uuid);
    REQUIRE(recovery.autosave_apr_path == auto_apr);

    mgr.remove_autosave_for(rec_uuid);
    mgr.release_session_lock();
    const auto cleared = mgr.inspect_crash_recovery(true);
    REQUIRE_FALSE(cleared.detected);
}

TEST_CASE("Rev 3.1 AprIoWorker SPSC background save & scan plus WIC 480x270 PNG decode roundtrip",
          "[session][worker][png]") {
    ScopedTempDir tmp("rev31_worker");

    // 1. Encode 16x9 RGBA8 -> 480x270 PNG -> WIC decode back to RGBA8
    std::vector<uint8_t> src_rgba(64u * 36u * 4u, 0u);
    for (size_t i = 0; i < 64u * 36u; ++i) {
        src_rgba[i * 4u + 0u] = 48u;
        src_rgba[i * 4u + 1u] = 160u;
        src_rgba[i * 4u + 2u] = 240u;
        src_rgba[i * 4u + 3u] = 255u;
    }
    auto png_res = aether::core::encode_thumbnail_png_480x270(src_rgba, 64u, 36u, false);
    REQUIRE(png_res.has_value());
    auto dec_res = aether::core::decode_png_rgba8(png_res.value());
    REQUIRE(dec_res.has_value());
    REQUIRE(dec_res.value().width == 480u);
    REQUIRE(dec_res.value().height == 270u);
    REQUIRE(dec_res.value().rgba8.size() == 480u * 270u * 4u);
    CHECK(dec_res.value().rgba8[0] == 48u);
    CHECK(dec_res.value().rgba8[1] == 160u);
    CHECK(dec_res.value().rgba8[2] == 240u);

    // 2. SPSC AprIoWorker background .apr save + library card scan
    const fs::path dst_apr = tmp.path / "WorkerCard.apr";
    aether::StudioStateStore st{};
    st.meta.project_uuid = aether::core::AprSerializer::generate_uuid_v4();
    st.meta.project_name = "Worker Card Studio";
    st.meta.avatar_display_name = "Momo";
    st.avatar.format = "VRM";
    st.mark_dirty(aether::DirtyDomain::LightingRig);

    aether::renderer::LightingRig rig = aether::core::AlfxSerializer::build_builtin_preset(1).rig;
    aether::core::AprIoWorker worker{};
    worker.start();
    REQUIRE(
        worker.enqueue_save(dst_apr, st, rig, "Anime Sunset Rim", png_res.value(), aether::core::AprWorkerOp::SaveApr));

    aether::HighResTimer wait_tmr;
    aether::core::AprWorkerResult save_res{};
    bool got_save = false;
    for (int iter = 0; iter < 500 && !got_save; ++iter) {
        if (worker.try_pop_result(save_res)) {
            got_save = true;
            break;
        }
        wait_tmr.sleep_us(5000);
    }
    REQUIRE(got_save);
    REQUIRE(save_res.success);
    REQUIRE(save_res.snapshot_dirty_generation == st.dirty_generation);
    REQUIRE(fs::is_regular_file(dst_apr));

    REQUIRE(worker.enqueue_scan({tmp.path}, {}));
    aether::core::AprWorkerResult scan_res{};
    bool got_scan = false;
    for (int iter = 0; iter < 500 && !got_scan; ++iter) {
        if (worker.try_pop_result(scan_res)) {
            got_scan = true;
            break;
        }
        wait_tmr.sleep_us(5000);
    }
    worker.stop();

    REQUIRE(got_scan);
    REQUIRE(scan_res.success);
    REQUIRE(scan_res.scanned_cards.size() == 1u);
    REQUIRE(scan_res.scanned_cards[0].manifest.project_name == "Worker Card Studio");
    REQUIRE(scan_res.scanned_cards[0].manifest.summary.has_thumbnail);
    REQUIRE_FALSE(scan_res.scanned_cards[0].thumbnail_png.empty());
}

TEST_CASE("Rev 3.1 ActionRegistry uniqueness and canonical bindings", "[ui][actions]") {
    using namespace aether::core;
    const auto& specs = ActionRegistry::all_specs();
    REQUIRE(specs.size() == kActionCount);

    std::unordered_set<std::string> seen_names;
    for (size_t i = 0; i < specs.size(); ++i) {
        REQUIRE(static_cast<size_t>(specs[i].id) == i);
        REQUIRE(specs[i].name != nullptr);
        REQUIRE(std::strlen(specs[i].name) > 0u);
        REQUIRE(specs[i].label != nullptr);
        REQUIRE(std::strlen(specs[i].label) > 0u);
        REQUIRE(seen_names.insert(specs[i].name).second);
    }

    CHECK(std::string(ActionRegistry::spec(ActionId::OpenProjectManager).default_chord) == "Shift+1");
    CHECK(std::string(ActionRegistry::spec(ActionId::RedoAlt).default_chord) == "Ctrl+Shift+Z");
    CHECK(std::string(ActionRegistry::spec(ActionId::ModalCancel).default_chord) == "Escape");
    CHECK(ActionRegistry::find_by_name("lighting.export_alfx") != nullptr);
    CHECK(ActionRegistry::find_by_name("lighting.import_alfx") != nullptr);
}
