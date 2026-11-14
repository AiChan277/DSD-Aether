//! Hard parser resource caps (`AETHER-TS-001` §3.6, `HR-06`).
//!
//! Every cap is checked BEFORE reserving buffers or allocating parser storage so corrupted
//! or malicious files fail deterministically with `AETHER_E_LIMIT_EXCEEDED`.

use crate::LoaderError;

/// Maximum input file size allowed (1 GiB).
pub const MAX_FILE_BYTES: u64 = 1024 * 1024 * 1024;
/// Maximum vertex count per avatar (4 million).
pub const MAX_VERTICES: usize = 4_000_000;
/// Maximum triangle index count per avatar (`3 * 8,000,000` indices).
pub const MAX_INDICES: usize = 24_000_000;
/// Maximum decoded texture dimension in pixels (`16384x16384`).
pub const MAX_TEXTURE_DIM: u32 = 16_384;
/// Maximum number of textures in one avatar.
pub const MAX_TEXTURES: usize = 512;
/// Maximum total decoded texture payload (2 GiB).
pub const MAX_TOTAL_TEXTURE_BYTES: u64 = 2 * 1024 * 1024 * 1024;
/// Maximum skeleton bone count per avatar (`1024`).
pub const MAX_BONES: usize = 1024;
/// Maximum morph targets per avatar (`1024`).
pub const MAX_MORPH_TARGETS: usize = 1024;
/// Maximum morph deltas per avatar (`16M`).
pub const MAX_MORPH_DELTAS: usize = 16_000_000;
/// Maximum SpringBone joints per avatar (`2048`).
pub const MAX_SPRING_JOINTS: usize = 2048;
/// Maximum colliders per avatar (`512`).
pub const MAX_COLLIDERS: usize = 512;
/// Maximum material or submesh entries per avatar (`1024`).
pub const MAX_MATERIALS: usize = 1024;

/// Checks `file_len` against `[1, MAX_FILE_BYTES]`.
pub fn check_file_size(file_len: u64) -> Result<(), LoaderError> {
    if file_len == 0 {
        return Err(LoaderError::Parse("empty asset file"));
    }
    if file_len > MAX_FILE_BYTES {
        return Err(LoaderError::LimitExceeded("asset file exceeds 1 GiB limit"));
    }
    Ok(())
}

/// Checks vertex count cap.
pub fn check_vertex_count(count: usize) -> Result<(), LoaderError> {
    if count > MAX_VERTICES {
        return Err(LoaderError::LimitExceeded("vertex count exceeds 4,000,000"));
    }
    Ok(())
}

/// Checks index count cap.
pub fn check_index_count(count: usize) -> Result<(), LoaderError> {
    if count > MAX_INDICES {
        return Err(LoaderError::LimitExceeded(
            "triangle index count exceeds 24,000,000",
        ));
    }
    Ok(())
}

/// Checks texture dimensions cap (`width, height <= 16384`).
pub fn check_texture_dims(width: u32, height: u32) -> Result<(), LoaderError> {
    if width == 0 || height == 0 {
        return Err(LoaderError::Parse("zero-sized texture image"));
    }
    if width > MAX_TEXTURE_DIM || height > MAX_TEXTURE_DIM {
        return Err(LoaderError::LimitExceeded(
            "texture dimension exceeds 16384px",
        ));
    }
    Ok(())
}

/// Checks skeleton bone count cap.
pub fn check_bone_count(count: usize) -> Result<(), LoaderError> {
    if count > MAX_BONES {
        return Err(LoaderError::LimitExceeded("bone count exceeds 1024"));
    }
    Ok(())
}

/// Checks morph target count cap.
pub fn check_morph_count(count: usize) -> Result<(), LoaderError> {
    if count > MAX_MORPH_TARGETS {
        return Err(LoaderError::LimitExceeded(
            "morph target count exceeds 1024",
        ));
    }
    Ok(())
}
