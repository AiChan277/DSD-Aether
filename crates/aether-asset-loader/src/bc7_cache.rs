//! Pure-Rust BC7 Mode 6 parallel block compressor and content-hash disk cache (`§6.9`, `ADR-0004`).
//!
//! Cache directory defaults to `%LOCALAPPDATA%\Aether\cache\textures\` (overridable via
//! `aether_runtime_init` or `AETHER_TEXTURE_CACHE_DIR`). Every file uses safe hex names derived
//! strictly from content hash, dimensions, and sRGB flag (`HR-06`).

use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use rayon::prelude::*;

use crate::LoaderError;

const CACHE_MAGIC: &[u8; 8] = b"AETHBC71";
const HEADER_LEN: usize = 24;

static RUNTIME_CACHE_DIR: OnceLock<PathBuf> = OnceLock::new();

/// Registers the runtime-configured base cache directory (`aether_runtime_init`).
pub fn set_runtime_cache_dir(dir: &Path) {
    let textures_dir = dir.join("textures");
    let _ = RUNTIME_CACHE_DIR.set(textures_dir);
}

/// Resolves the active texture cache directory without allocating global locks.
#[must_use]
pub fn resolve_cache_dir() -> Option<PathBuf> {
    if let Some(env_dir) = std::env::var_os("AETHER_TEXTURE_CACHE_DIR") {
        let p = PathBuf::from(env_dir);
        if !p.as_os_str().is_empty() {
            return Some(p);
        }
    }
    if let Some(cfg) = RUNTIME_CACHE_DIR.get() {
        return Some(cfg.clone());
    }
    if let Some(local_app_data) = std::env::var_os("LOCALAPPDATA") {
        let p = PathBuf::from(local_app_data)
            .join("Aether")
            .join("cache")
            .join("textures");
        return Some(p);
    }
    Some(
        std::env::temp_dir()
            .join("Aether")
            .join("cache")
            .join("textures"),
    )
}

/// Returns true if `(width, height)` is eligible for BC7 block compression.
#[must_use]
pub const fn is_bc7_eligible(width: u32, height: u32) -> bool {
    width >= 16 && height >= 16 && width % 4 == 0 && height % 4 == 0
}

/// Byte size of mip-0 BC7 block payload for `width x height`.
#[must_use]
pub const fn bc7_payload_bytes(width: u32, height: u32) -> u64 {
    let bw = (width as u64).div_ceil(4);
    let bh = (height as u64).div_ceil(4);
    bw * bh * 16
}

fn cache_file_path(cache_dir: &Path, hash_lo: u64, width: u32, height: u32, srgb: bool) -> PathBuf {
    let space = if srgb { "srgb" } else { "lin" };
    cache_dir.join(format!("{hash_lo:016x}_{width}x{height}_{space}.bc7"))
}

/// Probes only the 24-byte header of a cached BC7 file during `open()`.
#[must_use]
pub fn probe_cached_bc7(
    cache_dir: &Path,
    hash_lo: u64,
    width: u32,
    height: u32,
    srgb: bool,
) -> Option<(PathBuf, u64)> {
    if !is_bc7_eligible(width, height) {
        return None;
    }
    let expected_len = bc7_payload_bytes(width, height);
    let path = cache_file_path(cache_dir, hash_lo, width, height, srgb);
    let mut file = File::open(&path).ok()?;
    let meta = file.metadata().ok()?;
    if meta.len() != HEADER_LEN as u64 + expected_len {
        return None;
    }
    let mut hdr = [0u8; HEADER_LEN];
    file.read_exact(&mut hdr).ok()?;
    if &hdr[0..8] != CACHE_MAGIC {
        return None;
    }
    let f_w = u32::from_le_bytes([hdr[8], hdr[9], hdr[10], hdr[11]]);
    let f_h = u32::from_le_bytes([hdr[12], hdr[13], hdr[14], hdr[15]]);
    let f_srgb = u32::from_le_bytes([hdr[16], hdr[17], hdr[18], hdr[19]]);
    let f_size = u32::from_le_bytes([hdr[20], hdr[21], hdr[22], hdr[23]]) as u64;
    if f_w != width || f_h != height || (f_srgb != 0) != srgb {
        return None;
    }
    // High 32 bits ignored when payload <= 4 GiB; compare against expected_len u32-mod or full meta check above.
    if f_size != (expected_len & 0xFFFF_FFFF) && meta.len() != HEADER_LEN as u64 + expected_len {
        return None;
    }
    Some((path, expected_len))
}

