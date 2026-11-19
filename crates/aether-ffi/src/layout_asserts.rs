//! Compile-time size, alignment, and field-offset assertions for every FFI type (`HR-03`, `REQ-FFI-005`).

use core::mem::{align_of, offset_of, size_of};

use crate::types::{
    AetherArenaSlice, AetherAssetRequirements, AetherAvatarDesc, AetherColliderDesc,
    AetherDspSyncPacket, AetherExpressionBindingDesc, AetherGraphInputs, AetherGraphOutputs,
    AetherGraphPlanStats, AetherGraphTelemetryReadout, AetherHostCommand, AetherHostConfig,
    AetherHostEvent, AetherJointDesc, AetherLoadRequest, AetherMorphDelta, AetherMorphRangeDesc,
    AetherNprMaterial, AetherRuntimeConfig, AetherSpringJointDesc, AetherStudioLightGpu,
    AetherSubmeshDesc, AetherTextureDesc, AetherTrackingFrame, AetherVertex25D, AetherVertexSkin,
    AetherVertexSurface,
};

// ---- Frozen MTD Structs (§5.2) ----
const _: () = assert!(size_of::<AetherVertex25D>() == 64);
const _: () = assert!(align_of::<AetherVertex25D>() == 64);
const _: () = assert!(offset_of!(AetherVertex25D, position) == 0);
const _: () = assert!(offset_of!(AetherVertex25D, outline_width_scale) == 12);
const _: () = assert!(offset_of!(AetherVertex25D, true_normal) == 16);
const _: () = assert!(offset_of!(AetherVertex25D, z_flatten_weight) == 28);
const _: () = assert!(offset_of!(AetherVertex25D, proxy_normal) == 32);
const _: () = assert!(offset_of!(AetherVertex25D, sdf_face_mask) == 44);
const _: () = assert!(offset_of!(AetherVertex25D, smooth_tangent) == 48);

const _: () = assert!(size_of::<AetherVertexSurface>() == 32);
const _: () = assert!(align_of::<AetherVertexSurface>() == 4);
const _: () = assert!(offset_of!(AetherVertexSurface, uv0) == 0);
const _: () = assert!(offset_of!(AetherVertexSurface, uv1) == 8);
const _: () = assert!(offset_of!(AetherVertexSurface, color_rgba8) == 16);
const _: () = assert!(offset_of!(AetherVertexSurface, tangent_oct) == 20);
const _: () = assert!(offset_of!(AetherVertexSurface, flags) == 24);
const _: () = assert!(offset_of!(AetherVertexSurface, _reserved) == 28);

const _: () = assert!(size_of::<AetherVertexSkin>() == 32);
const _: () = assert!(align_of::<AetherVertexSkin>() == 2);
const _: () = assert!(offset_of!(AetherVertexSkin, joints) == 0);
const _: () = assert!(offset_of!(AetherVertexSkin, weights_unorm16) == 16);

const _: () = assert!(size_of::<AetherMorphDelta>() == 32);
const _: () = assert!(align_of::<AetherMorphDelta>() == 4);
const _: () = assert!(offset_of!(AetherMorphDelta, vertex_index) == 0);
const _: () = assert!(offset_of!(AetherMorphDelta, d_position) == 4);
const _: () = assert!(offset_of!(AetherMorphDelta, d_normal) == 16);
const _: () = assert!(offset_of!(AetherMorphDelta, d_proxy_weight) == 28);

const _: () = assert!(size_of::<AetherTrackingFrame>() == 2048);
const _: () = assert!(align_of::<AetherTrackingFrame>() == 64);
const _: () = assert!(offset_of!(AetherTrackingFrame, capture_timestamp_us) == 0);
const _: () = assert!(offset_of!(AetherTrackingFrame, inference_complete_us) == 8);
const _: () = assert!(offset_of!(AetherTrackingFrame, active_execution_provider) == 16);
const _: () = assert!(offset_of!(AetherTrackingFrame, upper_body_confidence) == 20);
const _: () = assert!(offset_of!(AetherTrackingFrame, left_hand_confidence) == 24);
const _: () = assert!(offset_of!(AetherTrackingFrame, right_hand_confidence) == 28);
const _: () = assert!(offset_of!(AetherTrackingFrame, lower_body_confidence) == 32);
const _: () = assert!(offset_of!(AetherTrackingFrame, arkit_blendshapes) == 36);
const _: () = assert!(offset_of!(AetherTrackingFrame, joint_positions_ws) == 244);
const _: () = assert!(offset_of!(AetherTrackingFrame, joint_rotations_quat) == 1124);
const _: () = assert!(offset_of!(AetherTrackingFrame, frame_sequence) == 2004);
const _: () = assert!(offset_of!(AetherTrackingFrame, valid_flags) == 2008);
const _: () = assert!(offset_of!(AetherTrackingFrame, webcam_visemes) == 2012);
const _: () = assert!(offset_of!(AetherTrackingFrame, _reserved) == 2032);

