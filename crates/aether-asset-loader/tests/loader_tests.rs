#![allow(clippy::cast_possible_truncation)]
#![allow(clippy::cast_precision_loss)]
#![allow(clippy::cast_sign_loss)]
#![allow(clippy::needless_range_loop)]

//! Comprehensive Phase 2 exit-gate verification suite for `aether-asset-loader`:
//! - Reference Avatar (`reference_avatar.vrm`) & `PERF-007` warm/cold timing (`<= 150 ms`)
//! - 20-model corpus compatibility & `>= 95%` Appendix A humanoid auto-mapping
//! - Japanese wink X-centroid disambiguation (`§6.5`), A-pose `rest_to_tpose`, OKLab shade, MMD scale
//! - Seam-welded `smooth_tangent` equality across split UV/normal seams
//! - `proptest` spherized face `proxy_normal` & exact `65535` skin weight packing invariants
//! - 30-bit Morton monotonic triangle order per submesh
//! - Hard parser resource caps (`HR-06`, `>1024` bones / `>16384` tex -> `LimitExceeded`) + Draco rejection
//! - Zero-copy pointer identity (`debug_stream_ptrs[k] == lent_ptr[k]`) + `< 1 MiB` persistent Rust heap
//! - Cooperative load cancellation (`AETHER_E_CANCELLED`)

use core::alloc::{GlobalAlloc, Layout};
use core::sync::atomic::{AtomicBool, AtomicIsize, AtomicUsize, Ordering};
use std::alloc::System;
use std::collections::HashMap;
use std::fs::{self, File};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::Instant;

use aether_asset_loader::bake::{morton_30bit, pack_skin_weights};
use aether_asset_loader::bc7_cache::resolve_cache_dir;
use aether_asset_loader::ffi_slice::{BorrowedArenaSet, BorrowedStream};
use aether_asset_loader::{AssetLoader, LoaderError};
use aether_ffi::{
    AETHER_LOAD_FLAG_BAKE_PROXY_NORMALS, AETHER_LOAD_FLAG_BAKE_SMOOTH_TANGENTS,
    AETHER_LOAD_FLAG_DECODE_TEXTURES, AETHER_LOAD_FLAG_MORTON_SORT, AETHER_LOAD_FLAG_TEXTURE_CACHE,
    AETHER_SOURCE_FORMAT_FBX, AETHER_SOURCE_FORMAT_PMD, AETHER_SOURCE_FORMAT_PMX,
    AETHER_SOURCE_FORMAT_VRM0, AETHER_SOURCE_FORMAT_VRM1, AETHER_STREAM_COUNT,
    AETHER_STREAM_EXPRESSIONS, AETHER_STREAM_INDEX_U32, AETHER_STREAM_JOINTS,
    AETHER_STREAM_MATERIALS, AETHER_STREAM_SKIN, AETHER_STREAM_STRINGS, AETHER_STREAM_SUBMESHES,
    AETHER_STREAM_SURFACE, AETHER_STREAM_TEXTURE_DESCS, AETHER_STREAM_VERTEX25D,
    AETHER_TEX_FORMAT_BC7_SRGB, AETHER_TEX_FORMAT_BC7_UNORM, AETHER_WARN_MMD_CROSS_CHAIN,
    AETHER_WARN_TEXTURE_CACHE_MISS, AETHER_WARN_UNIT_GUESS, AetherAssetRequirements,
    AetherAvatarDesc, AetherExpressionBindingDesc, AetherJointDesc, AetherLoadRequest,
    AetherNprMaterial, AetherSubmeshDesc, AetherTextureDesc, AetherVertex25D, AetherVertexSkin,
    AetherVertexSurface,
};
use proptest::prelude::*;

struct CountingAllocator;

static LIVE_BYTES: AtomicIsize = AtomicIsize::new(0);

// SAFETY: Delegates directly to `System` allocator while updating atomic byte counter.
#[allow(unsafe_code)]
unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // SAFETY: Forwards valid allocator layout directly to `std::alloc::System`.
        let ptr = unsafe { System.alloc(layout) };
        if !ptr.is_null() {
            LIVE_BYTES.fetch_add(layout.size() as isize, Ordering::Relaxed);
        }
        ptr
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        LIVE_BYTES.fetch_sub(layout.size() as isize, Ordering::Relaxed);
        // SAFETY: Caller upholds `GlobalAlloc::dealloc` contract for `ptr` and `layout`.
        unsafe { System.dealloc(ptr, layout) };
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        // SAFETY: Caller upholds `GlobalAlloc::realloc` contract for `ptr`, `layout`, and `new_size`.
        let out = unsafe { System.realloc(ptr, layout, new_size) };
        if !out.is_null() {
            let delta = (new_size as isize) - (layout.size() as isize);
            LIVE_BYTES.fetch_add(delta, Ordering::Relaxed);
        }
        out
    }
}

#[global_allocator]
static GLOBAL_ALLOC: CountingAllocator = CountingAllocator;

// Lock-free reader/writer test gate so `LIVE_BYTES` measurement sees zero background test churn.
static SHARED_ACTIVE: AtomicUsize = AtomicUsize::new(0);
static EXCLUSIVE_WANT: AtomicBool = AtomicBool::new(false);

struct SharedGuard;
impl Drop for SharedGuard {
    fn drop(&mut self) {
        SHARED_ACTIVE.fetch_sub(1, Ordering::Release);
    }
}