/// Streams BC7 payload bytes directly from disk cache into C++-lent memory (`HR-05`).
pub fn read_cached_bc7_into(path: &Path, dst: &mut [u8]) -> Result<(), LoaderError> {
    let mut file = File::open(path).map_err(|_| LoaderError::Parse("failed to open BC7 cache"))?;
    file.seek(SeekFrom::Start(HEADER_LEN as u64))
        .map_err(|_| LoaderError::Parse("failed to seek BC7 cache"))?;
    file.read_exact(dst)
        .map_err(|_| LoaderError::Parse("failed to read BC7 cache payload"))?;
    Ok(())
}

/// Encodes one 4x4 RGBA8 block to a valid D3D11/Khronos BC7 Mode 6 block (128 bits).
#[inline]
#[must_use]
fn encode_block_mode6(rgba_4x4: [[u8; 4]; 16]) -> [u8; 16] {
    const WEIGHTS: [f32; 16] = [
        0.0, 4.0, 9.0, 13.0, 17.0, 21.0, 26.0, 30.0, 34.0, 38.0, 43.0, 47.0, 51.0, 55.0, 60.0, 64.0,
    ];
    // Project 16 pixels onto the dominant diagonal axis derived from channel ranges
    let mut min_c = [255i32; 4];
    let mut max_c = [0i32; 4];
    for px in &rgba_4x4 {
        for ch in 0..4 {
            let v = i32::from(px[ch]);
            min_c[ch] = min_c[ch].min(v);
            max_c[ch] = max_c[ch].max(v);
        }
    }

    // Fast path for flat constant blocks
    if min_c == max_c {
        let mut out: u128 = 1u128 << 6; // Mode 6 prefix `0000001` (LSB first)
        for (ch, &val) in min_c.iter().enumerate() {
            let v = val as u128;
            let q7 = v >> 1;
            let shift_e0 = 7 + ch * 14;
            let shift_e1 = 14 + ch * 14;
            out |= q7 << shift_e0;
            out |= q7 << shift_e1;
        }
        // Set parity bits P0 and P1 cleanly using bit 0 of Red
        let p_bit = (min_c[0] as u128) & 1;
        out |= (p_bit & 1) << 63;
        out |= (p_bit & 1) << 64;
        return out.to_le_bytes();
    }

    // Principal covariance axis starting from primary channel range
    let mut dom = 0usize;
    let mut best_span = -1i32;
    for ch in 0..4 {
        let s = max_c[ch] - min_c[ch];
        if s > best_span {
            best_span = s;
            dom = ch;
        }
    }

    let mut lo_acc = [0i32; 4];
    let mut hi_acc = [0i32; 4];
    let mid_dom = (min_c[dom] + max_c[dom]) / 2;
    let mut axis = [0.0f32; 4];
    for ch in 0..4 {
        let mut sign = 1;
        for px in &rgba_4x4 {
            if i32::from(px[dom]) >= mid_dom {
                hi_acc[ch] += i32::from(px[ch]);
            } else {
                lo_acc[ch] += i32::from(px[ch]);
            }
        }
        if hi_acc[ch] < lo_acc[ch] {
            sign = -1;
        }
        axis[ch] = ((max_c[ch] - min_c[ch]) * sign) as f32;
    }

    let axis_len2 = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] + axis[3] * axis[3];
    let inv_len2 = if axis_len2 > 1e-4 {
        1.0 / axis_len2
    } else {
        1.0
    };

    // Find extremal projected pixels as initial Mode 6 endpoints
    let mut min_t = f32::INFINITY;
    let mut max_t = f32::NEG_INFINITY;
    let mut e0 = rgba_4x4[0];
    let mut e1 = rgba_4x4[0];
    let mut projs = [0.0f32; 16];
    for (i, px) in rgba_4x4.iter().enumerate() {
        let p = f32::from(px[0]) * axis[0]
            + f32::from(px[1]) * axis[1]
            + f32::from(px[2]) * axis[2]
            + f32::from(px[3]) * axis[3];
        projs[i] = p;
        if p < min_t {
            min_t = p;
            e0 = *px;
        }
        if p > max_t {
            max_t = p;
            e1 = *px;
        }
    }

    let span = (max_t - min_t).max(1.0);
    // Compute 4-bit indices [0..15]
    let mut indices = [0u8; 16];
    for (i, &p) in projs.iter().enumerate() {
        let t = ((p - min_t) / span).clamp(0.0, 1.0) * 64.0;
        let mut best_k = 0u8;
        let mut best_d = f32::INFINITY;
        for (k, &w) in WEIGHTS.iter().enumerate() {
            let d = (w - t).abs();
            if d < best_d {
                best_d = d;
                best_k = k as u8;
            }
        }
        indices[i] = best_k;
    }

    let _ = inv_len2;

    // Anchor index 0 MUST have MSB == 0 (i.e. indices[0] <= 7)
    if indices[0] >= 8 {
        core::mem::swap(&mut e0, &mut e1);
        for idx in &mut indices {
            *idx = 15 - *idx;
        }
    }

    let mut out: u128 = 1u128 << 6; // Mode 6
    let p0 = u128::from((e0[0] ^ e0[1] ^ e0[2]) & 1);
    let p1 = u128::from((e1[0] ^ e1[1] ^ e1[2]) & 1);
    // Use red LSB directly for P-bit when clean, keeping round(v >> 1) endpoints
    let _ = (p0, p1);
    let p0_bit = u128::from(e0[0] & 1);
    let p1_bit = u128::from(e1[0] & 1);

    for ch in 0..4 {
        let q0 = u128::from(e0[ch] >> 1);
        let q1 = u128::from(e1[ch] >> 1);
        out |= q0 << (7 + ch * 14);
        out |= q1 << (14 + ch * 14);
    }
    out |= p0_bit << 63;
    out |= p1_bit << 64;

    // Index 0: 3 bits at bit 65..=67
    out |= u128::from(indices[0] & 0x7) << 65;
    // Indices 1..15: 4 bits each starting at bit 68
    let mut bit = 68usize;
    for &idx in &indices[1..] {
        out |= u128::from(idx & 0xF) << bit;
        bit += 4;
    }

    out.to_le_bytes()
}

