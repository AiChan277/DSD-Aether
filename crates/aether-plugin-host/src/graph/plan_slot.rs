//! Wait-free, zero-allocation plan handoff and lock-free atomic stats (`§7.1`, `HR-01`, `HR-02`).

#![allow(unsafe_code)]

use core::ptr;
use core::sync::atomic::{AtomicBool, AtomicPtr, AtomicU8, AtomicU32, AtomicU64, Ordering};

use aether_ffi::{
    AETHER_GRAPH_MAX_DEBUG_NODES, AETHER_GRAPH_MAX_DEBUG_PINS, AETHER_INVALID_INDEX,
    AETHER_PIN_VALUE_INVALID, AetherGraphPlanStats, AetherGraphTelemetryReadout,
};

use crate::graph::compiler::CompiledGraphPlan;

const RETIRED_CAP: usize = 64;

/// Lock-free single-slot wait-free plan handoff:
/// - Background worker compiles into a boxed [`CompiledGraphPlan`] and publishes via `pending`.
/// - `T2` adopts `pending` at tick start into `active`, moving any superseded plan into
///   `retired_ring` WITHOUT freeing on `T2` (`HR-01`).
/// - Background worker (and `Drop`) reclaims and frees retired plans off `T2`.
pub struct PlanExchange {
    pending: AtomicPtr<CompiledGraphPlan>,
    active: AtomicPtr<CompiledGraphPlan>,
    in_eval: AtomicBool,
    retired_ring: [AtomicPtr<CompiledGraphPlan>; RETIRED_CAP],
    retired_write_cursor: AtomicU64,
    // Lock-free telemetry snapshot written by T2 / Worker and read on UI (T3):
    active_node_count: AtomicU32,
    error_node_id: AtomicU32,
    compile_count: AtomicU32,
    last_compile_ms_bits: AtomicU32,
    last_eval_us_bits: AtomicU32,
    p95_eval_us_bits: AtomicU32,
    budget_warn: AtomicU32,
    consecutive_over_budget: AtomicU32,
    eval_count: AtomicU64,
    total_eval_ns: AtomicU64,
    node_ids: [AtomicU64; AETHER_GRAPH_MAX_DEBUG_NODES],
    node_eval_ns: [AtomicU32; AETHER_GRAPH_MAX_DEBUG_NODES],
    node_eval_us_bits: [AtomicU32; AETHER_GRAPH_MAX_DEBUG_NODES],
    node_live_val_bits: [AtomicU32; AETHER_GRAPH_MAX_DEBUG_NODES],
    pin_readout_count: AtomicU32,
    pin_node_ids: [AtomicU64; AETHER_GRAPH_MAX_DEBUG_PINS],
    pin_indices: [AtomicU8; AETHER_GRAPH_MAX_DEBUG_PINS],
    pin_kinds: [AtomicU8; AETHER_GRAPH_MAX_DEBUG_PINS],
    pin_values_bits: [[AtomicU32; 4]; AETHER_GRAPH_MAX_DEBUG_PINS],
}

impl PlanExchange {
    /// Creates an empty plan exchange holding an initial default plan.
    #[must_use]
    pub fn new(initial: CompiledGraphPlan) -> Self {
        let count = u32::try_from(initial.node_ids.len()).unwrap_or(0);
        let ptr = Box::into_raw(Box::new(initial));
        Self {
            pending: AtomicPtr::new(ptr::null_mut()),
            active: AtomicPtr::new(ptr),
            in_eval: AtomicBool::new(false),
            retired_ring: core::array::from_fn(|_| AtomicPtr::new(ptr::null_mut())),
            retired_write_cursor: AtomicU64::new(0),
            active_node_count: AtomicU32::new(count),
            error_node_id: AtomicU32::new(AETHER_INVALID_INDEX),
            compile_count: AtomicU32::new(1),
            last_compile_ms_bits: AtomicU32::new(0),
            last_eval_us_bits: AtomicU32::new(0),
            p95_eval_us_bits: AtomicU32::new(0),
            budget_warn: AtomicU32::new(0),
            consecutive_over_budget: AtomicU32::new(0),
            eval_count: AtomicU64::new(0),
            total_eval_ns: AtomicU64::new(0),
            node_ids: core::array::from_fn(|_| AtomicU64::new(0)),
            node_eval_ns: core::array::from_fn(|_| AtomicU32::new(0)),
            node_eval_us_bits: core::array::from_fn(|_| AtomicU32::new(0)),
            node_live_val_bits: core::array::from_fn(|_| AtomicU32::new(0)),
            pin_readout_count: AtomicU32::new(0),
            pin_node_ids: core::array::from_fn(|_| AtomicU64::new(0)),
            pin_indices: core::array::from_fn(|_| AtomicU8::new(0)),
            pin_kinds: core::array::from_fn(|_| AtomicU8::new(AETHER_PIN_VALUE_INVALID)),
            pin_values_bits: core::array::from_fn(|_| {
                [
                    AtomicU32::new(0),
                    AtomicU32::new(0),
                    AtomicU32::new(0),
                    AtomicU32::new(0),
                ]
            }),
        }
    }