const _: () = assert!(size_of::<AetherDspSyncPacket>() == 128);
const _: () = assert!(align_of::<AetherDspSyncPacket>() == 64);
const _: () = assert!(offset_of!(AetherDspSyncPacket, magic_header) == 0);
const _: () = assert!(offset_of!(AetherDspSyncPacket, sequence_number) == 4);
const _: () = assert!(offset_of!(AetherDspSyncPacket, total_video_latency_ms) == 8);
const _: () = assert!(offset_of!(AetherDspSyncPacket, recommended_dsp_delay_ms) == 12);
const _: () = assert!(offset_of!(AetherDspSyncPacket, video_present_qpc_us) == 16);
const _: () = assert!(offset_of!(AetherDspSyncPacket, protocol_version) == 24);
const _: () = assert!(offset_of!(AetherDspSyncPacket, video_fps_milli) == 28);
const _: () = assert!(offset_of!(AetherDspSyncPacket, latency_jitter_ms) == 32);
const _: () = assert!(offset_of!(AetherDspSyncPacket, video_flags) == 36);
const _: () = assert!(offset_of!(AetherDspSyncPacket, _reserved0) == 40);
const _: () = assert!(offset_of!(AetherDspSyncPacket, dsp_sequence_number) == 64);
const _: () = assert!(offset_of!(AetherDspSyncPacket, dsp_input_rms_db) == 68);
const _: () = assert!(offset_of!(AetherDspSyncPacket, dsp_viseme_weights) == 72);
const _: () = assert!(offset_of!(AetherDspSyncPacket, dsp_reported_latency_ms) == 92);
const _: () = assert!(offset_of!(AetherDspSyncPacket, dsp_write_qpc_us) == 96);
const _: () = assert!(offset_of!(AetherDspSyncPacket, dsp_flags) == 104);
const _: () = assert!(offset_of!(AetherDspSyncPacket, _reserved1) == 108);

const _: () = assert!(size_of::<AetherArenaSlice>() == 24);
const _: () = assert!(align_of::<AetherArenaSlice>() == 8);
const _: () = assert!(offset_of!(AetherArenaSlice, ptr) == 0);
const _: () = assert!(offset_of!(AetherArenaSlice, size_bytes) == 8);
const _: () = assert!(offset_of!(AetherArenaSlice, stream_kind) == 16);
const _: () = assert!(offset_of!(AetherArenaSlice, _pad) == 20);

// ---- Descriptor, Graph & Config Structs (§5.2, ADR-0003) ----
const _: () = assert!(size_of::<AetherRuntimeConfig>() == 32);
const _: () = assert!(align_of::<AetherRuntimeConfig>() == 8);
const _: () = assert!(offset_of!(AetherRuntimeConfig, struct_size) == 0);
const _: () = assert!(offset_of!(AetherRuntimeConfig, log_level) == 4);
const _: () = assert!(offset_of!(AetherRuntimeConfig, worker_threads) == 8);
const _: () = assert!(offset_of!(AetherRuntimeConfig, flags) == 12);
const _: () = assert!(offset_of!(AetherRuntimeConfig, app_data_dir_utf8) == 16);
const _: () = assert!(offset_of!(AetherRuntimeConfig, cache_dir_utf8) == 24);

const _: () = assert!(size_of::<AetherLoadRequest>() == 16);
const _: () = assert!(align_of::<AetherLoadRequest>() == 8);
const _: () = assert!(offset_of!(AetherLoadRequest, path_utf8) == 0);
const _: () = assert!(offset_of!(AetherLoadRequest, flags) == 8);
const _: () = assert!(offset_of!(AetherLoadRequest, max_threads) == 12);

