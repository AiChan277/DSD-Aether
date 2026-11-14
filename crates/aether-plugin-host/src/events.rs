//! Lock-free atomic external parameter banks and non-blocking host -> UI event channel.

use core::mem::size_of;
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

use aether_ffi::{AETHER_OK, AetherHostEvent, AetherStatus};
use tokio::sync::{Mutex as AsyncMutex, mpsc};

/// Number of lock-free external float parameter slots shared between OSC/WS/UI and T2.
pub const EXTERNAL_PARAM_SLOTS: usize = 64;
/// Number of expression slots mapped by Appendix B.
pub const EXPRESSION_SLOTS: usize = 64;

/// Lock-free atomic parameter & expression trigger bank read wait-free by T2 (`HR-01`, `HR-02`).
pub struct SharedParamBank {
    external_params: [AtomicU32; EXTERNAL_PARAM_SLOTS],
    expression_overrides: [AtomicU32; EXPRESSION_SLOTS],
    hotkeys_pending: [AtomicU64; 2],
    on_air: AtomicBool,
    ws_enabled: AtomicBool,
    osc_packets_rx: AtomicU32,
    osc_rate_limited: AtomicU32,
}

impl SharedParamBank {
    /// Constructs a zero-initialized atomic parameter bank.
    #[must_use]
    pub fn new(ws_enabled: bool) -> Self {
        Self {
            external_params: core::array::from_fn(|_| AtomicU32::new(0.0_f32.to_bits())),
            expression_overrides: core::array::from_fn(|_| AtomicU32::new(0.0_f32.to_bits())),
            hotkeys_pending: [AtomicU64::new(0), AtomicU64::new(0)],
            on_air: AtomicBool::new(false),
            ws_enabled: AtomicBool::new(ws_enabled),
            osc_packets_rx: AtomicU32::new(0),
            osc_rate_limited: AtomicU32::new(0),
        }
    }

    /// Writes an external parameter slot atomically (filters non-finite inputs).
    pub fn set_external_param(&self, slot: usize, value: f32) {
        if let Some(cell) = self.external_params.get(slot) {
            let clean = if value.is_finite() { value } else { 0.0 };
            cell.store(clean.to_bits(), Ordering::Release);
        }
    }

    /// Reads an external parameter slot wait-free on T2.
    #[must_use]
    pub fn get_external_param(&self, slot: usize) -> f32 {
        self.external_params
            .get(slot)
            .map_or(0.0, |c| f32::from_bits(c.load(Ordering::Acquire)))
    }

    /// Stores a manual/OSC/WebSocket expression override weight in `[0.0, 1.0]`.
    pub fn set_expression_override(&self, slot: usize, weight: f32) {
        if let Some(cell) = self.expression_overrides.get(slot) {
            let clean = if weight.is_finite() {
                weight.clamp(0.0, 1.0)
            } else {
                0.0
            };
            cell.store(clean.to_bits(), Ordering::Release);
        }
    }

    /// Reads an expression override weight wait-free on T2.
    #[must_use]
    pub fn get_expression_override(&self, slot: usize) -> f32 {
        self.expression_overrides
            .get(slot)
            .map_or(0.0, |c| f32::from_bits(c.load(Ordering::Acquire)))
    }

    /// Latches a single-tick hotkey bit (`0..127`) from OSC/UI/WebSocket for T2 consumption.
    pub fn pulse_hotkey(&self, key_index: u32) {
        let idx = usize::try_from(key_index).unwrap_or(usize::MAX);
        if idx < 64 {
            self.hotkeys_pending[0].fetch_or(1_u64 << idx, Ordering::Release);
        } else if idx < 128 {
            self.hotkeys_pending[1].fetch_or(1_u64 << (idx - 64), Ordering::Release);
        }
    }

    /// Consumes and clears any pending externally pulsed hotkeys on T2.
    #[must_use]
    pub fn take_pending_hotkeys(&self) -> [u64; 2] {
        [
            self.hotkeys_pending[0].swap(0, Ordering::AcqRel),
            self.hotkeys_pending[1].swap(0, Ordering::AcqRel),
        ]
    }

    /// Updates ON AIR live broadcast lock flag.
    pub fn set_on_air(&self, on_air: bool) {
        self.on_air.store(on_air, Ordering::Release);
    }

    /// Returns whether ON AIR lock is active.
    #[must_use]
    pub fn is_on_air(&self) -> bool {
        self.on_air.load(Ordering::Acquire)
    }

    /// Updates active WebSocket state flag reflected in telemetry/stats.
    pub fn set_ws_enabled(&self, enabled: bool) {
        self.ws_enabled.store(enabled, Ordering::Release);
    }

    /// Returns whether WebSocket JSON-RPC endpoint is enabled.
    #[must_use]
    pub fn is_ws_enabled(&self) -> bool {
        self.ws_enabled.load(Ordering::Acquire)
    }

    /// Increments accepted inbound OSC packet counter.
    pub fn inc_osc_rx(&self) {
        self.osc_packets_rx.fetch_add(1, Ordering::Relaxed);
    }

    /// Increments rate-limited inbound OSC packet counter.
    pub fn inc_osc_rate_limited(&self) {
        self.osc_rate_limited.fetch_add(1, Ordering::Relaxed);
    }

    /// Returns `(accepted, rate_limited)` OSC counts.
    #[must_use]
    pub fn osc_counts(&self) -> (u32, u32) {
        (
            self.osc_packets_rx.load(Ordering::Relaxed),
            self.osc_rate_limited.load(Ordering::Relaxed),
        )
    }
}

/// Non-blocking host -> UI event queue with fixed `256`-byte message buffers.
pub struct HostEventQueue {
    tx: mpsc::Sender<AetherHostEvent>,
    rx: AsyncMutex<mpsc::Receiver<AetherHostEvent>>,
}

impl HostEventQueue {
    /// Creates an event queue with capacity `512` messages.
    #[must_use]
    pub fn new() -> Self {
        let (tx, rx) = mpsc::channel(512);
        Self {
            tx,
            rx: AsyncMutex::new(rx),
        }
    }

    /// Pushes a structured event toward the UI thread without blocking (`try_send`).
    pub fn push(&self, kind: u32, source_id: u64, status: AetherStatus, value_f32: f32, msg: &str) {
        let mut evt = AetherHostEvent {
            struct_size: u32::try_from(size_of::<AetherHostEvent>()).unwrap_or(280),
            kind,
            source_id,
            status,
            value_f32,
            message_utf8: [0_u8; 256],
        };
        let bytes = msg.as_bytes();
        let copy_len = bytes.len().min(255);
        evt.message_utf8[..copy_len].copy_from_slice(&bytes[..copy_len]);
        evt.message_utf8[copy_len] = 0;
        let _ = self.tx.try_send(evt);
    }

    /// Pushes an informational success event.
    pub fn push_ok(&self, kind: u32, source_id: u64, value_f32: f32, msg: &str) {
        self.push(kind, source_id, AETHER_OK, value_f32, msg);
    }

    /// Drains up to `out.len()` pending events without blocking the UI thread.
    pub fn drain(&self, out: &mut [AetherHostEvent]) -> u32 {
        let Ok(mut guard) = self.rx.try_lock() else {
            return 0;
        };
        let mut written = 0_u32;
        for dst in out {
            match guard.try_recv() {
                Ok(evt) => {
                    *dst = evt;
                    written = written.saturating_add(1);
                }
                Err(_) => break,
            }
        }
        written
    }
}

impl Default for HostEventQueue {
    fn default() -> Self {
        Self::new()
    }
}