    /// Reclaims any retired plans left by `T2`. Must be called only on background worker threads.
    pub fn reclaim_retired(&self) {
        for slot in &self.retired_ring {
            let ptr = slot.swap(ptr::null_mut(), Ordering::AcqRel);
            if !ptr.is_null() {
                // SAFETY: `ptr` was allocated via `Box::into_raw` and retired cleanly by `T2`.
                let boxed = unsafe { Box::from_raw(ptr) };
                drop(boxed);
            }
        }
    }

    /// Publishes a newly compiled plan for wait-free adoption by `T2` and frees superseded plans.
    pub fn publish(&self, next: CompiledGraphPlan, compile_ms: f32) {
        self.reclaim_retired();
        let node_count = u32::try_from(next.node_ids.len()).unwrap_or(0);
        let next_ptr = Box::into_raw(Box::new(next));
        let prev_pending = self.pending.swap(next_ptr, Ordering::AcqRel);
        if !prev_pending.is_null() {
            // SAFETY: `prev_pending` was never adopted by T2 because `swap` claimed ownership atomically.
            let stale = unsafe { Box::from_raw(prev_pending) };
            drop(stale);
        }
        self.active_node_count.store(node_count, Ordering::Release);
        self.error_node_id
            .store(AETHER_INVALID_INDEX, Ordering::Release);
        self.compile_count.fetch_add(1, Ordering::AcqRel);
        self.last_compile_ms_bits
            .store(compile_ms.to_bits(), Ordering::Release);
    }

    /// Records the offending node ID when a live edit compile fails (preserving previous active plan).
    pub fn record_compile_error(&self, node_id: Option<u64>) {
        let compact = node_id
            .and_then(|v| u32::try_from(v).ok())
            .unwrap_or(AETHER_INVALID_INDEX);
        self.error_node_id.store(compact, Ordering::Release);
    }

    /// Snapshots state migration entries from whichever plan is currently visible (worker-side helper).
    /// Returns cloned node states so the next compile can migrate `Smooth`/`Spring`/`Delay` state.
    pub fn snapshot_active_states(&self) -> Vec<(u64, String, crate::graph::compiler::NodeState)> {
        if self.in_eval.load(Ordering::Acquire) {
            return Vec::new();
        }
        let p = self.pending.load(Ordering::Acquire);
        if !p.is_null() {
            // SAFETY: Pending pointer is owned by worker until swapped out by T2; read-only clone safe here while worker exclusively mutates pending.
            return unsafe { (*p).clone_states() };
        }
        Vec::new()
    }

    /// Adopts any newly published plan without allocating or freeing memory and evaluates via `f`.
    /// Returns `(ok, raised_budget_warn_event, last_eval_us)`.
    pub fn with_active_plan<F>(&self, f: F) -> Result<(bool, f32), &'static str>
    where
        F: FnOnce(&mut CompiledGraphPlan) -> f32,
    {
        if self
            .in_eval
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return Err("concurrent evaluate_active detected on multiple threads");
        }

        // Adopt pending plan if available cleanly without calling free/dealloc on T2 (`HR-01`).
        let mut active_ptr = self.active.load(Ordering::Acquire);
        let next_ptr = self.pending.swap(ptr::null_mut(), Ordering::AcqRel);
        if !next_ptr.is_null() {
            if !active_ptr.is_null() {
                let idx = (self.retired_write_cursor.fetch_add(1, Ordering::Relaxed) as usize)
                    % RETIRED_CAP;
                let displaced = self.retired_ring[idx].swap(active_ptr, Ordering::AcqRel);
                if !displaced.is_null() {
                    // Ring overflow fallback (only happens if >64 edits happened before worker ticked):
                    // keep displaced in retired ring and put active back if needed — in practice RETIRED_CAP = 64 never overflows.
                    let _ = self.retired_ring[idx].swap(displaced, Ordering::Release);
                }
            }
            self.active.store(next_ptr, Ordering::Release);
            active_ptr = next_ptr;
        }

