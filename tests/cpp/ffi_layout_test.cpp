#include "aether_ffi_contract.h"
#include "aether_test_hooks.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_map>

// Compile-time frozen contract assertions (`HR-03`, `REQ-FFI-001`, `REQ-FFI-002`, `REQ-FFI-004`).
static_assert(sizeof(AetherVertex25D) == 64);
static_assert(alignof(AetherVertex25D) == 64);
static_assert(offsetof(AetherVertex25D, position) == 0);
static_assert(offsetof(AetherVertex25D, outline_width_scale) == 12);
static_assert(offsetof(AetherVertex25D, true_normal) == 16);
static_assert(offsetof(AetherVertex25D, z_flatten_weight) == 28);
static_assert(offsetof(AetherVertex25D, proxy_normal) == 32);
static_assert(offsetof(AetherVertex25D, sdf_face_mask) == 44);
static_assert(offsetof(AetherVertex25D, smooth_tangent) == 48);

static_assert(sizeof(AetherVertexSurface) == 32);
static_assert(sizeof(AetherVertexSkin) == 32);
static_assert(sizeof(AetherMorphDelta) == 32);

static_assert(sizeof(AetherTrackingFrame) == 2048);
static_assert(alignof(AetherTrackingFrame) == 64);
static_assert(offsetof(AetherTrackingFrame, arkit_blendshapes) == 36);
static_assert(offsetof(AetherTrackingFrame, joint_positions_ws) == 244);
static_assert(offsetof(AetherTrackingFrame, joint_rotations_quat) == 1124);
static_assert(offsetof(AetherTrackingFrame, frame_sequence) == 2004);
static_assert(offsetof(AetherTrackingFrame, valid_flags) == 2008);

static_assert(sizeof(AetherDspSyncPacket) == 128);
static_assert(alignof(AetherDspSyncPacket) == 64);
static_assert(offsetof(AetherDspSyncPacket, dsp_sequence_number) == 64);
static_assert(offsetof(AetherDspSyncPacket, dsp_viseme_weights) == 72);

static_assert(sizeof(AetherArenaSlice) == 24);
static_assert(sizeof(AetherRuntimeConfig) == 32);
static_assert(sizeof(AetherLoadRequest) == 16);
static_assert(sizeof(AetherAssetRequirements) == 168);
static_assert(sizeof(AetherAvatarDesc) == 504);
static_assert(sizeof(AetherHostConfig) == 24);
static_assert(sizeof(AetherHostCommand) == 40);
static_assert(sizeof(AetherHostEvent) == 280);
static_assert(sizeof(AetherGraphInputs) == 384);
static_assert(alignof(AetherGraphInputs) == 64);
static_assert(sizeof(AetherGraphOutputs) == 1792);
static_assert(alignof(AetherGraphOutputs) == 64);

static_assert(sizeof(AetherMorphRangeDesc) == 32);
static_assert(alignof(AetherMorphRangeDesc) == 8);
static_assert(sizeof(AetherSubmeshDesc) == 64);
static_assert(alignof(AetherSubmeshDesc) == 8);
static_assert(sizeof(AetherNprMaterial) == 192);
static_assert(alignof(AetherNprMaterial) == 8);
static_assert(sizeof(AetherJointDesc) == 128);
static_assert(alignof(AetherJointDesc) == 8);
static_assert(sizeof(AetherSpringJointDesc) == 80);
static_assert(alignof(AetherSpringJointDesc) == 8);
static_assert(sizeof(AetherColliderDesc) == 48);
static_assert(alignof(AetherColliderDesc) == 8);
static_assert(sizeof(AetherExpressionBindingDesc) == 24);
static_assert(alignof(AetherExpressionBindingDesc) == 8);
static_assert(sizeof(AetherTextureDesc) == 48);
static_assert(alignof(AetherTextureDesc) == 8);
static_assert(sizeof(AetherGraphPlanStats) == 2104);
static_assert(alignof(AetherGraphPlanStats) == 8);
static_assert(offsetof(AetherGraphPlanStats, eval_count) == 40);
static_assert(offsetof(AetherGraphPlanStats, node_eval_us) == 56);
static_assert(offsetof(AetherGraphPlanStats, node_live_values) == 1080);

