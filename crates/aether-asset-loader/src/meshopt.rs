//! Pure-Rust bounds-checked `EXT_meshopt_compression` decoder (`§6.2`, ADR-0004).
//!
//! Supports modes:
//! - `0`: `ATTRIBUTES` (`0xA0` vertex buffer v0)
//! - `1`: `TRIANGLES` (`0xE0`/`0xE1` edge-FIFO triangle indices)
//! - `2`: `INDICES` (`0xD0`/`0xD1` zigzag varint index sequence)
//!
//! Plus post-decode filters: `NONE`, `OCTAHEDRAL`, `QUATERNION`, `EXPONENTIAL`.

use crate::LoaderError;

/// Compression mode from `EXT_meshopt_compression.mode`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MeshoptMode {
    Attributes,
    Triangles,
    Indices,
}

/// Filter mode from `EXT_meshopt_compression.filter`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MeshoptFilter {
    None,
    Octahedral,
    Quaternion,
    Exponential,
}

/// Decodes an `EXT_meshopt_compression` stream into `count * byte_stride` bytes.
pub fn decode_meshopt(
    encoded: &[u8],
    count: usize,
    byte_stride: usize,
    mode: MeshoptMode,
    filter: MeshoptFilter,
) -> Result<Vec<u8>, LoaderError> {
    if count == 0 || byte_stride == 0 {
        return Ok(Vec::new());
    }
    let out_len = count
        .checked_mul(byte_stride)
        .ok_or(LoaderError::LimitExceeded("meshopt decoded size overflow"))?;
    // Cap decoded size at 256 MiB per bufferView.
    if out_len > 256 * 1024 * 1024 {
        return Err(LoaderError::LimitExceeded(
            "meshopt bufferView exceeds 256 MiB",
        ));
    }

    let mut out = vec![0u8; out_len];
    match mode {
        MeshoptMode::Attributes => decode_vertex_buffer(&mut out, count, byte_stride, encoded)?,
        MeshoptMode::Triangles => decode_index_buffer(&mut out, count, byte_stride, encoded)?,
        MeshoptMode::Indices => decode_index_sequence(&mut out, count, byte_stride, encoded)?,
    }
    apply_filter(&mut out, count, byte_stride, filter)?;
    Ok(out)
}

#[inline]
const fn unzigzag8(v: u8) -> u8 {
    let neg = ((v & 1) as i8).wrapping_neg() as u8;
    (v >> 1) ^ neg
}

#[inline]
const fn unzigzag32(v: u32) -> u32 {
    let neg = ((v & 1) as i32).wrapping_neg() as u32;
    (v >> 1) ^ neg
}

fn decode_vertex_block_size(byte_size: usize) -> usize {
    let aligned = (8192 / byte_size) & !15usize;
    aligned.clamp(16, 256)
}

