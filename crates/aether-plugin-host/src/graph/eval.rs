//! Allocation-free node graph evaluation VM (`REQ-PLG-002`) and lock-free WASM latched bridge (§7.3).

use std::time::Instant;

use aether_ffi::{
    AETHER_DYN_SIG_CIRCADIAN, AETHER_DYN_SIG_EXPRESSION, AETHER_DYN_SIG_PIN,
    AETHER_DYN_TGT_BLUSH_ALPHA, AETHER_DYN_TGT_RIM_BOOST, AETHER_INVALID_INDEX,
    AETHER_MAT_OVR_BLUSH_ALPHA, AETHER_MAT_OVR_EMISSIVE, AETHER_MAT_OVR_GRADE,
    AETHER_MAT_OVR_RIM_BOOST, AETHER_MAT_OVR_TEXTURE, AETHER_MAX_MATERIAL_OVERRIDES,
    AETHER_PIN_VALUE_MAT_OVERRIDE, AETHER_PIN_VALUE_MAT_TABLE, AETHER_PIN_VALUE_TEX_HANDLE,
    AetherGraphOutputs, AetherMaterialOverrideBuffer, AetherMaterialOverrideEntry,
    AetherTrackingFrame,
};

use crate::events::SharedParamBank;
use crate::graph::compiler::{
    AudioFrameData, CompareOp, CompiledGraphPlan, ExprSetData, MaterialOverrideData, Op, PoseData,
    ValuesArena,
};

/// Number of WASM node instances supported concurrently (`§7.5 <= 64`).
pub const MAX_LATCHED_INSTANCES: usize = 64;
/// Number of scalar floats exchanged per latched WASM instance per tick.
pub const LATCHED_IO_FLOATS: usize = 16;

/// Lock-free single-slot triple buffer transferring fixed `[f32; 16]` buffers between `T2` and Worker (`HR-02`).
struct AtomicFloatTripleBuffer {
    bufs: [[core::sync::atomic::AtomicU32; LATCHED_IO_FLOATS]; 3],
    shared: core::sync::atomic::AtomicU8,
    producer_idx: core::sync::atomic::AtomicU8,
    consumer_idx: core::sync::atomic::AtomicU8,
}

impl AtomicFloatTripleBuffer {
    fn new() -> Self {
        Self {
            bufs: core::array::from_fn(|_| {
                core::array::from_fn(|_| core::sync::atomic::AtomicU32::new(0.0_f32.to_bits()))
            }),
            shared: core::sync::atomic::AtomicU8::new(1),
            producer_idx: core::sync::atomic::AtomicU8::new(0),
            consumer_idx: core::sync::atomic::AtomicU8::new(2),
        }
    }

    fn write(&self, src: &[f32; LATCHED_IO_FLOATS]) {
        use core::sync::atomic::Ordering;
        let idx = usize::from(self.producer_idx.load(Ordering::Relaxed) & 0x03).min(2);
        for (dst, &val) in self.bufs[idx].iter().zip(src.iter()) {
            let clean = if val.is_finite() { val } else { 0.0 };
            dst.store(clean.to_bits(), Ordering::Relaxed);
        }
        let prev = self.shared.swap(0x80 | (idx as u8), Ordering::AcqRel);
        self.producer_idx.store(prev & 0x03, Ordering::Relaxed);
    }

    fn read(&self, out: &mut [f32; LATCHED_IO_FLOATS]) -> bool {
        use core::sync::atomic::Ordering;
        let cur = self.shared.load(Ordering::Acquire);
        if cur & 0x80 != 0 {
            let old = self.consumer_idx.load(Ordering::Relaxed) & 0x03;
            let swapped = self.shared.swap(old, Ordering::AcqRel) & 0x03;
            self.consumer_idx.store(swapped, Ordering::Relaxed);
        }
        let idx = usize::from(self.consumer_idx.load(Ordering::Relaxed) & 0x03).min(2);
        for (dst, src) in out.iter_mut().zip(self.bufs[idx].iter()) {
            *dst = f32::from_bits(src.load(Ordering::Relaxed));
        }
        (cur & 0x80) != 0
    }
}

/// Lock-free wait-free per-instance triple-buffered I/O bridge between `T2` (`Op::Latched`) and async WASM plugins (§7.3).
pub struct LatchedBridge {
    inputs: Vec<AtomicFloatTripleBuffer>,
    outputs: Vec<AtomicFloatTripleBuffer>,
    active_flags: [core::sync::atomic::AtomicBool; MAX_LATCHED_INSTANCES],
}

impl LatchedBridge {
    /// Allocates all 64 triple-buffered slots upfront at `PluginHost::create`.
    #[must_use]
    pub fn new() -> Self {
        let mut inputs = Vec::with_capacity(MAX_LATCHED_INSTANCES);
        let mut outputs = Vec::with_capacity(MAX_LATCHED_INSTANCES);
        for _ in 0..MAX_LATCHED_INSTANCES {
            inputs.push(AtomicFloatTripleBuffer::new());
            outputs.push(AtomicFloatTripleBuffer::new());
        }
        Self {
            inputs,
            outputs,
            active_flags: core::array::from_fn(|_| core::sync::atomic::AtomicBool::new(false)),
        }
    }

    /// Marks whether a latched plugin instance slot is active.
    pub fn set_active(&self, slot: usize, active: bool) {
        if let Some(flag) = self.active_flags.get(slot) {
            flag.store(active, core::sync::atomic::Ordering::Release);
        }
    }

    /// `T2` side: publishes latest inputs and reads latest asynchronously produced plugin outputs without blocking.
    pub fn exchange_t2(
        &self,
        slot: usize,
        in_floats: &[f32; LATCHED_IO_FLOATS],
        out_floats: &mut [f32; LATCHED_IO_FLOATS],
    ) {
        if let (Some(i_buf), Some(o_buf)) = (self.inputs.get(slot), self.outputs.get(slot)) {
            i_buf.write(in_floats);
            let _ = o_buf.read(out_floats);
        }
    }

    /// Worker side: fetches latest `T2` inputs if active.
    #[must_use]
    pub fn read_worker_input(&self, slot: usize, out_in: &mut [f32; LATCHED_IO_FLOATS]) -> bool {
        if self
            .active_flags
            .get(slot)
            .is_some_and(|f| f.load(core::sync::atomic::Ordering::Acquire))
        {
            if let Some(buf) = self.inputs.get(slot) {
                return buf.read(out_in);
            }
        }
        false
    }

    /// Worker side: publishes computed plugin outputs back to `T2`.
    pub fn write_worker_output(&self, slot: usize, out_floats: &[f32; LATCHED_IO_FLOATS]) {
        if let Some(buf) = self.outputs.get(slot) {
            buf.write(out_floats);
        }
    }
}

impl Default for LatchedBridge {
    fn default() -> Self {
        Self::new()
    }
}

/// Safe borrowed inputs passed from the FFI boundary or tests to graph evaluation.
pub struct SafeGraphInputs<'a> {
    /// Borrowed tracking frame if provided by C++.
    pub tracking: Option<&'a AetherTrackingFrame>,
    /// Current QPC timestamp (us).
    pub now_us: u64,
    /// Tick delta time (seconds).
    pub dt_s: f32,
    /// Monotonic tick counter.
    pub tick_index: u32,
    /// Latest DSP viseme weights `A, I, U, E, O`.
    pub dsp_visemes: [f32; 5],
    /// Latest DSP input level (dBFS).
    pub dsp_rms_db: f32,
    /// Held key bitset.
    pub keys_down: [u64; 2],
    /// Newly pressed key bitset.
    pub keys_pressed: [u64; 2],
    /// External parameter slots.
    pub external_params: [f32; 64],
}