const _: () = assert!(size_of::<AetherAssetRequirements>() == 168);
const _: () = assert!(align_of::<AetherAssetRequirements>() == 8);
const _: () = assert!(offset_of!(AetherAssetRequirements, stream_bytes) == 0);
const _: () = assert!(offset_of!(AetherAssetRequirements, vertex_count) == 120);
const _: () = assert!(offset_of!(AetherAssetRequirements, index_count) == 124);
const _: () = assert!(offset_of!(AetherAssetRequirements, submesh_count) == 128);
const _: () = assert!(offset_of!(AetherAssetRequirements, material_count) == 132);
const _: () = assert!(offset_of!(AetherAssetRequirements, joint_count) == 136);
const _: () = assert!(offset_of!(AetherAssetRequirements, spring_joint_count) == 140);
const _: () = assert!(offset_of!(AetherAssetRequirements, collider_count) == 144);
const _: () = assert!(offset_of!(AetherAssetRequirements, morph_target_count) == 148);
const _: () = assert!(offset_of!(AetherAssetRequirements, expression_count) == 152);
const _: () = assert!(offset_of!(AetherAssetRequirements, texture_count) == 156);
const _: () = assert!(offset_of!(AetherAssetRequirements, source_format) == 160);
const _: () = assert!(offset_of!(AetherAssetRequirements, warnings_mask) == 164);

const _: () = assert!(size_of::<AetherAvatarDesc>() == 504);
const _: () = assert!(align_of::<AetherAvatarDesc>() == 8);
const _: () = assert!(offset_of!(AetherAvatarDesc, source_format) == 0);
const _: () = assert!(offset_of!(AetherAvatarDesc, humanoid_to_joint) == 4);
const _: () = assert!(offset_of!(AetherAvatarDesc, bone_match_score) == 116);
const _: () = assert!(offset_of!(AetherAvatarDesc, aabb_min) == 336);
const _: () = assert!(offset_of!(AetherAvatarDesc, aabb_max) == 348);
const _: () = assert!(offset_of!(AetherAvatarDesc, face_center_head_local) == 360);
const _: () = assert!(offset_of!(AetherAvatarDesc, avatar_height_m) == 372);
const _: () = assert!(offset_of!(AetherAvatarDesc, head_joint) == 376);
const _: () = assert!(offset_of!(AetherAvatarDesc, warnings_mask) == 380);
const _: () = assert!(offset_of!(AetherAvatarDesc, debug_stream_ptrs) == 384);

const _: () = assert!(size_of::<AetherHostConfig>() == 24);
const _: () = assert!(align_of::<AetherHostConfig>() == 8);
const _: () = assert!(offset_of!(AetherHostConfig, struct_size) == 0);
const _: () = assert!(offset_of!(AetherHostConfig, worker_threads) == 4);
const _: () = assert!(offset_of!(AetherHostConfig, osc_in_port) == 8);
const _: () = assert!(offset_of!(AetherHostConfig, osc_out_port) == 10);
const _: () = assert!(offset_of!(AetherHostConfig, ws_port) == 12);
const _: () = assert!(offset_of!(AetherHostConfig, flags) == 14);
const _: () = assert!(offset_of!(AetherHostConfig, plugins_dir_utf8) == 16);

const _: () = assert!(size_of::<AetherHostCommand>() == 40);
const _: () = assert!(align_of::<AetherHostCommand>() == 8);
const _: () = assert!(offset_of!(AetherHostCommand, struct_size) == 0);
const _: () = assert!(offset_of!(AetherHostCommand, kind) == 4);
const _: () = assert!(offset_of!(AetherHostCommand, target_id) == 8);
const _: () = assert!(offset_of!(AetherHostCommand, param_f32) == 16);
const _: () = assert!(offset_of!(AetherHostCommand, param_u32) == 20);
const _: () = assert!(offset_of!(AetherHostCommand, payload_utf8) == 24);
const _: () = assert!(offset_of!(AetherHostCommand, payload_len) == 32);