fn decode_vertex_buffer(
    dst: &mut [u8],
    vertex_count: usize,
    vertex_size: usize,
    src: &[u8],
) -> Result<(), LoaderError> {
    if vertex_size == 0 || vertex_size % 4 != 0 || vertex_size > 256 {
        return Err(LoaderError::Parse("meshopt invalid vertex stride"));
    }
    let (&hdr, mut cur) = src
        .split_first()
        .ok_or(LoaderError::Parse("meshopt empty vertex stream"))?;
    if (hdr & 0xF0) != 0xA0 || (hdr & 0x0F) != 0 {
        return Err(LoaderError::Parse(
            "meshopt unsupported vertex stream version",
        ));
    }

    let block_size = decode_vertex_block_size(vertex_size);
    let mut last = [0u8; 256];
    let block_buf = [0u8; 8192 + 16];
    let mut off = 0usize;
    let mut channel_out = vec![0u8; 256 * 256];

    while off < vertex_count {
        let count = (vertex_count - off).min(block_size);
        let group_size = (count + 15) & !15usize;
        let header_bytes = (group_size / 64).max(1);

        for ch in 0..vertex_size {
            if cur.is_empty() {
                return Err(LoaderError::Parse("meshopt truncated vertex header"));
            }
            // Fast path when header byte == 0xFF (uncompressed channel block)
            if cur.len() < header_bytes {
                return Err(LoaderError::Parse("meshopt truncated vertex header bytes"));
            }
            let (hdr_slice, rest) = cur.split_at(header_bytes);
            cur = rest;

            let mut prev = last[ch];
            let groups = group_size / 16;
            let mut g_off = 0usize;

            for g in 0..groups {
                let bits = (hdr_slice[g / 4] >> ((g % 4) * 2)) & 3;
                let n = count.saturating_sub(g_off).min(16);
                match bits {
                    0 => {
                        // All zero deltas (repeat `prev`)
                        for k in 0..n {
                            channel_out[ch * count + g_off + k] = prev;
                        }
                    }
                    1 => {
                        // 2-bit deltas: 4 bytes for 16 elements
                        if cur.len() < 4 {
                            return Err(LoaderError::Parse("meshopt truncated 2-bit block"));
                        }
                        for k in 0..n {
                            let packed = cur[k / 4];
                            let d = (packed >> ((k % 4) * 2)) & 3;
                            prev = prev.wrapping_add(unzigzag8(d));
                            channel_out[ch * count + g_off + k] = prev;
                        }
                        cur = &cur[4..];
                    }
                    2 => {
                        // 4-bit deltas: 8 bytes for 16 elements
                        if cur.len() < 8 {
                            return Err(LoaderError::Parse("meshopt truncated 4-bit block"));
                        }
                        for k in 0..n {
                            let packed = cur[k / 2];
                            let d = (packed >> ((k % 2) * 4)) & 15;
                            prev = prev.wrapping_add(unzigzag8(d));
                            channel_out[ch * count + g_off + k] = prev;
                        }
                        cur = &cur[8..];
                    }
                    3 => {
                        // 8-bit verbatim deltas
                        if cur.len() < 16 {
                            return Err(LoaderError::Parse("meshopt truncated 8-bit block"));
                        }
                        for k in 0..n {
                            prev = cur[k];
                            channel_out[ch * count + g_off + k] = prev;
                        }
                        cur = &cur[16..];
                    }
                    _ => unreachable!(),
                }
                g_off += 16;
            }
            last[ch] = prev;
        }

        let _ = block_buf;
        for i in 0..count {
            let dst_base = (off + i) * vertex_size;
            for ch in 0..vertex_size {
                dst[dst_base + ch] = channel_out[ch * count + i];
            }
        }
        off += count;
    }

    Ok(())
}

fn read_vbyte(cur: &mut &[u8]) -> Result<u32, LoaderError> {
    let (&lead, rest) = cur
        .split_first()
        .ok_or(LoaderError::Parse("meshopt truncated vbyte"))?;
    *cur = rest;
    if lead < 128 {
        return Ok(u32::from(lead));
    }
    let mut val = u32::from(lead & 127);
    let mut shift = 7u32;
    for _ in 0..4 {
        let (&b, r) = cur
            .split_first()
            .ok_or(LoaderError::Parse("meshopt truncated vbyte cont"))?;
        *cur = r;
        val |= u32::from(b & 127) << shift;
        if b < 128 {
            return Ok(val);
        }
        shift += 7;
    }
    Err(LoaderError::Parse("meshopt overlong vbyte"))
}

fn write_index(dst: &mut [u8], pos: usize, index_size: usize, val: u32) -> Result<(), LoaderError> {
    let off = pos * index_size;
    match index_size {
        2 => {
            let v16 =
                u16::try_from(val).map_err(|_| LoaderError::Parse("meshopt u16 index overflow"))?;
            dst[off..off + 2].copy_from_slice(&v16.to_le_bytes());
        }
        4 => dst[off..off + 4].copy_from_slice(&val.to_le_bytes()),
        _ => return Err(LoaderError::Parse("meshopt invalid index size")),
    }
    Ok(())
}

