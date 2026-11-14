//! Zero-copy two-phase FFI slice validation & borrowing (§5.6, `REQ-FFI-010`).
//!
//! This module is the only place inside `aether-asset-loader` permitted to convert raw
//! `AetherArenaSlice` pointers lent by C++ into Rust `&mut [u8]` slices after proving
//! non-nullness, alignment, stream uniqueness, capacity, `isize::MAX` bounds, and pairwise
//! non-aliasing across all lent buffers.

#![allow(unsafe_code)]

use core::ffi::{CStr, c_char};
use core::mem::align_of;
use core::slice;
use std::fs::File;

use aether_ffi::{
    AETHER_STREAM_COLLIDERS, AETHER_STREAM_COUNT, AETHER_STREAM_EXPRESSIONS,
    AETHER_STREAM_INDEX_U32, AETHER_STREAM_JOINTS, AETHER_STREAM_MATERIALS,
    AETHER_STREAM_MORPH_DELTAS, AETHER_STREAM_MORPH_RANGES, AETHER_STREAM_SKIN,
    AETHER_STREAM_SPRINGS, AETHER_STREAM_STRINGS, AETHER_STREAM_SUBMESHES, AETHER_STREAM_SURFACE,
    AETHER_STREAM_TEXTURE_DESCS, AETHER_STREAM_TEXTURE_PIXELS, AETHER_STREAM_VERTEX25D,
    AetherArenaSlice, AetherAssetRequirements, AetherMorphDelta, AetherVertex25D, AetherVertexSkin,
    AetherVertexSurface,
};
use memmap2::Mmap;

use crate::LoaderError;

const STREAM_COUNT: usize = AETHER_STREAM_COUNT as usize;

/// Returns the minimum byte alignment required for a given `AETHER_STREAM_*` kind.
#[must_use]
pub const fn required_alignment(stream_kind: u32) -> usize {
    match stream_kind {
        AETHER_STREAM_VERTEX25D => align_of::<AetherVertex25D>(),
        AETHER_STREAM_SURFACE => align_of::<AetherVertexSurface>(),
        AETHER_STREAM_SKIN => align_of::<AetherVertexSkin>(),
        AETHER_STREAM_INDEX_U32 => align_of::<u32>(),
        AETHER_STREAM_MORPH_DELTAS => align_of::<AetherMorphDelta>(),
        AETHER_STREAM_MORPH_RANGES
        | AETHER_STREAM_SUBMESHES
        | AETHER_STREAM_MATERIALS
        | AETHER_STREAM_JOINTS
        | AETHER_STREAM_SPRINGS
        | AETHER_STREAM_COLLIDERS
        | AETHER_STREAM_EXPRESSIONS
        | AETHER_STREAM_TEXTURE_DESCS => 8,
        AETHER_STREAM_TEXTURE_PIXELS | AETHER_STREAM_STRINGS => 1,
        _ => usize::MAX,
    }
}

/// Validates and borrows a NUL-terminated UTF-8 path across the FFI boundary.
///
/// # Safety
///
/// `path_utf8` must either be null (rejected cleanly with `InvalidArg`) or point to a valid
/// NUL-terminated C string whose readable length does not exceed `isize::MAX`.
pub unsafe fn borrow_utf8_cstr<'a>(path_utf8: *const c_char) -> Result<&'a str, LoaderError> {
    if path_utf8.is_null() {
        return Err(LoaderError::InvalidArg("null UTF-8 path pointer"));
    }
    // SAFETY: Caller guarantees `path_utf8` is non-null and NUL-terminated if provided by C++.
    let cstr = unsafe { CStr::from_ptr(path_utf8) };
    cstr.to_str()
        .map_err(|_| LoaderError::InvalidArg("path is not valid UTF-8"))
}

/// One validated mutable arena slice borrowed from C++ for the duration of `aether_loader_fill`.
pub struct BorrowedStream<'a> {
    bytes: &'a mut [u8],
    raw_addr: u64,
}

impl<'a> BorrowedStream<'a> {
    /// Safely constructs a [`BorrowedStream`] from an aligned Rust mutable byte slice.
    pub fn from_rust_slice(stream_kind: u32, bytes: &'a mut [u8]) -> Result<Self, LoaderError> {
        let align = required_alignment(stream_kind);
        let addr = bytes.as_ptr() as usize;
        if !bytes.is_empty() && addr % align != 0 {
            return Err(LoaderError::InvalidArg("rust slice misaligned for stream"));
        }
        Ok(Self {
            raw_addr: addr as u64,
            bytes,
        })
    }
}

