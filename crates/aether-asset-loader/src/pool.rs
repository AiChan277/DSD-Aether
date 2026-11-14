//! Dedicated Rayon worker pool for asset baking & parallel image decode (`§3.6`).

use std::sync::OnceLock;

use rayon::{ThreadPool, ThreadPoolBuilder};

static ASSET_POOL: OnceLock<Option<ThreadPool>> = OnceLock::new();

/// Initializes (once) and runs `op` inside the bounded asset-loader Rayon worker pool.
pub fn run_in_worker_pool<R: Send>(requested_threads: usize, op: impl FnOnce() -> R + Send) -> R {
    let pool_opt = ASSET_POOL.get_or_init(|| {
        let default_threads =
            std::thread::available_parallelism().map_or(4, |n| (n.get() / 2).clamp(2, 8));
        let num_threads = if requested_threads == 0 {
            default_threads
        } else {
            requested_threads.clamp(1, 16)
        };
        ThreadPoolBuilder::new()
            .num_threads(num_threads)
            .thread_name(|idx| format!("aether-asset-{idx}"))
            .build()
            .or_else(|_| ThreadPoolBuilder::new().num_threads(1).build())
            .ok()
    });
    match pool_opt {
        Some(pool) => pool.install(op),
        None => op(),
    }
}