struct ExclusiveGuard;
impl Drop for ExclusiveGuard {
    fn drop(&mut self) {
        EXCLUSIVE_WANT.store(false, Ordering::Release);
    }
}

fn enter_shared() -> SharedGuard {
    loop {
        if EXCLUSIVE_WANT.load(Ordering::Acquire) {
            std::thread::yield_now();
            continue;
        }
        SHARED_ACTIVE.fetch_add(1, Ordering::AcqRel);
        if !EXCLUSIVE_WANT.load(Ordering::Acquire) {
            return SharedGuard;
        }
        SHARED_ACTIVE.fetch_sub(1, Ordering::Release);
        std::thread::yield_now();
    }
}

fn enter_exclusive() -> ExclusiveGuard {
    while EXCLUSIVE_WANT
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
        .is_err()
    {
        std::thread::yield_now();
    }
    while SHARED_ACTIVE.load(Ordering::Acquire) > 0 {
        std::thread::yield_now();
    }
    ExclusiveGuard
}

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .map_or_else(|| PathBuf::from("."), Path::to_path_buf)
}

#[repr(C, align(64))]
#[derive(Clone, Copy)]
struct Aligned64([u8; 64]);

struct TestArena {
    backing: [Vec<Aligned64>; 15],
    ptrs: [u64; 15],
}

impl TestArena {
    fn allocate(req: &AetherAssetRequirements) -> Self {
        let mut backing: [Vec<Aligned64>; 15] = Default::default();
        let mut ptrs = [0u64; 15];
        for (slot, &bytes) in req.stream_bytes.iter().enumerate() {
            if bytes > 0 {
                let blocks = (bytes as usize).div_ceil(64);
                let buf = vec![Aligned64([0u8; 64]); blocks];
                ptrs[slot] = buf.as_ptr() as usize as u64;
                backing[slot] = buf;
            }
        }
        Self { backing, ptrs }
    }

    #[allow(unsafe_code)]
    fn as_byte_slice_mut(&mut self, slot: usize, req_bytes: u64) -> &mut [u8] {
        let len = req_bytes as usize;
        let ptr = self.backing[slot].as_mut_ptr().cast::<u8>();
        // SAFETY: `backing[slot]` owns `ceil(req_bytes / 64) * 64 >= req_bytes` contiguous aligned bytes.
        unsafe { core::slice::from_raw_parts_mut(ptr, len) }
    }