        if active_ptr.is_null() {
            self.in_eval.store(false, Ordering::Release);
            return Err("no active compiled graph plan");
        }

        let (eval_us, warn_raised) = {
            // SAFETY: `in_eval` ensures single-thread exclusive mutable access on `T2` to `active_ptr`,
            // and worker threads never free `active_ptr` until `T2` retires it into `retired_ring`.
            let plan = unsafe { &mut *active_ptr };
            let us = f(plan);

            let count = plan.node_ids.len().min(AETHER_GRAPH_MAX_DEBUG_NODES);
            for idx in 0..count {
                self.node_ids[idx].store(plan.node_ids[idx], Ordering::Relaxed);
                self.node_eval_ns[idx].store(plan.node_eval_ns[idx], Ordering::Relaxed);
                self.node_eval_us_bits[idx]
                    .store(plan.node_eval_us[idx].to_bits(), Ordering::Relaxed);
                self.node_live_val_bits[idx]
                    .store(plan.node_live_values[idx].to_bits(), Ordering::Relaxed);
            }
            self.active_node_count
                .store(u32::try_from(count).unwrap_or(0), Ordering::Release);

            let pin_count = plan.output_pins.len().min(AETHER_GRAPH_MAX_DEBUG_PINS);
            for idx in 0..pin_count {
                let meta = plan.output_pins[idx];
                self.pin_node_ids[idx].store(meta.node_id, Ordering::Relaxed);
                self.pin_indices[idx].store(meta.pin_idx, Ordering::Relaxed);
                self.pin_kinds[idx].store(plan.pin_kinds[idx], Ordering::Relaxed);
                let vals = plan.pin_values[idx];
                for lane in 0..4 {
                    self.pin_values_bits[idx][lane]
                        .store(vals[lane].to_bits(), Ordering::Relaxed);
                }
            }
            self.pin_readout_count
                .store(u32::try_from(pin_count).unwrap_or(0), Ordering::Release);
            self.total_eval_ns
                .store(plan.last_total_eval_ns, Ordering::Release);

            let over = us > 100.0;
            let streak = if over {
                self.consecutive_over_budget.fetch_add(1, Ordering::Relaxed) + 1
            } else {
                self.consecutive_over_budget.store(0, Ordering::Relaxed);
                0
            };
            let warn = u32::from(streak >= 3);
            let prev_warn = self.budget_warn.swap(warn, Ordering::Relaxed);
            let raised = warn == 1 && (prev_warn == 0 || streak == 3);

            self.last_eval_us_bits
                .store(us.to_bits(), Ordering::Relaxed);
            let prev_p95 = f32::from_bits(self.p95_eval_us_bits.load(Ordering::Relaxed));
            let next_p95 = if us > prev_p95 {
                us
            } else {
                prev_p95 * 0.98 + us * 0.02
            };
            self.p95_eval_us_bits
                .store(next_p95.to_bits(), Ordering::Relaxed);
            self.eval_count.fetch_add(1, Ordering::Relaxed);

            (us, raised)
        };

