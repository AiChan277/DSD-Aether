//! Build script emitting `layout_table.json` with exact size, alignment, and field offsets (`REQ-FFI-005`).

#![allow(dead_code)]

use std::env;
use std::fmt::Write as _;
use std::fs;
use std::mem::{align_of, offset_of, size_of};
use std::path::PathBuf;

#[path = "src/status.rs"]
mod status;
#[path = "src/types.rs"]
mod types;

use types::{
    AetherArenaSlice, AetherAssetRequirements, AetherAvatarDesc, AetherColliderDesc,
    AetherDspSyncPacket, AetherExpressionBindingDesc, AetherGraphInputs, AetherGraphOutputs,
    AetherGraphPlanStats, AetherGraphTelemetryReadout, AetherHostCommand, AetherHostConfig,
    AetherHostEvent, AetherJointDesc, AetherLoadRequest, AetherMorphDelta, AetherMorphRangeDesc,
    AetherNprMaterial, AetherRuntimeConfig, AetherSpringJointDesc, AetherStudioLightGpu,
    AetherSubmeshDesc, AetherTextureDesc, AetherTrackingFrame, AetherVertex25D, AetherVertexSkin,
    AetherVertexSurface,
};

struct FieldEntry {
    name: &'static str,
    offset: usize,
}

fn push_struct(
    out: &mut String,
    first: &mut bool,
    name: &str,
    size: usize,
    align: usize,
    fields: &[FieldEntry],
) {
    if !*first {
        out.push_str(",\n");
    }
    *first = false;
    let _ = writeln!(out, "    {{");
    let _ = writeln!(out, "      \"name\": \"{name}\",");
    let _ = writeln!(out, "      \"size\": {size},");
    let _ = writeln!(out, "      \"align\": {align},");
    let _ = writeln!(out, "      \"fields\": [");
    for (idx, field) in fields.iter().enumerate() {
        let comma = if idx + 1 < fields.len() { "," } else { "" };
        let _ = writeln!(
            out,
            "        {{ \"name\": \"{}\", \"offset\": {} }}{comma}",
            field.name, field.offset
        );
    }
    let _ = writeln!(out, "      ]");
    let _ = write!(out, "    }}");
}

