#include "aether_ffi_contract.h"
#include "animation/pmx_physics.hpp"
#include "core/hot_path_guard.hpp"
#include "renderer/frame_globals_builder.hpp"
#include "renderer/material_override_cache.hpp"
#include "ui/panels/nodegraph/node_canvas.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void push_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

void push_i8(std::vector<uint8_t>& buf, int8_t v) {
    buf.push_back(static_cast<uint8_t>(v));
}

void push_u16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
}

void push_i32(std::vector<uint8_t>& buf, int32_t v) {
    uint32_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    buf.push_back(static_cast<uint8_t>(u & 0xFFu));
    buf.push_back(static_cast<uint8_t>((u >> 8u) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((u >> 16u) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((u >> 24u) & 0xFFu));
}

void push_f32(std::vector<uint8_t>& buf, float v) {
    uint32_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    buf.push_back(static_cast<uint8_t>(u & 0xFFu));
    buf.push_back(static_cast<uint8_t>((u >> 8u) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((u >> 16u) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((u >> 24u) & 0xFFu));
}

void push_f32_3(std::vector<uint8_t>& buf, float x, float y, float z) {
    push_f32(buf, x);
    push_f32(buf, y);
    push_f32(buf, z);
}

void push_f32_4(std::vector<uint8_t>& buf, float x, float y, float z, float w) {
    push_f32(buf, x);
    push_f32(buf, y);
    push_f32(buf, z);
    push_f32(buf, w);
}

void push_utf8_text(std::vector<uint8_t>& buf, std::string_view s) {
    push_i32(buf, static_cast<int32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

// Synthesizes a complete PMX 2.1 binary containing SDEF vertices, edge_scale=0 jawline/eye vertices,
// Ray-MMD materials (.spa/.sph), Japanese humanoid hierarchy + Append Transform + CCD-IK,
// Group/Vertex/Bone/UV/Material morphs, and RigidBody + 6-DOF Spring Joint physics.
std::vector<uint8_t> build_synthetic_pmx_21_binary() {
    std::vector<uint8_t> b;
    b.reserve(4096);

    // Header: "PMX ", version 2.1, globals_count = 8
    b.insert(b.end(), {'P', 'M', 'X', ' '});
    push_f32(b, 2.1f);
    push_u8(b, 8);
    push_u8(b, 1); // encoding = UTF-8
    push_u8(b, 0); // additional_uv = 0
    push_u8(b, 1); // vertex_index_size = 1 (u8)
    push_u8(b, 1); // texture_index_size = 1 (i8)
    push_u8(b, 1); // material_index_size = 1 (i8)
    push_u8(b, 1); // bone_index_size = 1 (i8)
    push_u8(b, 1); // morph_index_size = 1 (i8)
    push_u8(b, 1); // rigid_body_index_size = 1 (i8)

    push_utf8_text(b, "AetherTestPmx");
    push_utf8_text(b, "AetherTestPmxEn");
    push_utf8_text(b, "Phase 3 PMX 2.1 Test");
    push_utf8_text(b, "Phase 3 PMX 2.1 Test En");

    // 6 Vertices (3 for Face_Skin triangle with edge_scale=0.0 and SDEF on v0; 3 for Hair_Mat triangle with edge_scale=1.0)
    push_i32(b, 6);
    // v0: SDEF (deform_type = 3), edge_scale = 0.0 (jawline)
    push_f32_3(b, -1.0f, 15.0f, 2.5f); // LH pos
    push_f32_3(b, 0.0f, 0.0f, -1.0f);  // LH normal
    push_f32(b, 0.25f);
    push_f32(b, 0.75f);
    push_u8(b, 3); // SDEF
    push_i8(b, 4); // bone 0 = 首 (Neck)
    push_i8(b, 5); // bone 1 = 頭 (Head)
    push_f32(b, 0.5f);
    push_f32_3(b, 0.0f, 15.0f, 2.5f);  // C
    push_f32_3(b, 0.0f, 14.8f, 2.5f);  // R0
    push_f32_3(b, 0.0f, 15.2f, 2.5f);  // R1
    push_f32(b, 0.0f);                 // edge_scale = 0.0 (suppress ink!)

    // v1: BDEF1, edge_scale = 0.0
    push_f32_3(b, 1.0f, 15.0f, 2.5f);
    push_f32_3(b, 0.0f, 0.0f, -1.0f);
    push_f32(b, 0.75f);
    push_f32(b, 0.75f);
    push_u8(b, 0); // BDEF1
    push_i8(b, 5);
    push_f32(b, 0.0f); // edge_scale = 0.0

    // v2: BDEF1, edge_scale = 0.0
    push_f32_3(b, 0.0f, 17.0f, 2.5f);
    push_f32_3(b, 0.0f, 0.0f, -1.0f);
    push_f32(b, 0.50f);
    push_f32(b, 0.25f);
    push_u8(b, 0);
    push_i8(b, 5);
    push_f32(b, 0.0f);

    // v3..v5: Hair_Mat triangle with edge_scale = 1.0
    for (int k = 0; k < 3; ++k) {
        push_f32_3(b, static_cast<float>(k) - 1.0f, 18.0f + static_cast<float>(k), 1.0f);
        push_f32_3(b, 0.0f, 0.0f, -1.0f);
        push_f32(b, 0.1f * static_cast<float>(k));
        push_f32(b, 0.2f * static_cast<float>(k));
        push_u8(b, 0);
        push_i8(b, 8);     // hair physics bone
        push_f32(b, 1.0f); // edge_scale = 1.0
    }

    // 6 Indices (2 triangles: (0, 1, 2) and (3, 4, 5))
    push_i32(b, 6);
    push_u8(b, 0);
    push_u8(b, 1);
    push_u8(b, 2);
    push_u8(b, 3);
    push_u8(b, 4);
    push_u8(b, 5);

    // 2 Textures: "face.png", "hair_s.spa"
    push_i32(b, 2);
    push_utf8_text(b, "face.png");
    push_utf8_text(b, "hair_s.spa");

    // 2 Materials: "Face_Skin" (3 indices), "Hair_Mat" (3 indices, sphere_mode=2 .spa)
    push_i32(b, 2);
    auto write_mat = [&](std::string_view name, int8_t tex, int8_t sph, uint8_t sph_mode, int32_t idx_cnt) {
        push_utf8_text(b, name);
        push_utf8_text(b, name);
        push_f32_4(b, 1.0f, 0.92f, 0.88f, 1.0f); // diffuse
        push_f32_3(b, 0.2f, 0.2f, 0.2f);         // specular
        push_f32(b, 16.0f);                      // spec power
        push_f32_3(b, 0.5f, 0.5f, 0.5f);         // ambient
        push_u8(b, 0x1Fu);                       // flags (double_sided | ground_shadow | self_shadow | edge)
        push_f32_4(b, 0.1f, 0.08f, 0.08f, 1.0f); // edge color
        push_f32(b, 1.0f);                       // edge size
        push_i8(b, tex);
        push_i8(b, sph);
        push_u8(b, sph_mode);
        push_u8(b, 1); // shared toon
        push_u8(b, 0); // toon index 0
        push_utf8_text(b, "");
        push_i32(b, idx_cnt);
    };
    write_mat("Face_Skin", 0, -1, 0, 3);
    write_mat("Hair_Mat", 0, 1, 2, 3);

    // 10 Bones:
    // 0: 全ての親 (-1)
    // 1: センター (0)
    // 2: 下半身 (1)
    // 3: 上半身 (1)
    // 4: 首 (3)
    // 5: 頭 (4)
    // 6: 左腕 (3)
    // 7: 左ひじ (6)
    // 8: 前髪_物理 (5) with AppendRotate from 5 (ratio 0.5)
    // 9: 左腕ＩＫ (0) targeting 7 (link 6)
    push_i32(b, 10);
    auto write_bone = [&](std::string_view jp, int8_t parent, float x, float y, float z, uint16_t flags,
                          int8_t append_parent = -1, float append_ratio = 0.0f, int8_t ik_target = -1,
                          int8_t ik_link_bone = -1) {
        push_utf8_text(b, jp);
        push_utf8_text(b, jp);
        push_f32_3(b, x, y, z);
        push_i8(b, parent);
        push_i32(b, 0); // deform layer
        push_u16(b, flags);
        push_f32_3(b, 0.0f, 0.5f, 0.0f); // tail offset
        if ((flags & (0x0100u | 0x0200u)) != 0u) {
            push_i8(b, append_parent);
            push_f32(b, append_ratio);
        }
        if ((flags & 0x0020u) != 0u) {
            push_i8(b, ik_target);
            push_i32(b, 8);     // ik_loops
            push_f32(b, 0.5f);  // ik_limit_rad
            push_i32(b, 1);     // 1 ik link
            push_i8(b, ik_link_bone);
            push_u8(b, 1); // has_limit
            push_f32_3(b, -1.5f, -1.5f, -1.5f);
            push_f32_3(b, 1.5f, 1.5f, 1.5f);
        }
    };
    write_bone("全ての親", -1, 0.0f, 0.0f, 0.0f, 0x0006u);
    write_bone("センター", 0, 0.0f, 8.0f, 0.0f, 0x0006u);
    write_bone("下半身", 1, 0.0f, 9.5f, 0.0f, 0x0002u);
    write_bone("上半身", 1, 0.0f, 10.0f, 0.0f, 0x0002u);
    write_bone("首", 3, 0.0f, 14.5f, 0.0f, 0x0002u);
    write_bone("頭", 4, 0.0f, 15.5f, 0.0f, 0x0002u);
    write_bone("左腕", 3, 1.5f, 14.0f, 0.0f, 0x0002u);
    write_bone("左ひじ", 6, 3.5f, 14.0f, 0.0f, 0x0002u);
    write_bone("前髪_物理", 5, 0.0f, 17.5f, -1.0f, 0x0102u, 5, 0.5f);
    write_bone("左腕ＩＫ", 0, 3.2f, 14.5f, -0.5f, 0x0026u, -1, 0.0f, 7, 6);

    // 5 Morphs:
    // 0: Vertex morph ("あ")
    // 1: UV morph ("UV_Shift")
    // 2: Bone morph ("Bone_Shift")
    // 3: Material morph ("Mat_Tint", 112-byte payload!)
    // 4: Group morph ("Group_All") referencing morphs 0..3
    push_i32(b, 5);
    // Morph 0: Vertex
    push_utf8_text(b, "あ");
    push_utf8_text(b, "A");
    push_u8(b, 3); // Mouth panel
    push_u8(b, 1); // Vertex morph
    push_i32(b, 1);
    push_u8(b, 0); // vertex 0
    push_f32_3(b, 0.0f, -0.5f, 0.25f);

    // Morph 1: UV
    push_utf8_text(b, "UV_Shift");
    push_utf8_text(b, "UV_Shift");
    push_u8(b, 4);
    push_u8(b, 3); // UV morph
    push_i32(b, 1);
    push_u8(b, 0); // vertex 0
    push_f32_4(b, 0.125f, -0.25f, 0.0f, 0.0f);

    // Morph 2: Bone
    push_utf8_text(b, "Bone_Shift");
    push_utf8_text(b, "Bone_Shift");
    push_u8(b, 4);
    push_u8(b, 2); // Bone morph
    push_i32(b, 1);
    push_i8(b, 5); // Head bone
    push_f32_3(b, 0.0f, 0.25f, -0.5f);
    push_f32_4(b, 0.0f, 0.0f, 0.0f, 1.0f);

    // Morph 3: Material (exact 112 bytes after mat_idx + calc_mode!)
    push_utf8_text(b, "Mat_Tint");
    push_utf8_text(b, "Mat_Tint");
    push_u8(b, 4);
    push_u8(b, 8); // Material morph
    push_i32(b, 1);
    push_i8(b, 0);                   // material 0
    push_u8(b, 1);                   // additive mode
    push_f32_4(b, 0.1f, 0.2f, 0.3f, 0.0f); // diffuse (16 B)
    push_f32_3(b, 0.0f, 0.0f, 0.0f);       // specular (12 B)
    push_f32(b, 0.0f);                     // spec_power (4 B)
    push_f32_3(b, 0.05f, 0.05f, 0.05f);    // ambient (12 B)
    push_f32_4(b, 0.0f, 0.0f, 0.0f, 0.0f); // edge_color (16 B)
    push_f32(b, 0.0f);                     // edge_size (4 B)
    push_f32_4(b, 0.0f, 0.0f, 0.0f, 0.0f); // tex_tint (16 B)
    push_f32_4(b, 0.0f, 0.0f, 0.0f, 0.0f); // sphere_tint (16 B)
    push_f32_4(b, 0.15f, 0.0f, 0.0f, 0.0f); // toon_tint (16 B)

    // Morph 4: Group
    push_utf8_text(b, "Group_All");
    push_utf8_text(b, "Group_All");
    push_u8(b, 4);
    push_u8(b, 0); // Group morph
    push_i32(b, 2);
    push_i8(b, 1);
    push_f32(b, 0.5f);
    push_i8(b, 3);
    push_f32(b, 1.0f);

    // Display frames: 0
    push_i32(b, 0);

    // 2 Rigid Bodies (0 = Kinematic Head anchor on bone 5, 1 = Dynamic Hair tail on bone 8)
    push_i32(b, 2);
    auto write_rb = [&](std::string_view name, int8_t bone, uint8_t group, uint16_t mask, uint8_t shape, float y,
                        float mass, uint8_t mode) {
        push_utf8_text(b, name);
        push_utf8_text(b, name);
        push_i8(b, bone);
        push_u8(b, group);
        push_u16(b, mask);
        push_u8(b, shape);
        push_f32_3(b, 0.5f, 0.5f, 0.5f); // shape size
        push_f32_3(b, 0.0f, y, -1.0f);   // position
        push_f32_3(b, 0.0f, 0.0f, 0.0f); // rotation
        push_f32(b, mass);
        push_f32(b, 0.5f); // lin_damp
        push_f32(b, 0.5f); // ang_damp
        push_f32(b, 0.1f); // restitution
        push_f32(b, 0.5f); // friction
        push_u8(b, mode);
    };
    write_rb("Head_Anchor", 5, 0, 0xFFFFu, AETHER_PMX_SHAPE_SPHERE, 15.5f, 1.0f, AETHER_PMX_RB_KINEMATIC);
    write_rb("Hair_Tail", 8, 1, 0xFFFFu, AETHER_PMX_SHAPE_CAPSULE, 16.5f, 0.5f, AETHER_PMX_RB_DYNAMIC);

    // 1 6-DOF Spring Joint connecting body 0 and body 1
    push_i32(b, 1);
    push_utf8_text(b, "Hair_Joint");
    push_utf8_text(b, "Hair_Joint");
    push_u8(b, 0); // 6-DOF spring joint
    push_i8(b, 0);
    push_i8(b, 1);
    push_f32_3(b, 0.0f, 16.0f, -1.0f);
    push_f32_3(b, 0.0f, 0.0f, 0.0f);
    push_f32_3(b, -0.05f, -0.05f, -0.05f);
    push_f32_3(b, 0.05f, 0.05f, 0.05f);
    push_f32_3(b, -0.45f, -0.45f, -0.45f);
    push_f32_3(b, 0.45f, 0.45f, 0.45f);
    push_f32_3(b, 20.0f, 20.0f, 20.0f);
    push_f32_3(b, 12.0f, 12.0f, 12.0f);

    return b;
}

}  // namespace

TEST_CASE("Phase 3: Non-destructive Node Material Override, TextureGrade, and DynamicMaterialDriver zero-alloc",
          "[phase3][materials]") {
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);

    std::vector<AetherNprMaterial> base_mats(2);
    std::string pool = std::string("Face_Skin\0Hair_Mat\0", 19);
    base_mats[0].name_offset = 0;
    base_mats[0].base_color[0] = 1.0f;
    base_mats[0].base_color[1] = 0.9f;
    base_mats[0].base_color[2] = 0.85f;
    base_mats[0].base_color[3] = 1.0f;
    base_mats[0].shade_color_gi[0] = 0.85f;
    base_mats[0].shade_color_gi[1] = 0.72f;
    base_mats[0].shade_color_gi[2] = 0.68f;
    base_mats[0].toon_params[0] = -0.05f;
    base_mats[0].outline_width = 1.0f;
    base_mats[0].flags = AETHER_MAT_FLAG_HAS_PROFILE | (AETHER_MAT_PROFILE_SKIN_SDF << AETHER_MAT_FLAG_PROFILE_SHIFT);

    base_mats[1].name_offset = 10;
    base_mats[1].base_color[0] = 0.25f;
    base_mats[1].base_color[1] = 0.30f;
    base_mats[1].base_color[2] = 0.45f;
    base_mats[1].base_color[3] = 1.0f;
    base_mats[1].shade_color_gi[0] = 0.15f;
    base_mats[1].shade_color_gi[1] = 0.18f;
    base_mats[1].shade_color_gi[2] = 0.30f;
    base_mats[1].toon_params[0] = -0.02f;
    base_mats[1].outline_width = 1.2f;
    base_mats[1].flags = AETHER_MAT_FLAG_RECEIVES_HAIR_SHADOW;

    AetherHostConfig hcfg{};
    hcfg.struct_size = sizeof(hcfg);
    AetherHostHandle host = nullptr;
    REQUIRE(aether_host_create(&hcfg, &host) == AETHER_OK);
    REQUIRE(host != nullptr);

    aether_set_thread_role(AETHER_THREAD_ROLE_UI);
    REQUIRE(aether_host_set_model_metadata_json(
                host, R"({"format":1,"materials":["Face_Skin","Hair_Mat"]})") == AETHER_OK);

    const char* graph_json = R"({
        "schema_version": 1,
        "nodes": [
            { "id": 1, "type": "aether.model.source@1", "params": {} },
            { "id": 2, "type": "aether.texture.asset@1", "params": { "path": "custom_hair.png", "color_space": "srgb", "target_slot": "albedo" } },
            { "id": 3, "type": "aether.texture.grade@1", "params": { "uv_scale_x": 2.0, "uv_scale_y": 1.5, "hue_shift": 25.0, "saturation": 1.25, "brightness": 0.05, "contrast": 1.15 } },
            { "id": 4, "type": "aether.material.dynamic_driver@1", "params": { "signal_source": "audio_rms", "target_param": "emissive_intensity", "gain": 2.5, "bias": 0.1, "min": 0.0, "max": 4.0 } },
            { "id": 5, "type": "aether.material.slot_override@1", "params": { "slot_name": "Hair_Mat", "shader_profile": "Dual-Lobe Hair", "shade_color": [0.10, 0.12, 0.28, 1.0], "shade_shift": -0.18, "sss_fringe": 0.75, "outline_scale": 1.5, "rt_shadow_mask": 6 } }
        ],
        "edges": [
            { "id": 1, "from_node": 1, "from_port": "material_table", "to_node": 5, "to_port": "material_table" },
            { "id": 2, "from_node": 2, "from_port": "texture", "to_node": 3, "to_port": "texture" },
            { "id": 3, "from_node": 3, "from_port": "texture", "to_node": 5, "to_port": "texture" },
            { "id": 4, "from_node": 4, "from_port": "driver_out", "to_node": 5, "to_port": "dynamic_driver" }
        ]
    })";

    AetherHostCommand cmd{};
    cmd.struct_size = sizeof(cmd);
    cmd.kind = AETHER_HOST_CMD_SET_GRAPH_JSON;
    cmd.payload_len = static_cast<uint64_t>(std::strlen(graph_json));
    cmd.payload_utf8 = graph_json;
    REQUIRE(aether_host_post_command(host, &cmd) == AETHER_OK);

    bool compiled = false;
    for (int attempt = 0; attempt < 40 && !compiled; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        AetherHostEvent evs[8]{};
        uint32_t ev_count = 0;
        (void)aether_host_poll_events(host, evs, 8, &ev_count);
        for (uint32_t i = 0; i < ev_count; ++i) {
            if (evs[i].kind == AETHER_HOST_EVT_GRAPH_COMPILED) {
                compiled = true;
            }
        }
    }
    REQUIRE(compiled);

    AetherTrackingFrame tf{};
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        tf.joint_rotations_quat[b][3] = 1.0f;
    }
    AetherGraphInputs gin{};
    gin.tracking = &tf;
    gin.now_us = 16667;
    gin.dt_s = 1.0f / 60.0f;
    gin.tick_index = 1;
    gin.dsp_rms_db = -6.0f;

    AetherGraphOutputs gout{};
    AetherMaterialOverrideBuffer mat_ovr{};
    mat_ovr.struct_size = sizeof(mat_ovr);

    std::vector<AetherNprMaterial> active_mats(base_mats.size());
    std::vector<aether::renderer::GpuMaterialGrade> active_grades(base_mats.size());

    aether_set_thread_role(AETHER_THREAD_ROLE_T2);
    {
        AETHER_HOT_PATH_SCOPE;
        REQUIRE(aether_host_evaluate_active_ex(host, &gin, 12.0f, &gout, &mat_ovr) == AETHER_OK);
        aether::renderer::apply_material_overrides_non_destructive(
            base_mats, pool, mat_ovr, active_mats, active_grades);
    }
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);

    REQUIRE(mat_ovr.count == 1U);
    CHECK(mat_ovr.entries[0].material_index == 1U);
    CHECK(mat_ovr.entries[0].shader_profile == AETHER_MAT_PROFILE_DUAL_LOBE_HAIR);
    CHECK_THAT(active_mats[1].shade_color_gi[0], Catch::Matchers::WithinAbs(0.10f, 1e-4f));
    CHECK_THAT(active_mats[1].toon_params[0], Catch::Matchers::WithinAbs(-0.18f, 1e-4f));
    CHECK_THAT(active_mats[1].outline_width, Catch::Matchers::WithinAbs(1.2f * 1.5f, 1e-4f));
    CHECK(active_grades[1].dynamic_params[0] > 0.5f); // audio_rms driven emissive intensity
    CHECK_THAT(active_grades[1].uv_scale_offset[0], Catch::Matchers::WithinAbs(2.0f, 1e-4f));
    CHECK_THAT(active_grades[1].grade_hsbc[0], Catch::Matchers::WithinAbs(25.0f, 1e-4f));
    CHECK_THAT(active_grades[1].grade_hsbc[1], Catch::Matchers::WithinAbs(1.25f, 1e-4f));

    // Base materials remain 100% unmodified
    CHECK_THAT(base_mats[1].shade_color_gi[0], Catch::Matchers::WithinAbs(0.15f, 1e-5f));
    CHECK_THAT(base_mats[1].outline_width, Catch::Matchers::WithinAbs(1.2f, 1e-5f));

    // Disconnecting override immediately restores original model state
    AetherMaterialOverrideBuffer empty_ovr{};
    empty_ovr.struct_size = sizeof(empty_ovr);
    empty_ovr.count = 0U;
    aether::renderer::apply_material_overrides_non_destructive(
        base_mats, pool, empty_ovr, active_mats, active_grades);
    CHECK_THAT(active_mats[1].shade_color_gi[0], Catch::Matchers::WithinAbs(base_mats[1].shade_color_gi[0], 1e-5f));
    CHECK_THAT(active_mats[1].outline_width, Catch::Matchers::WithinAbs(base_mats[1].outline_width, 1e-5f));
    CHECK_THAT(active_grades[1].dynamic_params[3], Catch::Matchers::WithinAbs(0.0f, 1e-5f));

    aether_host_destroy(host);
}