impl BorrowedStream<'_> {
    /// Returns the original C++-owned base address recorded for zero-copy identity checks.
    #[must_use]
    pub const fn raw_addr(&self) -> u64 {
        self.raw_addr
    }

    /// Returns a mutable byte slice covering the borrowed region.
    #[must_use]
    pub fn as_bytes_mut(&mut self) -> &mut [u8] {
        self.bytes
    }

    /// Casts a prefix containing `count` elements of `T` as `&mut [T]` without allocating.
    pub fn cast_prefix_mut<T: bytemuck::Pod>(
        &mut self,
        count: usize,
    ) -> Result<&mut [T], LoaderError> {
        self.cast_prefix_for_stream_mut(0, count)
    }

    /// Casts a prefix containing `count` elements of `T` as `&mut [T]`, tagging errors with `stream_kind`.
    pub fn cast_prefix_for_stream_mut<T: bytemuck::Pod>(
        &mut self,
        stream_kind: u32,
        count: usize,
    ) -> Result<&mut [T], LoaderError> {
        let needed = count
            .checked_mul(core::mem::size_of::<T>())
            .ok_or(LoaderError::LimitExceeded("stream byte size overflow"))?;
        let provided = self.bytes.len() as u64;
        let prefix = self
            .bytes
            .get_mut(..needed)
            .ok_or(LoaderError::BufferTooSmall {
                stream_kind,
                required: needed as u64,
                provided,
            })?;
        bytemuck::try_cast_slice_mut::<u8, T>(prefix)
            .map_err(|_| LoaderError::InvalidArg("arena slice misaligned for element type"))
    }
}

/// Maps a non-empty regular file read-only via `memmap2`, keeping `unsafe` isolated to `ffi_slice`.
pub fn map_file_readonly(file: &File) -> Result<Mmap, LoaderError> {
    // SAFETY: Caller opened `file` read-only and verified `0 < len <= MAX_FILE_BYTES`.
    // The mapped region is immutable and dropped when `AssetLoader` closes.
    unsafe { Mmap::map(file) }.map_err(|_| LoaderError::Parse("failed to memory-map asset file"))
}

/// Collection of mutually non-aliasing arena slices indexed by `AETHER_STREAM_*`.
pub struct BorrowedArenaSet<'a> {
    streams: [Option<BorrowedStream<'a>>; STREAM_COUNT],
}

impl<'a> BorrowedArenaSet<'a> {
    /// Safely wraps a fixed array of optional pre-validated [`BorrowedStream`] slots.
    #[must_use]
    pub fn from_streams(streams: [Option<BorrowedStream<'a>>; STREAM_COUNT]) -> Self {
        Self { streams }
    }

    /// Returns mutable access to stream `stream_kind` if provided.
    pub fn stream_mut(&mut self, stream_kind: u32) -> Option<&mut BorrowedStream<'a>> {
        let idx = usize::try_from(stream_kind).ok()?;
        self.streams.get_mut(idx)?.as_mut()
    }

    /// Takes ownership of stream `stream_kind`, enabling non-overlapping simultaneous access across streams.
    pub fn take_stream(&mut self, stream_kind: u32) -> Option<BorrowedStream<'a>> {
        let idx = usize::try_from(stream_kind).ok()?;
        self.streams.get_mut(idx)?.take()
    }

    /// Returns the lent C++ base pointer address (`0` if stream absent or empty).
    #[must_use]
    pub fn raw_addr(&self, stream_kind: u32) -> u64 {
        let Ok(idx) = usize::try_from(stream_kind) else {
            return 0;
        };
        self.streams
            .get(idx)
            .and_then(|slot| slot.as_ref())
            .map_or(0, BorrowedStream::raw_addr)
    }
}