macro_rules! struct_entry {
    ($out:expr, $first:expr, $ty:ty, [$($field:ident),+ $(,)?]) => {
        push_struct(
            $out,
            $first,
            stringify!($ty),
            size_of::<$ty>(),
            align_of::<$ty>(),
            &[
                $(
                    FieldEntry {
                        name: stringify!($field),
                        offset: offset_of!($ty, $field),
                    },
                )+
            ],
        );
    };
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("cargo:rerun-if-changed=src/types.rs");
    println!("cargo:rerun-if-changed=src/status.rs");
    println!("cargo:rerun-if-changed=build.rs");

    let mut json = String::from("{\n  \"structs\": [\n");
    let mut first = true;

    struct_entry!(
        &mut json,
        &mut first,
        AetherRuntimeConfig,
        [
            struct_size,
            log_level,
            worker_threads,
            flags,
            app_data_dir_utf8,
            cache_dir_utf8
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherLoadRequest,
        [path_utf8, flags, max_threads]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherAssetRequirements,
        [
            stream_bytes,
            vertex_count,
            index_count,
            submesh_count,
            material_count,
            joint_count,
            spring_joint_count,
            collider_count,
            morph_target_count,
            expression_count,
            texture_count,
            source_format,
            warnings_mask
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherArenaSlice,
        [ptr, size_bytes, stream_kind, _pad]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherAvatarDesc,
        [
            source_format,
            humanoid_to_joint,
            bone_match_score,
            aabb_min,
            aabb_max,
            face_center_head_local,
            avatar_height_m,
            head_joint,
            warnings_mask,
            debug_stream_ptrs
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherHostConfig,
        [
            struct_size,
            worker_threads,
            osc_in_port,
            osc_out_port,
            ws_port,
            flags,
            plugins_dir_utf8
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherHostCommand,
        [
            struct_size,
            kind,
            target_id,
            param_f32,
            param_u32,
            payload_utf8,
            payload_len
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherHostEvent,
        [
            struct_size,
            kind,
            source_id,
            status,
            value_f32,
            message_utf8
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherTrackingFrame,
        [
            capture_timestamp_us,
            inference_complete_us,
            active_execution_provider,
            upper_body_confidence,
            left_hand_confidence,
            right_hand_confidence,
            lower_body_confidence,
            arkit_blendshapes,
            joint_positions_ws,
            joint_rotations_quat,
            frame_sequence,
            valid_flags,
            webcam_visemes,
            _reserved
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherGraphInputs,
        [
            tracking,
            now_us,
            dt_s,
            tick_index,
            dsp_visemes,
            dsp_rms_db,
            keys_down,
            keys_pressed,
            external_params
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherGraphOutputs,
        [
            expression_weights,
            expression_override,
            ik_targets,
            bone_rotation_offsets,
            look_at_ws,
            events,
            event_count
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherVertex25D,
        [
            position,
            outline_width_scale,
            true_normal,
            z_flatten_weight,
            proxy_normal,
            sdf_face_mask,
            smooth_tangent
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherVertexSurface,
        [uv0, uv1, color_rgba8, tangent_oct, flags, _reserved]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherVertexSkin,
        [joints, weights_unorm16]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherMorphDelta,
        [vertex_index, d_position, d_normal, d_proxy_weight]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherDspSyncPacket,
        [
            magic_header,
            sequence_number,
            total_video_latency_ms,
            recommended_dsp_delay_ms,
            video_present_qpc_us,
            protocol_version,
            video_fps_milli,
            latency_jitter_ms,
            video_flags,
            _reserved0,
            dsp_sequence_number,
            dsp_input_rms_db,
            dsp_viseme_weights,
            dsp_reported_latency_ms,
            dsp_write_qpc_us,
            dsp_flags,
            _reserved1
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherMorphRangeDesc,
        [
            delta_offset,
            delta_count,
            name_offset,
            name_len,
            centroid_x,
            is_face_morph,
            _reserved
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherSubmeshDesc,
        [
            index_offset,
            index_count,
            vertex_offset,
            vertex_count,
            material_index,
            flags,
            name_offset,
            name_len,
            render_queue_offset,
            _pad0,
            aabb_min,
            aabb_max
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherNprMaterial,
        [
            base_color,
            shade_color_gi,
            rim_color_mix,
            outline_color_mix,
            emissive_and_pad,
            matcap_color,
            toon_params,
            uv_outline_alpha,
            outline_width,
            alpha_mode,
            render_queue_offset,
            flags,
            tex_base,
            tex_shade,
            tex_shading_shift,
            tex_normal,
            tex_emissive,
            tex_rim,
            tex_matcap,
            tex_outline_width,
            tex_uv_mask,
            tex_ramp,
            tex_sdf_face,
            name_offset
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherJointDesc,
        [
            parent_index,
            humanoid_bone,
            region_tag,
            name_offset,
            name_len,
            bind_translation,
            tail_length,
            bind_rotation_quat,
            bind_scale,
            _pad0,
            rest_to_tpose_quat,
            inverse_bind_matrix_row_major
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherSpringJointDesc,
        [
            collider_group_mask,
            gravity_dir,
            hit_radius,
            rest_dir,
            bone_length,
            joint_index,
            parent_spring_index,
            chain_id,
            depth_level,
            center_joint_index,
            stiffness,
            drag_force,
            gravity_power,
            flags,
            _pad
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherColliderDesc,
        [
            joint_index,
            group_bit,
            shape_kind,
            radius,
            offset,
            _pad0,
            tail,
            _pad1
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherExpressionBindingDesc,
        [
            preset_index,
            input_slot,
            morph_index,
            weight,
            name_offset,
            name_len
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherTextureDesc,
        [
            pixel_offset,
            pixel_size,
            width,
            height,
            format,
            usage_flags,
            name_offset,
            name_len,
            content_hash_lo
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherGraphPlanStats,
        [
            struct_size,
            active_node_count,
            error_node_id,
            compile_count,
            last_compile_ms,
            last_eval_us,
            p95_eval_us,
            budget_warn,
            ws_enabled,
            osc_packets_rx,
            eval_count,
            osc_rate_limited,
            plugin_count,
            node_eval_us,
            node_live_values
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherGraphTelemetryReadout,
        [
            struct_size,
            active_node_count,
            pin_readout_count,
            valid,
            eval_sequence,
            total_eval_ns,
            node_ids,
            node_eval_ns,
            pin_node_ids,
            pin_values,
            pin_indices,
            pin_kinds,
            _reserved_tail
        ]
    );
    struct_entry!(
        &mut json,
        &mut first,
        AetherStudioLightGpu,
        [
            position_ws,
            flags_and_type,
            direction_ws,
            intensity_ev100,
            tangent_u_ws,
            kelvin_cct,
            color_rgb_linear,
            light_channel_mask,
            size_xy_radius,
            inner_outer_cos,
            barn_door_cuts,
            grid_collimation_cos,
            diffusion_softness,
            ies_profile_index,
            catchlight_weight,
            shadow_bias_scale,
            padding
        ]
    );

    json.push_str("\n  ]\n}\n");

    let out_dir = PathBuf::from(env::var_os("OUT_DIR").ok_or("OUT_DIR not set by Cargo")?);
    fs::write(out_dir.join("layout_table.json"), json)?;
    Ok(())
}