TEST_CASE("Phase 3: Native PMX 2.1 Loader, Ray-MMD Classification, SDEF Skinning, CCD-IK, Append & 60 Hz Physics",
          "[phase3][pmx]") {
    aether_set_thread_role(AETHER_THREAD_ROLE_LOADER);

    const std::vector<uint8_t> pmx_bytes = build_synthetic_pmx_21_binary();
    const std::filesystem::path tmp_pmx =
        std::filesystem::temp_directory_path() / "aether_phase3_synthetic_test.pmx";
    {
        std::ofstream ofs(tmp_pmx, std::ios::binary | std::ios::trunc);
        REQUIRE(ofs.is_open());
        ofs.write(reinterpret_cast<const char*>(pmx_bytes.data()), static_cast<std::streamsize>(pmx_bytes.size()));
    }

    const std::string path_utf8 = tmp_pmx.string();
    AetherLoadRequest req{};
    req.path_utf8 = path_utf8.c_str();
    req.flags = 0;
    req.max_threads = 1;

    AetherLoaderHandle loader = nullptr;
    REQUIRE(aether_loader_open(&req, &loader) == AETHER_OK);
    REQUIRE(loader != nullptr);

    AetherAssetRequirements reqs{};
    REQUIRE(aether_loader_query(loader, &reqs) == AETHER_OK);
    CHECK(reqs.vertex_count == 6U);
    CHECK(reqs.index_count == 6U);
    CHECK(reqs.material_count == 2U);
    CHECK(reqs.joint_count == 10U);

    // Allocate aligned buffers for zero-copy fill
    std::array<std::vector<uint8_t>, AETHER_STREAM_COUNT> arenas{};
    std::array<AetherArenaSlice, AETHER_STREAM_COUNT> slices{};
    for (uint32_t s = 0; s < AETHER_STREAM_COUNT; ++s) {
        slices[s].stream_kind = s;
        slices[s].size_bytes = reqs.stream_bytes[s];
        if (reqs.stream_bytes[s] > 0) {
            arenas[s].resize(static_cast<size_t>(reqs.stream_bytes[s]) + 128U);
            const uintptr_t raw = reinterpret_cast<uintptr_t>(arenas[s].data());
            const uintptr_t aligned = (raw + 63ULL) & ~uintptr_t{63ULL};
            slices[s].ptr = reinterpret_cast<uint8_t*>(aligned);
        }
    }

    AetherAvatarDesc desc{};
    REQUIRE(aether_loader_fill(loader, slices.data(), AETHER_STREAM_COUNT, &desc) == AETHER_OK);

    // Verify Japanese humanoid bone mapping (下半身 -> Hips=0, 上半身 -> Spine=1, 首 -> Neck=4, 頭 -> Head=5, 左腕 -> LeftUpperArm=18, 左ひじ -> LeftLowerArm=19)
    CHECK(desc.humanoid_to_joint[0] == 2U);  // Hips -> 下半身
    CHECK(desc.humanoid_to_joint[1] == 3U);  // Spine -> 上半身
    CHECK(desc.humanoid_to_joint[4] == 4U);  // Neck -> 首
    CHECK(desc.humanoid_to_joint[5] == 5U);  // Head -> 頭
    CHECK(desc.humanoid_to_joint[18] == 6U); // LeftUpperArm -> 左腕
    CHECK(desc.humanoid_to_joint[19] == 7U); // LeftLowerArm -> 左ひじ

    // Verify edge_scale = 0.0 on jawline/face vertices 0..2 and > 0.0 on hair vertices 3..5
    const auto* verts = reinterpret_cast<const AetherVertex25D*>(slices[AETHER_STREAM_VERTEX25D].ptr);
    REQUIRE(verts != nullptr);
    CHECK_THAT(verts[0].outline_width_scale, Catch::Matchers::WithinAbs(0.0f, 1e-5f));
    CHECK_THAT(verts[1].outline_width_scale, Catch::Matchers::WithinAbs(0.0f, 1e-5f));
    CHECK_THAT(verts[2].outline_width_scale, Catch::Matchers::WithinAbs(0.0f, 1e-5f));
    CHECK(verts[3].outline_width_scale > 0.5f);

    // Verify Ray-MMD auto-classification on materials
    const auto* mats = reinterpret_cast<const AetherNprMaterial*>(slices[AETHER_STREAM_MATERIALS].ptr);
    REQUIRE(mats != nullptr);
    CHECK((mats[0].flags & AETHER_MAT_FLAG_HAS_PROFILE) != 0U);
    CHECK(((mats[0].flags >> AETHER_MAT_FLAG_PROFILE_SHIFT) & 0x7U) == AETHER_MAT_PROFILE_SKIN_SDF);
    CHECK(((mats[1].flags >> AETHER_MAT_FLAG_PROFILE_SHIFT) & 0x7U) == AETHER_MAT_PROFILE_DUAL_LOBE_HAIR);

    // Query and fill PMX auxiliary slices
    AetherPmxAuxHeader pmx_hdr{};
    REQUIRE(aether_loader_query_pmx_aux(loader, &pmx_hdr) == AETHER_OK);
    CHECK(pmx_hdr.is_pmx == 1U);
    CHECK(pmx_hdr.sdef_vertex_count == 6U);
    CHECK(pmx_hdr.bone_ext_count == 10U);
    CHECK(pmx_hdr.ik_link_count == 1U);
    CHECK(pmx_hdr.morph_channel_count == 5U);
    CHECK(pmx_hdr.group_child_count == 2U);
    CHECK(pmx_hdr.uv_morph_count == 1U);
    CHECK(pmx_hdr.mat_morph_count == 1U);
    CHECK(pmx_hdr.bone_morph_count == 1U);
    CHECK(pmx_hdr.rigid_body_count == 2U);
    CHECK(pmx_hdr.constraint_6dof_count == 1U);

    std::vector<AetherPmxSdefVertex> sdef_verts(pmx_hdr.sdef_vertex_count);
    std::vector<AetherPmxBoneExt> bone_exts(pmx_hdr.bone_ext_count);
    std::vector<AetherPmxIkLink> ik_links(pmx_hdr.ik_link_count);
    std::vector<AetherPmxMorphChannelDesc> morph_ch(pmx_hdr.morph_channel_count);
    std::vector<AetherPmxGroupMorphChild> grp_children(pmx_hdr.group_child_count);
    std::vector<AetherPmxUvMorphDelta> uv_morphs(pmx_hdr.uv_morph_count);
    std::vector<AetherPmxMaterialMorphDelta> mat_morphs(pmx_hdr.mat_morph_count);
    std::vector<AetherPmxBoneMorphOffset> bone_morphs(pmx_hdr.bone_morph_count);
    std::vector<AetherPmxRigidBodyDesc> rbs(pmx_hdr.rigid_body_count);
    std::vector<AetherPmxConstraint6DofDesc> joints6(pmx_hdr.constraint_6dof_count);

    AetherPmxAuxSlices pmx_slices{};
    pmx_slices.struct_size = sizeof(AetherPmxAuxSlices);
    pmx_slices.sdef_vertices = sdef_verts.data();
    pmx_slices.sdef_vertex_cap = pmx_hdr.sdef_vertex_count;
    pmx_slices.bone_exts = bone_exts.data();
    pmx_slices.bone_ext_cap = pmx_hdr.bone_ext_count;
    pmx_slices.ik_links = ik_links.data();
    pmx_slices.ik_link_cap = pmx_hdr.ik_link_count;
    pmx_slices.morph_channels = morph_ch.data();
    pmx_slices.morph_channel_cap = pmx_hdr.morph_channel_count;
    pmx_slices.group_children = grp_children.data();
    pmx_slices.group_child_cap = pmx_hdr.group_child_count;
    pmx_slices.uv_morphs = uv_morphs.data();
    pmx_slices.uv_morph_cap = pmx_hdr.uv_morph_count;
    pmx_slices.mat_morphs = mat_morphs.data();
    pmx_slices.mat_morph_cap = pmx_hdr.mat_morph_count;
    pmx_slices.bone_morphs = bone_morphs.data();
    pmx_slices.bone_morph_cap = pmx_hdr.bone_morph_count;
    pmx_slices.rigid_bodies = rbs.data();
    pmx_slices.rigid_body_cap = pmx_hdr.rigid_body_count;
    pmx_slices.constraints_6dof = joints6.data();
    pmx_slices.constraint_6dof_cap = pmx_hdr.constraint_6dof_count;

    REQUIRE(aether_loader_fill_pmx_aux(loader, &pmx_slices) == AETHER_OK);
    aether_loader_close(loader);
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);

    std::error_code ec;
    std::filesystem::remove(tmp_pmx, ec);

    CHECK((sdef_verts[0].flags & 1U) == 1U);
    CHECK_THAT(uv_morphs[0].duv[0], Catch::Matchers::WithinAbs(0.125f, 1e-5f));
    CHECK(mat_morphs[0].op_mode == 1U);
    CHECK_THAT(mat_morphs[0].diffuse[0], Catch::Matchers::WithinAbs(0.1f, 1e-5f));

    // Verify SDEF volume preservation vs LBS collapse on a 120-degree elbow/neck twist (w0=0.5, w1=0.5)
    {
        AetherPmxSdefVertex sv{};
        sv.c[0] = 0.0f;
        sv.c[1] = 1.0f;
        sv.c[2] = 0.0f;
        sv.r0[0] = 0.0f;
        sv.r0[1] = 1.0f;
        sv.r0[2] = 0.0f;
        sv.r1[0] = 0.0f;
        sv.r1[1] = 1.0f;
        sv.r1[2] = 0.0f;
        sv.flags = 1U;

        // Bone 0 = identity; Bone 1 = 120 deg twist around Y axis centered at C=(0,1,0)
        std::vector<float> skin_mats(32, 0.0f);
        skin_mats[0] = 1.0f;
        skin_mats[5] = 1.0f;
        skin_mats[10] = 1.0f;
        skin_mats[15] = 1.0f;

        const float ang = 120.0f * 3.14159265f / 180.0f;
        const float ca = std::cos(ang);
        const float sa = std::sin(ang);
        skin_mats[16 + 0] = ca;
        skin_mats[16 + 2] = -sa;
        skin_mats[16 + 5] = 1.0f;
        skin_mats[16 + 8] = sa;
        skin_mats[16 + 10] = ca;
        skin_mats[16 + 15] = 1.0f;

        const float in_pos[3] = {0.10f, 1.0f, 0.0f}; // radius = 0.10 m from pivot C
        const float in_nrm[3] = {1.0f, 0.0f, 0.0f};
        const uint16_t joints[4] = {0, 1, 0, 0};
        const float weights[4] = {0.5f, 0.5f, 0.0f, 0.0f};

        float sdef_pos[3]{}, sdef_nrm[3]{};
        aether::animation::PmxSkeletonAndPhysicsSolver::evaluate_sdef_vertex(
            in_pos, in_nrm, joints, weights, sv, skin_mats, skin_mats, skin_mats, sdef_pos, sdef_nrm);

        AetherPmxSdefVertex lbs_sv = sv;
        lbs_sv.flags = 0U;
        float lbs_pos[3]{}, lbs_nrm[3]{};
        aether::animation::PmxSkeletonAndPhysicsSolver::evaluate_sdef_vertex(
            in_pos, in_nrm, joints, weights, lbs_sv, skin_mats, skin_mats, skin_mats, lbs_pos, lbs_nrm);

        const float sdef_radius = std::hypot(sdef_pos[0], sdef_pos[2]);
        const float lbs_radius = std::hypot(lbs_pos[0], lbs_pos[2]);
        // SDEF preserves 100% of the 0.10 m cylinder radius; LBS collapses to cos(60 deg) = 0.05 m (50% candy-wrapper collapse!)
        CHECK_THAT(sdef_radius, Catch::Matchers::WithinAbs(0.10f, 1e-4f));
        CHECK(lbs_radius < 0.055f);
        CHECK(sdef_radius > lbs_radius * 1.9f);
    }

    // Verify 60 Hz PmxSkeletonAndPhysicsSolver (CCD-IK + Append Transform + RigidBody + 6-DOF Spring Joint)
    // runs with zero heap allocations inside step() and stays stable over 120 frames with fast head rotation.
    const auto* bones = reinterpret_cast<const AetherJointDesc*>(slices[AETHER_STREAM_JOINTS].ptr);
    REQUIRE(bones != nullptr);
    const std::span<const AetherJointDesc> bone_span(bones, reqs.joint_count);

    aether::animation::PmxSkeletonAndPhysicsSolver solver;
    solver.init(bone_span, bone_exts, ik_links, rbs, joints6);

    std::vector<float> local_t(reqs.joint_count * 3U);
    std::vector<float> local_r(reqs.joint_count * 4U);
    std::vector<float> local_s(reqs.joint_count * 3U);
    std::vector<float> world_mats(reqs.joint_count * 16U);

    for (size_t j = 0; j < reqs.joint_count; ++j) {
        for (int k = 0; k < 3; ++k) {
            local_t[j * 3U + k] = bones[j].bind_translation[k];
            local_s[j * 3U + k] = bones[j].bind_scale[k];
        }
        for (int k = 0; k < 4; ++k) {
            local_r[j * 4U + k] = bones[j].bind_rotation_quat[k];
        }
    }

    // Prime initial rest pose once before entering hot loop
    solver.step(aether::animation::PmxSkeletonAndPhysicsSolver::kFixedDt,
                bone_span, local_t, local_r, local_s, world_mats);

    {
        AETHER_HOT_PATH_SCOPE;
        for (int frame = 0; frame < 120; ++frame) {
            // Apply fast sinusoidal head yaw rotation on bone 5 ("頭")
            const float yaw = 0.65f * std::sin(static_cast<float>(frame) * 0.25f);
            local_r[5 * 4U + 0] = 0.0f;
            local_r[5 * 4U + 1] = std::sin(yaw * 0.5f);
            local_r[5 * 4U + 2] = 0.0f;
            local_r[5 * 4U + 3] = std::cos(yaw * 0.5f);

            // Reset bone 8 ("前髪_物理") to identity before AppendRotate + Physics
            local_r[8 * 4U + 0] = 0.0f;
            local_r[8 * 4U + 1] = 0.0f;
            local_r[8 * 4U + 2] = 0.0f;
            local_r[8 * 4U + 3] = 1.0f;

            solver.step(aether::animation::PmxSkeletonAndPhysicsSolver::kFixedDt,
                        bone_span, local_t, local_r, local_s, world_mats);
        }
    }

    for (const auto& st : solver.rigid_body_states()) {
        CHECK(std::isfinite(st.pos.x));
        CHECK(std::isfinite(st.pos.y));
        CHECK(std::isfinite(st.pos.z));
        const float lin_speed = std::sqrt(st.lin_vel.x * st.lin_vel.x + st.lin_vel.y * st.lin_vel.y +
                                          st.lin_vel.z * st.lin_vel.z);
        CHECK(lin_speed <= aether::animation::PmxSkeletonAndPhysicsSolver::kMaxLinearVel + 1e-3f);
    }
}

