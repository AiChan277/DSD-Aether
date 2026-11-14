//! Parallel zero-copy texture prober and decoder (`§6.9`, `HR-05`, `HR-06`).
//!
//! Probes image dimensions/content digests during `aether_loader_open` without allocating
//! uncompressed pixel buffers, then decodes PNG (`zune-png`), JPEG (`zune-jpeg`), BMP,
//! TGA (uncompressed & RLE), and uncompressed DDS directly into disjoint 16-byte-aligned
//! sub-slices of C++-lent `AETHER_STREAM_TEXTURE_PIXELS`.

use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::fs::{self, File};
use std::io::Read;
use std::path::Path;

use aether_ffi::{
    AETHER_TEX_FORMAT_BC7_SRGB, AETHER_TEX_FORMAT_BC7_UNORM, AETHER_TEX_FORMAT_RGBA8_SRGB,
    AETHER_TEX_FORMAT_RGBA8_UNORM, AETHER_TEX_USAGE_NORMAL, AETHER_WARN_MISSING_TEXTURES,
    AETHER_WARN_TEXTURE_CACHE_MISS, AetherTextureDesc,
};
use ahash::RandomState;
use rayon::prelude::*;
use zune_jpeg::JpegDecoder;
use zune_png::PngDecoder;
use zune_png::zune_core::bytestream::ZCursor;
use zune_png::zune_core::colorspace::ColorSpace;
use zune_png::zune_core::options::DecoderOptions;

use crate::LoaderError;
use crate::bc7_cache::{
    compress_and_write_cache, probe_cached_bc7, read_cached_bc7_into, resolve_cache_dir,
};
use crate::ir::{RawTextureDesc, RawTextureSource};
use crate::limits::{MAX_FILE_BYTES, MAX_TEXTURE_DIM, MAX_TOTAL_TEXTURE_BYTES, check_texture_dims};

#[inline]
const fn align_up_16(v: u64) -> u64 {
    (v + 15) & !15
}

fn deterministic_hash_lo(data: &[u8], extra: u64) -> u64 {
    let state = RandomState::with_seeds(0x4145_5448, 0x3235_4454, 0x9E37_79B9, 0xBF58_476D);
    state.hash_one((data.len() as u64, data, extra))
}

#[derive(Clone, Copy, Debug)]
enum ImageKind {
    Png,
    Jpeg,
    Bmp,
    Tga,
    Dds,
}

fn detect_image_kind(bytes: &[u8]) -> Option<ImageKind> {
    if bytes.len() >= 8 && &bytes[..8] == b"\x89PNG\r\n\x1a\n" {
        Some(ImageKind::Png)
    } else if bytes.len() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF {
        Some(ImageKind::Jpeg)
    } else if bytes.len() >= 26 && &bytes[..2] == b"BM" {
        Some(ImageKind::Bmp)
    } else if bytes.len() >= 128 && &bytes[..4] == b"DDS " {
        Some(ImageKind::Dds)
    } else if bytes.len() >= 18 {
        // Basic TGA sanity check before trusting width/height
        let img_type = bytes[2];
        let bpp = bytes[16];
        if matches!(img_type, 2 | 3 | 10 | 11) && matches!(bpp, 8 | 24 | 32) {
            Some(ImageKind::Tga)
        } else {
            None
        }
    } else {
        None
    }
}

fn probe_dims_from_header(bytes: &[u8]) -> Result<Option<(u32, u32)>, LoaderError> {
    let Some(kind) = detect_image_kind(bytes) else {
        return Ok(None);
    };
    let (w, h) = match kind {
        ImageKind::Png => {
            if bytes.len() < 24 || &bytes[12..16] != b"IHDR" {
                return Ok(None);
            }
            let w = u32::from_be_bytes([bytes[16], bytes[17], bytes[18], bytes[19]]);
            let h = u32::from_be_bytes([bytes[20], bytes[21], bytes[22], bytes[23]]);
            (w, h)
        }
        ImageKind::Jpeg => {
            let mut dec = JpegDecoder::new(ZCursor::new(bytes));
            if dec.decode_headers().is_err() {
                return Ok(None);
            }
            let Some((w, h)) = dec.dimensions() else {
                return Ok(None);
            };
            (w as u32, h as u32)
        }
        ImageKind::Bmp => {
            if bytes.len() < 26 {
                return Ok(None);
            }
            let w = i32::from_le_bytes([bytes[18], bytes[19], bytes[20], bytes[21]]);
            let h = i32::from_le_bytes([bytes[22], bytes[23], bytes[24], bytes[25]]);
            if w <= 0 || h == 0 {
                return Ok(None);
            }
            (w.unsigned_abs(), h.unsigned_abs())
        }
        ImageKind::Tga => {
            let w = u32::from(u16::from_le_bytes([bytes[12], bytes[13]]));
            let h = u32::from(u16::from_le_bytes([bytes[14], bytes[15]]));
            (w, h)
        }
        ImageKind::Dds => {
            let h = u32::from_le_bytes([bytes[12], bytes[13], bytes[14], bytes[15]]);
            let w = u32::from_le_bytes([bytes[16], bytes[17], bytes[18], bytes[19]]);
            (w, h)
        }
    };
    check_texture_dims(w, h)?;
    Ok(Some((w, h)))
}

