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
    primed_ = false;
}

bool ExpressionMixer::is_mouth_arkit_slot(size_t slot) noexcept {
    // Appendix B: 14=jawForward..17=jawOpen, 18=mouthClose..40=mouthUpperUpRight, 46=cheekPuff, 51=tongueOut
    return (slot >= 14U && slot <= 40U) || slot == 46U || slot == 51U;
}

void ExpressionMixer::update(const float arkit_cam[52], const AetherDspSyncPacket& dsp,
                             const AetherGraphOutputs& graph_out, float dt_s, float out_morphs[kMaxMorphs],
                             float& out_mouth_owner_blend, const float* webcam_visemes_5) noexcept {
    const float safe_dt = std::clamp(dt_s, 1e-4f, 0.1f);
    // Warudo per-channel SmoothDamp on the camera signals (before ownership attenuation so the 60 ms DSP hand-off
    // stays exact): eye blinks 28 ms (snappy), mouth shapes + webcam vowels 38 ms; first frame snaps.
    const SmoothTimes st{};
    for (size_t i = 0; i < kArkitSlotCount; ++i) {
        const float target = std::clamp(arkit_cam[i], 0.0f, 1.0f);
        if (!primed_) {
            cam_val_[i] = target;
            cam_vel_[i] = 0.0f;
        } else if (i == 0U || i == 7U) {
            cam_val_[i] = smooth_damp(cam_val_[i], target, cam_vel_[i], st.blink, safe_dt);
        } else if (is_mouth_arkit_slot(i)) {
            cam_val_[i] = smooth_damp(cam_val_[i], target, cam_vel_[i], st.mouth, safe_dt);
        } else {
            cam_val_[i] = target;
        }
    }
    for (size_t v = 0; v < kVisemeCount; ++v) {
        const float target = (webcam_visemes_5 != nullptr) ? std::clamp(webcam_visemes_5[v], 0.0f, 1.0f) : 0.0f;
        vowel_val_[v] = primed_ ? smooth_damp(vowel_val_[v], target, vowel_vel_[v], st.mouth, safe_dt) : target;
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

    // DSP 5-viseme slots (`aa, ih, ou, ee, oh`) placed cleanly at indices [52..56] weighted by mouth_owner_blend_
    for (size_t v = 0; v < kVisemeCount; ++v) {
        // VRM `aa/ih/ou/ee/oh` presets live only in these slots: drive them from the webcam mouth shape too,
        // taking the stronger of webcam and DSP so VRM 0.x mouths move without a microphone.
        const float dsp_v = std::clamp(dsp.dsp_viseme_weights[v], 0.0f, 1.0f) * mouth_owner_blend_;
        out_morphs[kVisemeSlotOffset + v] = std::clamp(std::max(vowel_val_[v], dsp_v), 0.0f, 1.0f);
    }

    // Any remaining custom expression override slots (`52..63` in graph_out -> placed at `57..68` for custom
    // expressions)
    for (size_t s = kArkitSlotCount; s < kMaxExpressionSlots; ++s) {
        const size_t dst = kVisemeSlotOffset + kVisemeCount + (s - kArkitSlotCount);
        const float go = std::clamp(graph_out.expression_override[s], 0.0f, 1.0f);
        if (go > 0.0f) {
            out_morphs[dst] = std::clamp(graph_out.expression_weights[s] * go, 0.0f, 1.0f);
        } else {
            out_morphs[dst] = 0.0f;
        }
    }
}

} // namespace aether::animation