/// Optional host-level overrides mixed cleanly during [`CompiledGraphPlan::evaluate_with_host`].
pub struct HostEvalContext<'a> {
    /// Shared lock-free parameter / hotkey / expression override bank.
    pub params: &'a SharedParamBank,
    /// Lock-free triple-buffered WASM instance I/O bridge.
    pub latched: &'a LatchedBridge,
    /// Single-tick external hotkeys latched for this evaluation.
    pub pending_hotkeys: [u64; 2],
}

#[inline]
fn xorshift64(s: &mut u64) -> f32 {
    let mut x = *s;
    if x == 0 {
        x = 0x9E37_79B9_7F4A_7C15;
    }
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    ((x >> 40) as f32) / ((1_u64 << 24) as f32)
}

#[inline]
fn key_is_set(bits: [u64; 2], key_code: u32) -> bool {
    let idx = usize::try_from(key_code).unwrap_or(usize::MAX);
    if idx < 64 {
        (bits[0] & (1_u64 << idx)) != 0
    } else if idx < 128 {
        (bits[1] & (1_u64 << (idx - 64))) != 0
    } else {
        false
    }
}

#[inline]
fn vec3_norm(v: [f32; 3]) -> [f32; 3] {
    let len_sq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if len_sq <= 1e-12 {
        [0.0, 0.0, 1.0]
    } else {
        let inv = len_sq.sqrt().recip();
        [v[0] * inv, v[1] * inv, v[2] * inv]
    }
}

#[inline]
fn quat_slerp_norm(a: [f32; 4], b: [f32; 4], t: f32) -> [f32; 4] {
    let dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    let sign = if dot < 0.0 { -1.0 } else { 1.0 };
    let r = [
        a[0] + (b[0] * sign - a[0]) * t,
        a[1] + (b[1] * sign - a[1]) * t,
        a[2] + (b[2] * sign - a[2]) * t,
        a[3] + (b[3] * sign - a[3]) * t,
    ];
    let len_sq = r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3];
    if len_sq <= 1e-12 {
        [0.0, 0.0, 0.0, 1.0]
    } else {
        let inv = len_sq.sqrt().recip();
        [r[0] * inv, r[1] * inv, r[2] * inv, r[3] * inv]
    }
}

/// Humanoid bone (Appendix A) feeding each `AetherGraphOutputs::ik_targets` slot `0..6`.
const IK_SLOT_BONES: [usize; 6] = [5, 20, 24, 11, 15, 0];

#[inline]
fn quat_conj(q: [f32; 4]) -> [f32; 4] {
    [-q[0], -q[1], -q[2], q[3]]
}

/// Hamilton product `a * b` (xyzw), normalized; degenerate input collapses to identity.
#[inline]
fn quat_mul_norm(a: [f32; 4], b: [f32; 4]) -> [f32; 4] {
    let r = [
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    ];
    let len_sq = r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3];
    if !len_sq.is_finite() || len_sq <= 1e-12 {
        [0.0, 0.0, 0.0, 1.0]
    } else {
        let inv = len_sq.sqrt().recip();
        let s = if r[3] < 0.0 { -inv } else { inv };
        [r[0] * s, r[1] * s, r[2] * s, r[3] * s]
    }
}

#[inline]
fn push_event(outputs: &mut AetherGraphOutputs, event_id: u32) {
    let idx = usize::try_from(outputs.event_count).unwrap_or(16);
    if idx < outputs.events.len() {
        outputs.events[idx] = event_id;
        outputs.event_count = outputs.event_count.saturating_add(1);
    }
}