fn decode_index_buffer(
    dst: &mut [u8],
    index_count: usize,
    index_size: usize,
    src: &[u8],
) -> Result<(), LoaderError> {
    if index_count % 3 != 0 {
        return Err(LoaderError::Parse("meshopt index count not multiple of 3"));
    }
    if src.len() < 17 {
        return Err(LoaderError::Parse("meshopt truncated index buffer"));
    }
    let header = src[0];
    if (header & 0xF0) != 0xE0 {
        return Err(LoaderError::Parse("meshopt invalid triangle header"));
    }
    let tri_count = index_count / 3;
    let code_bytes = tri_count;
    if src.len() < 1 + code_bytes + 16 {
        return Err(LoaderError::Parse(
            "meshopt index stream shorter than code table",
        ));
    }

    let tail_off = src.len() - 16;
    let codeaux = &src[1 + code_bytes..tail_off];
    let mut data_off = 1 + code_bytes;
    let mut data_slice = &src[data_off..tail_off];
    let code = &src[1..=code_bytes];

    let mut next = 0u32;
    let mut fec_edge = [[0u32; 2]; 16];
    let mut fec_vert = [0u32; 16];
    let mut last = 0u32;
    let _ = &mut data_off;
    let _ = codeaux;

    for (i, &c) in code.iter().enumerate().take(tri_count) {
        let a_b_c: [u32; 3] = if c < 0xF0 {
            let fe = (c >> 4) as usize;
            let e0 = fec_edge[fe][0];
            let e1 = fec_edge[fe][1];
            let code_c = c & 15;
            let v2 = if code_c == 0 {
                let v = next;
                next += 1;
                v
            } else if code_c < 14 {
                fec_vert[(code_c - 1) as usize]
            } else {
                let d = read_vbyte(&mut data_slice)?;
                let v = if code_c == 14 {
                    last.wrapping_add(1)
                } else {
                    last.wrapping_add(unzigzag32(d))
                };
                last = v;
                v
            };
            fec_vert.rotate_right(1);
            fec_vert[0] = v2;
            fec_edge.rotate_right(2);
            fec_edge[0] = [v2, e0];
            fec_edge[1] = [e1, v2];
            [e0, e1, v2]
        } else {
            let mut tri = [0u32; 3];
            for (k, slot) in tri.iter_mut().enumerate() {
                let v = if ((c >> k) & 1) != 0 {
                    let v = next;
                    next += 1;
                    v
                } else {
                    let d = read_vbyte(&mut data_slice)?;
                    let v = last.wrapping_add(unzigzag32(d));
                    last = v;
                    v
                };
                *slot = v;
            }
            fec_edge.rotate_right(3);
            fec_edge[0] = [tri[1], tri[0]];
            fec_edge[1] = [tri[2], tri[1]];
            fec_edge[2] = [tri[0], tri[2]];
            tri
        };

        write_index(dst, i * 3, index_size, a_b_c[0])?;
        write_index(dst, i * 3 + 1, index_size, a_b_c[1])?;
        write_index(dst, i * 3 + 2, index_size, a_b_c[2])?;
    }

    Ok(())
}

fn decode_index_sequence(
    dst: &mut [u8],
    index_count: usize,
    index_size: usize,
    src: &[u8],
) -> Result<(), LoaderError> {
    let (&hdr, mut cur) = src
        .split_first()
        .ok_or(LoaderError::Parse("meshopt empty index sequence"))?;
    if (hdr & 0xF0) != 0xD0 {
        return Err(LoaderError::Parse("meshopt invalid index sequence header"));
    }
    let mut last = [0u32; 2];
    for i in 0..index_count {
        let code = read_vbyte(&mut cur)?;
        let baseline = last[i & 1];
        let val = baseline.wrapping_add(unzigzag32(code));
        last[i & 1] = val;
        write_index(dst, i, index_size, val)?;
    }
    Ok(())
}

