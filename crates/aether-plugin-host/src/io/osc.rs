//! Untrusted OSC UDP decoder, 1000 msg/s rate limiter, and outbound telemetry encoder (§7.8, `HR-06`).

use std::time::{Duration, Instant};

use rosc::{OscBundle, OscMessage, OscPacket, OscType};

use crate::error::HostError;

/// Maximum incoming OSC UDP packet size (`64 KiB`, `HR-06`).
pub const MAX_OSC_PACKET_BYTES: usize = 65536;
/// Maximum OSC control messages accepted per second (§7.8).
pub const OSC_MAX_MSG_PER_SEC: u32 = 1000;

/// Parsed OSC inbound control command (§7.8).
#[derive(Clone, Debug, PartialEq)]
pub enum OscControlMsg {
    /// `/aether/expression/<name> f`
    Expression { name: String, value: f32 },
    /// `/aether/param/<param_id> f`
    Param { param_id: String, value: f32 },
    /// `/aether/hotkey/<id> i`
    Hotkey { hotkey_id: u32 },
}

fn osc_arg_to_f32(arg: &OscType) -> Option<f32> {
    match *arg {
        OscType::Float(v) => v.is_finite().then_some(v),
        OscType::Double(v) => {
            let f = v as f32;
            f.is_finite().then_some(f)
        }
        OscType::Int(v) => Some(v as f32),
        OscType::Long(v) => Some(v as f32),
        OscType::Bool(b) => Some(if b { 1.0 } else { 0.0 }),
        _ => None,
    }
}

fn osc_arg_to_u32(arg: &OscType) -> Option<u32> {
    match *arg {
        OscType::Int(v) => u32::try_from(v).ok(),
        OscType::Long(v) => u32::try_from(v).ok(),
        OscType::Float(v) if v.is_finite() && v >= 0.0 => Some(v as u32),
        OscType::Bool(b) => Some(u32::from(b)),
        _ => None,
    }
}

fn collect_osc_message(msg: &OscMessage, out: &mut Vec<OscControlMsg>) {
    if let Some(name) = msg.addr.strip_prefix("/aether/expression/") {
        if !name.is_empty() {
            if let Some(val) = msg.args.first().and_then(osc_arg_to_f32) {
                out.push(OscControlMsg::Expression {
                    name: name.to_owned(),
                    value: val,
                });
            }
        }
    } else if let Some(param_id) = msg.addr.strip_prefix("/aether/param/") {
        if !param_id.is_empty() {
            if let Some(val) = msg.args.first().and_then(osc_arg_to_f32) {
                out.push(OscControlMsg::Param {
                    param_id: param_id.to_owned(),
                    value: val,
                });
            }
        }
    } else if let Some(hk_str) = msg.addr.strip_prefix("/aether/hotkey/") {
        if let Ok(hk) = hk_str.parse::<u32>() {
            let active = msg.args.first().and_then(osc_arg_to_u32).unwrap_or(hk);
            out.push(OscControlMsg::Hotkey { hotkey_id: active });
        } else if let Some(hotkey_id) = msg.args.first().and_then(osc_arg_to_u32) {
            out.push(OscControlMsg::Hotkey { hotkey_id });
        }
    }
}

fn walk_packet(
    pkt: &OscPacket,
    depth: usize,
    out: &mut Vec<OscControlMsg>,
) -> Result<(), HostError> {
    if depth > 8 {
        return Err(HostError::LimitExceeded(
            "OSC bundle nesting depth exceeds 8".to_owned(),
        ));
    }
    if out.len() >= 256 {
        return Err(HostError::LimitExceeded(
            "OSC bundle message count exceeds 256".to_owned(),
        ));
    }
    match pkt {
        OscPacket::Message(m) => collect_osc_message(m, out),
        OscPacket::Bundle(OscBundle { content, .. }) => {
            for child in content {
                walk_packet(child, depth + 1, out)?;
            }
        }
    }
    Ok(())
}