/// Validates C++-lent `AetherArenaSlice` descriptors and produces a safe non-aliasing
/// [`BorrowedArenaSet`].
///
/// # Safety
///
/// Every non-null `slice.ptr` must remain valid for exclusive mutable access across
/// `[slice.ptr, slice.ptr + slice.size_bytes)` for the lifetime `'a` (`aether_loader_fill`).
pub unsafe fn borrow_arena_slices<'a>(
    slices_ptr: *const AetherArenaSlice,
    count: u32,
    required: &AetherAssetRequirements,
) -> Result<BorrowedArenaSet<'a>, LoaderError> {
    if count == 0 {
        for (idx, &req_bytes) in required.stream_bytes.iter().enumerate() {
            if req_bytes > 0 {
                return Err(LoaderError::BufferTooSmall {
                    stream_kind: idx as u32,
                    required: req_bytes,
                    provided: 0,
                });
            }
        }
        return Ok(BorrowedArenaSet {
            streams: [const { None }; STREAM_COUNT],
        });
    }

    if slices_ptr.is_null() {
        return Err(LoaderError::InvalidArg("null AetherArenaSlice array"));
    }

    let count_usize =
        usize::try_from(count).map_err(|_| LoaderError::InvalidArg("slice count overflow"))?;
    if count_usize > STREAM_COUNT {
        return Err(LoaderError::InvalidArg(
            "slice count exceeds AETHER_STREAM_COUNT",
        ));
    }

    // SAFETY: Non-null pointer with `count_usize <= STREAM_COUNT` provided by caller for this call.
    let raw_slices = unsafe { slice::from_raw_parts(slices_ptr, count_usize) };

    let mut ranges: [(usize, usize); STREAM_COUNT] = [(0, 0); STREAM_COUNT];
    let mut seen: [bool; STREAM_COUNT] = [false; STREAM_COUNT];
    let mut provided_bytes: [u64; STREAM_COUNT] = [0; STREAM_COUNT];

    // Phase A: validate every descriptor BEFORE constructing any `&mut [u8]`.
    for s in raw_slices {
        if s.stream_kind >= AETHER_STREAM_COUNT {
            return Err(LoaderError::InvalidArg("unknown AETHER_STREAM_* kind"));
        }
        let kind_idx = s.stream_kind as usize;
        if seen[kind_idx] {
            return Err(LoaderError::InvalidArg("duplicate AETHER_STREAM_* entry"));
        }
        seen[kind_idx] = true;
        provided_bytes[kind_idx] = s.size_bytes;

        if s.size_bytes == 0 {
            continue;
        }
        if s.ptr.is_null() {
            return Err(LoaderError::InvalidArg(
                "non-empty arena slice has null pointer",
            ));
        }

        let len = usize::try_from(s.size_bytes)
            .map_err(|_| LoaderError::LimitExceeded("slice size exceeds usize"))?;
        if len > isize::MAX as usize {
            return Err(LoaderError::LimitExceeded("slice size exceeds isize::MAX"));
        }

        let start = s.ptr as usize;
        let align = required_alignment(s.stream_kind);
        if start % align != 0 {
            return Err(LoaderError::InvalidArg("arena slice pointer misaligned"));
        }

        let end = start
            .checked_add(len)
            .ok_or(LoaderError::InvalidArg("arena slice address overflow"))?;
        ranges[kind_idx] = (start, end);
    }

    // Ensure every non-empty required stream received sufficient byte capacity.
    for (idx, &req_bytes) in required.stream_bytes.iter().enumerate() {
        if provided_bytes[idx] < req_bytes {
            return Err(LoaderError::BufferTooSmall {
                stream_kind: idx as u32,
                required: req_bytes,
                provided: provided_bytes[idx],
            });
        }
    }

    // Prove pairwise non-aliasing across all non-empty slices.
    for i in 0..STREAM_COUNT {
        let (a_start, a_end) = ranges[i];
        if a_start == a_end {
            continue;
        }
        for &(b_start, b_end) in ranges.iter().skip(i + 1) {
            if b_start == b_end {
                continue;
            }
            if a_start < b_end && b_start < a_end {
                return Err(LoaderError::InvalidArg("overlapping arena slices"));
            }
        }
    }

    // Phase B: all invariants proven; materialize `&mut [u8]` streams.
    let mut out: [Option<BorrowedStream<'a>>; STREAM_COUNT] = [const { None }; STREAM_COUNT];
    for s in raw_slices {
        let kind_idx = s.stream_kind as usize;
        if s.size_bytes == 0 {
            continue;
        }
        let len = s.size_bytes as usize;
        let raw_addr = s.ptr as usize as u64;
        // SAFETY: Above checks proved `s.ptr` is non-null, aligned to stream requirements,
        // `len <= isize::MAX`, no wraparound, and all non-empty slices are pairwise disjoint.
        let bytes = unsafe { slice::from_raw_parts_mut(s.ptr, len) };
        out[kind_idx] = Some(BorrowedStream { bytes, raw_addr });
    }

    Ok(BorrowedArenaSet { streams: out })
}