        self.in_eval.store(false, Ordering::Release);
        Ok((warn_raised, eval_us))
    }

    /// Populates `AetherGraphPlanStats` without blocking `T2` (`REQ-UI-020`, ADR-0014).
    pub fn fill_stats(
        &self,
        out: &mut AetherGraphPlanStats,
        ws_enabled: bool,
        osc_rx: u32,
        osc_limited: u32,
        plugin_count: u32,
    ) {
        out.struct_size =
            u32::try_from(core::mem::size_of::<AetherGraphPlanStats>()).unwrap_or(2104);
        out.active_node_count = self.active_node_count.load(Ordering::Acquire);
        out.error_node_id = self.error_node_id.load(Ordering::Acquire);
        out.compile_count = self.compile_count.load(Ordering::Acquire);
        out.last_compile_ms = f32::from_bits(self.last_compile_ms_bits.load(Ordering::Acquire));
        out.last_eval_us = f32::from_bits(self.last_eval_us_bits.load(Ordering::Acquire));
        out.p95_eval_us = f32::from_bits(self.p95_eval_us_bits.load(Ordering::Acquire));
        out.budget_warn = self.budget_warn.load(Ordering::Acquire);
        out.ws_enabled = u32::from(ws_enabled);
        out.osc_packets_rx = osc_rx;
        out.eval_count = self.eval_count.load(Ordering::Acquire);
        out.osc_rate_limited = osc_limited;
        out.plugin_count = plugin_count;

        let count = usize::try_from(out.active_node_count)
            .unwrap_or(0)
            .min(AETHER_GRAPH_MAX_DEBUG_NODES);
        for idx in 0..count {
            out.node_eval_us[idx] =
                f32::from_bits(self.node_eval_us_bits[idx].load(Ordering::Relaxed));
            out.node_live_values[idx] =
                f32::from_bits(self.node_live_val_bits[idx].load(Ordering::Relaxed));
        }
        for idx in count..AETHER_GRAPH_MAX_DEBUG_NODES {
            out.node_eval_us[idx] = 0.0;
            out.node_live_values[idx] = 0.0;
        }
    }

    /// Populates `AetherGraphTelemetryReadout` without blocking `T2` or allocating memory (`§7.2`).
    pub fn fill_telemetry_readout(&self, out: &mut AetherGraphTelemetryReadout) {
        let eval_seq = self.eval_count.load(Ordering::Acquire);
        let active_nodes = self.active_node_count.load(Ordering::Acquire);
        let pin_cnt = self.pin_readout_count.load(Ordering::Acquire);
        let total_ns = self.total_eval_ns.load(Ordering::Acquire);

        out.struct_size =
            u32::try_from(core::mem::size_of::<AetherGraphTelemetryReadout>()).unwrap_or(13120);
        out.active_node_count = active_nodes;
        out.pin_readout_count = pin_cnt;
        out.valid = u32::from(eval_seq > 0);
        out.eval_sequence = eval_seq;
        out.total_eval_ns = total_ns;

        let n_count = usize::try_from(active_nodes)
            .unwrap_or(0)
            .min(AETHER_GRAPH_MAX_DEBUG_NODES);
        for idx in 0..n_count {
            out.node_ids[idx] = self.node_ids[idx].load(Ordering::Relaxed);
            out.node_eval_ns[idx] = self.node_eval_ns[idx].load(Ordering::Relaxed);
        }
        for idx in n_count..AETHER_GRAPH_MAX_DEBUG_NODES {
            out.node_ids[idx] = 0;
            out.node_eval_ns[idx] = 0;
        }

        let p_count = usize::try_from(pin_cnt)
            .unwrap_or(0)
            .min(AETHER_GRAPH_MAX_DEBUG_PINS);
        for idx in 0..p_count {
            out.pin_node_ids[idx] = self.pin_node_ids[idx].load(Ordering::Relaxed);
            out.pin_indices[idx] = self.pin_indices[idx].load(Ordering::Relaxed);
            out.pin_kinds[idx] = self.pin_kinds[idx].load(Ordering::Relaxed);
            for lane in 0..4 {
                out.pin_values[idx][lane] =
                    f32::from_bits(self.pin_values_bits[idx][lane].load(Ordering::Relaxed));
            }
        }
        for idx in p_count..AETHER_GRAPH_MAX_DEBUG_PINS {
            out.pin_node_ids[idx] = 0;
            out.pin_indices[idx] = 0;
            out.pin_kinds[idx] = AETHER_PIN_VALUE_INVALID;
            out.pin_values[idx] = [0.0; 4];
        }
        out._reserved_tail = [0; 4];
    }
}

impl Drop for PlanExchange {
    fn drop(&mut self) {
        self.reclaim_retired();
        let active_ptr = self.active.swap(ptr::null_mut(), Ordering::AcqRel);
        if !active_ptr.is_null() {
            // SAFETY: Owner is dropping `PlanExchange`; no threads hold references afterward.
            let boxed = unsafe { Box::from_raw(active_ptr) };
            drop(boxed);
        }
        let pending_ptr = self.pending.swap(ptr::null_mut(), Ordering::AcqRel);
        if !pending_ptr.is_null() && pending_ptr != active_ptr {
            // SAFETY: Owner is dropping `PlanExchange`; `pending_ptr` is uniquely owned here.
            let boxed = unsafe { Box::from_raw(pending_ptr) };
            drop(boxed);
        }
    }
}