/// Safely parses an untrusted OSC UDP payload without panicking or allocating unboundedly (`HR-06`, §7.8).
pub fn parse_osc_packet(bytes: &[u8]) -> Result<Vec<OscControlMsg>, HostError> {
    if bytes.is_empty() || bytes.len() > MAX_OSC_PACKET_BYTES {
        return Err(HostError::LimitExceeded(
            "OSC packet size invalid".to_owned(),
        ));
    }
    let (_, pkt) = rosc::decoder::decode_udp(bytes)
        .map_err(|e| HostError::parse(format!("OSC decode error: {e}")))?;
    let mut out = Vec::new();
    walk_packet(&pkt, 0, &mut out)?;
    Ok(out)
}

/// Encodes an OSC float message for telemetry output (`127.0.0.1:9001`, §7.8).
pub fn encode_osc_float(addr: &str, value: f32) -> Result<Vec<u8>, HostError> {
    let pkt = OscPacket::Message(OscMessage {
        addr: addr.to_owned(),
        args: vec![OscType::Float(if value.is_finite() { value } else { 0.0 })],
    });
    rosc::encoder::encode(&pkt)
        .map_err(|e| HostError::Internal(format!("OSC encode float failed: {e}")))
}

/// Encodes an OSC int32 message for telemetry output (`127.0.0.1:9001`, §7.8).
pub fn encode_osc_int(addr: &str, value: i32) -> Result<Vec<u8>, HostError> {
    let pkt = OscPacket::Message(OscMessage {
        addr: addr.to_owned(),
        args: vec![OscType::Int(value)],
    });
    rosc::encoder::encode(&pkt)
        .map_err(|e| HostError::Internal(format!("OSC encode int failed: {e}")))
}

/// Fixed-window `1000 msg/s` OSC ingress rate limiter (§7.8).
#[derive(Debug)]
pub struct OscRateLimiter {
    window_start: Instant,
    count_in_window: u32,
}

impl OscRateLimiter {
    /// Initializes a rate limiter starting at `now`.
    #[must_use]
    pub fn new(now: Instant) -> Self {
        Self {
            window_start: now,
            count_in_window: 0,
        }
    }

    /// Returns `true` if `count` incoming messages are within the `1000 msg/s` limit.
    pub fn allow(&mut self, now: Instant, count: u32) -> bool {
        if now.duration_since(self.window_start) >= Duration::from_secs(1) {
            self.window_start = now;
            self.count_in_window = 0;
        }
        if self.count_in_window.saturating_add(count) > OSC_MAX_MSG_PER_SEC {
            return false;
        }
        self.count_in_window = self.count_in_window.saturating_add(count);
        true
    }
}

/// Maps well-known parameter string IDs or numeric indices `0..63` onto external param slots `0..63`.
#[must_use]
pub fn map_param_id_to_slot(param_id: &str) -> u32 {
    if let Some(rest) = param_id.strip_prefix("external.") {
        if let Ok(n) = rest.parse::<u32>() {
            return n.min(63);
        }
    }
    if let Ok(n) = param_id.parse::<u32>() {
        return n.min(63);
    }
    match param_id {
        "render.flatness" => 0,
        "render.outline_width" => 1,
        "render.key_intensity" => 2,
        "render.rim_intensity" => 3,
        "avatar.transform.yaw_deg" => 4,
        "avatar.transform.pitch_deg" => 5,
        "tracking.head_smooth_hz" => 6,
        _ => {
            // Deterministic slot hash in `16..63` so any Appendix D parameter maps stably cleanly.
            let mut h: u32 = 2_166_136_261;
            for &b in param_id.as_bytes() {
                h ^= u32::from(b);
                h = h.wrapping_mul(16_777_619);
            }
            16 + (h % 48)
        }
    }
}

/// Maps common ARKit/VRM expression names or numeric indices onto expression slots `0..63`.
#[must_use]
pub fn map_expression_name_to_slot(name: &str) -> u32 {
    if let Ok(n) = name.parse::<u32>() {
        return n.min(63);
    }
    match name.to_ascii_lowercase().as_str() {
        "angry" => 59,
        "sad" | "sorrow" => 60,
        "fun" | "relaxed" => 61,
        "surprised" => 62,
        "blink" => 8,
        "neutral" => 63,
        _ => 58,
    }
}