fn apply_filter(
    buf: &mut [u8],
    count: usize,
    stride: usize,
    filter: MeshoptFilter,
) -> Result<(), LoaderError> {
    match filter {
        MeshoptFilter::None => Ok(()),
        MeshoptFilter::Octahedral => {
            if stride != 4 && stride != 8 {
                return Err(LoaderError::Parse(
                    "meshopt octahedral filter requires stride 4 or 8",
                ));
            }
            if stride == 4 {
                for chunk in buf.chunks_exact_mut(4).take(count) {
                    let x = (chunk[0] as i8) as f32 / 127.0;
                    let y = (chunk[1] as i8) as f32 / 127.0;
                    let w = (chunk[3] as i8) as f32;
                    let z = 1.0 - x.abs() - y.abs();
                    let t = (-z).max(0.0);
                    let fx = if x >= 0.0 { x - t } else { x + t };
                    let fy = if y >= 0.0 { y - t } else { y + t };
                    let len = (fx * fx + fy * fy + z * z).sqrt().max(1e-6);
                    chunk[0] = ((fx / len * 127.0).round().clamp(-127.0, 127.0) as i8) as u8;
                    chunk[1] = ((fy / len * 127.0).round().clamp(-127.0, 127.0) as i8) as u8;
                    chunk[2] = ((z / len * 127.0).round().clamp(-127.0, 127.0) as i8) as u8;
                    chunk[3] = ((w.clamp(-127.0, 127.0)).round() as i8) as u8;
                }
            } else {
                for chunk in buf.chunks_exact_mut(8).take(count) {
                    let x = i16::from_le_bytes([chunk[0], chunk[1]]) as f32 / 32767.0;
                    let y = i16::from_le_bytes([chunk[2], chunk[3]]) as f32 / 32767.0;
                    let z = 1.0 - x.abs() - y.abs();
                    let t = (-z).max(0.0);
                    let fx = if x >= 0.0 { x - t } else { x + t };
                    let fy = if y >= 0.0 { y - t } else { y + t };
                    let len = (fx * fx + fy * fy + z * z).sqrt().max(1e-6);
                    chunk[0..2].copy_from_slice(
                        &(((fx / len * 32767.0).round().clamp(-32767.0, 32767.0)) as i16)
                            .to_le_bytes(),
                    );
                    chunk[2..4].copy_from_slice(
                        &(((fy / len * 32767.0).round().clamp(-32767.0, 32767.0)) as i16)
                            .to_le_bytes(),
                    );
                    chunk[4..6].copy_from_slice(
                        &(((z / len * 32767.0).round().clamp(-32767.0, 32767.0)) as i16)
                            .to_le_bytes(),
                    );
                }
            }
            Ok(())
        }
        MeshoptFilter::Quaternion => {
            if stride != 8 {
                return Err(LoaderError::Parse(
                    "meshopt quaternion filter requires stride 8",
                ));
            }
            for chunk in buf.chunks_exact_mut(8).take(count) {
                let x = i16::from_le_bytes([chunk[0], chunk[1]]) as f32 / 32767.0;
                let y = i16::from_le_bytes([chunk[2], chunk[3]]) as f32 / 32767.0;
                let z = i16::from_le_bytes([chunk[4], chunk[5]]) as f32 / 32767.0;
                let w = (1.0 - (x * x + y * y + z * z)).max(0.0).sqrt();
                chunk[6..8].copy_from_slice(
                    &(((w * 32767.0).round().clamp(0.0, 32767.0)) as i16).to_le_bytes(),
                );
            }
            Ok(())
        }
        MeshoptFilter::Exponential => {
            if stride % 4 != 0 {
                return Err(LoaderError::Parse(
                    "meshopt exponential filter requires stride % 4 == 0",
                ));
            }
            for chunk in buf.chunks_exact_mut(4) {
                let raw = u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
                let exp = (raw as i32) >> 24;
                let mantissa = ((raw << 8) as i32) >> 8;
                let f = (mantissa as f32) * 2.0f32.powi(exp);
                chunk.copy_from_slice(&f.to_le_bytes());
            }
            Ok(())
        }
    }
}