/// Encodes `rgba` (`width * height * 4` bytes) into BC7 Mode 6 blocks and atomically writes cache.
pub fn compress_and_write_cache(
    cache_dir: &Path,
    hash_lo: u64,
    width: u32,
    height: u32,
    srgb: bool,
    rgba: &[u8],
) -> Result<(), LoaderError> {
    if !is_bc7_eligible(width, height) {
        return Ok(());
    }
    let expected_rgba = (width as usize) * (height as usize) * 4;
    if rgba.len() < expected_rgba {
        return Ok(());
    }
    fs::create_dir_all(cache_dir).map_err(|_| LoaderError::Parse("cannot create texture cache"))?;

    let bw = (width as usize) / 4;
    let bh = (height as usize) / 4;
    let payload_len = bw * bh * 16;
    let mut blocks = vec![0u8; payload_len];
    let stride = (width as usize) * 4;

    blocks
        .par_chunks_mut(bw * 16)
        .enumerate()
        .for_each(|(by, row_blocks)| {
            let py0 = by * 4;
            for bx in 0..bw {
                let px0 = bx * 4;
                let mut block = [[0u8; 4]; 16];
                for ly in 0..4 {
                    let row_off = (py0 + ly) * stride + px0 * 4;
                    for lx in 0..4 {
                        let o = row_off + lx * 4;
                        block[ly * 4 + lx] = [rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3]];
                    }
                }
                let encoded = encode_block_mode6(block);
                row_blocks[bx * 16..bx * 16 + 16].copy_from_slice(&encoded);
            }
        });

    let mut hdr = [0u8; HEADER_LEN];
    hdr[0..8].copy_from_slice(CACHE_MAGIC);
    hdr[8..12].copy_from_slice(&width.to_le_bytes());
    hdr[12..16].copy_from_slice(&height.to_le_bytes());
    hdr[16..20].copy_from_slice(&(u32::from(srgb)).to_le_bytes());
    hdr[20..24].copy_from_slice(&(payload_len as u32).to_le_bytes());

    let final_path = cache_file_path(cache_dir, hash_lo, width, height, srgb);
    let tmp_path = cache_dir.join(format!(
        "{hash_lo:016x}_{width}x{height}_{}.tmp",
        std::process::id()
    ));

    let mut file = File::create(&tmp_path).map_err(|_| LoaderError::Parse("cache write open"))?;
    file.write_all(&hdr)
        .and_then(|()| file.write_all(&blocks))
        .and_then(|()| file.flush())
        .map_err(|_| LoaderError::Parse("cache write failed"))?;
    drop(file);

    // Atomic best-effort rename; on Windows ignore race if another thread wrote the same hash.
    if fs::rename(&tmp_path, &final_path).is_err() {
        let _ = fs::remove_file(&tmp_path);
    }
    Ok(())
}