namespace {

struct ExpectedFieldOffset {
    const char* name;
    size_t offset;
};

template <size_t N>
void verify_struct_in_json(const nlohmann::json& map, const char* struct_name, size_t cpp_size, size_t cpp_align,
                           const ExpectedFieldOffset (&fields)[N]) {
    REQUIRE(map.contains(struct_name));
    const auto& entry = map.at(struct_name);
    REQUIRE(entry.at("size").get<size_t>() == cpp_size);
    REQUIRE(entry.at("align").get<size_t>() == cpp_align);
    const auto& jfields = entry.at("fields");
    for (const auto& f : fields) {
        REQUIRE(jfields.contains(f.name));
        CHECK(jfields.at(f.name).get<size_t>() == f.offset);
    }
}

} // namespace

TEST_CASE("FFI layout table JSON matches C++ sizeof, alignof, and offsetof", "[ffi]") {
    uintptr_t json_len = 0;
    const uint8_t* raw = aether_ffi_layout_table_json(&json_len);
    REQUIRE(raw != nullptr);
    REQUIRE(json_len > 100);

    const std::string text(reinterpret_cast<const char*>(raw), json_len);
    const auto doc = nlohmann::json::parse(text);
    REQUIRE(doc.contains("structs"));

    nlohmann::json map = nlohmann::json::object();
    for (const auto& s : doc.at("structs")) {
        nlohmann::json fdict = nlohmann::json::object();
        for (const auto& f : s.at("fields")) {
            fdict[f.at("name").get<std::string>()] = f.at("offset").get<size_t>();
        }
        map[s.at("name").get<std::string>()] = {
            {"size", s.at("size").get<size_t>()},
            {"align", s.at("align").get<size_t>()},
            {"fields", std::move(fdict)},
        };
    }

    const ExpectedFieldOffset rt_fields[] = {
        {"struct_size", offsetof(AetherRuntimeConfig, struct_size)},
        {"log_level", offsetof(AetherRuntimeConfig, log_level)},
        {"worker_threads", offsetof(AetherRuntimeConfig, worker_threads)},
        {"flags", offsetof(AetherRuntimeConfig, flags)},
        {"app_data_dir_utf8", offsetof(AetherRuntimeConfig, app_data_dir_utf8)},
        {"cache_dir_utf8", offsetof(AetherRuntimeConfig, cache_dir_utf8)},
    };
    verify_struct_in_json(map, "AetherRuntimeConfig", sizeof(AetherRuntimeConfig), alignof(AetherRuntimeConfig),
                          rt_fields);

    const ExpectedFieldOffset v25_fields[] = {
        {"position", offsetof(AetherVertex25D, position)},
        {"outline_width_scale", offsetof(AetherVertex25D, outline_width_scale)},
        {"true_normal", offsetof(AetherVertex25D, true_normal)},
        {"z_flatten_weight", offsetof(AetherVertex25D, z_flatten_weight)},
        {"proxy_normal", offsetof(AetherVertex25D, proxy_normal)},
        {"sdf_face_mask", offsetof(AetherVertex25D, sdf_face_mask)},
        {"smooth_tangent", offsetof(AetherVertex25D, smooth_tangent)},
    };
    verify_struct_in_json(map, "AetherVertex25D", sizeof(AetherVertex25D), alignof(AetherVertex25D), v25_fields);

    const ExpectedFieldOffset tf_fields[] = {
        {"capture_timestamp_us", offsetof(AetherTrackingFrame, capture_timestamp_us)},
        {"inference_complete_us", offsetof(AetherTrackingFrame, inference_complete_us)},
        {"active_execution_provider", offsetof(AetherTrackingFrame, active_execution_provider)},
        {"upper_body_confidence", offsetof(AetherTrackingFrame, upper_body_confidence)},
        {"arkit_blendshapes", offsetof(AetherTrackingFrame, arkit_blendshapes)},
        {"joint_positions_ws", offsetof(AetherTrackingFrame, joint_positions_ws)},
        {"joint_rotations_quat", offsetof(AetherTrackingFrame, joint_rotations_quat)},
        {"frame_sequence", offsetof(AetherTrackingFrame, frame_sequence)},
        {"valid_flags", offsetof(AetherTrackingFrame, valid_flags)},
    };
    verify_struct_in_json(map, "AetherTrackingFrame", sizeof(AetherTrackingFrame), alignof(AetherTrackingFrame),
                          tf_fields);

    const ExpectedFieldOffset dsp_fields[] = {
        {"magic_header", offsetof(AetherDspSyncPacket, magic_header)},
        {"sequence_number", offsetof(AetherDspSyncPacket, sequence_number)},
        {"total_video_latency_ms", offsetof(AetherDspSyncPacket, total_video_latency_ms)},
        {"dsp_sequence_number", offsetof(AetherDspSyncPacket, dsp_sequence_number)},
        {"dsp_input_rms_db", offsetof(AetherDspSyncPacket, dsp_input_rms_db)},
        {"dsp_viseme_weights", offsetof(AetherDspSyncPacket, dsp_viseme_weights)},
        {"dsp_flags", offsetof(AetherDspSyncPacket, dsp_flags)},
    };
    verify_struct_in_json(map, "AetherDspSyncPacket", sizeof(AetherDspSyncPacket), alignof(AetherDspSyncPacket),
                          dsp_fields);

    const ExpectedFieldOffset gin_fields[] = {
        {"tracking", offsetof(AetherGraphInputs, tracking)},
        {"now_us", offsetof(AetherGraphInputs, now_us)},
        {"dt_s", offsetof(AetherGraphInputs, dt_s)},
        {"tick_index", offsetof(AetherGraphInputs, tick_index)},
        {"dsp_visemes", offsetof(AetherGraphInputs, dsp_visemes)},
        {"dsp_rms_db", offsetof(AetherGraphInputs, dsp_rms_db)},
        {"external_params", offsetof(AetherGraphInputs, external_params)},
    };
    verify_struct_in_json(map, "AetherGraphInputs", sizeof(AetherGraphInputs), alignof(AetherGraphInputs), gin_fields);

    const ExpectedFieldOffset gout_fields[] = {
        {"expression_weights", offsetof(AetherGraphOutputs, expression_weights)},
        {"expression_override", offsetof(AetherGraphOutputs, expression_override)},
        {"ik_targets", offsetof(AetherGraphOutputs, ik_targets)},
        {"bone_rotation_offsets", offsetof(AetherGraphOutputs, bone_rotation_offsets)},
        {"look_at_ws", offsetof(AetherGraphOutputs, look_at_ws)},
        {"events", offsetof(AetherGraphOutputs, events)},
        {"event_count", offsetof(AetherGraphOutputs, event_count)},
    };
    verify_struct_in_json(map, "AetherGraphOutputs", sizeof(AetherGraphOutputs), alignof(AetherGraphOutputs),
                          gout_fields);

    const ExpectedFieldOffset morph_range_fields[] = {
        {"delta_offset", offsetof(AetherMorphRangeDesc, delta_offset)},
        {"delta_count", offsetof(AetherMorphRangeDesc, delta_count)},
        {"name_offset", offsetof(AetherMorphRangeDesc, name_offset)},
        {"name_len", offsetof(AetherMorphRangeDesc, name_len)},
        {"centroid_x", offsetof(AetherMorphRangeDesc, centroid_x)},
        {"is_face_morph", offsetof(AetherMorphRangeDesc, is_face_morph)},
    };
    verify_struct_in_json(map, "AetherMorphRangeDesc", sizeof(AetherMorphRangeDesc), alignof(AetherMorphRangeDesc),
                          morph_range_fields);

    const ExpectedFieldOffset submesh_fields[] = {
        {"index_offset", offsetof(AetherSubmeshDesc, index_offset)},
        {"index_count", offsetof(AetherSubmeshDesc, index_count)},
        {"vertex_offset", offsetof(AetherSubmeshDesc, vertex_offset)},
        {"vertex_count", offsetof(AetherSubmeshDesc, vertex_count)},
        {"material_index", offsetof(AetherSubmeshDesc, material_index)},
        {"flags", offsetof(AetherSubmeshDesc, flags)},
        {"aabb_min", offsetof(AetherSubmeshDesc, aabb_min)},
        {"aabb_max", offsetof(AetherSubmeshDesc, aabb_max)},
    };
    verify_struct_in_json(map, "AetherSubmeshDesc", sizeof(AetherSubmeshDesc), alignof(AetherSubmeshDesc),
                          submesh_fields);

    const ExpectedFieldOffset mat_fields[] = {
        {"base_color", offsetof(AetherNprMaterial, base_color)},
        {"shade_color_gi", offsetof(AetherNprMaterial, shade_color_gi)},
        {"toon_params", offsetof(AetherNprMaterial, toon_params)},
        {"outline_width", offsetof(AetherNprMaterial, outline_width)},
        {"tex_base", offsetof(AetherNprMaterial, tex_base)},
        {"tex_sdf_face", offsetof(AetherNprMaterial, tex_sdf_face)},
        {"name_offset", offsetof(AetherNprMaterial, name_offset)},
    };
    verify_struct_in_json(map, "AetherNprMaterial", sizeof(AetherNprMaterial), alignof(AetherNprMaterial), mat_fields);

    const ExpectedFieldOffset joint_fields[] = {
        {"parent_index", offsetof(AetherJointDesc, parent_index)},
        {"humanoid_bone", offsetof(AetherJointDesc, humanoid_bone)},
        {"region_tag", offsetof(AetherJointDesc, region_tag)},
        {"bind_translation", offsetof(AetherJointDesc, bind_translation)},
        {"tail_length", offsetof(AetherJointDesc, tail_length)},
        {"rest_to_tpose_quat", offsetof(AetherJointDesc, rest_to_tpose_quat)},
        {"inverse_bind_matrix_row_major", offsetof(AetherJointDesc, inverse_bind_matrix_row_major)},
    };
    verify_struct_in_json(map, "AetherJointDesc", sizeof(AetherJointDesc), alignof(AetherJointDesc), joint_fields);

    const ExpectedFieldOffset spring_fields[] = {
        {"collider_group_mask", offsetof(AetherSpringJointDesc, collider_group_mask)},
        {"rest_dir", offsetof(AetherSpringJointDesc, rest_dir)},
        {"bone_length", offsetof(AetherSpringJointDesc, bone_length)},
        {"joint_index", offsetof(AetherSpringJointDesc, joint_index)},
        {"stiffness", offsetof(AetherSpringJointDesc, stiffness)},
        {"gravity_power", offsetof(AetherSpringJointDesc, gravity_power)},
    };
    verify_struct_in_json(map, "AetherSpringJointDesc", sizeof(AetherSpringJointDesc), alignof(AetherSpringJointDesc),
                          spring_fields);

    const ExpectedFieldOffset collider_fields[] = {
        {"joint_index", offsetof(AetherColliderDesc, joint_index)},
        {"group_bit", offsetof(AetherColliderDesc, group_bit)},
        {"shape_kind", offsetof(AetherColliderDesc, shape_kind)},
        {"radius", offsetof(AetherColliderDesc, radius)},
        {"offset", offsetof(AetherColliderDesc, offset)},
        {"tail", offsetof(AetherColliderDesc, tail)},
    };
    verify_struct_in_json(map, "AetherColliderDesc", sizeof(AetherColliderDesc), alignof(AetherColliderDesc),
                          collider_fields);

    const ExpectedFieldOffset expr_fields[] = {
        {"preset_index", offsetof(AetherExpressionBindingDesc, preset_index)},
        {"input_slot", offsetof(AetherExpressionBindingDesc, input_slot)},
        {"morph_index", offsetof(AetherExpressionBindingDesc, morph_index)},
        {"weight", offsetof(AetherExpressionBindingDesc, weight)},
        {"name_offset", offsetof(AetherExpressionBindingDesc, name_offset)},
        {"name_len", offsetof(AetherExpressionBindingDesc, name_len)},
    };
    verify_struct_in_json(map, "AetherExpressionBindingDesc", sizeof(AetherExpressionBindingDesc),
                          alignof(AetherExpressionBindingDesc), expr_fields);

    const ExpectedFieldOffset tex_fields[] = {
        {"pixel_offset", offsetof(AetherTextureDesc, pixel_offset)},
        {"pixel_size", offsetof(AetherTextureDesc, pixel_size)},
        {"width", offsetof(AetherTextureDesc, width)},
        {"height", offsetof(AetherTextureDesc, height)},
        {"format", offsetof(AetherTextureDesc, format)},
        {"usage_flags", offsetof(AetherTextureDesc, usage_flags)},
        {"name_offset", offsetof(AetherTextureDesc, name_offset)},
        {"name_len", offsetof(AetherTextureDesc, name_len)},
        {"content_hash_lo", offsetof(AetherTextureDesc, content_hash_lo)},
    };
    verify_struct_in_json(map, "AetherTextureDesc", sizeof(AetherTextureDesc), alignof(AetherTextureDesc), tex_fields);

    const ExpectedFieldOffset stats_fields[] = {
        {"struct_size", offsetof(AetherGraphPlanStats, struct_size)},
        {"active_node_count", offsetof(AetherGraphPlanStats, active_node_count)},
        {"error_node_id", offsetof(AetherGraphPlanStats, error_node_id)},
        {"compile_count", offsetof(AetherGraphPlanStats, compile_count)},
        {"last_compile_ms", offsetof(AetherGraphPlanStats, last_compile_ms)},
        {"last_eval_us", offsetof(AetherGraphPlanStats, last_eval_us)},
        {"p95_eval_us", offsetof(AetherGraphPlanStats, p95_eval_us)},
        {"budget_warn", offsetof(AetherGraphPlanStats, budget_warn)},
        {"ws_enabled", offsetof(AetherGraphPlanStats, ws_enabled)},
        {"osc_packets_rx", offsetof(AetherGraphPlanStats, osc_packets_rx)},
        {"eval_count", offsetof(AetherGraphPlanStats, eval_count)},
        {"osc_rate_limited", offsetof(AetherGraphPlanStats, osc_rate_limited)},
        {"plugin_count", offsetof(AetherGraphPlanStats, plugin_count)},
        {"node_eval_us", offsetof(AetherGraphPlanStats, node_eval_us)},
        {"node_live_values", offsetof(AetherGraphPlanStats, node_live_values)},
    };
    verify_struct_in_json(map, "AetherGraphPlanStats", sizeof(AetherGraphPlanStats), alignof(AetherGraphPlanStats),
                          stats_fields);
}