fn read_external_prefix_and_hash(path: &Path) -> Option<(Vec<u8>, u64)> {
    let mut file = File::open(path).ok()?;
    let meta = file.metadata().ok()?;
    if meta.len() == 0 || meta.len() > MAX_FILE_BYTES {
        return None;
    }
    let prefix_len = (meta.len() as usize).min(65_536);
    let mut prefix = vec![0u8; prefix_len];
    file.read_exact(&mut prefix).ok()?;
    let hash_lo = deterministic_hash_lo(&prefix, meta.len());
    Some((prefix, hash_lo))
}

/// Probes headers & plans 16-byte-aligned byte offsets inside `AETHER_STREAM_TEXTURE_PIXELS`.
///
/// Only touches file headers / BC7 cache metadata so `open()` executes in `< 1 ms` per texture.
pub fn plan_textures(
    textures: &mut [RawTextureDesc],
    mmap: &[u8],
    decode_textures: bool,
    use_cache: bool,
    warnings: &mut u32,
) -> Result<u64, LoaderError> {
    if !decode_textures {
        for tex in textures.iter_mut() {
            tex.width = tex.width.max(1);
            tex.height = tex.height.max(1);
            tex.pixel_offset = 0;
            tex.pixel_size = 0;
            tex.cached_bc7_path = None;
            tex.cached_bc7_byte_len = 0;
            tex.final_format = if tex.srgb {
                AETHER_TEX_FORMAT_RGBA8_SRGB
            } else {
                AETHER_TEX_FORMAT_RGBA8_UNORM
            };
        }
        return Ok(0);
    }

    let cache_dir = if use_cache { resolve_cache_dir() } else { None };
    let mut cursor = 0u64;

    for tex in textures.iter_mut() {
        let header_probe: Option<(Vec<u8>, u64)> = match &tex.source {
            RawTextureSource::MappedSlice { offset, len } => {
                if let Some(slice) = offset
                    .checked_add(*len)
                    .and_then(|end| mmap.get(*offset..end))
                {
                    let hash = deterministic_hash_lo(slice, u64::from(tex.srgb));
                    if let Some((w, h)) = probe_dims_from_header(slice)? {
                        tex.width = w;
                        tex.height = h;
                        tex.content_hash_lo = hash;
                        tex.rgba_byte_len = u64::from(w) * u64::from(h) * 4;
                    } else {
                        tex.source = RawTextureSource::Missing;
                        *warnings |= AETHER_WARN_MISSING_TEXTURES;
                    }
                } else {
                    tex.source = RawTextureSource::Missing;
                    *warnings |= AETHER_WARN_MISSING_TEXTURES;
                }
                None
            }
            RawTextureSource::EmbeddedBytes(bytes) => {
                let hash = deterministic_hash_lo(bytes, u64::from(tex.srgb));
                if let Some((w, h)) = probe_dims_from_header(bytes)? {
                    tex.width = w;
                    tex.height = h;
                    tex.content_hash_lo = hash;
                    tex.rgba_byte_len = u64::from(w) * u64::from(h) * 4;
                } else {
                    tex.source = RawTextureSource::Missing;
                    *warnings |= AETHER_WARN_MISSING_TEXTURES;
                }
                None
            }
            RawTextureSource::ExternalFile(path) => read_external_prefix_and_hash(path),
            RawTextureSource::Missing => None,
        };

        if let Some((prefix, hash)) = header_probe {
            if let Some((w, h)) = probe_dims_from_header(&prefix)? {
                tex.width = w;
                tex.height = h;
                tex.content_hash_lo = hash ^ u64::from(tex.srgb);
                tex.rgba_byte_len = u64::from(w) * u64::from(h) * 4;
            } else {
                tex.source = RawTextureSource::Missing;
                *warnings |= AETHER_WARN_MISSING_TEXTURES;
            }
        } else if matches!(tex.source, RawTextureSource::ExternalFile(_)) {
            tex.source = RawTextureSource::Missing;
            *warnings |= AETHER_WARN_MISSING_TEXTURES;
        }

        if matches!(tex.source, RawTextureSource::Missing) {
            tex.width = 1;
            tex.height = 1;
            tex.rgba_byte_len = 4;
            tex.content_hash_lo = 0;
            tex.cached_bc7_path = None;
            tex.cached_bc7_byte_len = 0;
            tex.final_format = if tex.srgb {
                AETHER_TEX_FORMAT_RGBA8_SRGB
            } else {
                AETHER_TEX_FORMAT_RGBA8_UNORM
            };
            tex.pixel_size = 4;
        } else if let Some(dir) = cache_dir.as_deref() {
            if let Some((bc7_path, bc7_len)) =
                probe_cached_bc7(dir, tex.content_hash_lo, tex.width, tex.height, tex.srgb)
            {
                tex.cached_bc7_path = Some(bc7_path);
                tex.cached_bc7_byte_len = bc7_len;
                tex.pixel_size = bc7_len;
                tex.final_format = if tex.srgb {
                    AETHER_TEX_FORMAT_BC7_SRGB
                } else {
                    AETHER_TEX_FORMAT_BC7_UNORM
                };
            } else {
                *warnings |= AETHER_WARN_TEXTURE_CACHE_MISS;
                tex.cached_bc7_path = None;
                tex.cached_bc7_byte_len = 0;
                tex.pixel_size = tex.rgba_byte_len;
                tex.final_format = if tex.srgb {
                    AETHER_TEX_FORMAT_RGBA8_SRGB
                } else {
                    AETHER_TEX_FORMAT_RGBA8_UNORM
                };
            }
        } else {
            tex.cached_bc7_path = None;
            tex.cached_bc7_byte_len = 0;
            tex.pixel_size = tex.rgba_byte_len;
            tex.final_format = if tex.srgb {
                AETHER_TEX_FORMAT_RGBA8_SRGB
            } else {
                AETHER_TEX_FORMAT_RGBA8_UNORM
            };
        }

        cursor = align_up_16(cursor);
        tex.pixel_offset = cursor;
        cursor = cursor
            .checked_add(tex.pixel_size)
            .ok_or(LoaderError::LimitExceeded(
                "texture payload byte size overflow",
            ))?;
        if cursor > MAX_TOTAL_TEXTURE_BYTES {
            return Err(LoaderError::LimitExceeded(
                "decoded textures exceed 2 GiB total limit",
            ));
        }
    }

    Ok(cursor)
}

