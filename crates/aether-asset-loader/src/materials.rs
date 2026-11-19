//! Material conversion (`MToon 0.x/1.0`, glTF PBR, FBX, PMX) & OKLab shade synthesis (`§6.8`).

use aether_ffi::{
    AETHER_ALPHA_MODE_BLEND, AETHER_ALPHA_MODE_MASK, AETHER_INVALID_INDEX,
    AETHER_MAT_FLAG_BODY_SKIN, AETHER_MAT_FLAG_DOUBLE_SIDED, AETHER_MAT_FLAG_EYE_IRIS,
    AETHER_MAT_FLAG_HAIR_FRONT_STENCIL, AETHER_MAT_FLAG_HAS_EXT_VRM, AETHER_MAT_FLAG_HAS_PROFILE,
    AETHER_MAT_FLAG_OUTLINE_SCREEN, AETHER_MAT_FLAG_PROFILE_SHIFT,
    AETHER_MAT_FLAG_RECEIVES_HAIR_SHADOW, AETHER_MAT_FLAG_SDF_FACE, AETHER_MAT_FLAG_UNLIT,
    AETHER_MAT_PROFILE_EYE_CAUSTIC, AETHER_MAT_PROFILE_SKIN_SDF, AETHER_SUBMESH_FLAG_ALPHA_CUTOUT,
    AETHER_SUBMESH_FLAG_EYE_OR_BROW, AETHER_SUBMESH_FLAG_FACE, AETHER_SUBMESH_FLAG_HAIR_BACK,
    AETHER_SUBMESH_FLAG_HAIR_FRONT, AETHER_SUBMESH_FLAG_OPAQUE, AETHER_SUBMESH_FLAG_TRANSPARENT,
    AetherNprMaterial,
};

use crate::ir::RawMaterial;

/// Converts linear sRGB `[0, 1]^3` -> Björn Ottosson OKLab `(L, a, b)`.
#[must_use]
pub fn linear_srgb_to_oklab(c: [f32; 3]) -> [f32; 3] {
    let r = c[0].max(0.0);
    let g = c[1].max(0.0);
    let b = c[2].max(0.0);

    let l = 0.412_221_46 * r + 0.536_332_55 * g + 0.051_445_995 * b;
    let m = 0.211_903_5 * r + 0.680_699_5 * g + 0.107_396_96 * b;
    let s = 0.088_302_46 * r + 0.281_718_85 * g + 0.629_978_7 * b;

    let l_ = l.cbrt();
    let m_ = m.cbrt();
    let s_ = s.cbrt();

    [
        0.210_454_26 * l_ + 0.793_617_8 * m_ - 0.004_072_047 * s_,
        1.977_998_5 * l_ - 2.428_592_2 * m_ + 0.450_593_7 * s_,
        0.025_904_037 * l_ + 0.782_771_77 * m_ - 0.808_675_77 * s_,
    ]
}

/// Converts OKLab `(L, a, b)` -> linear sRGB `[0, 1]^3`.
#[must_use]
pub fn oklab_to_linear_srgb(lab: [f32; 3]) -> [f32; 3] {
    let l_ = lab[0] + 0.396_337_78 * lab[1] + 0.215_803_76 * lab[2];
    let m_ = lab[0] - 0.105_561_346 * lab[1] - 0.063_854_17 * lab[2];
    let s_ = lab[0] - 0.089_484_18 * lab[1] - 1.291_485_5 * lab[2];

    let l = l_ * l_ * l_;
    let m = m_ * m_ * m_;
    let s = s_ * s_ * s_;

    [
        (4.076_741_7 * l - 3.307_711_6 * m + 0.230_969_94 * s).clamp(0.0, 1.0),
        (-1.268_438 * l + 2.609_757_4 * m - 0.341_319_38 * s).clamp(0.0, 1.0),
        (-0.004_196_086 * l - 0.703_418_6 * m + 1.707_614_7 * s).clamp(0.0, 1.0),
    ]
}

/// Synthesizes NPR cel shade color from linear PBR base color (`L * 0.78`, `C * 1.05`, §6.8).
#[must_use]
pub fn synthesize_oklab_shade(linear_base: [f32; 3]) -> [f32; 3] {
    let mut lab = linear_srgb_to_oklab(linear_base);
    lab[0] *= 0.78;
    lab[1] *= 1.05;
    lab[2] *= 1.05;
    oklab_to_linear_srgb(lab)
}

