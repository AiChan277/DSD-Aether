#include "animation/expression_mixer.hpp"
#include "animation/motion_filters.hpp"

#include <algorithm>
#include <cmath>

namespace aether::animation {

void ExpressionMixer::reset() noexcept {
    mouth_owner_blend_ = 0.0f;
    smoothed_overrides_.fill(0.0f);
    cam_val_.fill(0.0f);
    cam_vel_.fill(0.0f);
    vowel_val_.fill(0.0f);
    vowel_vel_.fill(0.0f);
    live2d_val_.fill(0.0f);
    live2d_vel_.fill(0.0f);
    smooth_open_y_ = 0.0f;
    open_y_vel_ = 0.0f;
    primed_ = false;
}

bool ExpressionMixer::is_mouth_arkit_slot(size_t slot) noexcept {
    // Appendix B: 14=jawForward..17=jawOpen, 18=mouthClose..40=mouthUpperUpRight, 46=cheekPuff, 51=tongueOut
    return (slot >= 14U && slot <= 40U) || slot == 46U || slot == 51U;
}

bool ExpressionMixer::is_brow_arkit_slot(size_t slot) noexcept {
    // Appendix B: 41=browDownLeft, 42=browDownRight, 43=browInnerUp, 44=browOuterUpLeft, 45=browOuterUpRight
    return slot >= 41U && slot <= 45U;
}

void ExpressionMixer::update(const float arkit_cam[52], const AetherDspSyncPacket& dsp,
                             const AetherGraphOutputs& graph_out, float dt_s, float out_morphs[kMaxMorphs],
                             float& out_mouth_owner_blend, const float* webcam_visemes_5, bool has_split_brow_morphs,
                             const Live2DParameters* in_live2d, Live2DParameters* out_live2d,
                             const AudioLipSyncFrame* in_audio, float audio_assist_weight) noexcept {
    const float safe_dt = std::clamp(dt_s, 1e-4f, 0.1f);
    const SmoothTimes st{};
    constexpr float kBrowDeadzone = 0.015f;
    const bool audio_fusion = (in_audio != nullptr && in_audio->assist_active && in_audio->device_available);
    const bool audio_vad = audio_fusion && (in_audio->vad > 0.0f);
    const float cam_open_y_raw =
        std::clamp((in_live2d != nullptr) ? in_live2d->param_mouth_open_y : arkit_cam[17], 0.0f, 1.0f);
    const float cam_form_raw =
        std::clamp((in_live2d != nullptr) ? in_live2d->param_mouth_form
                                         : (0.5f * (arkit_cam[23] + arkit_cam[24]) - arkit_cam[20]),
                   -1.0f, 1.0f);
    const bool silence_lock_mouth = audio_fusion && !audio_vad && (cam_open_y_raw < 0.22f);
    const bool silent_smile_preserved = silence_lock_mouth && (cam_form_raw > 0.20f);

    // Warudo per-channel SmoothDamp on camera signals (blink 28 ms, mouth/vowels 38-45 ms, brows 45 ms + 0.015 deadzone)
    for (size_t i = 0; i < kArkitSlotCount; ++i) {
        float target = std::clamp(arkit_cam[i], 0.0f, 1.0f);
        if (is_brow_arkit_slot(i)) {
            target = (target < kBrowDeadzone) ? 0.0f : target;
        }
        if (silence_lock_mouth && is_mouth_arkit_slot(i)) {
            // Suppress closed-mouth webcam jitter while preserving silent closed-lip smiles (slots 23/24)
            if (!(silent_smile_preserved && (i == 23U || i == 24U))) {
                target = 0.0f;
            }
        }
        if (!primed_) {
            cam_val_[i] = target;
            cam_vel_[i] = 0.0f;
        } else if (i == 0U || i == 7U) {
            const float tau_blink = (in_live2d != nullptr) ? 0.006f : st.blink;
            cam_val_[i] = smooth_damp(cam_val_[i], target, cam_vel_[i], tau_blink, safe_dt);
            if (target <= 0.0f && cam_val_[i] < 1e-4f) {
                cam_val_[i] = 0.0f;
                cam_vel_[i] = 0.0f;
            }
        } else if ((i >= 1U && i <= 4U) || (i >= 8U && i <= 11U)) {
            cam_val_[i] =
                (in_live2d != nullptr) ? smooth_damp(cam_val_[i], target, cam_vel_[i], 0.006f, safe_dt) : target;
        } else if (is_brow_arkit_slot(i)) {
            cam_val_[i] = smooth_damp(cam_val_[i], target, cam_vel_[i], st.brow, safe_dt);
        } else if (is_mouth_arkit_slot(i)) {
            cam_val_[i] = smooth_damp(cam_val_[i], target, cam_vel_[i], st.mouth, safe_dt);
            if (silence_lock_mouth && target <= 0.0f && cam_val_[i] < 1e-4f) {
                cam_val_[i] = 0.0f;
                cam_vel_[i] = 0.0f;
            }
        } else {
            cam_val_[i] = target;
        }
    }
    if (!audio_fusion) {
        for (size_t v = 0; v < kVisemeCount; ++v) {
            const float target = (webcam_visemes_5 != nullptr) ? std::clamp(webcam_visemes_5[v], 0.0f, 1.0f) : 0.0f;
            vowel_val_[v] = primed_ ? smooth_damp(vowel_val_[v], target, vowel_vel_[v], st.mouth, safe_dt) : target;
        }
    }

    if (in_live2d != nullptr) {
        const auto dz_brow = [](float v) noexcept -> float {
            const float c = std::clamp(std::isfinite(v) ? v : 0.0f, -1.0f, 1.0f);
            return (std::abs(c) < kBrowDeadzone) ? 0.0f : c;
        };
        const std::array<float, 10> l2d_targets{
            dz_brow(in_live2d->param_brow_l_y),
            dz_brow(in_live2d->param_brow_r_y),
            dz_brow(in_live2d->param_brow_l_angle),
            dz_brow(in_live2d->param_brow_r_angle),
            dz_brow(in_live2d->param_brow_l_form),
            dz_brow(in_live2d->param_brow_r_form),
            cam_open_y_raw,
            cam_form_raw,
            std::clamp(in_live2d->param_eye_ball_x, -1.0f, 1.0f),
            std::clamp(in_live2d->param_eye_ball_y, -1.0f, 1.0f),
        };
        for (size_t k = 0; k < l2d_targets.size(); ++k) {
            if (!primed_) {
                live2d_val_[k] = l2d_targets[k];
                live2d_vel_[k] = 0.0f;
            } else {
                const float tau = (k < 6U) ? st.brow : ((k < 8U) ? st.mouth : 0.006f);
                live2d_val_[k] = smooth_damp(live2d_val_[k], l2d_targets[k], live2d_vel_[k], tau, safe_dt);
            }
        }
        // Map smoothed Live2D brow & mouth manifold into ARKit slots + 5-vowel channels
        const float brow_l_y = live2d_val_[0];
        const float brow_r_y = live2d_val_[1];
        const float brow_l_ang = live2d_val_[2];
        const float brow_r_ang = live2d_val_[3];
        cam_val_[41] = std::max(cam_val_[41], std::clamp(-brow_l_y, 0.0f, 1.0f));
        cam_val_[42] = std::max(cam_val_[42], std::clamp(-brow_r_y, 0.0f, 1.0f));
        cam_val_[43] = std::max(
            cam_val_[43],
            std::clamp(0.5f * (std::max(0.0f, brow_l_y) + std::max(0.0f, brow_r_y)) +
                           0.25f * std::max(0.0f, brow_l_ang + brow_r_ang),
                       0.0f, 1.0f));
        cam_val_[44] = std::max(
            cam_val_[44], std::clamp(std::max(0.0f, brow_l_y) - 0.35f * std::max(0.0f, brow_l_ang), 0.0f, 1.0f));
        cam_val_[45] = std::max(
            cam_val_[45], std::clamp(std::max(0.0f, brow_r_y) - 0.35f * std::max(0.0f, brow_r_ang), 0.0f, 1.0f));

        if (!audio_fusion) {
            const float open_y = live2d_val_[6];
            const float form = live2d_val_[7];
            cam_val_[17] = std::max(cam_val_[17], open_y);
            std::array<float, kVisemeCount> l2d_vowels{};
            solve_live2d_vowels(form, open_y, l2d_vowels[0], l2d_vowels[1], l2d_vowels[2], l2d_vowels[3],
                                l2d_vowels[4]);
            for (size_t v = 0; v < kVisemeCount; ++v) {
                vowel_val_[v] = std::max(vowel_val_[v], l2d_vowels[v]);
            }
            const float smile_pos = std::clamp(std::max(0.0f, form) * 0.45f, 0.0f, 1.0f);
            cam_val_[23] = std::max(cam_val_[23], smile_pos);
            cam_val_[24] = std::max(cam_val_[24], smile_pos);
            cam_val_[47] = std::max(cam_val_[47], smile_pos * 0.65f);
            cam_val_[48] = std::max(cam_val_[48], smile_pos * 0.65f);
        }
    }

    if (audio_fusion) {
        // Task 4 Hybrid Camera + Audio Lip-Sync Fusion:
        // Rule 1 (Silence Lock): !vad && Cam_OpenY < 0.22 -> gate Final_OpenY -> 0.0 (preserving >= 0.25 yawns & smiles)
        // Rule 2 (Speech OpenY): clamp(0.45 * Cam_OpenY + 0.65 * Audio_OpenY, 0, 1) w/ 25 ms attack / 65 ms release
        // Rule 3 (Vowels): lerp(Cam_Vowel, Audio_Vowel, Audio_Assist_Weight) w/ 45 ms SmoothDamp, < 0.03 -> 0, sum <= 1
        float target_open_y = 0.0f;
        if (audio_vad) {
            const float audio_open = std::clamp(in_audio->audio_open_y, 0.0f, 1.0f);
            target_open_y = std::clamp(0.45f * cam_open_y_raw + 0.65f * audio_open, 0.0f, 1.0f);
        } else if (cam_open_y_raw >= 0.22f) {
            const float u = std::clamp((cam_open_y_raw - 0.22f) / 0.03f, 0.0f, 1.0f);
            target_open_y = cam_open_y_raw * (u * u * (3.0f - 2.0f * u));
        }

        const float tau_open = (target_open_y > smooth_open_y_) ? 0.025f : 0.065f;
        smooth_open_y_ = primed_ ? smooth_damp(smooth_open_y_, target_open_y, open_y_vel_, tau_open, safe_dt)
                                 : target_open_y;
        if (target_open_y <= 0.0f && smooth_open_y_ < 1e-4f) {
            smooth_open_y_ = 0.0f;
            open_y_vel_ = 0.0f;
        }
        cam_val_[17] = smooth_open_y_;
        if (in_live2d != nullptr) {
            live2d_val_[6] = smooth_open_y_;
        }

        // Preserve subtle smile corners / cheek squint from Cam_MouthForm even under Silence Lock
        const float form_s = (in_live2d != nullptr) ? live2d_val_[7] : cam_form_raw;
        const float smile_pos = std::clamp(std::max(0.0f, form_s) * 0.45f, 0.0f, 1.0f);
        cam_val_[23] = std::max(cam_val_[23], smile_pos);
        cam_val_[24] = std::max(cam_val_[24], smile_pos);
        cam_val_[47] = std::max(cam_val_[47], smile_pos * 0.65f);
        cam_val_[48] = std::max(cam_val_[48], smile_pos * 0.65f);

        std::array<float, kVisemeCount> cam_vowels{};
        if (in_live2d != nullptr || webcam_visemes_5 == nullptr) {
            solve_live2d_vowels(cam_form_raw, cam_open_y_raw, cam_vowels[0], cam_vowels[1], cam_vowels[2],
                                cam_vowels[3], cam_vowels[4]);
        } else {
            for (size_t v = 0; v < kVisemeCount; ++v) {
                cam_vowels[v] = std::clamp(webcam_visemes_5[v], 0.0f, 1.0f);
            }
        }

        std::array<float, kVisemeCount> target_vowels{};
        if (audio_vad) {
            const float audio_open = std::clamp(in_audio->audio_open_y, 0.0f, 1.0f);
            const float aw = std::clamp(audio_assist_weight, 0.0f, 1.0f);
            float max_w = 0.0f;
            for (size_t v = 0; v < kVisemeCount; ++v) {
                const float av = std::clamp(in_audio->vowel_probs[v], 0.0f, 1.0f) * audio_open;
                target_vowels[v] = cam_vowels[v] * (1.0f - aw) + av * aw;
                max_w = std::max(max_w, target_vowels[v]);
            }
            // Soft Winner-Take-Most focus while preserving dominant amplitude
            if (max_w > 1e-4f) {
                for (size_t v = 0; v < kVisemeCount; ++v) {
                    const float rel = target_vowels[v] / max_w;
                    target_vowels[v] *= (0.45f + 0.55f * rel);
                }
            }
        } else if (cam_open_y_raw >= 0.22f) {
            const float scale = target_open_y / std::max(cam_open_y_raw, 1e-4f);
            for (size_t v = 0; v < kVisemeCount; ++v) {
                target_vowels[v] = cam_vowels[v] * scale;
            }
        }

        for (size_t v = 0; v < kVisemeCount; ++v) {
            vowel_val_[v] = primed_ ? smooth_damp(vowel_val_[v], target_vowels[v], vowel_vel_[v], 0.045f, safe_dt)
                                    : target_vowels[v];
            if (target_vowels[v] <= 0.0f && vowel_val_[v] < 1e-4f) {
                vowel_val_[v] = 0.0f;
                vowel_vel_[v] = 0.0f;
            }
        }
    } else {
        smooth_open_y_ = cam_val_[17];
        open_y_vel_ = 0.0f;
    }
    primed_ = true;

    // Appendix C: 60 ms mouth ownership handoff between camera and DSP visemes
    const float target_owner = ((dsp.dsp_flags & 0x05U) == 0x05U) ? 1.0f : 0.0f;
    const float max_delta = safe_dt / 0.060f;
    if (target_owner > mouth_owner_blend_) {
        mouth_owner_blend_ = std::min(target_owner, mouth_owner_blend_ + max_delta);
    } else if (target_owner < mouth_owner_blend_) {
        mouth_owner_blend_ = std::max(target_owner, mouth_owner_blend_ - max_delta);
    }
    out_mouth_owner_blend = mouth_owner_blend_;

    // Combine camera ARKit-52 with mouth-ownership attenuator and Rust graph expression overrides
    for (size_t i = 0; i < kArkitSlotCount; ++i) {
        float val = std::clamp(cam_val_[i], 0.0f, 1.0f);
        if (is_mouth_arkit_slot(i)) {
            val *= (1.0f - mouth_owner_blend_);
        }
        const float gw = graph_out.expression_weights[i];
        const float go = graph_out.expression_override[i];
        if (go > 0.0f) {
            const float target = std::clamp(gw, 0.0f, 1.0f);
            val = val * (1.0f - std::clamp(go, 0.0f, 1.0f)) + target * std::clamp(go, 0.0f, 1.0f);
        }
        out_morphs[i] = std::clamp(val, 0.0f, 1.0f);
    }

    // 5-viseme slots (`aa, ih, ou, ee, oh`) placed cleanly at indices [52..56] weighted by mouth_owner_blend_
    float vowel_sum = 0.0f;
    for (size_t v = 0; v < kVisemeCount; ++v) {
        const float dsp_v = std::clamp(dsp.dsp_viseme_weights[v], 0.0f, 1.0f) * mouth_owner_blend_;
        float vis_v = audio_fusion ? ((vowel_val_[v] < 0.03f) ? 0.0f : vowel_val_[v]) * (1.0f - mouth_owner_blend_)
                                   : vowel_val_[v];
        const float out_v = std::clamp(std::max(vis_v, dsp_v), 0.0f, 1.0f);
        out_morphs[kVisemeSlotOffset + v] = out_v;
        vowel_sum += out_v;
    }
    if (audio_fusion && vowel_sum > 1.0f) {
        const float inv = 1.0f / vowel_sum;
        for (size_t v = 0; v < kVisemeCount; ++v) {
            out_morphs[kVisemeSlotOffset + v] *= inv;
        }
    }

    // Any remaining custom expression override slots (`52..63` in graph_out -> placed at `57..68` for custom
    // expressions)
    float emotion_from_override = 0.0f;
    for (size_t s = kArkitSlotCount; s < kMaxExpressionSlots; ++s) {
        const size_t dst = kVisemeSlotOffset + kVisemeCount + (s - kArkitSlotCount);
        const float go = std::clamp(graph_out.expression_override[s], 0.0f, 1.0f);
        if (go > 0.0f) {
            out_morphs[dst] = std::clamp(graph_out.expression_weights[s] * go, 0.0f, 1.0f);
            if (dst >= 57U && dst <= 60U) {
                emotion_from_override = std::max(emotion_from_override, out_morphs[dst]);
            }
        } else {
            out_morphs[dst] = 0.0f;
        }
    }

    // Fallback subtle VRM preset brow blending (`58 Angry`, `59 Sorrow`, `60 Surprised`) ONLY when the avatar
    // has no split brow morphs bound to ARKit slots `41..45`, without overriding closed mouth or tracked blinks.
    if (!has_split_brow_morphs) {
        const float eyelid_guard = 1.0f - std::clamp(std::max(out_morphs[0], out_morphs[7]), 0.0f, 1.0f);
        const float angry_brow = std::max(out_morphs[41], out_morphs[42]) * 0.40f * eyelid_guard;
        const float surprised_brow = std::max(out_morphs[44], out_morphs[45]) * 0.40f * eyelid_guard;
        const float sorrow_brow = (surprised_brow < 0.12f) ? (out_morphs[43] * 0.35f * eyelid_guard) : 0.0f;
        out_morphs[58] = std::clamp(std::max(out_morphs[58], angry_brow), 0.0f, 1.0f);
        out_morphs[59] = std::clamp(std::max(out_morphs[59], sorrow_brow), 0.0f, 1.0f);
        out_morphs[60] = std::clamp(std::max(out_morphs[60], surprised_brow), 0.0f, 1.0f);
    }

    // VRM "overrideBlink" behaviour: explicit emotion presets (57..60) reshape the eyes, so tracked blink fades out
    // while active; an explicit graph/hotkey override on a blink slot (e.g. F2 Wink) is left untouched.
    if (emotion_from_override > 0.0f) {
        for (size_t b : {size_t{0U}, size_t{7U}}) {
            if (!(graph_out.expression_override[b] > 0.0f)) {
                out_morphs[b] *= (1.0f - std::clamp(emotion_from_override, 0.0f, 1.0f));
            }
        }
    }

    if (out_live2d != nullptr) {
        if (in_live2d != nullptr) {
            out_live2d->param_brow_l_y = live2d_val_[0];
            out_live2d->param_brow_r_y = live2d_val_[1];
            out_live2d->param_brow_l_angle = live2d_val_[2];
            out_live2d->param_brow_r_angle = live2d_val_[3];
            out_live2d->param_brow_l_form = live2d_val_[4];
            out_live2d->param_brow_r_form = live2d_val_[5];
            out_live2d->param_mouth_open_y = live2d_val_[6];
            out_live2d->param_mouth_form = live2d_val_[7];
            out_live2d->param_eye_ball_x = live2d_val_[8];
            out_live2d->param_eye_ball_y = live2d_val_[9];
        } else {
            out_live2d->param_brow_l_y = std::clamp(0.6f * out_morphs[43] + 0.6f * out_morphs[44] - out_morphs[41], -1.0f, 1.0f);
            out_live2d->param_brow_r_y = std::clamp(0.6f * out_morphs[43] + 0.6f * out_morphs[45] - out_morphs[42], -1.0f, 1.0f);
            out_live2d->param_mouth_open_y = std::clamp(out_morphs[17], 0.0f, 1.0f);
            out_live2d->param_mouth_form = std::clamp(0.5f * (out_morphs[23] + out_morphs[24]) - out_morphs[20], -1.0f, 1.0f);
        }
        out_live2d->param_eye_l_open = std::clamp(1.0f - out_morphs[0], 0.0f, 1.0f);
        out_live2d->param_eye_r_open = std::clamp(1.0f - out_morphs[7], 0.0f, 1.0f);
        out_live2d->vowel_a = out_morphs[kVisemeSlotOffset + 0];
        out_live2d->vowel_i = out_morphs[kVisemeSlotOffset + 1];
        out_live2d->vowel_u = out_morphs[kVisemeSlotOffset + 2];
        out_live2d->vowel_e = out_morphs[kVisemeSlotOffset + 3];
        out_live2d->vowel_o = out_morphs[kVisemeSlotOffset + 4];
    }
}

} // namespace aether::animation