#[inline]
fn eval_op(
    op: &Op,
    arena: &mut ValuesArena,
    states: &mut [crate::graph::compiler::NodeState],
    inputs: &SafeGraphInputs<'_>,
    host: Option<&HostEvalContext<'_>>,
    circadian_hour: f32,
    outputs: &mut AetherGraphOutputs,
) -> f32 {
    use crate::graph::compiler::NodeState;
    let dt = if inputs.dt_s.is_finite() && inputs.dt_s > 0.0 {
        inputs.dt_s.min(0.1)
    } else {
        1.0 / 60.0
    };

    match *op {
        Op::CopyFloat { src, dst } => {
            arena.floats[dst] = arena.floats[src];
            arena.floats[dst]
        }
        Op::CopyBool { src, dst } => {
            arena.bools[dst] = arena.bools[src];
            arena.bools[dst] as f32
        }
        Op::CopyEvent { src, dst } => {
            arena.events[dst] = arena.events[src];
            arena.events[dst] as f32
        }
        Op::CopyVec3 { src, dst } => {
            arena.vec3s[dst] = arena.vec3s[src];
            arena.vec3s[dst][0]
        }
        Op::CopyQuat { src, dst } => {
            arena.quats[dst] = arena.quats[src];
            arena.quats[dst][3]
        }
        Op::CopyTransform { src, dst } => {
            arena.transforms[dst] = arena.transforms[src];
            arena.transforms[dst][0]
        }
        Op::CopyPose { src, dst } => {
            arena.poses[dst] = arena.poses[src];
            arena.poses[dst].pos[0][3]
        }
        Op::CopyExprSet { src, dst } => {
            arena.expr_sets[dst] = arena.expr_sets[src];
            arena.expr_sets[dst].weights[0]
        }
        Op::CopyVisemeSet { src, dst } => {
            arena.viseme_sets[dst] = arena.viseme_sets[src];
            arena.viseme_sets[dst][0]
        }
        Op::CopyAudioFrame { src, dst } => {
            arena.audio_frames[dst] = arena.audio_frames[src];
            arena.audio_frames[dst].env
        }
        Op::VisemeToExprSet { src, dst } => {
            let vis = arena.viseme_sets[src];
            let mut expr = ExprSetData::default();
            for (i, &w) in vis.iter().enumerate() {
                if let Some(slot) = expr.weights.get_mut(52 + i) {
                    *slot = w.clamp(0.0, 1.0);
                }
            }
            expr.weights[17] = (vis[0] * 0.85 + vis[3] * 0.45 + vis[4] * 0.65).clamp(0.0, 1.0);
            arena.expr_sets[dst] = expr;
            expr.weights[17]
        }
        Op::CopyColor { src, dst } => {
            arena.colors[dst] = arena.colors[src];
            arena.colors[dst][0]
        }
        Op::CopyMaterialTable { src, dst } => {
            arena.mat_tables[dst] = arena.mat_tables[src];
            arena.mat_tables[dst].slot_count as f32
        }
        Op::CopyMaterialOverride { src, dst } => {
            arena.mat_overrides[dst] = arena.mat_overrides[src];
            if arena.mat_overrides[dst].valid { 1.0 } else { 0.0 }
        }
        Op::CopyTextureHandle { src, dst } => {
            arena.tex_handles[dst] = arena.tex_handles[src];
            if arena.tex_handles[dst].handle != 0 { 1.0 } else { 0.0 }
        }
        Op::CastBoolToFloat { src, dst } => {
            let v = if arena.bools[src] != 0 { 1.0 } else { 0.0 };
            arena.floats[dst] = v;
            v
        }
        Op::CastFloatToBool { src, dst } => {
            let b = u32::from(arena.floats[src].abs() > 0.5);
            arena.bools[dst] = b;
            b as f32
        }
        Op::BroadcastFloatToVec3 { src, dst } => {
            let v = arena.floats[src];
            arena.vec3s[dst] = [v, v, v];
            v
        }
        Op::TransformPosToVec3 { src, dst } => {
            let xf = arena.transforms[src];
            arena.vec3s[dst] = [xf[0], xf[1], xf[2]];
            xf[0]
        }

        Op::NpuPose {
            out_pose,
            out_expr,
            out_face_conf,
            out_hand_conf,
            out_body_conf,
            out_ep_id,
        } => {
            if let Some(tr) = inputs.tracking {
                let pose = PoseData {
                    pos: tr.joint_positions_ws,
                    rot: tr.joint_rotations_quat,
                    from_tracking: true,
                };
                arena.poses[out_pose] = pose;

                let mut expr = ExprSetData::default();
                for (dst, &w) in expr.weights.iter_mut().zip(tr.arkit_blendshapes.iter()) {
                    *dst = if w.is_finite() {
                        w.clamp(0.0, 1.0)
                    } else {
                        0.0
                    };
                }
                arena.expr_sets[out_expr] = expr;
                let head_conf = tr.joint_positions_ws[5][3];
                let face_conf = if head_conf.is_finite() && head_conf > 0.0 {
                    head_conf.clamp(0.0, 1.0)
                } else {
                    tr.upper_body_confidence.clamp(0.0, 1.0)
                };
                arena.floats[out_face_conf] = if (tr.valid_flags & 1) != 0 {
                    face_conf
                } else {
                    0.0
                };
                arena.floats[out_hand_conf] = 0.0;
                arena.floats[out_body_conf] = tr.upper_body_confidence.clamp(0.0, 1.0);
                arena.floats[out_ep_id] = tr.active_execution_provider as f32;
            } else {
                arena.poses[out_pose] = PoseData::default();
                arena.expr_sets[out_expr] = ExprSetData::default();
                arena.floats[out_face_conf] = 0.0;
                arena.floats[out_hand_conf] = 0.0;
                arena.floats[out_body_conf] = 0.0;
                arena.floats[out_ep_id] = 3.0;
            }
            arena.floats[out_face_conf]
        }

        Op::DspAudio {
            out_audio,
            out_rms_db,
            out_voice_active,
        } => {
            let mut vis = [0.0_f32; 5];
            let mut vis_sum = 0.0_f32;
            for (dst, &v) in vis.iter_mut().zip(inputs.dsp_visemes.iter()) {
                let c = if v.is_finite() {
                    v.clamp(0.0, 1.0)
                } else {
                    0.0
                };
                *dst = c;
                vis_sum += c;
            }
            let rms = if inputs.dsp_rms_db.is_finite() {
                inputs.dsp_rms_db
            } else {
                -90.0
            };
            let env = ((rms + 60.0) / 60.0).clamp(0.0, 1.0);
            let voiced = rms > -50.0 || vis.iter().any(|&v| v > 0.05);
            // Derive F1/F2 formant frequencies (Hz) from vowel distribution (A/I/U/E/O centroids).
            let (f1_hz, f2_hz) = if vis_sum > 1e-4 {
                const F1_HZ: [f32; 5] = [730.0, 270.0, 300.0, 530.0, 570.0];
                const F2_HZ: [f32; 5] = [1090.0, 2290.0, 870.0, 1840.0, 840.0];
                let mut f1 = 0.0_f32;
                let mut f2 = 0.0_f32;
                for i in 0..5 {
                    let w = vis[i] / vis_sum;
                    f1 += w * F1_HZ[i];
                    f2 += w * F2_HZ[i];
                }
                (f1, f2)
            } else if voiced {
                vis[0] = env * 0.45;
                (730.0, 1090.0)
            } else {
                (500.0, 1500.0)
            };
            arena.audio_frames[out_audio] = AudioFrameData {
                env,
                rms_db: rms,
                voiced: if voiced { 1.0 } else { 0.0 },
                f1_hz,
                f2_hz,
                visemes: vis,
            };
            arena.floats[out_rms_db] = rms;
            arena.bools[out_voice_active] = u32::from(voiced);
            env
        }

        Op::Hotkey {
            key_code,
            out_pressed,
            out_held,
        } => {
            let ext_pressed = host.is_some_and(|h| key_is_set(h.pending_hotkeys, key_code));
            let pressed = u32::from(key_is_set(inputs.keys_pressed, key_code) || ext_pressed);
            let held = u32::from(key_is_set(inputs.keys_down, key_code) || pressed != 0);
            arena.events[out_pressed] = pressed;
            arena.bools[out_held] = held;
            pressed as f32
        }

        Op::Clock {
            out_time_s,
            out_dt_s,
            out_tick,
        } => {
            let t = (inputs.now_us as f64 * 1e-6) as f32;
            arena.floats[out_time_s] = t;
            arena.floats[out_dt_s] = dt;
            arena.floats[out_tick] = inputs.tick_index as f32;
            t
        }

        Op::ExternalParam {
            slot,
            hotkey_slot,
            out_value,
            out_trigger,
        } => {
            let mut v = inputs.external_params.get(slot).copied().unwrap_or(0.0);
            if let Some(h) = host {
                let bank_val = h.params.get_external_param(slot);
                if bank_val != 0.0 || v == 0.0 {
                    v = bank_val;
                }
            }
            arena.floats[out_value] = if v.is_finite() { v } else { 0.0 };
            let trig = u32::from(host.is_some_and(|h| key_is_set(h.pending_hotkeys, hotkey_slot)));
            arena.events[out_trigger] = trig;
            arena.floats[out_value]
        }

        Op::VisibilityFirstIk {
            in_pose,
            in_hand_conf,
            in_body_conf,
            out_pose,
        } => {
            let mut pose = arena.poses[in_pose];
            let _ = arena.floats[in_hand_conf];
            let body_w = arena.floats[in_body_conf].clamp(0.0, 1.0);
            // Appendix A: legs/feet/toes are bones 9..=16; hands (20, 24) & fingers (25..=54) bypassed for stability.
            for p in &mut pose.pos[9..17] {
                p[3] *= body_w;
            }
            pose.pos[20][3] = 0.0;
            for p in &mut pose.pos[24..55] {
                p[3] = 0.0;
            }
            arena.poses[out_pose] = pose;
            0.0
        }

        Op::LookAt {
            in_target,
            in_weight,
            out_yaw,
            out_pitch,
        } => {
            let dir = vec3_norm(arena.vec3s[in_target]);
            let w = arena.floats[in_weight].clamp(0.0, 1.0);
            let yaw = dir[0].atan2(dir[2]) * w;
            let pitch = (-dir[1]).clamp(-1.0, 1.0).asin() * w;
            outputs.look_at_ws = [
                arena.vec3s[in_target][0],
                arena.vec3s[in_target][1],
                arena.vec3s[in_target][2],
                w,
            ];
            arena.floats[out_yaw] = yaw;
            arena.floats[out_pitch] = pitch;
            yaw
        }

        Op::IdleBreath {
            in_rate_hz,
            in_amp,
            out_pose,
        } => {
            let mut pose = PoseData::default();
            let rate = arena.floats[in_rate_hz].clamp(0.05, 4.0);
            let amp = arena.floats[in_amp].clamp(0.0, 0.25);
            let t = (inputs.now_us as f64 * 1e-6) as f32;
            let phase = (t * rate * core::f32::consts::TAU).sin() * amp;
            // Subtle additive chest rotation on bone 8 (UpperChest)
            pose.rot[8] = [
                phase * 0.5,
                0.0,
                0.0,
                (1.0 - (phase * phase * 0.25).min(0.5)).sqrt(),
            ];
            arena.poses[out_pose] = pose;
            phase
        }

        Op::IdleAttractor { in_blend, out_pose } => {
            let blend = arena.floats[in_blend].clamp(0.0, 1.0);
            let mut pose = PoseData::default();
            pose.pos[0][3] = blend;
            arena.poses[out_pose] = pose;
            blend
        }

        Op::FootPlanting {
            in_pose,
            in_floor_y,
            out_pose,
        } => {
            let mut pose = arena.poses[in_pose];
            let floor_y = arena.floats[in_floor_y];
            for foot_bone in [5_usize, 6_usize] {
                if pose.pos[foot_bone][1] < floor_y {
                    pose.pos[foot_bone][1] = floor_y;
                }
            }
            arena.poses[out_pose] = pose;
            floor_y
        }

        Op::FormantLipSync {
            in_audio,
            in_gain,
            out_visemes,
        } => {
            let af = arena.audio_frames[in_audio];
            let gain = arena.floats[in_gain].clamp(0.0, 4.0);
            let mut vis = [0.0_f32; 5];
            for (i, &w) in af.visemes.iter().enumerate() {
                vis[i] = (w * gain).clamp(0.0, 1.0);
            }
            // Synthesize formant weights if envelope is voiced without explicit viseme channels.
            if af.voiced > 0.5 && vis.iter().all(|&v| v < 1e-4) {
                let open = ((af.f1_hz - 250.0) / 550.0).clamp(0.0, 1.0);
                let front = ((af.f2_hz - 800.0) / 1600.0).clamp(0.0, 1.0);
                vis[0] = (af.env * open * gain).clamp(0.0, 1.0);
                vis[1] = (af.env * front * (1.0 - open) * gain).clamp(0.0, 1.0);
            }
            arena.viseme_sets[out_visemes] = vis;
            (vis[0] * 0.85 + vis[3] * 0.45 + vis[4] * 0.65).clamp(0.0, 1.0)
        }

        Op::ExpressionTrigger {
            in_trigger,
            slot,
            fade_in_ms,
            hold_ms,
            fade_out_ms,
            target_weight,
            state_idx,
            out_expr,
            out_active,
        } => {
            let trig = arena.events[in_trigger];
            let NodeState::Trigger(st) = &mut states[state_idx] else {
                return 0.0;
            };
            let fi = (fade_in_ms * 0.001).max(0.001);
            let ho = (hold_ms * 0.001).max(0.0);
            let fo = (fade_out_ms * 0.001).max(0.001);
            let total = fi + ho + fo;
            if trig > 0 && st.prev_event == 0 {
                st.active = true;
                st.elapsed_s = 0.0;
            }
            st.prev_event = trig;

            let env = if st.active {
                let w = if st.elapsed_s < fi {
                    st.elapsed_s / fi
                } else if st.elapsed_s < fi + ho {
                    1.0
                } else if st.elapsed_s < total {
                    1.0 - (st.elapsed_s - fi - ho) / fo
                } else {
                    st.active = false;
                    0.0
                };
                st.elapsed_s += dt;
                (w * target_weight).clamp(0.0, 1.0)
            } else {
                0.0
            };

            let mut expr = ExprSetData::default();
            if slot < 64 {
                expr.weights[slot] = env;
                expr.override_mask[slot] = if env > 0.001 { 1.0 } else { 0.0 };
            }
            arena.expr_sets[out_expr] = expr;
            arena.floats[out_active] = env;
            env
        }

        Op::ExpressionMixer {
            in_a,
            in_b,
            in_weight,
            out_expr,
        } => {
            let a = arena.expr_sets[in_a];
            let b = arena.expr_sets[in_b];
            let t = arena.floats[in_weight].clamp(0.0, 1.0);
            let mut out = ExprSetData::default();
            for i in 0..64 {
                out.weights[i] = (a.weights[i] * (1.0 - t) + b.weights[i] * t).clamp(0.0, 1.0);
                out.override_mask[i] = a.override_mask[i].max(b.override_mask[i] * t);
            }
            arena.expr_sets[out_expr] = out;
            t
        }

        Op::AutoBlink {
            in_face_conf,
            in_interval_s,
            state_idx,
            out_expr,
            out_blink,
        } => {
            let conf = arena.floats[in_face_conf];
            let interval = arena.floats[in_interval_s].clamp(0.5, 20.0);
            let NodeState::AutoBlink(st) = &mut states[state_idx] else {
                return 0.0;
            };
            let blink = if conf < 0.25 {
                st.timer_s += dt;
                if st.timer_s >= interval {
                    st.timer_s = 0.0;
                    st.blink_s = 0.16;
                }
                if st.blink_s > 0.0 {
                    st.blink_s = (st.blink_s - dt).max(0.0);
                    let phase = 1.0 - (st.blink_s / 0.16 - 0.5).abs() * 2.0;
                    phase.clamp(0.0, 1.0)
                } else {
                    0.0
                }
            } else {
                st.timer_s = 0.0;
                st.blink_s = 0.0;
                0.0
            };
            let mut expr = ExprSetData::default();
            // Appendix B slots: 0 = eyeBlinkLeft, 7 = eyeBlinkRight.
            // Assert override_mask only when face tracking is absent (`conf < 0.25`)
            // so idle auto-blink never collides with live ballistic eye tracking.
            expr.weights[0] = blink;
            expr.weights[7] = blink;
            if conf < 0.25 {
                expr.override_mask[0] = 1.0;
                expr.override_mask[7] = 1.0;
            }
            arena.expr_sets[out_expr] = expr;
            arena.floats[out_blink] = blink;
            blink
        }

        Op::BlendshapeRemap {
            in_expr,
            slot,
            in_min,
            in_max,
            out_min,
            out_max,
            out_expr,
        } => {
            let mut expr = arena.expr_sets[in_expr];
            let denom = if (in_max - in_min).abs() < 1e-6 {
                1.0
            } else {
                in_max - in_min
            };
            if slot < 64 {
                let u = ((expr.weights[slot] - in_min) / denom).clamp(0.0, 1.0);
                expr.weights[slot] = (out_min + (out_max - out_min) * u).clamp(0.0, 1.0);
            }
            arena.expr_sets[out_expr] = expr;
            expr.weights.get(slot).copied().unwrap_or(0.0)
        }

        Op::AddFloat { in_a, in_b, out } => {
            let v = arena.floats[in_a] + arena.floats[in_b];
            arena.floats[out] = v;
            v
        }
        Op::AddVec3 { in_a, in_b, out } => {
            let a = arena.vec3s[in_a];
            let b = arena.vec3s[in_b];
            arena.vec3s[out] = [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
            arena.vec3s[out][0]
        }
        Op::MulFloat { in_a, in_b, out } => {
            let v = arena.floats[in_a] * arena.floats[in_b];
            arena.floats[out] = v;
            v
        }
        Op::MulVec3 { in_a, in_b, out } => {
            let a = arena.vec3s[in_a];
            let b = arena.vec3s[in_b];
            arena.vec3s[out] = [a[0] * b[0], a[1] * b[1], a[2] * b[2]];
            arena.vec3s[out][0]
        }
        Op::LerpFloat {
            in_a,
            in_b,
            in_t,
            out,
        } => {
            let t = arena.floats[in_t].clamp(0.0, 1.0);
            let v = arena.floats[in_a] + (arena.floats[in_b] - arena.floats[in_a]) * t;
            arena.floats[out] = v;
            v
        }
        Op::LerpVec3 {
            in_a,
            in_b,
            in_t,
            out,
        } => {
            let t = arena.floats[in_t].clamp(0.0, 1.0);
            let a = arena.vec3s[in_a];
            let b = arena.vec3s[in_b];
            arena.vec3s[out] = [
                a[0] + (b[0] - a[0]) * t,
                a[1] + (b[1] - a[1]) * t,
                a[2] + (b[2] - a[2]) * t,
            ];
            arena.vec3s[out][0]
        }
        Op::LerpQuat {
            in_a,
            in_b,
            in_t,
            out,
        } => {
            let t = arena.floats[in_t].clamp(0.0, 1.0);
            let q = quat_slerp_norm(arena.quats[in_a], arena.quats[in_b], t);
            arena.quats[out] = q;
            q[3]
        }
        Op::Remap {
            src,
            in_min,
            in_max,
            out_min,
            out_max,
            clamp,
            dst,
        } => {
            let denom = if (in_max - in_min).abs() < 1e-6 {
                1.0
            } else {
                in_max - in_min
            };
            let mut u = (arena.floats[src] - in_min) / denom;
            if clamp {
                u = u.clamp(0.0, 1.0);
            }
            let v = out_min + (out_max - out_min) * u;
            arena.floats[dst] = v;
            v
        }
        Op::Clamp {
            src,
            min_v,
            max_v,
            dst,
        } => {
            let v = arena.floats[src].clamp(min_v, max_v);
            arena.floats[dst] = v;
            v
        }
        Op::Smooth {
            src,
            cutoff_hz,
            state_idx,
            dst,
        } => {
            let x = arena.floats[src];
            let NodeState::Smooth(st) = &mut states[state_idx] else {
                return 0.0;
            };
            let v = if st.initialized {
                let tau = (core::f32::consts::TAU * cutoff_hz.max(0.05)).recip();
                let alpha = dt / (dt + tau);
                st.prev += (x - st.prev) * alpha;
                st.prev
            } else {
                st.initialized = true;
                st.prev = x;
                x
            };
            arena.floats[dst] = v;
            v
        }
        Op::Spring {
            src,
            stiffness,
            damping,
            state_idx,
            dst,
        } => {
            let target = arena.floats[src];
            let NodeState::Spring(st) = &mut states[state_idx] else {
                return 0.0;
            };
            if st.initialized {
                let accel = (target - st.pos) * stiffness - st.vel * damping;
                st.vel += accel * dt;
                st.pos += st.vel * dt;
            } else {
                st.initialized = true;
                st.pos = target;
                st.vel = 0.0;
            }
            arena.floats[dst] = st.pos;
            st.pos
        }
        Op::Compare {
            in_a,
            in_b,
            cmp,
            out,
        } => {
            let a = arena.floats[in_a];
            let b = arena.floats[in_b];
            let ok = match cmp {
                CompareOp::Gt => a > b,
                CompareOp::Lt => a < b,
                CompareOp::Ge => a >= b,
                CompareOp::Le => a <= b,
                CompareOp::Eq => (a - b).abs() < 1e-5,
            };
            let res = u32::from(ok);
            arena.bools[out] = res;
            res as f32
        }
        Op::And { in_a, in_b, out } => {
            let r = u32::from(arena.bools[in_a] != 0 && arena.bools[in_b] != 0);
            arena.bools[out] = r;
            r as f32
        }
        Op::Or { in_a, in_b, out } => {
            let r = u32::from(arena.bools[in_a] != 0 || arena.bools[in_b] != 0);
            arena.bools[out] = r;
            r as f32
        }
        Op::Not { src, out } => {
            let r = u32::from(arena.bools[src] == 0);
            arena.bools[out] = r;
            r as f32
        }
        Op::Select {
            in_cond,
            in_a,
            in_b,
            out,
        } => {
            let v = if arena.bools[in_cond] != 0 {
                arena.floats[in_a]
            } else {
                arena.floats[in_b]
            };
            arena.floats[out] = v;
            v
        }
        Op::DelayRead { state_idx, dst } => {
            let prev = match &states[state_idx] {
                NodeState::Delay(s) => s.prev,
                _ => 0.0,
            };
            arena.floats[dst] = prev;
            prev
        }
        Op::DelayWrite { src, state_idx } => {
            let val = arena.floats[src];
            if let NodeState::Delay(s) = &mut states[state_idx] {
                s.prev = val;
            }
            val
        }
        Op::Random {
            in_trigger,
            state_idx,
            dst,
        } => {
            let trig = arena.events[in_trigger];
            let NodeState::Random(st) = &mut states[state_idx] else {
                return 0.0;
            };
            if trig > 0 {
                st.last = xorshift64(&mut st.seed);
            }
            arena.floats[dst] = st.last;
            st.last
        }
        Op::Curve { src, lut_idx, dst } => {
            let u = arena.floats[src].clamp(0.0, 1.0) * 63.0;
            let lo = (u.floor() as usize).min(63);
            let hi = (lo + 1).min(63);
            let frac = u - (lo as f32);
            let lut = &arena.luts[lut_idx];
            let v = lut[lo] + (lut[hi] - lut[lo]) * frac;
            arena.floats[dst] = v;
            v
        }

        Op::OutputPose { in_pose } => {
            let pose = &arena.poses[in_pose];
            // A pose sourced from `NpuPose` is the raw tracking frame: the C++ Visibility-First solver already
            // consumes and filters that, so only emit what the graph *changed* relative to it. Passing the raw
            // pose through would re-apply the unfiltered 30 Hz head rotation on top of the spring-smoothed one
            // (double rotation + jitter).
            let track = if pose.from_tracking {
                inputs.tracking
            } else {
                None
            };
            for (b, dst) in outputs.bone_rotation_offsets.iter_mut().enumerate() {
                *dst = match track {
                    Some(tr) => quat_mul_norm(quat_conj(tr.joint_rotations_quat[b]), pose.rot[b]),
                    None => pose.rot[b],
                };
            }
            // IK slots are addressed by role (`AETHER_IK_HEAD = 0`, `LEFT_HAND = 1`, `RIGHT_HAND = 2`,
            // `LEFT_FOOT = 3`, `RIGHT_FOOT = 4`, `HIPS = 5`), not by humanoid bone index.
            for (slot, &bone) in IK_SLOT_BONES.iter().enumerate() {
                let p = pose.pos[bone];
                let finite = p.iter().all(|v| v.is_finite());
                let moved = match track {
                    Some(tr) => {
                        let tp = tr.joint_positions_ws[bone];
                        let d = [p[0] - tp[0], p[1] - tp[1], p[2] - tp[2]];
                        d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1e-6
                    }
                    None => true,
                };
                let w = if finite && moved {
                    p[3].clamp(0.0, 1.0)
                } else {
                    0.0
                };
                if w > 0.0 {
                    outputs.ik_targets[slot] = [p[0], p[1], p[2], 0.0, 0.0, 0.0, 1.0, w];
                }
            }
            pose.pos[0][3]
        }

        Op::OutputExpressions { in_expr } => {
            let expr = &arena.expr_sets[in_expr];
            outputs.expression_weights = expr.weights;
            outputs.expression_override = expr.override_mask;
            if let Some(h) = host {
                for slot in 0..64 {
                    let ov = h.params.get_expression_override(slot);
                    if ov > 0.001 {
                        outputs.expression_weights[slot] = ov;
                        outputs.expression_override[slot] = 1.0;
                    }
                }
            }
            if inputs.external_params[0].is_finite() && inputs.external_params[0] != 0.0 {
                outputs.expression_override[0] = inputs.external_params[0].clamp(0.0, 1.0);
            }
            outputs.expression_weights[0]
        }

        Op::CameraControl { in_focal, in_flat } => {
            let focal = arena.floats[in_focal];
            let flat = arena.floats[in_flat];
            outputs.ik_targets[6][0] = focal;
            outputs.ik_targets[6][1] = flat;
            flat
        }

        Op::LightControl {
            in_dir,
            in_intensity,
        } => {
            let dir = vec3_norm(arena.vec3s[in_dir]);
            let intensity = arena.floats[in_intensity];
            outputs.look_at_ws = [dir[0], dir[1], dir[2], intensity];
            intensity
        }

        Op::PropAttach { in_transform } => {
            let xf = arena.transforms[in_transform];
            outputs.ik_targets[7] = [xf[0], xf[1], xf[2], xf[3], xf[4], xf[5], xf[6], xf[7]];
            xf[0]
        }

        Op::AsyncOutputSignal {
            in_val,
            in_trigger,
            event_tag,
        } => {
            if arena.events[in_trigger] > 0 {
                push_event(outputs, event_tag);
            }
            arena.floats[in_val]
        }

        Op::Latched {
            instance_slot,
            ref in_floats,
            ref out_floats,
        } => {
            let mut in_buf = [0.0_f32; LATCHED_IO_FLOATS];
            for (dst, &slot) in in_buf.iter_mut().zip(in_floats.iter()) {
                if let Some(s) = slot {
                    *dst = arena.floats[s];
                }
            }
            let mut out_buf = [0.0_f32; LATCHED_IO_FLOATS];
            if let Some(h) = host {
                h.latched.exchange_t2(instance_slot, &in_buf, &mut out_buf);
            }
            for (i, &slot) in out_floats.iter().enumerate() {
                if let Some(s) = slot {
                    arena.floats[s] = out_buf[i];
                }
            }
            out_buf[0]
        }

        Op::ModelSource {
            table,
            out_pose,
            out_expr,
            out_table,
            out_count,
        } => {
            if let Some(tr) = inputs.tracking {
                arena.poses[out_pose] = PoseData {
                    pos: tr.joint_positions_ws,
                    rot: tr.joint_rotations_quat,
                    from_tracking: true,
                };
                let mut expr = ExprSetData::default();
                for (dst, &w) in expr.weights.iter_mut().zip(tr.arkit_blendshapes.iter()) {
                    *dst = if w.is_finite() {
                        w.clamp(0.0, 1.0)
                    } else {
                        0.0
                    };
                }
                for (vi, &vw) in inputs.dsp_visemes.iter().enumerate() {
                    if let Some(slot) = expr.weights.get_mut(52 + vi) {
                        *slot = if vw.is_finite() {
                            vw.clamp(0.0, 1.0)
                        } else {
                            0.0
                        };
                    }
                }
                arena.expr_sets[out_expr] = expr;
            } else {
                arena.poses[out_pose] = PoseData::default();
                arena.expr_sets[out_expr] = ExprSetData::default();
            }
            arena.mat_tables[out_table] = table;
            let count_f = table.slot_count as f32;
            arena.floats[out_count] = count_f;
            count_f
        }

        Op::TextureAsset {
            tex_data,
            out_tex,
            out_ready,
        } => {
            arena.tex_handles[out_tex] = tex_data;
            let rdy = u32::from(tex_data.handle != 0);
            arena.bools[out_ready] = rdy;
            rdy as f32
        }

        Op::TextureGrade {
            in_tex,
            in_hue,
            in_sat,
            in_bright,
            in_contrast,
            in_tint,
            tint_from_param,
            tint_connected,
            uv_scale_offset,
            out_tex,
        } => {
            let mut td = arena.tex_handles[in_tex];
            let hue = arena.floats[in_hue].clamp(-180.0, 180.0);
            let sat = arena.floats[in_sat].clamp(0.0, 4.0);
            let bright = arena.floats[in_bright].clamp(-2.0, 2.0);
            let contrast = arena.floats[in_contrast].clamp(0.0, 4.0);
            let tint = if tint_connected {
                arena.colors[in_tint]
            } else {
                tint_from_param
            };
            td.grade_active = true;
            td.uv_scale_offset = uv_scale_offset;
            td.grade_hsbc = [hue, sat, bright, contrast];
            td.grade_tint = tint;
            arena.tex_handles[out_tex] = td;
            sat
        }

        Op::DynamicMaterialDriver {
            in_signal,
            in_audio_rms,
            in_expr_w,
            in_circadian,
            in_table,
            signal_connected,
            audio_connected,
            expr_connected,
            circadian_connected,
            signal_source,
            expr_slot,
            target_param,
            material_index,
            material_name_hash,
            gain,
            bias,
            min_v,
            max_v,
            out_ovr,
            out_val,
        } => {
            let raw_sig = if signal_source == AETHER_DYN_SIG_PIN || signal_connected {
                arena.floats[in_signal]
            } else if signal_source == AETHER_DYN_SIG_EXPRESSION {
                if expr_connected {
                    arena.floats[in_expr_w]
                } else {
                    let from_out = outputs
                        .expression_weights
                        .get(expr_slot)
                        .copied()
                        .unwrap_or(0.0);
                    let from_tr = inputs
                        .tracking
                        .and_then(|t| t.arkit_blendshapes.get(expr_slot).copied())
                        .unwrap_or(0.0);
                    from_out.max(from_tr)
                }
            } else if signal_source == AETHER_DYN_SIG_CIRCADIAN {
                if circadian_connected {
                    arena.floats[in_circadian]
                } else {
                    let h = if circadian_hour.is_finite() {
                        circadian_hour.rem_euclid(24.0)
                    } else {
                        12.0
                    };
                    (0.5 - 0.5 * (h * core::f32::consts::PI / 12.0).cos()).clamp(0.0, 1.0)
                }
            } else if audio_connected {
                arena.floats[in_audio_rms]
            } else {
                let db = if inputs.dsp_rms_db.is_finite() {
                    inputs.dsp_rms_db
                } else {
                    -60.0
                };
                ((db + 60.0) / 60.0).clamp(0.0, 1.0)
            };

            let lo = min_v.min(max_v);
            let hi = min_v.max(max_v);
            let driven = (raw_sig * gain + bias).clamp(lo, hi);
            arena.floats[out_val] = driven;

            let mut resolved_idx = material_index;
            if resolved_idx == AETHER_INVALID_INDEX && material_name_hash != 0 {
                let tbl = &arena.mat_tables[in_table];
                let limit = (tbl.slot_count as usize).min(64);
                for (si, &sh) in tbl.slot_hashes[..limit].iter().enumerate() {
                    if sh == material_name_hash {
                        resolved_idx = si as u32;
                        break;
                    }
                }
            }

            let mut ovr = MaterialOverrideData {
                valid: true,
                ..Default::default()
            };
            ovr.entry.material_index = resolved_idx;
            ovr.entry.material_name_hash = material_name_hash;
            match target_param {
                AETHER_DYN_TGT_BLUSH_ALPHA => {
                    ovr.entry.override_mask = AETHER_MAT_OVR_BLUSH_ALPHA;
                    ovr.entry.dynamic_params[1] = driven.clamp(0.0, 1.0);
                }
                AETHER_DYN_TGT_RIM_BOOST => {
                    ovr.entry.override_mask = AETHER_MAT_OVR_RIM_BOOST;
                    ovr.entry.dynamic_params[2] = driven.max(0.0);
                }
                _ => {
                    ovr.entry.override_mask = AETHER_MAT_OVR_EMISSIVE;
                    ovr.entry.dynamic_params[0] = driven.max(0.0);
                }
            }
            arena.mat_overrides[out_ovr] = ovr;
            driven
        }

        Op::MaterialSlotOverride {
            in_table,
            in_tex,
            in_shade_shift,
            in_sss_fringe,
            in_outline_scale,
            in_driver,
            tex_connected,
            driver_connected,
            material_index,
            material_name_hash,
            base_override_mask,
            shader_profile,
            shade_color,
            rt_shadow_mask,
            explicit_tex_slot,
            out_ovr,
            out_slot_idx,
        } => {
            let mut resolved_idx = material_index;
            if resolved_idx == AETHER_INVALID_INDEX && material_name_hash != 0 {
                let tbl = &arena.mat_tables[in_table];
                let limit = (tbl.slot_count as usize).min(64);
                for (si, &sh) in tbl.slot_hashes[..limit].iter().enumerate() {
                    if sh == material_name_hash {
                        resolved_idx = si as u32;
                        break;
                    }
                }
            }

            let mut entry = AetherMaterialOverrideEntry {
                material_index: resolved_idx,
                material_name_hash,
                override_mask: base_override_mask,
                shader_profile,
                shade_color,
                scalar_params: [
                    arena.floats[in_shade_shift].clamp(-1.0, 1.0),
                    arena.floats[in_sss_fringe].clamp(0.0, 2.0),
                    arena.floats[in_outline_scale].clamp(0.0, 4.0),
                    0.0,
                ],
                rt_shadow_mask,
                uv_scale_offset: [1.0, 1.0, 0.0, 0.0],
                grade_hsbc: [0.0, 1.0, 0.0, 1.0],
                grade_tint: [1.0, 1.0, 1.0, 1.0],
                ..Default::default()
            };

            if tex_connected {
                let td = arena.tex_handles[in_tex];
                let slot = explicit_tex_slot.unwrap_or(td.target_slot).min(5) as usize;
                if td.handle != 0 {
                    entry.override_mask |= AETHER_MAT_OVR_TEXTURE;
                    entry.texture_handles[slot] = td.handle;
                    if td.srgb {
                        entry.tex_srgb_mask |= 1u32 << slot;
                    }
                    if td.is_spout {
                        entry.tex_spout_mask |= 1u32 << slot;
                    }
                }
                if td.grade_active {
                    entry.override_mask |= AETHER_MAT_OVR_GRADE;
                    entry.uv_scale_offset = td.uv_scale_offset;
                    entry.grade_hsbc = td.grade_hsbc;
                    entry.grade_tint = td.grade_tint;
                    entry.source_flags |= 1;
                }
            }

            if driver_connected {
                let drv = arena.mat_overrides[in_driver];
                if drv.valid {
                    let dm = drv.entry.override_mask
                        & (AETHER_MAT_OVR_EMISSIVE
                            | AETHER_MAT_OVR_BLUSH_ALPHA
                            | AETHER_MAT_OVR_RIM_BOOST);
                    entry.override_mask |= dm;
                    if (dm & AETHER_MAT_OVR_EMISSIVE) != 0 {
                        entry.dynamic_params[0] = drv.entry.dynamic_params[0];
                    }
                    if (dm & AETHER_MAT_OVR_BLUSH_ALPHA) != 0 {
                        entry.dynamic_params[1] = drv.entry.dynamic_params[1];
                    }
                    if (dm & AETHER_MAT_OVR_RIM_BOOST) != 0 {
                        entry.dynamic_params[2] = drv.entry.dynamic_params[2];
                    }
                }
            }

            arena.mat_overrides[out_ovr] = MaterialOverrideData { entry, valid: true };
            let slot_f = if resolved_idx == AETHER_INVALID_INDEX {
                -1.0
            } else {
                resolved_idx as f32
            };
            arena.floats[out_slot_idx] = slot_f;
            slot_f
        }
    }
}

impl CompiledGraphPlan {
    /// Evaluates the compiled graph deterministically with zero heap allocation (`HR-01`, `REQ-PLG-002`).
    pub fn evaluate(&mut self, inputs: &SafeGraphInputs<'_>, outputs: &mut AetherGraphOutputs) {
        let _ = self.evaluate_with_host(inputs, None, outputs);
    }

    /// Evaluates the compiled graph with full `PluginHost` external params & latched WASM bridge; returns elapsed `us`.
    pub fn evaluate_with_host(
        &mut self,
        inputs: &SafeGraphInputs<'_>,
        host: Option<&HostEvalContext<'_>>,
        outputs: &mut AetherGraphOutputs,
    ) -> f32 {
        self.evaluate_with_host_and_materials(inputs, host, 12.0, outputs, None)
    }

    /// Evaluates pose/expression outputs and non-destructive `AetherMaterialOverrideBuffer` in a single allocation-free pass.
    pub fn evaluate_with_host_and_materials(
        &mut self,
        inputs: &SafeGraphInputs<'_>,
        host: Option<&HostEvalContext<'_>>,
        circadian_hour: f32,
        outputs: &mut AetherGraphOutputs,
        mut out_mats: Option<&mut AetherMaterialOverrideBuffer>,
    ) -> f32 {
        let t0 = Instant::now();
        outputs.expression_weights = [0.0; 64];
        outputs.expression_override = [0.0; 64];
        outputs.ik_targets = [[0.0; 8]; 8];
        outputs.bone_rotation_offsets = [[0.0; 4]; 55];
        outputs.look_at_ws = [0.0, 0.0, 1.0, 0.0];
        outputs.events = [0; 16];
        outputs.event_count = 0;

        if let Some(ref mut mbuf) = out_mats {
            mbuf.struct_size = core::mem::size_of::<AetherMaterialOverrideBuffer>() as u32;
            mbuf.count = 0;
            mbuf.circadian_hour = if circadian_hour.is_finite() {
                circadian_hour
            } else {
                12.0
            };
            mbuf.eval_epoch = inputs.tick_index;
            mbuf.model_format = self.model_format;
            mbuf.model_material_count = self.model_material_count;
        }

        // If tracking arrived and the graph contains no OutputPose, still keep expressions sane? No: deterministic ops drive outputs; fallback only if empty op list.
        if self.ops.is_empty() {
            if let Some(track) = inputs.tracking {
                for (dst, &src) in outputs
                    .expression_weights
                    .iter_mut()
                    .zip(track.arkit_blendshapes.iter())
                {
                    *dst = src.clamp(0.0, 1.0);
                }
            }
            for (idx, &v) in inputs.dsp_visemes.iter().enumerate() {
                if let Some(slot) = outputs.expression_weights.get_mut(52 + idx) {
                    *slot = v.clamp(0.0, 1.0);
                }
            }
            if inputs.external_params[0].is_finite() {
                outputs.expression_override[0] = inputs.external_params[0].clamp(0.0, 1.0);
            }
            return t0.elapsed().as_secs_f32() * 1_000_000.0;
        }

        // Split borrows between ops/meta and mutable arena/states.
        let arena = &mut self.arena;
        let states = &mut self.states;
        self.node_eval_ns.fill(0);
        self.node_eval_us.fill(0.0);
        let plan_start = Instant::now();
        for (op_idx, op) in self.ops.iter().enumerate() {
            let op_start = Instant::now();
            let val = eval_op(
                op,
                arena,
                states,
                inputs,
                host,
                circadian_hour,
                outputs,
            );
            let op_ns = u32::try_from(op_start.elapsed().as_nanos()).unwrap_or(u32::MAX);
            if let Some(node_slot) = self.op_to_node_slot.get(op_idx).copied().flatten() {
                if node_slot < self.node_eval_us.len() {
                    let accum_ns = self.node_eval_ns[node_slot].saturating_add(op_ns);
                    self.node_eval_ns[node_slot] = accum_ns;
                    self.node_eval_us[node_slot] = accum_ns as f32 * 0.001;
                    self.node_live_values[node_slot] = val;
                }
            }
        }
        if let Some(mbuf) = out_mats {
            let mut cnt = 0usize;
            for &slot in &self.mat_override_sink_slots {
                if cnt >= AETHER_MAX_MATERIAL_OVERRIDES {
                    break;
                }
                if let Some(ovr) = arena.mat_overrides.get(slot)
                    && ovr.valid
                {
                    mbuf.entries[cnt] = ovr.entry;
                    cnt += 1;
                }
            }
            mbuf.count = cnt as u32;
        }
        self.last_total_eval_ns =
            u64::try_from(plan_start.elapsed().as_nanos()).unwrap_or(u64::MAX);
        for (i, pin_meta) in self.output_pins.iter().enumerate() {
            let (kind, values) = summarize_slot(arena, pin_meta.slot);
            self.pin_kinds[i] = kind;
            self.pin_values[i] = values;
        }
        t0.elapsed().as_secs_f32() * 1_000_000.0
    }
}

fn summarize_slot(
    arena: &ValuesArena,
    slot: Option<crate::graph::compiler::SlotRef>,
) -> (u8, [f32; 4]) {
    use crate::graph::compiler::SlotRef;
    let Some(slot_ref) = slot else {
        return (aether_ffi::AETHER_PIN_VALUE_INVALID, [0.0; 4]);
    };
    match slot_ref {
        SlotRef::Event(i) => (
            aether_ffi::AETHER_PIN_VALUE_EVENT,
            [arena.events[i] as f32, 0.0, 0.0, 0.0],
        ),
        SlotRef::Bool(i) => (
            aether_ffi::AETHER_PIN_VALUE_BOOL,
            [if arena.bools[i] != 0 { 1.0 } else { 0.0 }, 0.0, 0.0, 0.0],
        ),
        SlotRef::Float(i) => (
            aether_ffi::AETHER_PIN_VALUE_FLOAT,
            [arena.floats[i], 0.0, 0.0, 0.0],
        ),
        SlotRef::Vec3(i) => {
            let v = arena.vec3s[i];
            (aether_ffi::AETHER_PIN_VALUE_VEC3, [v[0], v[1], v[2], 0.0])
        }
        SlotRef::Quat(i) => (aether_ffi::AETHER_PIN_VALUE_QUAT, arena.quats[i]),
        SlotRef::Transform(i) => {
            let t = arena.transforms[i];
            (
                aether_ffi::AETHER_PIN_VALUE_TRANSFORM,
                [t[0], t[1], t[2], t[7]],
            )
        }
        SlotRef::Pose(i) => {
            let p = &arena.poses[i];
            (
                aether_ffi::AETHER_PIN_VALUE_POSE,
                [p.pos[12][0], p.pos[12][1], p.pos[12][2], p.pos[0][3]],
            )
        }
        SlotRef::ExprSet(i) => {
            let expr = &arena.expr_sets[i];
            let mut max_w = 0.0_f32;
            let mut active = 0_u32;
            for &w in &expr.weights {
                if w > max_w {
                    max_w = w;
                }
                if w > 0.01 {
                    active += 1;
                }
            }
            (
                aether_ffi::AETHER_PIN_VALUE_EXPR,
                [max_w, active as f32, expr.weights[17], expr.weights[0]],
            )
        }
        SlotRef::VisemeSet(i) => {
            let v = arena.viseme_sets[i];
            let mut best_idx = 0_usize;
            let mut best_w = 0.0_f32;
            let mut sum = 0.0_f32;
            for (k, &w) in v.iter().enumerate() {
                sum += w;
                if w > best_w {
                    best_w = w;
                    best_idx = k;
                }
            }
            (
                aether_ffi::AETHER_PIN_VALUE_VISEME,
                [best_w, best_idx as f32, sum, v[0]],
            )
        }
        SlotRef::AudioFrame(i) => {
            let af = arena.audio_frames[i];
            (
                aether_ffi::AETHER_PIN_VALUE_AUDIO,
                [af.rms_db, af.env, af.f1_hz, af.f2_hz],
            )
        }
        SlotRef::Color(i) => (aether_ffi::AETHER_PIN_VALUE_QUAT, arena.colors[i]),
        SlotRef::MaterialTable(i) => {
            let mt = &arena.mat_tables[i];
            (
                AETHER_PIN_VALUE_MAT_TABLE,
                [
                    mt.slot_count as f32,
                    mt.model_format as f32,
                    mt.slot_hashes[0] as f32,
                    0.0,
                ],
            )
        }
        SlotRef::MaterialOverride(i) => {
            let mo = &arena.mat_overrides[i];
            (
                AETHER_PIN_VALUE_MAT_OVERRIDE,
                [
                    if mo.entry.material_index == AETHER_INVALID_INDEX {
                        -1.0
                    } else {
                        mo.entry.material_index as f32
                    },
                    mo.entry.override_mask as f32,
                    mo.entry.shader_profile as f32,
                    if mo.valid { 1.0 } else { 0.0 },
                ],
            )
        }
        SlotRef::TextureHandle(i) => {
            let th = &arena.tex_handles[i];
            (
                AETHER_PIN_VALUE_TEX_HANDLE,
                [
                    (th.handle & 0xFFFF_FFFF) as f32,
                    (th.handle >> 32) as f32,
                    if th.srgb { 1.0 } else { 0.0 },
                    if th.grade_active { 1.0 } else { 0.0 },
                ],
            )
        }
    }
}