const _: () = assert!(size_of::<AetherHostEvent>() == 280);
const _: () = assert!(align_of::<AetherHostEvent>() == 8);
const _: () = assert!(offset_of!(AetherHostEvent, struct_size) == 0);
const _: () = assert!(offset_of!(AetherHostEvent, kind) == 4);
const _: () = assert!(offset_of!(AetherHostEvent, source_id) == 8);
const _: () = assert!(offset_of!(AetherHostEvent, status) == 16);
const _: () = assert!(offset_of!(AetherHostEvent, value_f32) == 20);
const _: () = assert!(offset_of!(AetherHostEvent, message_utf8) == 24);

const _: () = assert!(size_of::<AetherGraphInputs>() == 384);
const _: () = assert!(align_of::<AetherGraphInputs>() == 64);
const _: () = assert!(offset_of!(AetherGraphInputs, tracking) == 0);
const _: () = assert!(offset_of!(AetherGraphInputs, now_us) == 8);
const _: () = assert!(offset_of!(AetherGraphInputs, dt_s) == 16);
const _: () = assert!(offset_of!(AetherGraphInputs, tick_index) == 20);
const _: () = assert!(offset_of!(AetherGraphInputs, dsp_visemes) == 24);
const _: () = assert!(offset_of!(AetherGraphInputs, dsp_rms_db) == 44);
const _: () = assert!(offset_of!(AetherGraphInputs, keys_down) == 48);
const _: () = assert!(offset_of!(AetherGraphInputs, keys_pressed) == 64);
const _: () = assert!(offset_of!(AetherGraphInputs, external_params) == 80);

const _: () = assert!(size_of::<AetherGraphOutputs>() == 1792);
const _: () = assert!(align_of::<AetherGraphOutputs>() == 64);
const _: () = assert!(offset_of!(AetherGraphOutputs, expression_weights) == 0);
const _: () = assert!(offset_of!(AetherGraphOutputs, expression_override) == 256);
const _: () = assert!(offset_of!(AetherGraphOutputs, ik_targets) == 512);
const _: () = assert!(offset_of!(AetherGraphOutputs, bone_rotation_offsets) == 768);
const _: () = assert!(offset_of!(AetherGraphOutputs, look_at_ws) == 1648);
const _: () = assert!(offset_of!(AetherGraphOutputs, events) == 1664);
const _: () = assert!(offset_of!(AetherGraphOutputs, event_count) == 1728);

const _: () = assert!(size_of::<AetherMorphRangeDesc>() == 32);
const _: () = assert!(align_of::<AetherMorphRangeDesc>() == 8);
const _: () = assert!(offset_of!(AetherMorphRangeDesc, delta_offset) == 0);
const _: () = assert!(offset_of!(AetherMorphRangeDesc, delta_count) == 4);
const _: () = assert!(offset_of!(AetherMorphRangeDesc, name_offset) == 8);
const _: () = assert!(offset_of!(AetherMorphRangeDesc, name_len) == 12);
const _: () = assert!(offset_of!(AetherMorphRangeDesc, centroid_x) == 16);
const _: () = assert!(offset_of!(AetherMorphRangeDesc, is_face_morph) == 20);
const _: () = assert!(offset_of!(AetherMorphRangeDesc, _reserved) == 24);

const _: () = assert!(size_of::<AetherSubmeshDesc>() == 64);
const _: () = assert!(align_of::<AetherSubmeshDesc>() == 8);
const _: () = assert!(offset_of!(AetherSubmeshDesc, index_offset) == 0);
const _: () = assert!(offset_of!(AetherSubmeshDesc, index_count) == 4);
const _: () = assert!(offset_of!(AetherSubmeshDesc, vertex_offset) == 8);
const _: () = assert!(offset_of!(AetherSubmeshDesc, vertex_count) == 12);
const _: () = assert!(offset_of!(AetherSubmeshDesc, material_index) == 16);
const _: () = assert!(offset_of!(AetherSubmeshDesc, flags) == 20);
const _: () = assert!(offset_of!(AetherSubmeshDesc, name_offset) == 24);
const _: () = assert!(offset_of!(AetherSubmeshDesc, name_len) == 28);
const _: () = assert!(offset_of!(AetherSubmeshDesc, render_queue_offset) == 32);
const _: () = assert!(offset_of!(AetherSubmeshDesc, aabb_min) == 40);
const _: () = assert!(offset_of!(AetherSubmeshDesc, aabb_max) == 52);

