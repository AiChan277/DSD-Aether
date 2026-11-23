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
    Ktx2,
}

fn detect_image_kind(bytes: &[u8]) -> Option<ImageKind> {
    if bytes.len() >= 8 && &bytes[..8] == b"\x89PNG\r\n\x1a\n" {
        Some(ImageKind::Png)
    } else if bytes.len() >= 12 && &bytes[..12] == b"\xABKTX 20\xBB\r\n\x1A\n" {
        Some(ImageKind::Ktx2)
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
        ImageKind::Ktx2 => {
            if bytes.len() < 80 {
                return Ok(None);
            }
            let w = u32::from_le_bytes([bytes[20], bytes[21], bytes[22], bytes[23]]);
            let h = u32::from_le_bytes([bytes[24], bytes[25], bytes[26], bytes[27]]).max(1);
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

fn decode_rgb565(c: u16) -> [u8; 3] {
    let r = (((c >> 11) & 0x1F) as u32 * 255 + 15) / 31;
    let g = (((c >> 5) & 0x3F) as u32 * 255 + 31) / 63;
    let b = ((c & 0x1F) as u32 * 255 + 15) / 31;
    [r as u8, g as u8, b as u8]
}

fn decode_bc1_or_bc3_blocks(
    blocks: &[u8],
    dst: &mut [u8],
    width: u32,
    height: u32,
    is_bc3: bool,
) -> bool {
    let w = width as usize;
    let h = height as usize;
    let bx_cnt = w.div_ceil(4);
    let by_cnt = h.div_ceil(4);
    let blk_sz = if is_bc3 { 16 } else { 8 };
    if blocks.len() < bx_cnt * by_cnt * blk_sz || dst.len() < w * h * 4 {
        return false;
    }
    for by in 0..by_cnt {
        for bx in 0..bx_cnt {
            let b_off = (by * bx_cnt + bx) * blk_sz;
            let blk = &blocks[b_off..b_off + blk_sz];
            let mut alphas = [255u8; 16];
            let c_slice = if is_bc3 {
                let a0 = blk[0];
                let a1 = blk[1];
                let mut a_pal = [0u8; 8];
                a_pal[0] = a0;
                a_pal[1] = a1;
                if a0 > a1 {
                    for k in 1..=6u32 {
                        a_pal[(k + 1) as usize] =
                            (((7 - k) * u32::from(a0) + k * u32::from(a1) + 3) / 7) as u8;
                    }
                } else {
                    for k in 1..=4u32 {
                        a_pal[(k + 1) as usize] =
                            (((5 - k) * u32::from(a0) + k * u32::from(a1) + 2) / 5) as u8;
                    }
                    a_pal[6] = 0;
                    a_pal[7] = 255;
                }
                let mut a_bits = 0u64;
                for i in 0..6 {
                    a_bits |= u64::from(blk[2 + i]) << (i * 8);
                }
                for (i, slot) in alphas.iter_mut().enumerate() {
                    let idx = ((a_bits >> (i * 3)) & 0x7) as usize;
                    *slot = a_pal[idx];
                }
                &blk[8..16]
            } else {
                &blk[0..8]
            };

            let c0_u16 = u16::from_le_bytes([c_slice[0], c_slice[1]]);
            let c1_u16 = u16::from_le_bytes([c_slice[2], c_slice[3]]);
            let rgb0 = decode_rgb565(c0_u16);
            let rgb1 = decode_rgb565(c1_u16);
            let mut c_pal = [[0u8; 4]; 4];
            c_pal[0] = [rgb0[0], rgb0[1], rgb0[2], 255];
            c_pal[1] = [rgb1[0], rgb1[1], rgb1[2], 255];
            if c0_u16 > c1_u16 || is_bc3 {
                for ch in 0..3 {
                    c_pal[2][ch] = ((2 * u32::from(rgb0[ch]) + u32::from(rgb1[ch]) + 1) / 3) as u8;
                    c_pal[3][ch] = ((u32::from(rgb0[ch]) + 2 * u32::from(rgb1[ch]) + 1) / 3) as u8;
                }
                c_pal[2][3] = 255;
                c_pal[3][3] = 255;
            } else {
                for ch in 0..3 {
                    c_pal[2][ch] = u8::midpoint(rgb0[ch], rgb1[ch]);
                }
                c_pal[2][3] = 255;
                c_pal[3] = [0, 0, 0, 0];
            }
            let lut = u32::from_le_bytes([c_slice[4], c_slice[5], c_slice[6], c_slice[7]]);
            for py in 0..4 {
                let y = by * 4 + py;
                if y >= h {
                    continue;
                }
                for px in 0..4 {
                    let x = bx * 4 + px;
                    if x >= w {
                        continue;
                    }
                    let t_i = py * 4 + px;
                    let c_idx = ((lut >> (t_i * 2)) & 0x3) as usize;
                    let mut rgba = c_pal[c_idx];
                    if is_bc3 {
                        rgba[3] = alphas[t_i];
                    }
                    let out_off = (y * w + x) * 4;
                    dst[out_off..out_off + 4].copy_from_slice(&rgba);
                }
            }
        }
    }
    true
}

fn decode_dds_into(src: &[u8], dst: &mut [u8], width: u32, height: u32) -> bool {
    if src.len() < 128 || &src[..4] != b"DDS " {
        return false;
    }
    let pf_flags = u32::from_le_bytes([src[80], src[81], src[82], src[83]]);
    let fourcc = &src[84..88];
    let bpp = u32::from_le_bytes([src[88], src[89], src[90], src[91]]);
    let r_mask = u32::from_le_bytes([src[92], src[93], src[94], src[95]]);
    if (pf_flags & 0x04) != 0 {
        let data_off = if fourcc == b"DX10" { 148 } else { 128 };
        let Some(blocks) = src.get(data_off..) else {
            return false;
        };
        if fourcc == b"DXT1" {
            return decode_bc1_or_bc3_blocks(blocks, dst, width, height, false);
        }
        if fourcc == b"DXT5" || fourcc == b"DXT3" {
            return decode_bc1_or_bc3_blocks(blocks, dst, width, height, true);
        }
        if fourcc == b"DX10" && src.len() >= 148 {
            let dxgi_fmt = u32::from_le_bytes([src[128], src[129], src[130], src[131]]);
            // DXGI_FORMAT_R8G8B8A8_UNORM (28) / SRGB (29) / B8G8R8A8 (87 / 91)
            let px_count = (width as usize) * (height as usize);
            if matches!(dxgi_fmt, 28 | 29 | 87 | 91) && blocks.len() >= px_count * 4 {
                if matches!(dxgi_fmt, 87 | 91) {
                    for (i, out) in dst[..px_count * 4].chunks_exact_mut(4).enumerate() {
                        let s = &blocks[i * 4..i * 4 + 4];
                        out.copy_from_slice(&[s[2], s[1], s[0], s[3]]);
                    }
                } else {
                    dst[..px_count * 4].copy_from_slice(&blocks[..px_count * 4]);
                }
                return true;
            }
            if matches!(dxgi_fmt, 71 | 72) {
                return decode_bc1_or_bc3_blocks(blocks, dst, width, height, false);
            }
            if matches!(dxgi_fmt, 77 | 78) {
                return decode_bc1_or_bc3_blocks(blocks, dst, width, height, true);
            }
        }
    }
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

fn decode_ktx2_into(src: &[u8], dst: &mut [u8], width: u32, height: u32) -> bool {
    if src.len() < 104 || &src[..12] != b"\xABKTX 20\xBB\r\n\x1A\n" {
        return false;
    }
    let vk_format = u32::from_le_bytes([src[12], src[13], src[14], src[15]]);
    let supercomp = u32::from_le_bytes([src[44], src[45], src[46], src[47]]);
    if supercomp != 0 {
        return false;
    }
    // Level 0 index entry starts at byte 80: byteOffset (u64), byteLength (u64), uncompressedByteLength (u64)
    let lvl_off = usize::try_from(u64::from_le_bytes([
        src[80], src[81], src[82], src[83], src[84], src[85], src[86], src[87],
    ]))
    .unwrap_or(0);
    let lvl_len = usize::try_from(u64::from_le_bytes([
        src[88], src[89], src[90], src[91], src[92], src[93], src[94], src[95],
    ]))
    .unwrap_or(0);
    let Some(payload) = src.get(lvl_off..lvl_off.saturating_add(lvl_len)) else {
        return false;
    };
    let px_count = (width as usize) * (height as usize);
    // VK_FORMAT_R8G8B8A8_UNORM (37) / SRGB (43) / B8G8R8A8_UNORM (44) / SRGB (50)
    if matches!(vk_format, 37 | 43 | 0) && payload.len() >= px_count * 4 {
        dst[..px_count * 4].copy_from_slice(&payload[..px_count * 4]);
        return true;
    }
    if matches!(vk_format, 44 | 50) && payload.len() >= px_count * 4 {
        for (i, out) in dst[..px_count * 4].chunks_exact_mut(4).enumerate() {
            let s = &payload[i * 4..i * 4 + 4];
            out.copy_from_slice(&[s[2], s[1], s[0], s[3]]);
        }
        return true;
    }
    // VK_FORMAT_BC1_RGB/RGBA (131..134), VK_FORMAT_BC3 (137..138)
    if (131..=134).contains(&vk_format) {
        return decode_bc1_or_bc3_blocks(payload, dst, width, height, false);
    }
    if matches!(vk_format, 137 | 138) {
        return decode_bc1_or_bc3_blocks(payload, dst, width, height, true);
    }
    false
}

fn apply_alpha_bleed_dilation_1pass(rgba: &mut [u8], width: usize, height: usize) {
    let px_count = width.saturating_mul(height);
    if width < 2 || height < 2 || rgba.len() < px_count * 4 {
        return;
    }
    let mut has_zero_alpha = false;
    let mut has_opaque = false;
    for chunk in rgba[..px_count * 4].chunks_exact(4) {
        if chunk[3] == 0 {
            has_zero_alpha = true;
        } else {
            has_opaque = true;
        }
        if has_zero_alpha && has_opaque {
            break;
        }
    }
    if !has_zero_alpha || !has_opaque {
        return;
    }
    let src = rgba[..px_count * 4].to_vec();
    for y in 0..height {
        let y_lo = y.saturating_sub(1);
        let y_hi = (y + 1).min(height - 1);
        let x_lo_base = 0usize;
        let _ = x_lo_base;
        for x in 0..width {
            let o = (y * width + x) * 4;
            if src[o + 3] != 0 {
                continue;
            }
            let x_lo = x.saturating_sub(1);
            let x_hi = (x + 1).min(width - 1);
            let mut sum_r = 0u32;
            let mut sum_g = 0u32;
            let mut sum_b = 0u32;
            let mut cnt = 0u32;
            for ny in y_lo..=y_hi {
                for nx in x_lo..=x_hi {
                    if nx == x && ny == y {
                        continue;
                    }
                    let no = (ny * width + nx) * 4;
                    if src[no + 3] > 0 {
                        sum_r += u32::from(src[no]);
                        sum_g += u32::from(src[no + 1]);
                        sum_b += u32::from(src[no + 2]);
                        cnt += 1;
                    }
                }
            }
            if let (Some(r), Some(g), Some(b)) = (
                sum_r.checked_div(cnt),
                sum_g.checked_div(cnt),
                sum_b.checked_div(cnt),
            ) {
                rgba[o] = r as u8;
                rgba[o + 1] = g as u8;
                rgba[o + 2] = b as u8;
            }
        }
    }
}

fn decode_raw_bytes_into(tex: &RawTextureDesc, src: &[u8], dst: &mut [u8]) -> bool {
    match detect_image_kind(src) {
        Some(ImageKind::Png) => decode_png_into(src, dst, tex.width, tex.height),
        Some(ImageKind::Jpeg) => decode_jpeg_into(src, dst, tex.width, tex.height),
        Some(ImageKind::Bmp) => decode_bmp_into(src, dst, tex.width, tex.height),
        Some(ImageKind::Tga) => decode_tga_into(src, dst, tex.width, tex.height),
        Some(ImageKind::Dds) => decode_dds_into(src, dst, tex.width, tex.height),
        Some(ImageKind::Ktx2) => decode_ktx2_into(src, dst, tex.width, tex.height),
        None => false,
    }
}

/// Decodes a standalone local texture file (`.png/.jpg/.tga/.bmp/.dds/.ktx2/.spa/.sph`) into RGBA8.
pub fn decode_image_file_rgba(
    path: &Path,
    srgb: bool,
) -> Result<(u32, u32, u32, Vec<u8>), LoaderError> {
    let meta = fs::metadata(path).map_err(|_| LoaderError::Io("failed to stat texture file"))?;
    if !meta.is_file() || meta.len() == 0 {
        return Err(LoaderError::Io("texture path is not a non-empty file"));
    }
    if meta.len() > MAX_FILE_BYTES {
        return Err(LoaderError::LimitExceeded("texture file exceeds size cap"));
    }
    let bytes = fs::read(path).map_err(|_| LoaderError::Io("failed to read texture file"))?;
    let (w, h) = probe_dims_from_header(&bytes)?.ok_or(LoaderError::Parse(
        "unsupported or corrupted texture header",
    ))?;
    let byte_len = (w as usize)
        .checked_mul(h as usize)
        .and_then(|px| px.checked_mul(4))
        .ok_or(LoaderError::LimitExceeded("texture RGBA buffer overflow"))?;
    let mut rgba = vec![0u8; byte_len];
    let dummy_desc = RawTextureDesc {
        name: String::new(),
        source: RawTextureSource::Missing,
        width: w,
        height: h,
        srgb,
        usage_flags: aether_ffi::AETHER_TEX_USAGE_COLOR,
        content_hash_lo: 0,
        rgba_byte_len: byte_len as u64,
        cached_bc7_path: None,
        cached_bc7_byte_len: 0,
        pixel_offset: 0,
        pixel_size: byte_len as u64,
        final_format: if srgb {
            AETHER_TEX_FORMAT_RGBA8_SRGB
        } else {
            AETHER_TEX_FORMAT_RGBA8_UNORM
        },
    };
    if !decode_raw_bytes_into(&dummy_desc, &bytes, &mut rgba) {
        return Err(LoaderError::Parse("failed to decode texture pixels"));
    }
    Ok((w, h, dummy_desc.final_format, rgba))
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
        if !matches!(tex.source, RawTextureSource::Missing)
            && (tex.usage_flags & aether_ffi::AETHER_TEX_USAGE_COLOR) != 0
        {
            apply_alpha_bleed_dilation_1pass(dst, tex.width as usize, tex.height as usize);
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