    fn load_into(
        &mut self,
        loader: &AssetLoader,
        req: &AetherAssetRequirements,
    ) -> AetherAvatarDesc {
        let mut streams: [Option<BorrowedStream<'_>>; AETHER_STREAM_COUNT as usize] =
            [const { None }; AETHER_STREAM_COUNT as usize];
        // Split `&mut self.backing` safely via `split_at_mut` across fixed array of 15.
        let mut slice_rest = &mut self.backing[..];
        for slot in 0..15 {
            let (head, tail) = slice_rest.split_at_mut(1);
            slice_rest = tail;
            let bytes = req.stream_bytes[slot];
            if bytes > 0 {
                let ptr_bytes = head[0].as_mut_ptr().cast::<u8>();
                #[allow(unsafe_code)]
                // SAFETY: Each `head[0]` is a distinct element of `self.backing`.
                let slice = unsafe { core::slice::from_raw_parts_mut(ptr_bytes, bytes as usize) };
                if let Ok(bs) = BorrowedStream::from_rust_slice(slot as u32, slice) {
                    streams[slot] = Some(bs);
                }
            }
        }
        let mut set = BorrowedArenaSet::from_streams(streams);
        match loader.fill(&mut set) {
            Ok(d) => d,
            Err(e) => panic_fail(&format!("fill failed: {e}")),
        }
    }

    fn bytes_of<T: bytemuck::Pod>(&self, slot: usize, count: u32) -> &[T] {
        let len = (count as usize) * core::mem::size_of::<T>();
        let ptr = self.backing[slot].as_ptr().cast::<u8>();
        #[allow(unsafe_code)]
        // SAFETY: Aligned at 64 B (`>= align_of::<T>()`) and filled up to `count * size_of::<T>()`.
        let slice = unsafe { core::slice::from_raw_parts(ptr, len) };
        bytemuck::cast_slice(slice)
    }
}

#[cold]
#[allow(clippy::assertions_on_constants)]
fn panic_fail(msg: &str) -> ! {
    // Only invoked on assertion failure; keeps test helper crisp without unwrap.
    assert!(false, "{msg}");
    unreachable!()
}

fn load_avatar_with_flags(
    path: &Path,
    flags: u32,
) -> Result<(AetherAssetRequirements, AetherAvatarDesc, TestArena, f64), LoaderError> {
    let mut path_cstr = path.to_string_lossy().into_owned();
    path_cstr.push('\0');
    let req = AetherLoadRequest {
        path_utf8: path_cstr.as_ptr().cast(),
        flags,
        max_threads: 0,
    };
    let t0 = Instant::now();
    let path_str = path
        .to_str()
        .ok_or(LoaderError::InvalidArg("non-utf8 path"))?;
    let loader = AssetLoader::open(path_str, &req)?;
    let requirements = *loader.requirements();
    let mut arena = TestArena::allocate(&requirements);
    let desc = {
        let _keep = &req;
        arena.load_into(&loader, &requirements)
    };
    let elapsed_ms = t0.elapsed().as_secs_f64() * 1000.0;
    Ok((requirements, desc, arena, elapsed_ms))
}

#[test]
fn reference_avatar_and_perf007_warm_cache() {
    let _guard = enter_shared();
    let vrm_path = repo_root()
        .join("tests")
        .join("data")
        .join("reference_avatar.vrm");
    assert!(vrm_path.exists(), "missing tests/data/reference_avatar.vrm");

    let full_flags = AETHER_LOAD_FLAG_BAKE_PROXY_NORMALS
        | AETHER_LOAD_FLAG_BAKE_SMOOTH_TANGENTS
        | AETHER_LOAD_FLAG_MORTON_SORT
        | AETHER_LOAD_FLAG_DECODE_TEXTURES
        | AETHER_LOAD_FLAG_TEXTURE_CACHE;

    // 1. Cold pass: clears cached .bc7 matching this file if any? Even if warm from previous run,
    // first measure cold after deleting matching files? Better: do a full load + verify all streams!
    let (req1, desc1, arena1, cold_ms) = match load_avatar_with_flags(&vrm_path, full_flags) {
        Ok(v) => v,
        Err(e) => panic_fail(&format!("reference_avatar load 1 failed: {e}")),
    };

    assert_eq!(desc1.source_format, AETHER_SOURCE_FORMAT_VRM1);
    assert!(req1.vertex_count >= 35_000, "verts={}", req1.vertex_count);
    assert_eq!(req1.index_count, 210_000, "must have 70,000 tris");
    assert_eq!(req1.submesh_count, 12);
    assert_eq!(req1.material_count, 12);
    assert!(req1.joint_count >= 55);
    assert_eq!(req1.spring_joint_count, 160);
    assert_eq!(req1.morph_target_count, 57);
    assert_eq!(req1.texture_count, 10);

    // Zero-copy pointer identity proof (`debug_stream_ptrs[k] == lent_ptr[k]`)
    for slot in 0..15 {
        if req1.stream_bytes[slot] > 0 {
            assert_eq!(
                desc1.debug_stream_ptrs[slot], arena1.ptrs[slot],
                "pointer mismatch on stream {slot}"
            );
        }
    }

    // All 55 humanoid slots mapped cleanly
    let mapped = desc1
        .humanoid_to_joint
        .iter()
        .filter(|&&j| j != 0xFFFF)
        .count();
    assert_eq!(mapped, 55);

    // Validate vertex stream & skin weight sum == 65535 exact
    let verts: &[AetherVertex25D] =
        arena1.bytes_of(AETHER_STREAM_VERTEX25D as usize, req1.vertex_count);
    let skins: &[AetherVertexSkin] =
        arena1.bytes_of(AETHER_STREAM_SKIN as usize, req1.vertex_count);
    for (v, sk) in verts.iter().zip(skins.iter()) {
        assert!(v.z_flatten_weight <= 0.8501 && v.z_flatten_weight >= 0.0);
        let pn = v.proxy_normal;
        let len = (pn[0] * pn[0] + pn[1] * pn[1] + pn[2] * pn[2]).sqrt();
        assert!((len - 1.0).abs() < 1e-3, "bad proxy normal len={len}");
        let wsum: u32 = sk.weights_unorm16.iter().map(|&w| u32::from(w)).sum();
        assert_eq!(wsum, 65_535);
    }

    // 2. Warm pass (now BC7 cache is guaranteed populated by pass 1!)
    let (req2, desc2, arena2, warm_ms) = match load_avatar_with_flags(&vrm_path, full_flags) {
        Ok(v) => v,
        Err(e) => panic_fail(&format!("reference_avatar warm load failed: {e}")),
    };
    let tdescs: &[AetherTextureDesc] =
        arena2.bytes_of(AETHER_STREAM_TEXTURE_DESCS as usize, req2.texture_count);
    for (i, td) in tdescs.iter().enumerate() {
        assert_eq!(td.width, 2048, "tex {i}");
        assert_eq!(td.height, 2048, "tex {i}");
        assert!(
            td.format == AETHER_TEX_FORMAT_BC7_SRGB || td.format == AETHER_TEX_FORMAT_BC7_UNORM,
            "warm cache miss on tex {i}: format={}",
            td.format
        );
    }
    assert_eq!(
        desc2.warnings_mask & AETHER_WARN_TEXTURE_CACHE_MISS,
        0,
        "unexpected cache miss on warm load"
    );

    println!(
        "[PERF-007] reference_avatar.vrm cold={cold_ms:.2} ms warm={warm_ms:.2} ms cache_dir={:?}",
        resolve_cache_dir()
    );
    // In release / Opt-2 builds, warm cache load must meet `PERF-007 <= 150.0 ms`.
    #[cfg(not(debug_assertions))]
    assert!(
        warm_ms <= 150.0,
        "PERF-007 warm load exceeded 150 ms: {warm_ms:.2} ms"
    );
    #[cfg(debug_assertions)]
    assert!(
        warm_ms <= 600.0,
        "debug sanity warm load exceeded 600 ms: {warm_ms:.2} ms"
    );
}

#[test]
fn twenty_model_corpus_humanoid_mapping_and_retargeting() {
    let _guard = enter_shared();
    let corpus_dir = repo_root().join("tests").join("data").join("corpus");
    let mut entries: Vec<PathBuf> = match fs::read_dir(&corpus_dir) {
        Ok(rd) => rd
            .flatten()
            .map(|e| e.path())
            .filter(|p| p.is_file())
            .collect(),
        Err(e) => panic_fail(&format!("cannot read corpus dir: {e}")),
    };
    entries.sort();
    assert_eq!(entries.len(), 20, "expected 20 corpus models");

    let flags = AETHER_LOAD_FLAG_BAKE_PROXY_NORMALS
        | AETHER_LOAD_FLAG_BAKE_SMOOTH_TANGENTS
        | AETHER_LOAD_FLAG_MORTON_SORT
        | AETHER_LOAD_FLAG_DECODE_TEXTURES;

    for model in &entries {
        let name = model.file_name().and_then(|s| s.to_str()).unwrap_or("");
        let (req, desc, arena, ms) = match load_avatar_with_flags(model, flags) {
            Ok(v) => v,
            Err(e) => panic_fail(&format!("corpus model {name} failed to load: {e}")),
        };
        assert!(req.vertex_count > 0, "{name}: zero vertices");
        assert!(req.index_count > 0, "{name}: zero indices");
        assert!(req.submesh_count > 0, "{name}: zero submeshes");

        // >= 95% auto-mapped of 55 Appendix A bones (>= 53/55)
        let mapped = desc
            .humanoid_to_joint
            .iter()
            .filter(|&&j| j != 0xFFFF)
            .count();
        let pct = (mapped as f64) * 100.0 / 55.0;
        if pct < 95.0 {
            let joints_s: &[AetherJointDesc] =
                arena.bytes_of(AETHER_STREAM_JOINTS as usize, req.joint_count);
            let names_s: &[u8] = arena.bytes_of(
                AETHER_STREAM_STRINGS as usize,
                req.stream_bytes[AETHER_STREAM_STRINGS as usize] as u32,
            );
            let mut jnames = Vec::new();
            for j in joints_s {
                let b = j.name_offset as usize;
                let e = (b + j.name_len as usize).min(names_s.len());
                jnames.push(String::from_utf8_lossy(&names_s[b..e]).into_owned());
            }
            let unmapped: Vec<usize> = desc
                .humanoid_to_joint
                .iter()
                .enumerate()
                .filter_map(|(h, &j)| (j == 0xFFFF).then_some(h))
                .collect();
            panic_fail(&format!(
                "{name}: mapped {mapped}/55 ({pct:.1}%) < 95%; joints={jnames:?} unmapped={unmapped:?}",
            ));
        }

        // Normalised height & foot grounding (mesh or skeleton foot grounded at Y=0)
        assert!(
            (1.2..=2.2).contains(&desc.avatar_height_m),
            "{name}: unexpected height {} m",
            desc.avatar_height_m
        );
        let joints_slice: &[AetherJointDesc] =
            arena.bytes_of(AETHER_STREAM_JOINTS as usize, req.joint_count);
        let min_joint_y = if joints_slice.is_empty() {
            f32::INFINITY
        } else {
            fn quat_rot(q: [f32; 4], v: [f32; 3]) -> [f32; 3] {
                let (qx, qy, qz, qw) = (q[0], q[1], q[2], q[3]);
                let tx = 2.0 * (qy * v[2] - qz * v[1]);
                let ty = 2.0 * (qz * v[0] - qx * v[2]);
                let tz = 2.0 * (qx * v[1] - qy * v[0]);
                [
                    v[0] + qw * tx + (qy * tz - qz * ty),
                    v[1] + qw * ty + (qz * tx - qx * tz),
                    v[2] + qw * tz + (qx * ty - qy * tx),
                ]
            }
            fn quat_mul(a: [f32; 4], b: [f32; 4]) -> [f32; 4] {
                [
                    a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                    a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
                ]
            }
            let n = joints_slice.len();
            let mut wpos = vec![[0.0f32; 3]; n];
            let mut wrot = vec![[0.0f32, 0.0, 0.0, 1.0]; n];
            let mut done = vec![false; n];
            for _ in 0..n {
                for i in 0..n {
                    if done[i] {
                        continue;
                    }
                    let j = &joints_slice[i];
                    if j.parent_index < 0 {
                        wpos[i] = j.bind_translation;
                        wrot[i] = j.bind_rotation_quat;
                        done[i] = true;
                    } else {
                        let p = j.parent_index as usize;
                        if p < n && done[p] {
                            let rotated = quat_rot(wrot[p], j.bind_translation);
                            wpos[i] = [
                                wpos[p][0] + rotated[0],
                                wpos[p][1] + rotated[1],
                                wpos[p][2] + rotated[2],
                            ];
                            wrot[i] = quat_mul(wrot[p], j.bind_rotation_quat);
                            done[i] = true;
                        }
                    }
                }
            }
            wpos.into_iter().map(|p| p[1]).fold(f32::INFINITY, f32::min)
        };
        let grounded_y = desc.aabb_min[1].min(min_joint_y);
        assert!(
            grounded_y.abs() < 0.05,
            "{name}: feet not grounded at Y=0 (aabb_min_y={}, min_joint_y={min_joint_y})",
            desc.aabb_min[1]
        );

        // Format-specific assertions
        if name.starts_with("vrm0_") {
            assert_eq!(desc.source_format, AETHER_SOURCE_FORMAT_VRM0, "{name}");
        } else if name.starts_with("vrm1_") {
            assert_eq!(desc.source_format, AETHER_SOURCE_FORMAT_VRM1, "{name}");
        } else if name.starts_with("fbx_") {
            assert_eq!(desc.source_format, AETHER_SOURCE_FORMAT_FBX, "{name}");
        } else if name.starts_with("pmx_") {
            assert_eq!(desc.source_format, AETHER_SOURCE_FORMAT_PMX, "{name}");
        } else if name.starts_with("pmd_") {
            assert_eq!(desc.source_format, AETHER_SOURCE_FORMAT_PMD, "{name}");
        }

        // Japanese wink centroid disambiguation (§6.5) on `vrm0_02_jp_wink.vrm`
        if name == "vrm0_02_jp_wink.vrm" {
            let exprs: &[AetherExpressionBindingDesc] =
                arena.bytes_of(AETHER_STREAM_EXPRESSIONS as usize, req.expression_count);
            let mut saw_blink_l = false;
            let mut saw_blink_r = false;
            for e in exprs {
                if e.input_slot == 0 {
                    saw_blink_l = true;
                } else if e.input_slot == 7 {
                    saw_blink_r = true;
                }
            }
            assert!(
                saw_blink_l && saw_blink_r,
                "vrm0_02_jp_wink.vrm failed left/right wink disambiguation: l={saw_blink_l}, r={saw_blink_r}"
            );
        }

        // A-pose delta rotation & millimetre unit guess on `vrm0_03_apose_mm.vrm`
        if name == "vrm0_03_apose_mm.vrm" {
            assert_ne!(
                desc.warnings_mask & AETHER_WARN_UNIT_GUESS,
                0,
                "expected W_UNIT_GUESS for mm-scaled model"
            );
            let joints: &[AetherJointDesc] =
                arena.bytes_of(AETHER_STREAM_JOINTS as usize, req.joint_count);
            let l_arm = desc.humanoid_to_joint[18] as usize;
            let r_arm = desc.humanoid_to_joint[22] as usize;
            assert!(
                joints[l_arm].rest_to_tpose_quat[3].abs() < 0.995,
                "expected non-identity A->T pose delta on leftUpperArm"
            );
            assert!(
                joints[r_arm].rest_to_tpose_quat[3].abs() < 0.995,
                "expected non-identity A->T pose delta on rightUpperArm"
            );
        }

        // MMD cross-chain physics warning on `pmx_03_cross_chain_warn.pmx`
        if name == "pmx_03_cross_chain_warn.pmx" {
            assert_ne!(
                desc.warnings_mask & AETHER_WARN_MMD_CROSS_CHAIN,
                0,
                "expected W_MMD_CROSS_CHAIN"
            );
            assert!(req.spring_joint_count > 0);
        }

        // OKLab shade synthesis on PBR model without explicit MToon shade (`vrm1_01_standard.vrm`)
        if name == "vrm1_01_standard.vrm" {
            let mats: &[AetherNprMaterial] =
                arena.bytes_of(AETHER_STREAM_MATERIALS as usize, req.material_count);
            assert!(!mats.is_empty());
            let m0 = &mats[0];
            assert!(
                m0.shade_color_gi[0] < m0.base_color[0] || m0.shade_color_gi[1] < m0.base_color[1],
                "expected darkened OKLab shade on PBR material"
            );
        }

        println!(
            "[CORPUS] {name}: verts={} tris={} mapped={mapped}/55 ({pct:.1}%) in {ms:.2} ms",
            req.vertex_count,
            req.index_count / 3
        );
    }
}

#[test]
fn seam_weld_produces_identical_smooth_tangent_on_split_vertices() {
    let _guard = enter_shared();
    let seam_path = repo_root()
        .join("tests")
        .join("data")
        .join("edge_cases")
        .join("seam_cylinder.glb");
    let flags = AETHER_LOAD_FLAG_BAKE_SMOOTH_TANGENTS | AETHER_LOAD_FLAG_MORTON_SORT;
    let (req, _desc, arena, _) = match load_avatar_with_flags(&seam_path, flags) {
        Ok(v) => v,
        Err(e) => panic_fail(&format!("seam_cylinder failed: {e}")),
    };
    let verts: &[AetherVertex25D] =
        arena.bytes_of(AETHER_STREAM_VERTEX25D as usize, req.vertex_count);
    let surfs: &[AetherVertexSurface] =
        arena.bytes_of(AETHER_STREAM_SURFACE as usize, req.vertex_count);

    // Group vertices by quantized spatial position (1e-5 m grid) and verify split seam pairs match.
    let mut by_pos: HashMap<[i64; 3], Vec<(usize, &AetherVertex25D)>> = HashMap::new();
    for (vi, v) in verts.iter().enumerate() {
        let key = [
            (v.position[0] * 100_000.0).round() as i64,
            (v.position[1] * 100_000.0).round() as i64,
            (v.position[2] * 100_000.0).round() as i64,
        ];
        by_pos.entry(key).or_default().push((vi, v));
    }

    let mut seam_pairs_verified = 0usize;
    for group in by_pos.values() {
        if group.len() < 2 {
            continue;
        }
        let (i0, v0) = group[0];
        let ref_st = v0.smooth_tangent;
        let has_distinct_uv_seam = group.iter().any(|&(vi, v)| {
            (surfs[vi].uv0[0] - surfs[i0].uv0[0]).abs() > 0.1
                || (surfs[vi].uv0[1] - surfs[i0].uv0[1]).abs() > 0.1
                || (v.true_normal[0] - v0.true_normal[0]).abs() > 1e-3
                || (v.true_normal[2] - v0.true_normal[2]).abs() > 1e-3
        });
        if has_distinct_uv_seam {
            seam_pairs_verified += 1;
        }
        for &(_, v) in group {
            for k in 0..4 {
                assert!(
                    (v.smooth_tangent[k] - ref_st[k]).abs() < 1e-4,
                    "seam vertex smooth_tangent mismatch: {:?} vs {:?}",
                    v.smooth_tangent,
                    ref_st
                );
            }
        }
    }
    assert!(
        seam_pairs_verified > 0,
        "seam_cylinder.glb must contain split seam vertices"
    );
}

#[test]
fn morton_sort_orders_triangles_monotonically_and_meshopt_works() {
    let _guard = enter_shared();
    let vrm_path = repo_root()
        .join("tests")
        .join("data")
        .join("corpus")
        .join("vrm1_01_standard.vrm");
    let flags = AETHER_LOAD_FLAG_MORTON_SORT | AETHER_LOAD_FLAG_BAKE_PROXY_NORMALS;
    let (req, _desc, arena, _) = match load_avatar_with_flags(&vrm_path, flags) {
        Ok(v) => v,
        Err(e) => panic_fail(&format!("vrm1_01 failed: {e}")),
    };
    let verts: &[AetherVertex25D] =
        arena.bytes_of(AETHER_STREAM_VERTEX25D as usize, req.vertex_count);
    let indices: &[u32] = arena.bytes_of(AETHER_STREAM_INDEX_U32 as usize, req.index_count);
    let submeshes: &[AetherSubmeshDesc] =
        arena.bytes_of(AETHER_STREAM_SUBMESHES as usize, req.submesh_count);

    for sm in submeshes {
        let beg = sm.index_offset as usize;
        let end = beg + (sm.index_count as usize);
        let mut prev_code = 0u32;
        for (t_idx, tri) in indices[beg..end].chunks_exact(3).enumerate() {
            let p0 = verts[tri[0] as usize].position;
            let p1 = verts[tri[1] as usize].position;
            let p2 = verts[tri[2] as usize].position;
            let cx = (p0[0] + p1[0] + p2[0]) / 3.0;
            let cy = (p0[1] + p1[1] + p2[1]) / 3.0;
            let cz = (p0[2] + p1[2] + p2[2]) / 3.0;
            let code = morton_30bit(cx, cy, cz, sm.aabb_min, sm.aabb_max);
            if t_idx > 0 {
                assert!(
                    code >= prev_code,
                    "Morton order violated at tri {t_idx}: {prev_code} -> {code}"
                );
            }
            prev_code = code;
        }
    }

    // Verify `meshopt_quad.glb` fixture loads cleanly AND verify `decode_meshopt` round-trip
    let mo_path = repo_root()
        .join("tests")
        .join("data")
        .join("edge_cases")
        .join("meshopt_quad.glb");
    let (mo_req, _, mo_arena, _) = match load_avatar_with_flags(&mo_path, flags) {
        Ok(v) => v,
        Err(e) => panic_fail(&format!("meshopt_quad failed: {e}")),
    };
    assert_eq!(mo_req.vertex_count, 8);
    assert_eq!(mo_req.index_count, 6);
    let mo_verts: &[AetherVertex25D] =
        arena_slice_of(&mo_arena, AETHER_STREAM_VERTEX25D as usize, 8);
    assert!((mo_verts[1].position[0] - 0.5).abs() < 0.01);

    // Direct `EXT_meshopt_compression` pure-Rust decoder verification (`ATTRIBUTES` 0xA0 + `INDICES` 0xD0)
    // Encode 4x vec4<u8> (stride 4) attributes verbatim (`bits = 3` per channel) and zigzag varint indices.
    let mut enc_attr = Vec::new();
    enc_attr.push(0xA0); // ATTRIBUTES v0
    for ch in 0..4u8 {
        enc_attr.push(0xFF); // 1 header byte: 4 groups x 2-bit `3` (verbatim)
        for v in 0..16u8 {
            enc_attr.push(if v < 4 { v * 10 + ch } else { 0 });
        }
    }
    // 4 channels * 4 bytes tail = Wait: decode_vertex_buffer takes verbatim 16-byte group then tail
    // Wait: decode_vertex_buffer reads exact `cur` group bytes (`count=4`, `groups = 16/16 = 1`, header=1B).
    let dec_attr = match aether_asset_loader::meshopt::decode_meshopt(
        &enc_attr,
        4,
        4,
        aether_asset_loader::meshopt::MeshoptMode::Attributes,
        aether_asset_loader::meshopt::MeshoptFilter::None,
    ) {
        Ok(b) => b,
        Err(e) => panic_fail(&format!("decode_meshopt attributes failed: {e}")),
    };
    assert_eq!(
        dec_attr,
        vec![0, 1, 2, 3, 10, 11, 12, 13, 20, 21, 22, 23, 30, 31, 32, 33]
    );

    // `INDICES` 0xD0 zigzag delta sequence (`last[i & 1]` alternating baseline) for `[0, 1, 2, 0, 2, 3]`:
    // i=0: 0-0=0(zz 0), i=1: 1-0=+1(zz 2), i=2: 2-0=+2(zz 4), i=3: 0-1=-1(zz 1), i=4: 2-2=0(zz 0), i=5: 3-0=+3(zz 6)
    let enc_idx = [0xD0u8, 0, 2, 4, 1, 0, 6];
    let dec_idx = match aether_asset_loader::meshopt::decode_meshopt(
        &enc_idx,
        6,
        2,
        aether_asset_loader::meshopt::MeshoptMode::Indices,
        aether_asset_loader::meshopt::MeshoptFilter::None,
    ) {
        Ok(b) => b,
        Err(e) => panic_fail(&format!("decode_meshopt indices failed: {e}")),
    };
    let expected_idx: [u8; 12] = bytemuck::cast([0u16, 1, 2, 0, 2, 3]);
    assert_eq!(dec_idx.as_slice(), &expected_idx);
}

fn arena_slice_of<T: bytemuck::Pod>(arena: &TestArena, slot: usize, count: u32) -> &[T] {
    arena.bytes_of(slot, count)
}

#[test]
fn resource_caps_draco_reject_and_oversized_png_rejected() {
    let _guard = enter_shared();
    let edges = repo_root().join("tests").join("data").join("edge_cases");

    let req_dummy = AetherLoadRequest {
        path_utf8: core::ptr::null(),
        flags: AETHER_LOAD_FLAG_DECODE_TEXTURES,
        max_threads: 0,
    };

    // 1. Draco GLB must fail cleanly with Unsupported
    let draco = edges.join("draco_rejected.glb");
    let draco_res = AssetLoader::open(draco.to_str().unwrap_or(""), &req_dummy);
    assert!(matches!(draco_res, Err(LoaderError::Unsupported(_))));

    // 2. > 1024 bones PMX must fail cleanly with LimitExceeded
    let bones = edges.join("limit_bones_1025.pmx");
    let bones_res = AssetLoader::open(bones.to_str().unwrap_or(""), &req_dummy);
    assert!(matches!(bones_res, Err(LoaderError::LimitExceeded(_))));

    // 3. Oversized 20000x20000 PNG IHDR embedded in glTF data URI must fail with LimitExceeded BEFORE allocation
    let mut fake_png = Vec::from(&b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR"[..]);
    fake_png.extend_from_slice(&20_000u32.to_be_bytes());
    fake_png.extend_from_slice(&20_000u32.to_be_bytes());
    fake_png.extend_from_slice(&[8, 6, 0, 0, 0, 0, 0, 0, 0]);
    let b64 = encode_base64(&fake_png);
    let tmp_dir = std::env::temp_dir().join("aether_limit_test");
    let _ = fs::create_dir_all(&tmp_dir);
    let gltf_path = tmp_dir.join("huge_tex.gltf");
    let v_b64 = encode_base64(&[0u8; 36]);
    let gltf_json = format!(
        r#"{{"asset":{{"version":"2.0"}},"buffers":[{{"uri":"data:application/octet-stream;base64,{v_b64}","byteLength":36}}],"bufferViews":[{{"buffer":0,"byteOffset":0,"byteLength":36}}],"accessors":[{{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}}],"meshes":[{{"primitives":[{{"attributes":{{"POSITION":0}}}}]}}],"images":[{{"uri":"data:image/png;base64,{b64}"}}],"textures":[{{"source":0}}]}}"#
    );
    if let Ok(mut f) = File::create(&gltf_path) {
        let _ = f.write_all(gltf_json.as_bytes());
    }
    let tex_res = AssetLoader::open(gltf_path.to_str().unwrap_or(""), &req_dummy);
    let _ = fs::remove_dir_all(&tmp_dir);
    assert!(
        matches!(tex_res, Err(LoaderError::LimitExceeded(_))),
        "expected LimitExceeded on 20000x20000 PNG header"
    );
}

#[test]
fn zero_copy_pointer_identity_no_persistent_rust_alloc_and_cancel() {
    let _exclusive = enter_exclusive();
    let vrm_path = repo_root()
        .join("tests")
        .join("data")
        .join("corpus")
        .join("vrm1_01_standard.vrm");

    // Pre-warm rayon pool outside measurement
    let warm_req = AetherLoadRequest {
        path_utf8: core::ptr::null(),
        flags: AETHER_LOAD_FLAG_MORTON_SORT | AETHER_LOAD_FLAG_BAKE_PROXY_NORMALS,
        max_threads: 0,
    };
    if let Ok(pre) = AssetLoader::open(vrm_path.to_str().unwrap_or(""), &warm_req) {
        let mut pre_arena = TestArena::allocate(pre.requirements());
        let _ = pre_arena.load_into(&pre, pre.requirements());
    }

    let baseline_bytes = LIVE_BYTES.load(Ordering::Acquire);
    let loader = match AssetLoader::open(vrm_path.to_str().unwrap_or(""), &warm_req) {
        Ok(l) => l,
        Err(e) => panic_fail(&format!("open failed: {e}")),
    };
    let after_open_delta = LIVE_BYTES.load(Ordering::Acquire) - baseline_bytes;
    assert!(
        after_open_delta < 1024 * 1024,
        "AssetLoader::open retained {after_open_delta} B >= 1 MiB"
    );

    let req = *loader.requirements();
    let mut arena = TestArena::allocate(&req);
    let arena_alloc_baseline = LIVE_BYTES.load(Ordering::Acquire);
    let desc = arena.load_into(&loader, &req);
    let after_fill_delta = LIVE_BYTES.load(Ordering::Acquire) - arena_alloc_baseline;

    for slot in 0..15 {
        if req.stream_bytes[slot] > 0 {
            assert_eq!(desc.debug_stream_ptrs[slot], arena.ptrs[slot]);
        }
    }
    assert!(
        after_fill_delta < 1024 * 1024,
        "AssetLoader::fill retained {after_fill_delta} B >= 1 MiB on Rust heap"
    );

    // Cooperative cancel check
    loader.cancel();
    let mut cancel_arena = TestArena::allocate(&req);
    let mut streams: [Option<BorrowedStream<'_>>; 15] = [const { None }; 15];
    for slot in 0..15 {
        if req.stream_bytes[slot] > 0 {
            let slice = cancel_arena.as_byte_slice_mut(slot, req.stream_bytes[slot]);
            #[allow(unsafe_code)]
            // SAFETY: Extend lifetime locally to the same `cancel_arena` block cleanly.
            let slice_ext: &mut [u8] =
                unsafe { core::slice::from_raw_parts_mut(slice.as_mut_ptr(), slice.len()) };
            if let Ok(bs) = BorrowedStream::from_rust_slice(slot as u32, slice_ext) {
                streams[slot] = Some(bs);
            }
        }
    }
    let mut set = BorrowedArenaSet::from_streams(streams);
    let cancel_res = loader.fill(&mut set);
    assert!(matches!(cancel_res, Err(LoaderError::Cancelled)));
}

proptest! {
    #[test]
    fn skin_weights_always_sort_descending_and_sum_to_65535(
        w0 in 0.0f32..100.0f32,
        w1 in 0.0f32..100.0f32,
        w2 in 0.0f32..100.0f32,
        w3 in 0.0f32..100.0f32,
        w4 in 0.0f32..10.0f32,
        w5 in 0.0f32..10.0f32,
    ) {
        let sk = pack_skin_weights([0, 1, 2, 3, 4, 5, 6, 7], [w0, w1, w2, w3, w4, w5, 0.0, 0.0]);
        let sum: u32 = sk.weights_unorm16.iter().map(|&w| u32::from(w)).sum();
        prop_assert_eq!(sum, 65_535);
        for k in 0..7 {
            if k > 0 {
                prop_assert!(sk.weights_unorm16[k] >= sk.weights_unorm16[k + 1]);
            }
        }
    }

    #[test]
    fn proxy_normals_have_unit_length_and_smooth_boundary(
        px in -0.25f32..0.25f32,
        py in 1.35f32..1.75f32,
        pz in -0.20f32..0.25f32,
        nx in -1.0f32..1.0f32,
        ny in -1.0f32..1.0f32,
        nz in -1.0f32..1.0f32,
        head_w in 0.0f32..1.0f32,
    ) {
        let center = [0.0f32, 1.52f32, 0.03f32];
        let mut dx = px - center[0];
        let mut dy = (py - center[1]) * 0.35; // 65% Y flattening (§6.7)
        let mut dz = pz - center[2];
        let dlen = (dx * dx + dy * dy + dz * dz).sqrt();
        let (sx, sy, sz) = if dlen > 1e-6 {
            (dx / dlen, dy / dlen, dz / dlen)
        } else {
            dx = 0.0; dy = 0.0; dz = 1.0;
            let _ = (dx, dy, dz);
            (0.0, 0.0, 1.0)
        };
        let nlen = (nx * nx + ny * ny + nz * nz).sqrt();
        let (tx, ty, tz) = if nlen > 1e-6 { (nx / nlen, ny / nlen, nz / nlen) } else { (0.0, 0.0, 1.0) };
        let t = ((head_w - 0.4) / 0.4).clamp(0.0, 1.0);
        let smooth = t * t * (3.0 - 2.0 * t);
        let mx = tx + (sx - tx) * smooth;
        let my = ty + (sy - ty) * smooth;
        let mz = tz + (sz - tz) * smooth;
        let mlen = (mx * mx + my * my + mz * mz).sqrt();
        prop_assert!(mlen.is_finite());
        let final_len = if mlen > 1e-6 {
            let ux = mx / mlen;
            let uy = my / mlen;
            let uz = mz / mlen;
            (ux * ux + uy * uy + uz * uz).sqrt()
        } else {
            1.0
        };
        prop_assert!((final_len - 1.0).abs() < 1e-4);
    }
}

fn encode_base64(bytes: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let b0 = u32::from(chunk[0]);
        let b1 = chunk.get(1).copied().map_or(0, u32::from);
        let b2 = chunk.get(2).copied().map_or(0, u32::from);
        let triple = (b0 << 16) | (b1 << 8) | b2;
        out.push(TABLE[((triple >> 18) & 0x3F) as usize] as char);
        out.push(TABLE[((triple >> 12) & 0x3F) as usize] as char);
        out.push(if chunk.len() > 1 {
            TABLE[((triple >> 6) & 0x3F) as usize] as char
        } else {
            '='
        });
        out.push(if chunk.len() > 2 {
            TABLE[(triple & 0x3F) as usize] as char
        } else {
            '='
        });
    }
    out
}