#[inline]
fn fill_fallback_pixel(dst: &mut [u8], usage_flags: u32) {
    let pixel = if (usage_flags & AETHER_TEX_USAGE_NORMAL) != 0 {
        [128u8, 128, 255, 255]
    } else {
        [255u8, 255, 255, 255]
    };
    for chunk in dst.chunks_exact_mut(4) {
        chunk.copy_from_slice(&pixel);
    }
}

fn expand_channels_inplace_rgba(buf: &mut [u8], px_count: usize, src_channels: usize) {
    match src_channels {
        3 => {
            for i in (0..px_count).rev() {
                let s = i * 3;
                let d = i * 4;
                buf[d + 3] = 255;
                buf[d + 2] = buf[s + 2];
                buf[d + 1] = buf[s + 1];
                buf[d] = buf[s];
            }
        }
        2 => {
            for i in (0..px_count).rev() {
                let s = i * 2;
                let d = i * 4;
                let l = buf[s];
                let a = buf[s + 1];
                buf[d] = l;
                buf[d + 1] = l;
                buf[d + 2] = l;
                buf[d + 3] = a;
            }
        }
        1 => {
            for i in (0..px_count).rev() {
                let l = buf[i];
                let d = i * 4;
                buf[d] = l;
                buf[d + 1] = l;
                buf[d + 2] = l;
                buf[d + 3] = 255;
            }
        }
        _ => {}
    }
}