TEST_CASE("Phase 3: NodeCanvas authors ModelSource, MaterialSlotOverride, TextureAsset, TextureGrade & DynamicDriver",
          "[phase3][ui]") {
    aether::ui::NodeCanvas canvas;
    aether::ui::UndoStack undo;
    aether::ui::StudioActions actions{};

    const uint64_t n_model = canvas.add_node("aether.model.source@1", -320.0f, 120.0f, undo, actions);
    const uint64_t n_tex = canvas.add_node("aether.texture.asset@1", -320.0f, 240.0f, undo, actions);
    const uint64_t n_grade = canvas.add_node("aether.texture.grade@1", -120.0f, 240.0f, undo, actions);
    const uint64_t n_drv = canvas.add_node("aether.material.dynamic_driver@1", -120.0f, 380.0f, undo, actions);
    const uint64_t n_ovr = canvas.add_node("aether.material.slot_override@1", 120.0f, 180.0f, undo, actions);

    REQUIRE(n_model != 0);
    REQUIRE(n_tex != 0);
    REQUIRE(n_grade != 0);
    REQUIRE(n_drv != 0);
    REQUIRE(n_ovr != 0);

    CHECK(aether::ui::connect_succeeded(canvas.connect_pins(n_model, 2, n_ovr, 0, undo, actions)));
    CHECK(aether::ui::connect_succeeded(canvas.connect_pins(n_tex, 0, n_grade, 0, undo, actions)));
    CHECK(aether::ui::connect_succeeded(canvas.connect_pins(n_grade, 0, n_ovr, 1, undo, actions)));
    CHECK(aether::ui::connect_succeeded(canvas.connect_pins(n_drv, 0, n_ovr, 5, undo, actions)));

    const std::string json = canvas.serialize_graph_json();
    aether::ui::NodeCanvas roundtrip;
    roundtrip.set_graph_json(json);
    CHECK(roundtrip.find_node(n_model) != nullptr);
    CHECK(roundtrip.find_node(n_tex) != nullptr);
    CHECK(roundtrip.find_node(n_grade) != nullptr);
    CHECK(roundtrip.find_node(n_drv) != nullptr);
    CHECK(roundtrip.find_node(n_ovr) != nullptr);
}