const _: () = assert!(size_of::<AetherNprMaterial>() == 192);
const _: () = assert!(align_of::<AetherNprMaterial>() == 8);
const _: () = assert!(offset_of!(AetherNprMaterial, base_color) == 0);
const _: () = assert!(offset_of!(AetherNprMaterial, shade_color_gi) == 16);
const _: () = assert!(offset_of!(AetherNprMaterial, rim_color_mix) == 32);
const _: () = assert!(offset_of!(AetherNprMaterial, outline_color_mix) == 48);
const _: () = assert!(offset_of!(AetherNprMaterial, emissive_and_pad) == 64);
const _: () = assert!(offset_of!(AetherNprMaterial, matcap_color) == 80);
const _: () = assert!(offset_of!(AetherNprMaterial, toon_params) == 96);
const _: () = assert!(offset_of!(AetherNprMaterial, uv_outline_alpha) == 112);
const _: () = assert!(offset_of!(AetherNprMaterial, outline_width) == 128);
const _: () = assert!(offset_of!(AetherNprMaterial, alpha_mode) == 132);
const _: () = assert!(offset_of!(AetherNprMaterial, render_queue_offset) == 136);
const _: () = assert!(offset_of!(AetherNprMaterial, flags) == 140);
const _: () = assert!(offset_of!(AetherNprMaterial, tex_base) == 144);
const _: () = assert!(offset_of!(AetherNprMaterial, tex_shade) == 148);
const _: () = assert!(offset_of!(AetherNprMaterial, tex_shading_shift) == 152);
const _: () = assert!(offset_of!(AetherNprMaterial, tex_normal) == 156);
const _: () = assert!(offset_of!(AetherNprMaterial, tex_emissive) == 160);
const _: () = assert!(offset_of!(AetherNprMaterial, tex_rim) == 164);
const _: () = assert!(offset_of!(AetherNprMaterial, tex_matcap) == 168);
const _: () = assert!(offset_of!(AetherNprMaterial, tex_outline_width) == 172);
const _: () = assert!(offset_of!(AetherNprMaterial, tex_uv_mask) == 176);
const _: () = assert!(offset_of!(AetherNprMaterial, tex_ramp) == 180);
const _: () = assert!(offset_of!(AetherNprMaterial, tex_sdf_face) == 184);
const _: () = assert!(offset_of!(AetherNprMaterial, name_offset) == 188);

const _: () = assert!(size_of::<AetherJointDesc>() == 128);
const _: () = assert!(align_of::<AetherJointDesc>() == 8);
const _: () = assert!(offset_of!(AetherJointDesc, parent_index) == 0);
const _: () = assert!(offset_of!(AetherJointDesc, humanoid_bone) == 4);
const _: () = assert!(offset_of!(AetherJointDesc, region_tag) == 6);
const _: () = assert!(offset_of!(AetherJointDesc, name_offset) == 8);
const _: () = assert!(offset_of!(AetherJointDesc, name_len) == 12);
const _: () = assert!(offset_of!(AetherJointDesc, bind_translation) == 16);
const _: () = assert!(offset_of!(AetherJointDesc, tail_length) == 28);
const _: () = assert!(offset_of!(AetherJointDesc, bind_rotation_quat) == 32);
const _: () = assert!(offset_of!(AetherJointDesc, bind_scale) == 48);
const _: () = assert!(offset_of!(AetherJointDesc, rest_to_tpose_quat) == 64);
const _: () = assert!(offset_of!(AetherJointDesc, inverse_bind_matrix_row_major) == 80);

const _: () = assert!(size_of::<AetherSpringJointDesc>() == 80);
const _: () = assert!(align_of::<AetherSpringJointDesc>() == 8);
const _: () = assert!(offset_of!(AetherSpringJointDesc, collider_group_mask) == 0);
const _: () = assert!(offset_of!(AetherSpringJointDesc, gravity_dir) == 8);
const _: () = assert!(offset_of!(AetherSpringJointDesc, hit_radius) == 20);
const _: () = assert!(offset_of!(AetherSpringJointDesc, rest_dir) == 24);
const _: () = assert!(offset_of!(AetherSpringJointDesc, bone_length) == 36);
const _: () = assert!(offset_of!(AetherSpringJointDesc, joint_index) == 40);
const _: () = assert!(offset_of!(AetherSpringJointDesc, parent_spring_index) == 44);
const _: () = assert!(offset_of!(AetherSpringJointDesc, chain_id) == 48);
const _: () = assert!(offset_of!(AetherSpringJointDesc, depth_level) == 52);
const _: () = assert!(offset_of!(AetherSpringJointDesc, center_joint_index) == 56);
const _: () = assert!(offset_of!(AetherSpringJointDesc, stiffness) == 60);
const _: () = assert!(offset_of!(AetherSpringJointDesc, drag_force) == 64);
const _: () = assert!(offset_of!(AetherSpringJointDesc, gravity_power) == 68);
const _: () = assert!(offset_of!(AetherSpringJointDesc, flags) == 72);