TEST_CASE("FFI boundary handles stubs, thread roles, and Rust panics cleanly", "[ffi]") {
    REQUIRE(aether_ffi_version() == ((AETHER_FFI_VERSION_MAJOR << 16) | AETHER_FFI_VERSION_MINOR));

    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);

    AetherHostConfig hcfg{};
    hcfg.struct_size = static_cast<uint32_t>(sizeof(hcfg));
    hcfg.worker_threads = 2;
    hcfg.osc_in_port = 0; // ephemeral localhost port for unit tests
    hcfg.osc_out_port = 0;
    hcfg.ws_port = 0;
    AetherHostHandle host = nullptr;
    REQUIRE(aether_host_create(&hcfg, &host) == AETHER_OK);
    REQUIRE(host != nullptr);

    aether_set_thread_role(AETHER_THREAD_ROLE_UI);
    AetherGraphPlanStats stats{};
    stats.struct_size = static_cast<uint32_t>(sizeof(stats));
    REQUIRE(aether_host_get_plan_stats(host, &stats) == AETHER_OK);
    REQUIRE(stats.active_node_count == 6);

    aether_set_thread_role(AETHER_THREAD_ROLE_T2);
    AetherGraphInputs gin{};
    AetherGraphOutputs gout{};
    gin.dt_s = 1.0f / 60.0f;
    REQUIRE(aether_host_evaluate_active(host, &gin, &gout) == AETHER_OK);

    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    aether_host_destroy(host);

    char err_buf[256]{};

    aether_set_thread_role(AETHER_THREAD_ROLE_LOADER);
    AetherLoadRequest req{};
    req.path_utf8 = "avatar.vrm";
    AetherLoaderHandle loader = nullptr;
    REQUIRE(aether_loader_open(&req, &loader) == AETHER_E_IO);
    REQUIRE(loader == nullptr);

#if !defined(NDEBUG)
    aether_set_thread_role(AETHER_THREAD_ROLE_UI);
    REQUIRE(aether_loader_open(&req, &loader) == AETHER_E_INTERNAL);
#endif

#ifdef AETHER_ENABLE_TEST_HOOKS
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
    REQUIRE(aether_test_inject_panic() == AETHER_E_PANIC);
    const size_t panic_len = aether_last_error(err_buf, sizeof(err_buf));
    REQUIRE(panic_len > 0);
#endif

    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    aether_runtime_shutdown();
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
}