/// Packs normalized submesh classification + `AetherNprMaterial` GPU scalar block (192 B).
pub fn convert_material(
    raw: &RawMaterial,
    submesh_flags_or: u32,
    name_offset: u32,
) -> AetherNprMaterial {
    let shade = raw.explicit_shade_color.unwrap_or_else(|| {
        synthesize_oklab_shade([raw.base_color[0], raw.base_color[1], raw.base_color[2]])
    });

    let mut flags = 0u32;
    if raw.double_sided {
        flags |= AETHER_MAT_FLAG_DOUBLE_SIDED;
    }
    if raw.tex_sdf_face.is_some() {
        flags |= AETHER_MAT_FLAG_SDF_FACE;
    }
    if (submesh_flags_or & AETHER_SUBMESH_FLAG_HAIR_FRONT) != 0 {
        flags |= AETHER_MAT_FLAG_HAIR_FRONT_STENCIL;
    }
    if (submesh_flags_or & AETHER_SUBMESH_FLAG_FACE) != 0 {
        flags |= AETHER_MAT_FLAG_RECEIVES_HAIR_SHADOW;
    }
    if raw.outline_screen_space {
        flags |= AETHER_MAT_FLAG_OUTLINE_SCREEN;
    }
    if raw.unlit {
        flags |= AETHER_MAT_FLAG_UNLIT;
    }
    if raw.has_vrm_mtoon {
        flags |= AETHER_MAT_FLAG_HAS_EXT_VRM;
    }
    if let Some(profile) = raw.shader_profile {
        flags |= AETHER_MAT_FLAG_HAS_PROFILE | ((profile & 0x7) << AETHER_MAT_FLAG_PROFILE_SHIFT);
        if profile == AETHER_MAT_PROFILE_EYE_CAUSTIC {
            flags |= AETHER_MAT_FLAG_EYE_IRIS;
        } else if profile == AETHER_MAT_PROFILE_SKIN_SDF {
            if (submesh_flags_or & AETHER_SUBMESH_FLAG_FACE) != 0 {
                flags |= AETHER_MAT_FLAG_SDF_FACE | AETHER_MAT_FLAG_RECEIVES_HAIR_SHADOW;
            } else {
                flags |= AETHER_MAT_FLAG_BODY_SKIN;
            }
        }
    }

    let opt = |idx: Option<u32>| idx.unwrap_or(AETHER_INVALID_INDEX);

    AetherNprMaterial {
        base_color: raw.base_color,
        shade_color_gi: [shade[0], shade[1], shade[2], raw.gi_equalization],
        rim_color_mix: [
            raw.rim_color[0],
            raw.rim_color[1],
            raw.rim_color[2],
            raw.rim_lighting_mix,
        ],
        outline_color_mix: [
            raw.outline_color[0],
            raw.outline_color[1],
            raw.outline_color[2],
            raw.outline_lighting_mix,
        ],
        emissive_and_pad: [
            raw.emissive[0],
            raw.emissive[1],
            raw.emissive[2],
            raw.matcap_strength,
        ],
        matcap_color: [
            raw.matcap_color[0],
            raw.matcap_color[1],
            raw.matcap_color[2],
            raw.matcap_add_mix,
        ],
        toon_params: [
            raw.shading_shift,
            raw.shading_toony,
            raw.rim_fresnel_power,
            raw.rim_lift,
        ],
        uv_outline_alpha: [
            raw.uv_scroll[0],
            raw.uv_scroll[1],
            raw.uv_rotation,
            raw.alpha_cutoff,
        ],
        outline_width: raw.outline_width,
        alpha_mode: raw.alpha_mode,
        render_queue_offset: raw.render_queue_offset,
        flags,
        tex_base: opt(raw.tex_base),
        tex_shade: opt(raw.tex_shade),
        tex_shading_shift: opt(raw.tex_shading_shift),
        tex_normal: opt(raw.tex_normal),
        tex_emissive: opt(raw.tex_emissive),
        tex_rim: opt(raw.tex_rim),
        tex_matcap: opt(raw.tex_matcap),
        tex_outline_width: opt(raw.tex_outline_width),
        tex_uv_mask: opt(raw.tex_uv_mask),
        tex_ramp: opt(raw.tex_ramp),
        tex_sdf_face: opt(raw.tex_sdf_face),
        name_offset,
    }
}

/// Computes `AETHER_SUBMESH_FLAG_*` for one submesh from geometry, head weights, centroid & material.
#[must_use]
pub fn classify_submesh_flags(
    name: &str,
    mat_alpha_mode: u32,
    is_face: bool,
    is_eye_hint: bool,
    centroid: [f32; 3],
    eye_level_y: f32,
    z_front: f32,
) -> u32 {
    let mut flags = 0u32;
    let lower = name.to_ascii_lowercase();
    let is_hair = lower.contains("hair") || name.contains("髪");
    if is_face {
        flags |= AETHER_SUBMESH_FLAG_FACE;
    } else if is_eye_hint {
        flags |= AETHER_SUBMESH_FLAG_EYE_OR_BROW;
    } else if is_hair {
        let is_front = lower.contains("front")
            || lower.contains("bang")
            || name.contains("前")
            || (centroid[2] >= z_front - 0.08 && centroid[1] >= eye_level_y - 0.02);
        if is_front {
            flags |= AETHER_SUBMESH_FLAG_HAIR_FRONT;
        } else {
            flags |= AETHER_SUBMESH_FLAG_HAIR_BACK;
        }
    }

    match mat_alpha_mode {
        AETHER_ALPHA_MODE_MASK => flags |= AETHER_SUBMESH_FLAG_ALPHA_CUTOUT,
        AETHER_ALPHA_MODE_BLEND => flags |= AETHER_SUBMESH_FLAG_TRANSPARENT,
        _ => flags |= AETHER_SUBMESH_FLAG_OPAQUE,
    }
    flags
}
