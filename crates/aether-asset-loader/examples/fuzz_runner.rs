#![allow(clippy::cast_possible_truncation)]
#![allow(clippy::cast_precision_loss)]

//! Native Windows coverage-guided-style mutating fuzz runner for AETHER-2.5D (`ADR-0005`, `HR-06`).
//!
//! Runs `fuzz_glb`, `fuzz_vrm_json`, `fuzz_fbx`, or `fuzz_pmx` continuously for `--seconds N`
//! (600 s per target in release mode for the Phase 2 exit gate) seeded from `tests/data/`.

use std::fs;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use aether_asset_loader::fuzz_entry::{fuzz_fbx, fuzz_glb, fuzz_pmx, fuzz_vrm_json};

#[derive(Clone, Copy)]
enum FuzzTarget {
    Glb,
    VrmJson,
    Fbx,
    Pmx,
}

impl FuzzTarget {
    fn from_str(s: &str) -> Option<Self> {
        match s {
            "glb" | "fuzz_glb" => Some(Self::Glb),
            "vrm_json" | "fuzz_vrm_json" => Some(Self::VrmJson),
            "fbx" | "fuzz_fbx" => Some(Self::Fbx),
            "pmx" | "fuzz_pmx" => Some(Self::Pmx),
            _ => None,
        }
    }

    const fn name(self) -> &'static str {
        match self {
            Self::Glb => "fuzz_glb",
            Self::VrmJson => "fuzz_vrm_json",
            Self::Fbx => "fuzz_fbx",
            Self::Pmx => "fuzz_pmx",
        }
    }

    fn run(self, data: &[u8]) {
        match self {
            Self::Glb => fuzz_glb(data),
            Self::VrmJson => fuzz_vrm_json(data),
            Self::Fbx => fuzz_fbx(data),
            Self::Pmx => fuzz_pmx(data),
        }
    }
}

struct XorShift64(u64);

impl XorShift64 {
    fn next_u64(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x
    }

    fn gen_range(&mut self, upper: usize) -> usize {
        if upper <= 1 {
            0
        } else {
            (self.next_u64() as usize) % upper
        }
    }
}

fn collect_seeds(target: FuzzTarget, root: &Path) -> Vec<Vec<u8>> {
    let mut seeds = Vec::new();
    let dirs = [
        root.join("tests").join("data"),
        root.join("tests").join("data").join("corpus"),
        root.join("tests").join("data").join("edge_cases"),
    ];
    for d in dirs {
        if let Ok(entries) = fs::read_dir(&d) {
            for ent in entries.flatten() {
                let p = ent.path();
                if !p.is_file() {
                    continue;
                }
                let ext = p
                    .extension()
                    .and_then(|e| e.to_str())
                    .map(str::to_ascii_lowercase)
                    .unwrap_or_default();
                let want = match target {
                    FuzzTarget::Glb | FuzzTarget::VrmJson => {
                        matches!(ext.as_str(), "vrm" | "glb" | "gltf")
                    }
                    FuzzTarget::Fbx => ext == "fbx",
                    FuzzTarget::Pmx => matches!(ext.as_str(), "pmx" | "pmd"),
                };
                if want && let Ok(bytes) = fs::read(&p) {
                    // Cap single seed size in fuzzer for high exec/s throughput
                    let trimmed = if bytes.len() > 262_144 {
                        bytes[..262_144].to_vec()
                    } else {
                        bytes
                    };
                    if matches!(target, FuzzTarget::VrmJson)
                        && trimmed.starts_with(b"glTF")
                        && trimmed.len() > 24
                    {
                        let json_len = u32::from_le_bytes([
                            trimmed[12],
                            trimmed[13],
                            trimmed[14],
                            trimmed[15],
                        ]) as usize;
                        if let Some(j_slice) =
                            trimmed.get(20..20 + json_len.min(trimmed.len() - 20))
                        {
                            seeds.push(j_slice.to_vec());
                            continue;
                        }
                    }
                    seeds.push(trimmed);
                }
            }
        }
    }
    if seeds.is_empty() {
        let fallback = match target {
            FuzzTarget::Glb => b"glTF\x02\x00\x00\x00\x14\x00\x00\x00".to_vec(),
            FuzzTarget::VrmJson => br#"{"asset":{"version":"2.0"}}"#.to_vec(),
            FuzzTarget::Fbx => b"Kaydara FBX Binary  \x00\x1a\x00".to_vec(),
            FuzzTarget::Pmx => b"PMX \x00\x00\x00\x40\x08\x00\x00\x01\x01\x01\x01\x01\x01".to_vec(),
        };
        seeds.push(fallback);
    }
    seeds
}