fn decode_png_into(src: &[u8], dst: &mut [u8], width: u32, height: u32) -> bool {
    let opts = DecoderOptions::default()
        .set_max_width(MAX_TEXTURE_DIM as usize)
        .set_max_height(MAX_TEXTURE_DIM as usize)
        .png_set_add_alpha_channel(true)
        .png_set_strip_to_8bit(true)
        .png_set_decode_animated(false);
    let mut dec = PngDecoder::new_with_options(ZCursor::new(src), opts);
    if dec.decode_headers().is_err() {
        return false;
    }
    let Some((dw, dh)) = dec.dimensions() else {
        return false;
    };
    if dw != width as usize || dh != height as usize {
        return false;
    }
    let Some(cs) = dec.colorspace() else {
        return false;
    };
    let expected_rgba = dw * dh * 4;
    if dst.len() < expected_rgba {
        return false;
    }
    if dec.decode_into(&mut dst[..expected_rgba]).is_err() {
        return false;
    }
    let src_ch = cs.num_components();
    if src_ch != 4 {
        expand_channels_inplace_rgba(dst, dw * dh, src_ch);
    }
    true
}

fn decode_jpeg_into(src: &[u8], dst: &mut [u8], width: u32, height: u32) -> bool {
    let opts = DecoderOptions::default()
        .set_max_width(MAX_TEXTURE_DIM as usize)
        .set_max_height(MAX_TEXTURE_DIM as usize)
        .jpeg_set_out_colorspace(ColorSpace::RGBA);
    let mut dec = JpegDecoder::new_with_options(ZCursor::new(src), opts);
    if dec.decode_headers().is_err() {
        return false;
    }
    let Some((dw, dh)) = dec.dimensions() else {
        return false;
    };
    if dw != width as usize || dh != height as usize {
        return false;
    }
    let expected_rgba = dw * dh * 4;
    if dst.len() < expected_rgba {
        return false;
    }
    if dec.decode_into(&mut dst[..expected_rgba]).is_err() {
        return false;
    }
    let out_cs = dec.output_colorspace().unwrap_or(ColorSpace::RGBA);
    let ch = out_cs.num_components();
    if ch != 4 {
        expand_channels_inplace_rgba(dst, dw * dh, ch);
    }
    true
}

fn decode_bmp_into(src: &[u8], dst: &mut [u8], width: u32, height: u32) -> bool {
    if src.len() < 54 || &src[..2] != b"BM" {
        return false;
    }
    let data_off = u32::from_le_bytes([src[10], src[11], src[12], src[13]]) as usize;
    let bpp = u16::from_le_bytes([src[28], src[29]]);
    let comp = u32::from_le_bytes([src[30], src[31], src[32], src[33]]);
    let raw_h = i32::from_le_bytes([src[22], src[23], src[24], src[25]]);
    let top_down = raw_h < 0;
    if !matches!(bpp, 24 | 32) || (comp != 0 && comp != 3) {
        return false;
    }
    let w = width as usize;
    let h = height as usize;
    let row_bytes = (w * usize::from(bpp) / 8).div_ceil(4) * 4;
    let needed = match row_bytes
        .checked_mul(h)
        .and_then(|n| data_off.checked_add(n))
    {
        Some(n) if n <= src.len() => n,
        _ => return false,
    };
    let _ = needed;
    for y in 0..h {
        let src_y = if top_down { y } else { h - 1 - y };
        let row = &src[data_off + src_y * row_bytes..];
        let out_row = &mut dst[y * w * 4..(y + 1) * w * 4];
        if bpp == 24 {
            for x in 0..w {
                out_row[x * 4] = row[x * 3 + 2];
                out_row[x * 4 + 1] = row[x * 3 + 1];
                out_row[x * 4 + 2] = row[x * 3];
                out_row[x * 4 + 3] = 255;
            }
        } else {
            for x in 0..w {
                out_row[x * 4] = row[x * 4 + 2];
                out_row[x * 4 + 1] = row[x * 4 + 1];
                out_row[x * 4 + 2] = row[x * 4];
                out_row[x * 4 + 3] = row[x * 4 + 3];
            }
        }
    }
    true
}