const _: () = assert!(size_of::<AetherColliderDesc>() == 48);
const _: () = assert!(align_of::<AetherColliderDesc>() == 8);
const _: () = assert!(offset_of!(AetherColliderDesc, joint_index) == 0);
const _: () = assert!(offset_of!(AetherColliderDesc, group_bit) == 4);
const _: () = assert!(offset_of!(AetherColliderDesc, shape_kind) == 8);
const _: () = assert!(offset_of!(AetherColliderDesc, radius) == 12);
const _: () = assert!(offset_of!(AetherColliderDesc, offset) == 16);
const _: () = assert!(offset_of!(AetherColliderDesc, tail) == 32);

const _: () = assert!(size_of::<AetherExpressionBindingDesc>() == 24);
const _: () = assert!(align_of::<AetherExpressionBindingDesc>() == 8);
const _: () = assert!(offset_of!(AetherExpressionBindingDesc, preset_index) == 0);
const _: () = assert!(offset_of!(AetherExpressionBindingDesc, input_slot) == 4);
const _: () = assert!(offset_of!(AetherExpressionBindingDesc, morph_index) == 8);
const _: () = assert!(offset_of!(AetherExpressionBindingDesc, weight) == 12);
const _: () = assert!(offset_of!(AetherExpressionBindingDesc, name_offset) == 16);
const _: () = assert!(offset_of!(AetherExpressionBindingDesc, name_len) == 20);

const _: () = assert!(size_of::<AetherTextureDesc>() == 48);
const _: () = assert!(align_of::<AetherTextureDesc>() == 8);
const _: () = assert!(offset_of!(AetherTextureDesc, pixel_offset) == 0);
const _: () = assert!(offset_of!(AetherTextureDesc, pixel_size) == 8);
const _: () = assert!(offset_of!(AetherTextureDesc, width) == 16);
const _: () = assert!(offset_of!(AetherTextureDesc, height) == 20);
const _: () = assert!(offset_of!(AetherTextureDesc, format) == 24);
const _: () = assert!(offset_of!(AetherTextureDesc, usage_flags) == 28);
const _: () = assert!(offset_of!(AetherTextureDesc, name_offset) == 32);
const _: () = assert!(offset_of!(AetherTextureDesc, name_len) == 36);
const _: () = assert!(offset_of!(AetherTextureDesc, content_hash_lo) == 40);

const _: () = assert!(size_of::<AetherGraphPlanStats>() == 2104);
const _: () = assert!(align_of::<AetherGraphPlanStats>() == 8);
const _: () = assert!(offset_of!(AetherGraphPlanStats, struct_size) == 0);
const _: () = assert!(offset_of!(AetherGraphPlanStats, active_node_count) == 4);
const _: () = assert!(offset_of!(AetherGraphPlanStats, error_node_id) == 8);
const _: () = assert!(offset_of!(AetherGraphPlanStats, compile_count) == 12);
const _: () = assert!(offset_of!(AetherGraphPlanStats, last_compile_ms) == 16);
const _: () = assert!(offset_of!(AetherGraphPlanStats, last_eval_us) == 20);
const _: () = assert!(offset_of!(AetherGraphPlanStats, p95_eval_us) == 24);
const _: () = assert!(offset_of!(AetherGraphPlanStats, budget_warn) == 28);
const _: () = assert!(offset_of!(AetherGraphPlanStats, ws_enabled) == 32);
const _: () = assert!(offset_of!(AetherGraphPlanStats, osc_packets_rx) == 36);
const _: () = assert!(offset_of!(AetherGraphPlanStats, eval_count) == 40);
const _: () = assert!(offset_of!(AetherGraphPlanStats, osc_rate_limited) == 48);
const _: () = assert!(offset_of!(AetherGraphPlanStats, plugin_count) == 52);
const _: () = assert!(offset_of!(AetherGraphPlanStats, node_eval_us) == 56);
const _: () = assert!(offset_of!(AetherGraphPlanStats, node_live_values) == 1080);