fn mutate_buf(rng: &mut XorShift64, seeds: &[Vec<u8>], work: &mut Vec<u8>) {
    let base = &seeds[rng.gen_range(seeds.len())];
    work.clear();
    work.extend_from_slice(base);
    if work.is_empty() {
        work.extend_from_slice(&[0u8; 32]);
        return;
    }

    let num_ops = 1 + rng.gen_range(8);
    for _ in 0..num_ops {
        if work.is_empty() {
            break;
        }
        match rng.gen_range(6) {
            // Bit flip
            0 => {
                let pos = rng.gen_range(work.len());
                work[pos] ^= 1u8 << (rng.gen_range(8) as u8);
            }
            // Extreme byte overwrite
            1 => {
                let pos = rng.gen_range(work.len());
                const EXTREME: [u8; 6] = [0x00, 0x7F, 0x80, 0xFF, b'{', b'"'];
                work[pos] = EXTREME[rng.gen_range(EXTREME.len())];
            }
            // Little-endian extreme u32 length/count poison
            2 if work.len() >= 4 => {
                let pos = rng.gen_range(work.len() - 3);
                const VALS: [u32; 5] = [0, 1, 0x7FFF_FFFF, 0xFFFF_FFFE, 0xFFFF_FFFF];
                let v = VALS[rng.gen_range(VALS.len())].to_le_bytes();
                work[pos..pos + 4].copy_from_slice(&v);
            }
            // Random truncation
            3 if work.len() > 8 => {
                let new_len = 1 + rng.gen_range(work.len() - 1);
                work.truncate(new_len);
            }
            // Random slice duplication / growth (capped at 512 KiB)
            4 if work.len() < 524_288 => {
                let src = rng.gen_range(work.len());
                let max_chunk = (work.len() - src).min(64);
                let chunk_len = 1 + rng.gen_range(max_chunk);
                let chunk = work[src..src + chunk_len].to_vec();
                let dst = rng.gen_range(work.len() + 1);
                work.splice(dst..dst, chunk);
            }
            // Byte range erase
            _ if work.len() > 4 => {
                let pos = rng.gen_range(work.len());
                let del = (1 + rng.gen_range(16)).min(work.len() - pos);
                work.drain(pos..pos + del);
            }
            _ => {}
        }
    }
}

fn find_repo_root() -> PathBuf {
    let mut cur = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    for _ in 0..5 {
        if cur.join("tests").join("data").is_dir() {
            return cur;
        }
        if let Some(p) = cur.parent() {
            cur = p.to_path_buf();
        } else {
            break;
        }
    }
    PathBuf::from(".")
}

fn main() {
    let mut target_arg = String::from("glb");
    let mut seconds: u64 = 5;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--target"
            && let Some(val) = args.next()
        {
            target_arg = val;
        } else if arg == "--seconds"
            && let Some(val) = args.next()
            && let Ok(s) = val.parse::<u64>()
        {
            seconds = s.max(1);
        }
    }

    let targets: Vec<FuzzTarget> = if target_arg == "all" {
        vec![
            FuzzTarget::Glb,
            FuzzTarget::VrmJson,
            FuzzTarget::Fbx,
            FuzzTarget::Pmx,
        ]
    } else if let Some(t) = FuzzTarget::from_str(&target_arg) {
        vec![t]
    } else {
        eprintln!("Unknown fuzz target: {target_arg}");
        std::process::exit(2);
    };

    let root = find_repo_root();
    let base_seed: u64 = std::env::var("AETHER_FUZZ_SEED")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(0xA374_E25D_0000_0001);

    for (idx, &target) in targets.iter().enumerate() {
        let seeds = collect_seeds(target, &root);
        let mut rng = XorShift64(base_seed.wrapping_add((idx as u64 + 1) * 0x9E37_79B9_7F4A_7C15));
        let mut work = Vec::with_capacity(262_144);

        // First replay every corpus seed cleanly.
        for seed in &seeds {
            if catch_unwind(AssertUnwindSafe(|| target.run(seed))).is_err() {
                eprintln!("PANIC on unmutated seed for target {}", target.name());
                std::process::exit(1);
            }
        }

        let start = Instant::now();
        let budget = Duration::from_secs(seconds);
        let mut iters: u64 = 0;

        while start.elapsed() < budget {
            mutate_buf(&mut rng, &seeds, &mut work);
            if catch_unwind(AssertUnwindSafe(|| target.run(&work))).is_err() {
                eprintln!(
                    "FUZZ PANIC in target={} iter={} bytes={}",
                    target.name(),
                    iters,
                    work.len()
                );
                std::process::exit(1);
            }
            iters += 1;
        }

        let elapsed = start.elapsed().as_secs_f64().max(1e-6);
        println!(
            "[{}] completed={}s iterations={} rate={:.1} exec/s crashes=0",
            target.name(),
            elapsed.round() as u64,
            iters,
            (iters as f64) / elapsed
        );
    }
}