fn decode_tga_into(src: &[u8], dst: &mut [u8], width: u32, height: u32) -> bool {
    if src.len() < 18 {
        return false;
    }
    let id_len = usize::from(src[0]);
    let img_type = src[2];
    let bpp = src[16];
    let top_origin = (src[17] & 0x20) != 0;
    let mut pos = 18 + id_len;
    if pos > src.len() {
        return false;
    }
    let w = width as usize;
    let h = height as usize;
    let px_total = w * h;
    let ch = usize::from(bpp / 8);
    if !matches!(ch, 1 | 3 | 4) {
        return false;
    }

    let read_px = |slice: &[u8]| -> [u8; 4] {
        match ch {
            1 => [slice[0], slice[0], slice[0], 255],
            3 => [slice[2], slice[1], slice[0], 255],
            _ => [slice[2], slice[1], slice[0], slice[3]],
        }
    };

    if matches!(img_type, 2 | 3) {
        if pos + px_total * ch > src.len() {
            return false;
        }
        for i in 0..px_total {
            let y = i / w;
            let x = i % w;
            let dst_y = if top_origin { y } else { h - 1 - y };
            let o = (dst_y * w + x) * 4;
            dst[o..o + 4].copy_from_slice(&read_px(&src[pos + i * ch..]));
        }
        return true;
    }

    if matches!(img_type, 10 | 11) {
        let mut idx = 0usize;
        while idx < px_total {
            let Some(&hdr) = src.get(pos) else {
                return false;
            };
            pos += 1;
            let count = usize::from(hdr & 0x7F) + 1;
            if (hdr & 0x80) != 0 {
                let Some(px_slice) = src.get(pos..pos + ch) else {
                    return false;
                };
                pos += ch;
                let px = read_px(px_slice);
                for _ in 0..count {
                    if idx >= px_total {
                        break;
                    }
                    let y = idx / w;
                    let x = idx % w;
                    let dst_y = if top_origin { y } else { h - 1 - y };
                    let o = (dst_y * w + x) * 4;
                    dst[o..o + 4].copy_from_slice(&px);
                    idx += 1;
                }
            } else {
                for _ in 0..count {
                    let Some(px_slice) = src.get(pos..pos + ch) else {
                        return false;
                    };
                    pos += ch;
                    if idx >= px_total {
                        break;
                    }
                    let y = idx / w;
                    let x = idx % w;
                    let dst_y = if top_origin { y } else { h - 1 - y };
                    let o = (dst_y * w + x) * 4;
                    dst[o..o + 4].copy_from_slice(&read_px(px_slice));
                    idx += 1;
                }
            }
        }
        return true;
    }

    false
}

fn decode_dds_into(src: &[u8], dst: &mut [u8], width: u32, height: u32) -> bool {
    if src.len() < 128 || &src[..4] != b"DDS " {
        return false;
    }
    let pf_flags = u32::from_le_bytes([src[80], src[81], src[82], src[83]]);
    let bpp = u32::from_le_bytes([src[88], src[89], src[90], src[91]]);
    let r_mask = u32::from_le_bytes([src[92], src[93], src[94], src[95]]);
    // Support uncompressed 32-bit RGB/RGBA/BGRA payloads (`DDPF_RGB = 0x40`)
    if (pf_flags & 0x40) == 0 || bpp != 32 {
        return false;
    }
    let px_count = (width as usize) * (height as usize);
    let Some(data) = src.get(128..128 + px_count * 4) else {
        return false;
    };
    let is_bgra = r_mask == 0x00FF_0000;
    if is_bgra {
        for (i, out) in dst[..px_count * 4].chunks_exact_mut(4).enumerate() {
            let s = &data[i * 4..i * 4 + 4];
            out[0] = s[2];
            out[1] = s[1];
            out[2] = s[0];
            out[3] = s[3];
        }
    } else {
        dst[..px_count * 4].copy_from_slice(data);
    }
    true
}

fn decode_raw_bytes_into(tex: &RawTextureDesc, src: &[u8], dst: &mut [u8]) -> bool {
    match detect_image_kind(src) {
        Some(ImageKind::Png) => decode_png_into(src, dst, tex.width, tex.height),
        Some(ImageKind::Jpeg) => decode_jpeg_into(src, dst, tex.width, tex.height),
        Some(ImageKind::Bmp) => decode_bmp_into(src, dst, tex.width, tex.height),
        Some(ImageKind::Tga) => decode_tga_into(src, dst, tex.width, tex.height),
        Some(ImageKind::Dds) => decode_dds_into(src, dst, tex.width, tex.height),
        None => false,
    }
}

