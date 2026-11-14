# Test Data & Corpus Licences

All model and texture assets under `tests/data/` are **100% in-house procedurally generated synthetic test fixtures** produced by `tools/gen_test_corpus.py`.

- **Author**: AETHER-2.5D Project Contributors
- **Licence**: `CC0-1.0` (Public Domain Dedication) / `MIT OR Apache-2.0`
- **External downloads**: None (`0` third-party copyrighted avatars or scans are redistributed in this repository).

## Generated Fixtures

| Path | Format / Convention | Purpose |
|---|---|---|
| `tests/data/reference_avatar.vrm` | VRM 1.0 GLB (70,000 tris, 45k verts, 12 mats, 10x 2048x2048 RGBA PNG, 55 joints, 160 springs, 57 exprs, SDF face tex) | `PERF-007` warm/cold load benchmark (`<= 150 ms` warm), full bake & material pipeline |
| `tests/data/corpus/vrm0_*.vrm` (4 files) | VRM 0.x (`VRM` GLB extension, MToon 0.x, secondaryAnimation, BlendShapeMaster) | VRM 0.x migration, wink X-centroid disambiguation, A-pose & heuristic unit tests |
| `tests/data/corpus/vrm1_*.vrm` (4 files) | VRM 1.0 (`VRMC_vrm`, `VRMC_materials_mtoon`, `VRMC_springBone`) | VRM 1.0 parsing, face SDF + bangs stencil + expressions + chains |
| `tests/data/corpus/fbx_mixamo_*.fbx` (2 files) | Autodesk FBX 7.4 (Binary & ASCII, `mixamorig:*`, cm scale) | `ufbx` Binary & ASCII parsing, Mixamo bone matching, cm -> m normalization |
| `tests/data/corpus/fbx_ue_*.fbx` (2 files) | Autodesk FBX 7.4 (ASCII & Binary, Unreal `pelvis`/`clavicle_l`/`ball_r`, Z-up) | Unreal bone matching, Z-up -> Y-up axis conversion |
| `tests/data/corpus/glb_rigify_*.glb` (3 files) | glTF 2.0 Binary (`DEF-*` Rigify bones, A-pose arms, PBR -> OKLab shade) | Rigify Fuzzy Bone Matcher, `rest_to_tpose_quat`, OKLab PBR shade |
| `tests/data/corpus/pmx_*.pmx` (4 files) | MMD PMX 2.0 (UTF-16LE & UTF-8, Japanese names, Bullet rigid bodies/joints, decimetre scale) | Native PMX parser, LH->RH flip, `* 0.08` scale, Bullet -> SpringBone migration |
| `tests/data/corpus/pmd_01_shiftjis.pmd` (1 file) | MMD PMD 1.0 (Shift-JIS `cp932` names) | Native PMD -> PMX migration + `encoding_rs` Shift-JIS decoding |
| `tests/data/edge_cases/*` | `EXT_meshopt_compression` GLB, Draco-flagged GLB, seam-split cylinder GLB, >1024 bone PMX | Parser edge-case & hard resource limit (`HR-06`) tests |