const _: () = assert!(size_of::<AetherGraphTelemetryReadout>() == 13120);
const _: () = assert!(align_of::<AetherGraphTelemetryReadout>() == 64);
const _: () = assert!(offset_of!(AetherGraphTelemetryReadout, struct_size) == 0);
const _: () = assert!(offset_of!(AetherGraphTelemetryReadout, active_node_count) == 4);
const _: () = assert!(offset_of!(AetherGraphTelemetryReadout, pin_readout_count) == 8);
const _: () = assert!(offset_of!(AetherGraphTelemetryReadout, valid) == 12);
const _: () = assert!(offset_of!(AetherGraphTelemetryReadout, eval_sequence) == 16);
const _: () = assert!(offset_of!(AetherGraphTelemetryReadout, total_eval_ns) == 24);
const _: () = assert!(offset_of!(AetherGraphTelemetryReadout, node_ids) == 32);
const _: () = assert!(offset_of!(AetherGraphTelemetryReadout, node_eval_ns) == 2080);
const _: () = assert!(offset_of!(AetherGraphTelemetryReadout, pin_node_ids) == 3104);
const _: () = assert!(offset_of!(AetherGraphTelemetryReadout, pin_values) == 6176);
const _: () = assert!(offset_of!(AetherGraphTelemetryReadout, pin_indices) == 12320);
const _: () = assert!(offset_of!(AetherGraphTelemetryReadout, pin_kinds) == 12704);
const _: () = assert!(offset_of!(AetherGraphTelemetryReadout, _reserved_tail) == 13088);

const _: () = assert!(size_of::<AetherStudioLightGpu>() == 128);
const _: () = assert!(align_of::<AetherStudioLightGpu>() == 64);
const _: () = assert!(offset_of!(AetherStudioLightGpu, position_ws) == 0);
const _: () = assert!(offset_of!(AetherStudioLightGpu, flags_and_type) == 12);
const _: () = assert!(offset_of!(AetherStudioLightGpu, direction_ws) == 16);
const _: () = assert!(offset_of!(AetherStudioLightGpu, intensity_ev100) == 28);
const _: () = assert!(offset_of!(AetherStudioLightGpu, tangent_u_ws) == 32);
const _: () = assert!(offset_of!(AetherStudioLightGpu, kelvin_cct) == 44);
const _: () = assert!(offset_of!(AetherStudioLightGpu, color_rgb_linear) == 48);
const _: () = assert!(offset_of!(AetherStudioLightGpu, light_channel_mask) == 60);
const _: () = assert!(offset_of!(AetherStudioLightGpu, size_xy_radius) == 64);
const _: () = assert!(offset_of!(AetherStudioLightGpu, inner_outer_cos) == 72);
const _: () = assert!(offset_of!(AetherStudioLightGpu, barn_door_cuts) == 80);
const _: () = assert!(offset_of!(AetherStudioLightGpu, grid_collimation_cos) == 96);
const _: () = assert!(offset_of!(AetherStudioLightGpu, diffusion_softness) == 100);
const _: () = assert!(offset_of!(AetherStudioLightGpu, ies_profile_index) == 104);
const _: () = assert!(offset_of!(AetherStudioLightGpu, catchlight_weight) == 108);
const _: () = assert!(offset_of!(AetherStudioLightGpu, shadow_bias_scale) == 112);
const _: () = assert!(offset_of!(AetherStudioLightGpu, padding) == 116);

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn telemetry_readout_is_64_byte_aligned_and_packed() {
        assert_eq!(align_of::<AetherGraphTelemetryReadout>(), 64);
        assert_eq!(size_of::<AetherGraphTelemetryReadout>(), 13120);
        assert_eq!(size_of::<AetherGraphTelemetryReadout>() % 64, 0);
        assert_eq!(offset_of!(AetherGraphTelemetryReadout, node_ids), 32);
        assert_eq!(offset_of!(AetherGraphTelemetryReadout, pin_values), 6176);
    }
}