/// Decodes all textures directly into disjoint sub-slices of C++-lent `TEXTURE_PIXELS`
/// on the dedicated Rayon worker pool (`HR-05`, `§6.9`).
#[allow(clippy::too_many_arguments)]
pub fn decode_textures_into_arena(
    textures: &[RawTextureDesc],
    mmap: &[u8],
    out_pixels: &mut [u8],
    out_descs: &mut [AetherTextureDesc],
    write_cache: bool,
    cancelled: &AtomicBool,
    string_buf: &mut Vec<u8>,
) -> Result<u32, LoaderError> {
    let cache_dir = if write_cache {
        resolve_cache_dir()
    } else {
        None
    };
    let extra_warnings = AtomicU32::new(0);

    // Build disjoint `&mut [u8]` windows safely in one linear pass (zero unsafe, zero copies).
    let mut cursor = 0usize;
    let mut rest = out_pixels;
    let mut jobs: Vec<(&RawTextureDesc, &mut [u8])> = Vec::with_capacity(textures.len());
    for tex in textures {
        let off = tex.pixel_offset as usize;
        let len = tex.pixel_size as usize;
        if off < cursor {
            return Err(LoaderError::InvalidArg("non-monotonic texture offset"));
        }
        let gap = off - cursor;
        if rest.len() < gap + len {
            return Err(LoaderError::BufferTooSmall {
                stream_kind: aether_ffi::AETHER_STREAM_TEXTURE_PIXELS,
                required: (off + len) as u64,
                provided: (cursor + rest.len()) as u64,
            });
        }
        let (_, tail) = rest.split_at_mut(gap);
        let (target, next) = tail.split_at_mut(len);
        rest = next;
        cursor = off + len;
        if len > 0 {
            jobs.push((tex, target));
        }
    }

    jobs.into_par_iter().for_each(|(tex, dst)| {
        if cancelled.load(Ordering::Acquire) {
            return;
        }
        if let Some(bc7_path) = tex.cached_bc7_path.as_deref() {
            if read_cached_bc7_into(bc7_path, dst).is_ok() {
                return;
            }
            extra_warnings.fetch_or(AETHER_WARN_MISSING_TEXTURES, Ordering::Relaxed);
            dst.fill(255);
            return;
        }

        let decoded_ok = match &tex.source {
            RawTextureSource::MappedSlice { offset, len } => offset
                .checked_add(*len)
                .and_then(|end| mmap.get(*offset..end))
                .is_some_and(|slice| decode_raw_bytes_into(tex, slice, dst)),
            RawTextureSource::EmbeddedBytes(bytes) => decode_raw_bytes_into(tex, bytes, dst),
            RawTextureSource::ExternalFile(path) => match fs::read(path) {
                Ok(bytes) => decode_raw_bytes_into(tex, &bytes, dst),
                Err(_) => false,
            },
            RawTextureSource::Missing => {
                fill_fallback_pixel(dst, tex.usage_flags);
                true
            }
        };

        if !decoded_ok {
            extra_warnings.fetch_or(AETHER_WARN_MISSING_TEXTURES, Ordering::Relaxed);
            fill_fallback_pixel(dst, tex.usage_flags);
            return;
        }

        if write_cache
            && !matches!(tex.source, RawTextureSource::Missing)
            && let Some(dir) = cache_dir.as_deref()
        {
            let _ = compress_and_write_cache(
                dir,
                tex.content_hash_lo,
                tex.width,
                tex.height,
                tex.srgb,
                dst,
            );
        }
    });

    if cancelled.load(Ordering::Acquire) {
        return Err(LoaderError::Cancelled);
    }

    for (idx, tex) in textures.iter().enumerate() {
        let name_offset = string_buf.len() as u32;
        string_buf.extend_from_slice(tex.name.as_bytes());
        string_buf.push(0);
        let name_len = tex.name.len() as u32;
        if idx < out_descs.len() {
            out_descs[idx] = AetherTextureDesc {
                pixel_offset: tex.pixel_offset,
                pixel_size: tex.pixel_size,
                width: tex.width,
                height: tex.height,
                format: tex.final_format,
                usage_flags: tex.usage_flags,
                name_offset,
                name_len,
                content_hash_lo: tex.content_hash_lo,
            };
        }
    }

    Ok(extra_warnings.load(Ordering::Relaxed))
}
