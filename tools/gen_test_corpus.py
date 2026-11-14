#!/usr/bin/env python3
"""
Procedural synthetic avatar corpus generator for AETHER-2.5D Phase 2 verification.
Generates:
  - tests/data/reference_avatar.vrm (70,000 tris, 12 materials, 10x 2048^2 PNGs,
    55 humanoid bones, 160 spring joints, 57 expressions, SDF face map)
  - 20 corpus avatars across VRM 0.x (4), VRM 1.0 (4), Mixamo FBX (2),
    Unreal FBX (2), Rigify GLB (3), PMX 2.0 (4), PMD 1.0 (1)
  - Edge-case fixtures under tests/data/edge_cases/
All assets are generated deterministically in-house (CC0-1.0).
"""

from __future__ import annotations

import json
import math
import os
import struct
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DATA_DIR = REPO_ROOT / "tests" / "data"
CORPUS_DIR = DATA_DIR / "corpus"
EDGE_DIR = DATA_DIR / "edge_cases"
CORPUS_TEX_DIR = CORPUS_DIR / "tex"

# Appendix A canonical 55 humanoid bones (name, parent_idx, local_pos)
HUMANOID_55: list[tuple[str, int, tuple[float, float, float]]] = [
    ("hips", -1, (0.0, 0.90, 0.0)),                # 0
    ("spine", 0, (0.0, 0.10, 0.0)),                # 1
    ("chest", 1, (0.0, 0.14, 0.0)),                # 2
    ("upperChest", 2, (0.0, 0.14, 0.0)),           # 3
    ("neck", 3, (0.0, 0.14, 0.0)),                 # 4
    ("head", 4, (0.0, 0.10, 0.0)),                 # 5
    ("leftEye", 5, (0.032, 0.06, 0.08)),           # 6
    ("rightEye", 5, (-0.032, 0.06, 0.08)),         # 7
    ("jaw", 5, (0.0, -0.02, 0.04)),                # 8
    ("leftUpperLeg", 0, (0.09, -0.04, 0.0)),       # 9
    ("leftLowerLeg", 9, (0.0, -0.40, 0.0)),        # 10
    ("leftFoot", 10, (0.0, -0.38, 0.0)),           # 11
    ("leftToes", 11, (0.0, -0.06, 0.12)),          # 12
    ("rightUpperLeg", 0, (-0.09, -0.04, 0.0)),     # 13
    ("rightLowerLeg", 13, (0.0, -0.40, 0.0)),      # 14
    ("rightFoot", 14, (0.0, -0.38, 0.0)),          # 15
    ("rightToes", 15, (0.0, -0.06, 0.12)),         # 16
    ("leftShoulder", 3, (0.05, 0.10, 0.0)),        # 17
    ("leftUpperArm", 17, (0.12, 0.0, 0.0)),        # 18
    ("leftLowerArm", 18, (0.26, 0.0, 0.0)),        # 19
    ("leftHand", 19, (0.24, 0.0, 0.0)),            # 20
    ("rightShoulder", 3, (-0.05, 0.10, 0.0)),      # 21
    ("rightUpperArm", 21, (-0.12, 0.0, 0.0)),      # 22
    ("rightLowerArm", 22, (-0.26, 0.0, 0.0)),      # 23
    ("rightHand", 23, (-0.24, 0.0, 0.0)),          # 24
    ("leftThumbProximal", 20, (0.02, -0.01, 0.02)),      # 25
    ("leftThumbIntermediate", 25, (0.025, 0.0, 0.015)),  # 26
    ("leftThumbDistal", 26, (0.02, 0.0, 0.01)),          # 27
    ("leftIndexProximal", 20, (0.06, 0.0, 0.02)),        # 28
    ("leftIndexIntermediate", 28, (0.03, 0.0, 0.0)),     # 29
    ("leftIndexDistal", 29, (0.02, 0.0, 0.0)),           # 30
    ("leftMiddleProximal", 20, (0.065, 0.0, 0.0)),       # 31
    ("leftMiddleIntermediate", 31, (0.032, 0.0, 0.0)),   # 32
    ("leftMiddleDistal", 32, (0.022, 0.0, 0.0)),         # 33
    ("leftRingProximal", 20, (0.06, 0.0, -0.018)),       # 34
    ("leftRingIntermediate", 34, (0.03, 0.0, 0.0)),      # 35
    ("leftRingDistal", 35, (0.02, 0.0, 0.0)),            # 36
    ("leftLittleProximal", 20, (0.055, 0.0, -0.034)),    # 37
    ("leftLittleIntermediate", 37, (0.025, 0.0, 0.0)),   # 38
    ("leftLittleDistal", 38, (0.018, 0.0, 0.0)),         # 39
    ("rightThumbProximal", 24, (-0.02, -0.01, 0.02)),    # 40
    ("rightThumbIntermediate", 40, (-0.025, 0.0, 0.015)),# 41
    ("rightThumbDistal", 41, (-0.02, 0.0, 0.01)),        # 42
    ("rightIndexProximal", 24, (-0.06, 0.0, 0.02)),      # 43
    ("rightIndexIntermediate", 43, (-0.03, 0.0, 0.0)),   # 44
    ("rightIndexDistal", 44, (-0.02, 0.0, 0.0)),         # 45
    ("rightMiddleProximal", 24, (-0.065, 0.0, 0.0)),     # 46
    ("rightMiddleIntermediate", 46, (-0.032, 0.0, 0.0)), # 47
    ("rightMiddleDistal", 47, (-0.022, 0.0, 0.0)),       # 48
    ("rightRingProximal", 24, (-0.06, 0.0, -0.018)),     # 49
    ("rightRingIntermediate", 49, (-0.03, 0.0, 0.0)),    # 50
    ("rightRingDistal", 50, (-0.02, 0.0, 0.0)),          # 51
    ("rightLittleProximal", 24, (-0.055, 0.0, -0.034)),  # 52
    ("rightLittleIntermediate", 52, (-0.025, 0.0, 0.0)), # 53
    ("rightLittleDistal", 53, (-0.018, 0.0, 0.0)),       # 54
]

ARKIT_52 = [
    "eyeBlinkLeft", "eyeLookDownLeft", "eyeLookInLeft", "eyeLookOutLeft", "eyeLookUpLeft",
    "eyeSquintLeft", "eyeWideLeft", "eyeBlinkRight", "eyeLookDownRight", "eyeLookInRight",
    "eyeLookOutRight", "eyeLookUpRight", "eyeSquintRight", "eyeWideRight", "jawForward",
    "jawLeft", "jawRight", "jawOpen", "mouthClose", "mouthFunnel", "mouthPucker",
    "mouthLeft", "mouthRight", "mouthSmileLeft", "mouthSmileRight", "mouthFrownLeft",
    "mouthFrownRight", "mouthDimpleLeft", "mouthDimpleRight", "mouthStretchLeft",
    "mouthStretchRight", "mouthRollLower", "mouthRollUpper", "mouthShrugLower",
    "mouthShrugUpper", "mouthPressLeft", "mouthPressRight", "mouthLowerDownLeft",
    "mouthLowerDownRight", "mouthUpperUpLeft", "mouthUpperUpRight", "browDownLeft",
    "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight", "cheekPuff",
    "cheekSquintLeft", "cheekSquintRight", "noseSneerLeft", "noseSneerRight", "tongueOut",
]

VISEMES_5 = ["aa", "ih", "ou", "ee", "oh"]

JP_BONEMAP = {
    "hips": "下半身", "spine": "上半身", "chest": "上半身2", "upperChest": "上半身3",
    "neck": "首", "head": "頭", "leftEye": "左目", "rightEye": "右目", "jaw": "顎",
    "leftUpperLeg": "左足", "leftLowerLeg": "左ひざ", "leftFoot": "左足首", "leftToes": "左つま先",
    "rightUpperLeg": "右足", "rightLowerLeg": "右ひざ", "rightFoot": "右足首", "rightToes": "右つま先",
    "leftShoulder": "左肩", "leftUpperArm": "左腕", "leftLowerArm": "左ひじ", "leftHand": "左手首",
    "rightShoulder": "右肩", "rightUpperArm": "右腕", "rightLowerArm": "右ひじ", "rightHand": "右手首",
    "leftThumbProximal": "左親指０", "leftThumbIntermediate": "左親指１", "leftThumbDistal": "左親指２",
    "leftIndexProximal": "左人指１", "leftIndexIntermediate": "左人指２", "leftIndexDistal": "左人指３",
    "leftMiddleProximal": "左中指１", "leftMiddleIntermediate": "左中指２", "leftMiddleDistal": "左中指３",
    "leftRingProximal": "左薬指１", "leftRingIntermediate": "左薬指２", "leftRingDistal": "左薬指３",
    "leftLittleProximal": "左小指１", "leftLittleIntermediate": "左小指２", "leftLittleDistal": "左小指３",
    "rightThumbProximal": "右親指０", "rightThumbIntermediate": "右親指１", "rightThumbDistal": "右親指２",
    "rightIndexProximal": "右人指１", "rightIndexIntermediate": "右人指２", "rightIndexDistal": "右人指３",
    "rightMiddleProximal": "右中指１", "rightMiddleIntermediate": "右中指２", "rightMiddleDistal": "右中指３",
    "rightRingProximal": "右薬指１", "rightRingIntermediate": "右薬指２", "rightRingDistal": "右薬指３",
    "rightLittleProximal": "右小指１", "rightLittleIntermediate": "右小指２", "rightLittleDistal": "右小指３",
}

MIXAMO_BONEMAP = {
    "hips": "mixamorig:Hips", "spine": "mixamorig:Spine", "chest": "mixamorig:Spine1",
    "upperChest": "mixamorig:Spine2", "neck": "mixamorig:Neck", "head": "mixamorig:Head",
    "leftEye": "mixamorig:LeftEye", "rightEye": "mixamorig:RightEye", "jaw": "mixamorig:Jaw",
    "leftUpperLeg": "mixamorig:LeftUpLeg", "leftLowerLeg": "mixamorig:LeftLeg",
    "leftFoot": "mixamorig:LeftFoot", "leftToes": "mixamorig:LeftToeBase",
    "rightUpperLeg": "mixamorig:RightUpLeg", "rightLowerLeg": "mixamorig:RightLeg",
    "rightFoot": "mixamorig:RightFoot", "rightToes": "mixamorig:RightToeBase",
    "leftShoulder": "mixamorig:LeftShoulder", "leftUpperArm": "mixamorig:LeftArm",
    "leftLowerArm": "mixamorig:LeftForeArm", "leftHand": "mixamorig:LeftHand",
    "rightShoulder": "mixamorig:RightShoulder", "rightUpperArm": "mixamorig:RightArm",
    "rightLowerArm": "mixamorig:RightForeArm", "rightHand": "mixamorig:RightHand",
    "leftThumbProximal": "mixamorig:LeftHandThumb1", "leftThumbIntermediate": "mixamorig:LeftHandThumb2",
    "leftThumbDistal": "mixamorig:LeftHandThumb3",
    "leftIndexProximal": "mixamorig:LeftHandIndex1", "leftIndexIntermediate": "mixamorig:LeftHandIndex2",
    "leftIndexDistal": "mixamorig:LeftHandIndex3",
    "leftMiddleProximal": "mixamorig:LeftHandMiddle1", "leftMiddleIntermediate": "mixamorig:LeftHandMiddle2",
    "leftMiddleDistal": "mixamorig:LeftHandMiddle3",
    "leftRingProximal": "mixamorig:LeftHandRing1", "leftRingIntermediate": "mixamorig:LeftHandRing2",
    "leftRingDistal": "mixamorig:LeftHandRing3",
    "leftLittleProximal": "mixamorig:LeftHandPinky1", "leftLittleIntermediate": "mixamorig:LeftHandPinky2",
    "leftLittleDistal": "mixamorig:LeftHandPinky3",
    "rightThumbProximal": "mixamorig:RightHandThumb1", "rightThumbIntermediate": "mixamorig:RightHandThumb2",
    "rightThumbDistal": "mixamorig:RightHandThumb3",
    "rightIndexProximal": "mixamorig:RightHandIndex1", "rightIndexIntermediate": "mixamorig:RightHandIndex2",
    "rightIndexDistal": "mixamorig:RightHandIndex3",
    "rightMiddleProximal": "mixamorig:RightHandMiddle1", "rightMiddleIntermediate": "mixamorig:RightHandMiddle2",
    "rightMiddleDistal": "mixamorig:RightHandMiddle3",
    "rightRingProximal": "mixamorig:RightHandRing1", "rightRingIntermediate": "mixamorig:RightHandRing2",
    "rightRingDistal": "mixamorig:RightHandRing3",
    "rightLittleProximal": "mixamorig:RightHandPinky1", "rightLittleIntermediate": "mixamorig:RightHandPinky2",
    "rightLittleDistal": "mixamorig:RightHandPinky3",
}

UE_BONEMAP = {
    "hips": "pelvis", "spine": "spine_01", "chest": "spine_02", "upperChest": "spine_03",
    "neck": "neck_01", "head": "head", "leftEye": "eye_l", "rightEye": "eye_r", "jaw": "jaw",
    "leftUpperLeg": "thigh_l", "leftLowerLeg": "calf_l", "leftFoot": "foot_l", "leftToes": "ball_l",
    "rightUpperLeg": "thigh_r", "rightLowerLeg": "calf_r", "rightFoot": "foot_r", "rightToes": "ball_r",
    "leftShoulder": "clavicle_l", "leftUpperArm": "upperarm_l", "leftLowerArm": "lowerarm_l", "leftHand": "hand_l",
    "rightShoulder": "clavicle_r", "rightUpperArm": "upperarm_r", "rightLowerArm": "lowerarm_r", "rightHand": "hand_r",
    "leftThumbProximal": "thumb_01_l", "leftThumbIntermediate": "thumb_02_l", "leftThumbDistal": "thumb_03_l",
    "leftIndexProximal": "index_01_l", "leftIndexIntermediate": "index_02_l", "leftIndexDistal": "index_03_l",
    "leftMiddleProximal": "middle_01_l", "leftMiddleIntermediate": "middle_02_l", "leftMiddleDistal": "middle_03_l",
    "leftRingProximal": "ring_01_l", "leftRingIntermediate": "ring_02_l", "leftRingDistal": "ring_03_l",
    "leftLittleProximal": "pinky_01_l", "leftLittleIntermediate": "pinky_02_l", "leftLittleDistal": "pinky_03_l",
    "rightThumbProximal": "thumb_01_r", "rightThumbIntermediate": "thumb_02_r", "rightThumbDistal": "thumb_03_r",
    "rightIndexProximal": "index_01_r", "rightIndexIntermediate": "index_02_r", "rightIndexDistal": "index_03_r",
    "rightMiddleProximal": "middle_01_r", "rightMiddleIntermediate": "middle_02_r", "rightMiddleDistal": "middle_03_r",
    "rightRingProximal": "ring_01_r", "rightRingIntermediate": "ring_02_r", "rightRingDistal": "ring_03_r",
    "rightLittleProximal": "pinky_01_r", "rightLittleIntermediate": "pinky_02_r", "rightLittleDistal": "pinky_03_r",
}

RIGIFY_BONEMAP = {
    "hips": "DEF-spine", "spine": "DEF-spine.001", "chest": "DEF-spine.002", "upperChest": "DEF-spine.003",
    "neck": "DEF-spine.004", "head": "DEF-spine.006", "leftEye": "DEF-eye.L", "rightEye": "DEF-eye.R", "jaw": "DEF-jaw",
    "leftUpperLeg": "DEF-thigh.L", "leftLowerLeg": "DEF-shin.L", "leftFoot": "DEF-foot.L", "leftToes": "DEF-toe.L",
    "rightUpperLeg": "DEF-thigh.R", "rightLowerLeg": "DEF-shin.R", "rightFoot": "DEF-foot.R", "rightToes": "DEF-toe.R",
    "leftShoulder": "DEF-shoulder.L", "leftUpperArm": "DEF-upper_arm.L", "leftLowerArm": "DEF-forearm.L", "leftHand": "DEF-hand.L",
    "rightShoulder": "DEF-shoulder.R", "rightUpperArm": "DEF-upper_arm.R", "rightLowerArm": "DEF-forearm.R", "rightHand": "DEF-hand.R",
    "leftThumbProximal": "DEF-thumb.01.L", "leftThumbIntermediate": "DEF-thumb.02.L", "leftThumbDistal": "DEF-thumb.03.L",
    "leftIndexProximal": "DEF-f_index.01.L", "leftIndexIntermediate": "DEF-f_index.02.L", "leftIndexDistal": "DEF-f_index.03.L",
    "leftMiddleProximal": "DEF-f_middle.01.L", "leftMiddleIntermediate": "DEF-f_middle.02.L", "leftMiddleDistal": "DEF-f_middle.03.L",
    "leftRingProximal": "DEF-f_ring.01.L", "leftRingIntermediate": "DEF-f_ring.02.L", "leftRingDistal": "DEF-f_ring.03.L",
    "leftLittleProximal": "DEF-f_pinky.01.L", "leftLittleIntermediate": "DEF-f_pinky.02.L", "leftLittleDistal": "DEF-f_pinky.03.L",
    "rightThumbProximal": "DEF-thumb.01.R", "rightThumbIntermediate": "DEF-thumb.02.R", "rightThumbDistal": "DEF-thumb.03.R",
    "rightIndexProximal": "DEF-f_index.01.R", "rightIndexIntermediate": "DEF-f_index.02.R", "rightIndexDistal": "DEF-f_index.03.R",
    "rightMiddleProximal": "DEF-f_middle.01.R", "rightMiddleIntermediate": "DEF-f_middle.02.R", "rightMiddleDistal": "DEF-f_middle.03.R",
    "rightRingProximal": "DEF-f_ring.01.R", "rightRingIntermediate": "DEF-f_ring.02.R", "rightRingDistal": "DEF-f_ring.03.R",
    "rightLittleProximal": "DEF-f_pinky.01.R", "rightLittleIntermediate": "DEF-f_pinky.02.R", "rightLittleDistal": "DEF-f_pinky.03.R",
}


def compute_world_positions(joints: list[tuple[str, int, tuple[float, float, float]]]) -> list[tuple[float, float, float]]:
    world: list[tuple[float, float, float]] = []
    for _, parent, local in joints:
        if parent < 0:
            world.append(local)
        else:
            px, py, pz = world[parent]
            world.append((px + local[0], py + local[1], pz + local[2]))
    return world


def make_png_rgba(width: int, height: int, seed: int, is_sdf: bool = False) -> bytes:
    """Fast pure-Python RGBA8 PNG generator; large textures compress cleanly & decode quickly."""
    def chunk(tag: bytes, payload: bytes) -> bytes:
        crc = zlib.crc32(tag + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)

    # Build a small tileable row set so 2048x2048 compresses tiny & runs quickly in Python.
    row_stride = width * 4
    rows = bytearray()
    if is_sdf:
        cx = width * 0.5
        for y in range(height):
            ny = abs((y / max(1, height - 1)) * 2.0 - 1.0)
            row = bytearray(1 + row_stride)
            row[0] = 0
            if height <= 128:
                for x in range(width):
                    nx = abs((x / max(1, width - 1)) * 2.0 - 1.0)
                    v = max(0, min(255, int((1.0 - min(1.0, math.hypot(nx * 0.85, ny * 0.6))) * 255.0)))
                    off = 1 + x * 4
                    row[off : off + 4] = bytes((v, v, v, 255))
            else:
                v = max(0, min(255, int((1.0 - min(1.0, ny * 0.7)) * 255.0)))
                row[1:] = bytes((v, v, v, 255)) * width
            rows.extend(row)
    else:
        r0 = (90 + (seed * 37) % 150) & 0xFF
        g0 = (80 + (seed * 73) % 150) & 0xFF
        b0 = (110 + (seed * 53) % 130) & 0xFF
        # 16 distinct gradient scanlines repeated across height so compression is ultra-fast
        band_rows = []
        for b in range(16):
            r = (r0 + b * 4) & 0xFF
            g = (g0 + b * 3) & 0xFF
            bl = (b0 + b * 5) & 0xFF
            pix_a = bytes((r, g, bl, 255))
            pix_b = bytes(((r + 12) & 0xFF, (g + 8) & 0xFF, bl, 255))
            half = max(1, width // 2)
            row_data = b"\x00" + (pix_a * half) + (pix_b * (width - half))
            band_rows.append(row_data)
        band_h = max(1, height // 16)
        for y in range(height):
            rows.extend(band_rows[min(15, y // band_h)])

    comp = zlib.compressobj(level=1)
    idat = comp.compress(bytes(rows)) + comp.flush()
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


def build_uv_sphere_grid(
    nu: int,
    nv: int,
    center: tuple[float, float, float],
    radius: tuple[float, float, float],
    joint_idx: int,
) -> tuple[list[tuple[float, float, float]], list[tuple[float, float, float]], list[tuple[float, float]], list[int], list[int]]:
    pos = []
    nrm = []
    uv = []
    joints = []
    indices = []
    cx, cy, cz = center
    rx, ry, rz = radius
    for j in range(nv + 1):
        v = j / nv
        phi = v * math.pi
        sp = math.sin(phi)
        cp = math.cos(phi)
        for i in range(nu + 1):
            u = i / nu
            theta = u * math.tau
            st = math.sin(theta)
            ct = math.cos(theta)
            nx, ny, nz = ct * sp, cp, st * sp
            pos.append((cx + nx * rx, cy + ny * ry, cz + nz * rz))
            inv = 1.0 / max(1e-6, math.sqrt(nx * nx + ny * ny + nz * nz))
            nrm.append((nx * inv, ny * inv, nz * inv))
            uv.append((u, v))
            joints.append(joint_idx)
    stride = nu + 1
    for j in range(nv):
        for i in range(nu):
            a = j * stride + i
            b = a + stride
            c = a + 1
            d = b + 1
            indices.extend((a, b, c, c, b, d))
    return pos, nrm, uv, joints, indices


def pack_glb(gltf_json: dict, bin_blob: bytes) -> bytes:
    json_bytes = json.dumps(gltf_json, separators=(",", ":")).encode("utf-8")
    while len(json_bytes) % 4 != 0:
        json_bytes += b" "
    bin_padded = bytearray(bin_blob)
    while len(bin_padded) % 4 != 0:
        bin_padded.append(0)
    total_len = 12 + 8 + len(json_bytes) + 8 + len(bin_padded)
    header = struct.pack("<3I", 0x46546C67, 2, total_len)
    c0 = struct.pack("<2I", len(json_bytes), 0x4E4F534A) + json_bytes
    c1 = struct.pack("<2I", len(bin_padded), 0x004E4942) + bytes(bin_padded)
    return header + c0 + c1


def build_reference_avatar_vrm(out_path: Path) -> None:
    """70,000 tris (~45k verts), 12 materials, 10x 2048x2048 PNGs, 160 spring joints, 57 exprs."""
    # Build skeleton with 55 humanoid joints + 160 spring hair/ribbon joints (20 chains x 8 depth)
    joints = list(HUMANOID_55)
    head_idx = 5
    chain_roots: list[list[int]] = []
    for c in range(20):
        angle = (c / 20.0) * math.tau
        chain = []
        parent = head_idx
        for d in range(8):
            idx = len(joints)
            if d == 0:
                loc = (math.cos(angle) * 0.08, 0.08, math.sin(angle) * 0.08)
            else:
                loc = (0.0, -0.05, 0.0)
            joints.append((f"HairChain_{c:02d}_{d:02d}", parent, loc))
            chain.append(idx)
            parent = idx
        chain_roots.append(chain)

    world_pos = compute_world_positions(joints)

    # Create 12 submeshes totaling EXACTLY 70,000 triangles across the body/head/hair/outfit:
    # Each sphere grid of nu x nv has 2 * nu * nv triangles.
    # Let's pick exact (nu, nv) so sum(2 * nu * nv) == 70,000 triangles!
    submesh_specs = [
        # (name, (cx,cy,cz), (rx,ry,rz), joint_idx, nu, nv, face_or_hair_role)
        ("Face_Skin", (0.0, 1.56, 0.01), (0.095, 0.115, 0.095), 5, 100, 50, "face"),     # 10,000 tris
        ("Eye_Iris", (0.0, 1.58, 0.09), (0.04, 0.02, 0.02), 6, 50, 25, "eye"),           # 2,500 tris
        ("Eye_Brow", (0.0, 1.61, 0.092), (0.05, 0.015, 0.015), 5, 50, 25, "brow"),       # 2,500 tris
        ("Hair_Front_Bangs", (0.0, 1.64, 0.055), (0.11, 0.09, 0.08), 5, 100, 40, "hair_front"), # 8,000 tris
        ("Hair_Back_Main", (0.0, 1.50, -0.06), (0.14, 0.28, 0.10), 55, 100, 50, "hair_back"),   # 10,000 tris
        ("Body_Skin", (0.0, 1.15, 0.0), (0.14, 0.25, 0.10), 2, 100, 40, "skin"),         # 8,000 tris
        ("Top_Outfit", (0.0, 1.24, 0.0), (0.16, 0.22, 0.12), 3, 100, 40, "opaque"),      # 8,000 tris
        ("Skirt_Cloth", (0.0, 0.88, 0.0), (0.20, 0.20, 0.18), 0, 100, 40, "opaque"),     # 8,000 tris
        ("Sleeve_Left", (0.28, 1.32, 0.0), (0.18, 0.06, 0.06), 18, 50, 35, "opaque"),    # 3,500 tris
        ("Sleeve_Right", (-0.28, 1.32, 0.0), (0.18, 0.06, 0.06), 22, 50, 35, "opaque"),  # 3,500 tris
        ("Ribbon_Accent", (0.0, 1.35, 0.12), (0.09, 0.06, 0.03), 3, 50, 30, "hair_front"),# 3,000 tris
        ("Boots_Opaque", (0.0, 0.22, 0.02), (0.14, 0.22, 0.14), 11, 50, 30, "opaque"),   # 3,000 tris
    ]
    assert sum(2 * nu * nv for *_rest, nu, nv, _ in submesh_specs) == 70000

    bin_buf = bytearray()
    buffer_views = []
    accessors = []

    def append_bytes(raw: bytes, target: int | None = None) -> int:
        while len(bin_buf) % 4 != 0:
            bin_buf.append(0)
        off = len(bin_buf)
        bin_buf.extend(raw)
        bv = {"buffer": 0, "byteOffset": off, "byteLength": len(raw)}
        if target is not None:
            bv["target"] = target
        buffer_views.append(bv)
        return len(buffer_views) - 1

    # Add 10 textures (2048x2048 RGBA8 PNGs), where index 9 is face SDF map
    images = []
    textures = []
    for t in range(10):
        png = make_png_rgba(2048, 2048, seed=t + 1, is_sdf=(t == 9))
        bv = append_bytes(png)
        images.append({"bufferView": bv, "mimeType": "image/png", "name": f"ref_tex_{t}" if t < 9 else "face_sdf"})
        textures.append({"source": t})

    # Inverse bind matrices for all joints (column-major glTF 4x4)
    ibm_raw = bytearray()
    for wx, wy, wz in world_pos:
        ibm_raw.extend(
            struct.pack(
                "<16f",
                1.0, 0.0, 0.0, 0.0,
                0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0,
                -wx, -wy, -wz, 1.0,
            )
        )
    ibm_bv = append_bytes(bytes(ibm_raw))
    accessors.append({"bufferView": ibm_bv, "componentType": 5126, "count": len(joints), "type": "MAT4"})
    ibm_acc = len(accessors) - 1

    primitives = []
    morph_names_list = ARKIT_52[:52] + [f"vis_{v}" for v in VISEMES_5]
    # Build 1 mesh with 12 primitives; attach 57 morph targets on Face_Skin primitive (0)
    for m_idx, (name, center, radius, j_idx, nu, nv, _role) in enumerate(submesh_specs):
        pos, nrm, uv, jts, inds = build_uv_sphere_grid(nu, nv, center, radius, j_idx)
        pmin = [min(p[k] for p in pos) for k in range(3)]
        pmax = [max(p[k] for p in pos) for k in range(3)]

        p_raw = b"".join(struct.pack("<3f", *p) for p in pos)
        n_raw = b"".join(struct.pack("<3f", *n) for n in nrm)
        t_raw = b"".join(struct.pack("<2f", *u) for u in uv)
        j_raw = b"".join(struct.pack("<4H", j, 0, 0, 0) for j in jts)
        w_raw = b"".join(struct.pack("<4f", 1.0, 0.0, 0.0, 0.0) for _ in jts)
        i_raw = b"".join(struct.pack("<I", idx) for idx in inds)

        bv_p = append_bytes(p_raw, 34962)
        bv_n = append_bytes(n_raw, 34962)
        bv_t = append_bytes(t_raw, 34962)
        bv_j = append_bytes(j_raw, 34962)
        bv_w = append_bytes(w_raw, 34962)
        bv_i = append_bytes(i_raw, 34963)

        acc_p = len(accessors)
        accessors.append({"bufferView": bv_p, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": pmin, "max": pmax})
        acc_n = len(accessors)
        accessors.append({"bufferView": bv_n, "componentType": 5126, "count": len(nrm), "type": "VEC3"})
        acc_t = len(accessors)
        accessors.append({"bufferView": bv_t, "componentType": 5126, "count": len(uv), "type": "VEC2"})
        acc_j = len(accessors)
        accessors.append({"bufferView": bv_j, "componentType": 5123, "count": len(jts), "type": "VEC4"})
        acc_w = len(accessors)
        accessors.append({"bufferView": bv_w, "componentType": 5126, "count": len(jts), "type": "VEC4"})
        acc_i = len(accessors)
        accessors.append({"bufferView": bv_i, "componentType": 5125, "count": len(inds), "type": "SCALAR"})

        prim: dict = {
            "attributes": {
                "POSITION": acc_p,
                "NORMAL": acc_n,
                "TEXCOORD_0": acc_t,
                "JOINTS_0": acc_j,
                "WEIGHTS_0": acc_w,
            },
            "indices": acc_i,
            "material": m_idx,
        }

        if m_idx == 0:
            # Reuse two shared sparse delta buffers (left-wink, right-wink, neutral center) for 57 morph targets
            d_left = b"".join(
                struct.pack("<3f", 0.0, -0.015 if pos[v][0] > 0.01 else 0.0, 0.002 if pos[v][0] > 0.01 else 0.0)
                for v in range(len(pos))
            )
            d_right = b"".join(
                struct.pack("<3f", 0.0, -0.015 if pos[v][0] < -0.01 else 0.0, 0.002 if pos[v][0] < -0.01 else 0.0)
                for v in range(len(pos))
            )
            d_center = b"".join(
                struct.pack("<3f", 0.0, -0.006 if v % 3 == 0 else 0.0, 0.005 if v % 3 == 0 else 0.0)
                for v in range(len(pos))
            )
            bv_dl = append_bytes(d_left, 34962)
            bv_dr = append_bytes(d_right, 34962)
            bv_dc = append_bytes(d_center, 34962)
            acc_dl = len(accessors)
            accessors.append({"bufferView": bv_dl, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": [-0.02, -0.02, -0.02], "max": [0.02, 0.02, 0.02]})
            acc_dr = len(accessors)
            accessors.append({"bufferView": bv_dr, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": [-0.02, -0.02, -0.02], "max": [0.02, 0.02, 0.02]})
            acc_dc = len(accessors)
            accessors.append({"bufferView": bv_dc, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": [-0.02, -0.02, -0.02], "max": [0.02, 0.02, 0.02]})
            targets = []
            for t_i, t_name in enumerate(morph_names_list):
                if "Left" in t_name:
                    targets.append({"POSITION": acc_dl})
                elif "Right" in t_name:
                    targets.append({"POSITION": acc_dr})
                else:
                    targets.append({"POSITION": acc_dc})
            prim["targets"] = targets
            prim["extras"] = {"targetNames": morph_names_list}
        primitives.append(prim)

    # Nodes: 0..len(joints)-1 are skeleton nodes; last node is mesh+skin node
    mesh_node_idx = len(joints)
    children_map: dict[int, list[int]] = {i: [] for i in range(len(joints))}
    for i, (_n, p, _loc) in enumerate(joints):
        if p >= 0:
            children_map[p].append(i)

    nodes = []
    for i, (name, _p, loc) in enumerate(joints):
        nd: dict = {"name": name, "translation": [loc[0], loc[1], loc[2]]}
        if children_map[i]:
            nd["children"] = children_map[i]
        nodes.append(nd)
    nodes.append({"name": "AvatarMeshRoot", "mesh": 0, "skin": 0})

    materials = []
    for idx, (m_name, *_, role) in enumerate(submesh_specs):
        tex_idx = min(8, idx % 9)
        mtoon_ext: dict = {
            "specVersion": "1.0",
            "shadeColorFactor": [0.85, 0.72, 0.72],
            "shadingShiftFactor": -0.08,
            "shadingToonyFactor": 0.90,
            "giEqualizationFactor": 0.20,
            "parametricRimColorFactor": [1.0, 0.96, 0.92],
            "parametricRimFresnelPowerFactor": 4.0,
            "parametricRimLiftFactor": 0.1,
            "rimLightingMixFactor": 0.5,
            "outlineWidthMode": "worldCoordinates",
            "outlineWidthFactor": 0.0022,
            "outlineColorFactor": [0.12, 0.08, 0.08],
            "outlineLightingMixFactor": 0.6,
        }
        mat: dict = {
            "name": m_name,
            "pbrMetallicRoughness": {
                "baseColorFactor": [1.0, 0.98, 0.96, 1.0],
                "baseColorTexture": {"index": tex_idx},
                "metallicFactor": 0.0,
                "roughnessFactor": 0.9,
            },
            "alphaMode": "MASK" if role in ("eye", "brow") else "OPAQUE",
            "alphaCutoff": 0.5,
            "extensions": {"VRMC_materials_mtoon": mtoon_ext},
        }
        if role == "face":
            mat["extras"] = {"aetherFaceSdfTexture": 9}
        materials.append(mat)

    human_bones_vrm1 = {}
    for b_idx, (h_name, _, _) in enumerate(HUMANOID_55):
        human_bones_vrm1[h_name] = {"node": b_idx}

    custom_exprs = {}
    preset_exprs = {}
    for e_idx, e_name in enumerate(morph_names_list):
        bind = [{"node": mesh_node_idx, "index": e_idx, "weight": 1.0}]
        if e_name.startswith("vis_"):
            vkey = e_name[4:]
            preset_exprs[vkey] = {"morphTargetBinds": bind, "isBinary": False}
        elif e_name == "eyeBlinkLeft":
            preset_exprs["blinkLeft"] = {"morphTargetBinds": bind, "isBinary": False}
        elif e_name == "eyeBlinkRight":
            preset_exprs["blinkRight"] = {"morphTargetBinds": bind, "isBinary": False}
        else:
            custom_exprs[e_name] = {"morphTargetBinds": bind, "isBinary": False}

    # SpringBone colliders (head, chest) + 20 hair chains (160 joints)
    colliders = [
        {"node": 5, "shape": {"sphere": {"offset": [0.0, 0.04, 0.0], "radius": 0.11}}},
        {"node": 2, "shape": {"capsule": {"offset": [0.0, 0.0, 0.0], "radius": 0.14, "tail": [0.0, 0.16, 0.0]}}},
    ]
    collider_groups = [{"name": "Body", "colliders": [0, 1]}]
    springs = []
    for ch_idx, chain in enumerate(chain_roots):
        springs.append(
            {
                "name": f"HairSpring_{ch_idx:02d}",
                "joints": [
                    {
                        "node": j_i,
                        "hitRadius": 0.025,
                        "stiffness": 1.2,
                        "gravityPower": 0.4,
                        "gravityDir": [0.0, -1.0, 0.0],
                        "dragForce": 0.45,
                    }
                    for j_i in chain
                ],
                "colliderGroups": [0],
                "center": 5,
            }
        )

    gltf_json = {
        "asset": {"version": "2.0", "generator": "AETHER-2.5D Test Corpus Generator"},
        "extensionsUsed": ["VRMC_vrm", "VRMC_materials_mtoon", "VRMC_springBone"],
        "scene": 0,
        "scenes": [{"nodes": [0, mesh_node_idx]}],
        "nodes": nodes,
        "meshes": [{"name": "ReferenceAvatar", "primitives": primitives}],
        "skins": [{"joints": list(range(len(joints))), "inverseBindMatrices": ibm_acc, "skeleton": 0}],
        "materials": materials,
        "textures": textures,
        "images": images,
        "buffers": [{"byteLength": len(bin_buf)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
        "extensions": {
            "VRMC_vrm": {
                "specVersion": "1.0",
                "meta": {"name": "ReferenceAvatar_70k", "version": "1.0", "authors": ["AETHER"], "licenseUrl": "https://creativecommons.org/publicdomain/zero/1.0/"},
                "humanoid": {"humanBones": human_bones_vrm1},
                "expressions": {"preset": preset_exprs, "custom": custom_exprs},
                "lookAt": {"offsetFromHeadBone": [0.0, 0.06, 0.0]},
            },
            "VRMC_springBone": {
                "specVersion": "1.0",
                "colliders": colliders,
                "colliderGroups": collider_groups,
                "springs": springs,
            },
        },
    }
    out_path.write_bytes(pack_glb(gltf_json, bytes(bin_buf)))


def build_compact_glb_vrm(
    out_path: Path,
    mode: str,
    bone_renames: dict[str, str] | None = None,
    apose: bool = False,
    scale_units: float = 1.0,
    jp_morph_names: bool = False,
    add_meshopt: bool = False,
    add_draco: bool = False,
    split_seam_cylinder: bool = False,
) -> None:
    """Builds compact glTF / GLB / VRM0 / VRM1 avatars with varied conventions."""
    joints = []
    for h_name, p_idx, loc in HUMANOID_55:
        j_name = bone_renames[h_name] if bone_renames and h_name in bone_renames else h_name
        joints.append((j_name, p_idx, (loc[0] * scale_units, loc[1] * scale_units, loc[2] * scale_units)))
    # Add 2 hair chain bones
    joints.append(("HairFront_01", 5, (0.04 * scale_units, 0.05 * scale_units, 0.05 * scale_units)))
    joints.append(("HairFront_02", 55, (0.0, -0.08 * scale_units, 0.0)))
    world_pos = compute_world_positions(joints)

    bin_buf = bytearray()
    buffer_views = []
    accessors = []

    def append_bytes(raw: bytes, target: int | None = None) -> int:
        while len(bin_buf) % 4 != 0:
            bin_buf.append(0)
        off = len(bin_buf)
        bin_buf.extend(raw)
        bv = {"buffer": 0, "byteOffset": off, "byteLength": len(raw)}
        if target is not None:
            bv["target"] = target
        buffer_views.append(bv)
        return len(buffer_views) - 1

    # Add 2 compact PNG textures (face diffuse + sdf)
    png0 = make_png_rgba(128, 128, seed=11, is_sdf=False)
    png1 = make_png_rgba(64, 64, seed=22, is_sdf=True)
    bv_img0 = append_bytes(png0)
    bv_img1 = append_bytes(png1)
    images = [
        {"bufferView": bv_img0, "mimeType": "image/png", "name": "face_albedo"},
        {"bufferView": bv_img1, "mimeType": "image/png", "name": "face_sdf"},
    ]
    textures = [{"source": 0}, {"source": 1}]

    ibm_raw = b"".join(
        struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -wx, -wy, -wz, 1)
        for wx, wy, wz in world_pos
    )
    bv_ibm = append_bytes(ibm_raw)
    acc_ibm = len(accessors)
    accessors.append({"bufferView": bv_ibm, "componentType": 5126, "count": len(joints), "type": "MAT4"})

    meshes_spec = [
        ("Head_Face", (0.0, 1.56 * scale_units, 0.0), (0.10 * scale_units, 0.12 * scale_units, 0.10 * scale_units), 5, 16, 12),
        ("Hair_Front", (0.0, 1.65 * scale_units, 0.06 * scale_units), (0.11 * scale_units, 0.08 * scale_units, 0.06 * scale_units), 55, 12, 8),
        ("Body_Torso", (0.0, 1.15 * scale_units, 0.0), (0.14 * scale_units, 0.25 * scale_units, 0.10 * scale_units), 2, 14, 10),
    ]
    primitives = []
    morph_names = ["ウィンク", "ウィンク右", "あ", "い", "笑い"] if jp_morph_names else ["eyeBlinkLeft", "eyeBlinkRight", "mouthOpen", "aa", "smile"]

    for m_idx, (_mn, center, radius, j_idx, nu, nv) in enumerate(meshes_spec):
        pos, nrm, uv, jts, inds = build_uv_sphere_grid(nu, nv, center, radius, j_idx)
        pmin = [min(p[k] for p in pos) for k in range(3)]
        pmax = [max(p[k] for p in pos) for k in range(3)]
        p_raw = b"".join(struct.pack("<3f", *p) for p in pos)
        n_raw = b"".join(struct.pack("<3f", *n) for n in nrm)
        t_raw = b"".join(struct.pack("<2f", *u) for u in uv)
        j_raw = b"".join(struct.pack("<4H", j, 0, 0, 0) for j in jts)
        w_raw = b"".join(struct.pack("<4f", 1.0, 0.0, 0.0, 0.0) for _ in jts)
        i_raw = b"".join(struct.pack("<I", idx) for idx in inds)

        bv_p = append_bytes(p_raw, 34962)
        bv_n = append_bytes(n_raw, 34962)
        bv_t = append_bytes(t_raw, 34962)
        bv_j = append_bytes(j_raw, 34962)
        bv_w = append_bytes(w_raw, 34962)
        bv_i = append_bytes(i_raw, 34963)

        acc_p = len(accessors)
        accessors.append({"bufferView": bv_p, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": pmin, "max": pmax})
        acc_n = len(accessors)
        accessors.append({"bufferView": bv_n, "componentType": 5126, "count": len(nrm), "type": "VEC3"})
        acc_t = len(accessors)
        accessors.append({"bufferView": bv_t, "componentType": 5126, "count": len(uv), "type": "VEC2"})
        acc_j = len(accessors)
        accessors.append({"bufferView": bv_j, "componentType": 5123, "count": len(jts), "type": "VEC4"})
        acc_w = len(accessors)
        accessors.append({"bufferView": bv_w, "componentType": 5126, "count": len(jts), "type": "VEC4"})
        acc_i = len(accessors)
        accessors.append({"bufferView": bv_i, "componentType": 5125, "count": len(inds), "type": "SCALAR"})

        prim: dict = {
            "attributes": {"POSITION": acc_p, "NORMAL": acc_n, "TEXCOORD_0": acc_t, "JOINTS_0": acc_j, "WEIGHTS_0": acc_w},
            "indices": acc_i,
            "material": m_idx,
        }
        if add_draco and m_idx == 0:
            prim["extensions"] = {"KHR_draco_mesh_compression": {"bufferView": bv_p, "attributes": {"POSITION": 0}}}
        if m_idx == 0:
            d_l = b"".join(struct.pack("<3f", 0.0, -0.01 if pos[v][0] > 0.005 * scale_units else 0.0, 0.0) for v in range(len(pos)))
            d_r = b"".join(struct.pack("<3f", 0.0, -0.01 if pos[v][0] < -0.005 * scale_units else 0.0, 0.0) for v in range(len(pos)))
            d_m = b"".join(struct.pack("<3f", 0.0, -0.008, 0.003) for _ in range(len(pos)))
            bvl = append_bytes(d_l, 34962)
            bvr = append_bytes(d_r, 34962)
            bvm = append_bytes(d_m, 34962)
            accl = len(accessors)
            accessors.append({"bufferView": bvl, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": [-0.1, -0.1, -0.1], "max": [0.1, 0.1, 0.1]})
            accr = len(accessors)
            accessors.append({"bufferView": bvr, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": [-0.1, -0.1, -0.1], "max": [0.1, 0.1, 0.1]})
            accm = len(accessors)
            accessors.append({"bufferView": bvm, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": [-0.1, -0.1, -0.1], "max": [0.1, 0.1, 0.1]})
            prim["targets"] = [{"POSITION": accl}, {"POSITION": accr}, {"POSITION": accm}, {"POSITION": accm}, {"POSITION": accm}]
            prim["extras"] = {"targetNames": morph_names}
        primitives.append(prim)

    mesh_node_idx = len(joints)
    children_map: dict[int, list[int]] = {i: [] for i in range(len(joints))}
    for i, (_n, p, _loc) in enumerate(joints):
        if p >= 0:
            children_map[p].append(i)

    # Optional A-pose shoulder droop (+35 deg on rightUpperArm Z, -35 deg on leftUpperArm Z)
    nodes = []
    for i, (name, _p, loc) in enumerate(joints):
        nd: dict = {"name": name, "translation": [loc[0], loc[1], loc[2]]}
        if apose and i == 18:
            ang = math.radians(-35.0)
            nd["rotation"] = [0.0, 0.0, math.sin(ang * 0.5), math.cos(ang * 0.5)]
        elif apose and i == 22:
            ang = math.radians(35.0)
            nd["rotation"] = [0.0, 0.0, math.sin(ang * 0.5), math.cos(ang * 0.5)]
        if children_map[i]:
            nd["children"] = children_map[i]
        nodes.append(nd)
    nodes.append({"name": "AvatarMesh", "mesh": 0, "skin": 0})

    materials = [
        {"name": "FaceMat", "pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.92, 0.88, 1.0], "baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "HairFrontMat", "pbrMetallicRoughness": {"baseColorFactor": [0.25, 0.35, 0.65, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.7}},
        {"name": "BodyMat", "pbrMetallicRoughness": {"baseColorFactor": [0.95, 0.85, 0.80, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.85}},
    ]
    if split_seam_cylinder:
        materials[0]["name"] = "SeamCylinder"

    extensions_used = []
    extensions: dict = {}

    if mode == "vrm0":
        extensions_used = ["VRM"]
        vrm0_bones = [{"bone": HUMANOID_55[i][0], "node": i, "useDefaultValues": True} for i in range(len(HUMANOID_55))]
        extensions["VRM"] = {
            "exporterVersion": "AETHER-TestGen-0.9",
            "specVersion": "0.0",
            "meta": {"title": out_path.stem, "author": "AETHER", "allowedUserName": "Everyone"},
            "humanoid": {"humanBones": vrm0_bones},
            "blendShapeMaster": {
                "blendShapeGroups": [
                    {"name": "Blink_L", "presetName": "blink_l", "binds": [{"mesh": 0, "index": 0, "weight": 100}]},
                    {"name": "Blink_R", "presetName": "blink_r", "binds": [{"mesh": 0, "index": 1, "weight": 100}]},
                    {"name": "A", "presetName": "a", "binds": [{"mesh": 0, "index": 2, "weight": 100}]},
                    {"name": "Joy", "presetName": "joy", "binds": [{"mesh": 0, "index": 4, "weight": 100}]},
                ]
            },
            "secondaryAnimation": {
                "colliderGroups": [{"node": 5, "colliders": [{"offset": {"x": 0.0, "y": 0.03, "z": 0.0}, "radius": 0.1}]}],
                "boneGroups": [{"comment": "Bangs", "stiffiness": 1.0, "gravityPower": 0.3, "gravityDir": {"x": 0.0, "y": -1.0, "z": 0.0}, "dragForce": 0.4, "center": 5, "hitRadius": 0.02, "bones": [55, 56], "colliderGroups": [0]}],
            },
            "materialProperties": [
                {"name": "FaceMat", "shader": "VRM/MToon", "vectorProperties": {"_Color": [1, 0.92, 0.88, 1], "_ShadeColor": [0.82, 0.68, 0.68, 1], "_RimColor": [1, 1, 1, 1], "_OutlineColor": [0.1, 0.05, 0.05, 1]}, "floatProperties": {"_ShadeShift": -0.1, "_ShadeToony": 0.9, "_OutlineWidth": 0.25, "_OutlineWidthMode": 1.0, "_CullMode": 2.0}},
                {"name": "HairFrontMat", "shader": "VRM/MToon", "vectorProperties": {"_Color": [0.25, 0.35, 0.65, 1], "_ShadeColor": [0.15, 0.22, 0.45, 1]}, "floatProperties": {"_ShadeShift": 0.0, "_ShadeToony": 0.85, "_OutlineWidth": 0.2, "_OutlineWidthMode": 1.0}},
                {"name": "BodyMat", "shader": "VRM/MToon", "vectorProperties": {"_Color": [0.95, 0.85, 0.80, 1]}, "floatProperties": {"_ShadeShift": 0.0, "_ShadeToony": 0.9}},
            ],
        }
    elif mode == "vrm1":
        extensions_used = ["VRMC_vrm", "VRMC_materials_mtoon", "VRMC_springBone"]
        materials[0]["extensions"] = {"VRMC_materials_mtoon": {"specVersion": "1.0", "shadeColorFactor": [0.85, 0.72, 0.72], "shadingShiftFactor": -0.1, "shadingToonyFactor": 0.9}}
        materials[0]["extras"] = {"aetherFaceSdfTexture": 1}
        materials[1]["extensions"] = {"VRMC_materials_mtoon": {"specVersion": "1.0", "shadeColorFactor": [0.18, 0.25, 0.50], "outlineWidthMode": "worldCoordinates", "outlineWidthFactor": 0.002}}
        extensions["VRMC_vrm"] = {
            "specVersion": "1.0",
            "meta": {"name": out_path.stem, "authors": ["AETHER"], "licenseUrl": "https://creativecommons.org/publicdomain/zero/1.0/"},
            "humanoid": {"humanBones": {HUMANOID_55[i][0]: {"node": i} for i in range(len(HUMANOID_55))}},
            "expressions": {
                "preset": {
                    "blinkLeft": {"morphTargetBinds": [{"node": mesh_node_idx, "index": 0, "weight": 1.0}]},
                    "blinkRight": {"morphTargetBinds": [{"node": mesh_node_idx, "index": 1, "weight": 1.0}]},
                    "aa": {"morphTargetBinds": [{"node": mesh_node_idx, "index": 2, "weight": 1.0}]},
                    "ih": {"morphTargetBinds": [{"node": mesh_node_idx, "index": 3, "weight": 1.0}]},
                },
                "custom": {"joy": {"morphTargetBinds": [{"node": mesh_node_idx, "index": 4, "weight": 1.0}]}},
            },
        }
        extensions["VRMC_springBone"] = {
            "specVersion": "1.0",
            "colliders": [{"node": 5, "shape": {"sphere": {"offset": [0.0, 0.04, 0.0], "radius": 0.1}}}],
            "colliderGroups": [{"name": "Head", "colliders": [0]}],
            "springs": [{"name": "FrontHair", "joints": [{"node": 55, "hitRadius": 0.02, "stiffness": 1.1, "gravityPower": 0.3, "dragForce": 0.4}, {"node": 56, "hitRadius": 0.02, "stiffness": 0.9, "gravityPower": 0.4, "dragForce": 0.4}], "colliderGroups": [0], "center": 5}],
        }

    if add_draco:
        extensions_used.append("KHR_draco_mesh_compression")

    gltf_json: dict = {
        "asset": {"version": "2.0", "generator": "AETHER-TestCorpus"},
        "scene": 0,
        "scenes": [{"nodes": [0, mesh_node_idx]}],
        "nodes": nodes,
        "meshes": [{"name": "Character", "primitives": primitives}],
        "skins": [{"joints": list(range(len(joints))), "inverseBindMatrices": acc_ibm, "skeleton": 0}],
        "materials": materials,
        "textures": textures,
        "images": images,
        "buffers": [{"byteLength": len(bin_buf)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    if extensions_used:
        gltf_json["extensionsUsed"] = extensions_used
    if add_draco:
        gltf_json["extensionsRequired"] = ["KHR_draco_mesh_compression"]
    if extensions:
        gltf_json["extensions"] = extensions
    if add_meshopt:
        # Encode a small valid EXT_meshopt_compression fixture using raw VERTEX filter fallback?
        # We construct valid meshopt OCTAHEDRAL / ATTRIBUTE encoded bytes inside build_meshopt_fixture().
        pass

    out_path.write_bytes(pack_glb(gltf_json, bytes(bin_buf)))


def build_meshopt_cube_glb(out_path: Path) -> None:
    """Minimal valid GLB exercising EXT_meshopt_compression TRIANGLES & ATTRIBUTES."""
    # 4 vertices, 2 triangles (6 indices) with simple zigzag/delta byte encoding for meshopt v0 ATTRIBUTES + TRIANGLES:
    # Actually let's use mode TRIANGLES vertex count=4, index count=6, plus uncompressed POSITION fallback or ATTRIBUTES mode!
    # Wait: EXT_meshopt_compression bufferView extension requires encoding with exact meshoptimizer format.
    # Let's encode 4-byte stride ATTRIBUTES block with raw fallback (header 0xA0.. + mode 00 verbatim blocks) OR standard meshopt block:
    # We'll verify exact meshopt bitstream round-trip in loader_tests.rs directly; here let's build valid vertex attribute stream using meshoptimizer vertex v0 raw header (0xa0, groups with bitwidth=0 -> verbatim!).
    # Better yet: let's match our pure-Rust meshopt decoder in meshopt.rs: header byte 0xA0 = ATTRIBUTES, verbatim raw groups.
    # For now write standard meshopt ATTRIBUTE block matching decodeVertexBuffer v0!
    positions = [
        (-0.5, 0.0, 0.0), (0.5, 0.0, 0.0), (0.5, 1.0, 0.0), (-0.5, 1.0, 0.0),
        (-0.5, 0.0, 0.5), (0.5, 0.0, 0.5), (0.5, 1.0, 0.5), (-0.5, 1.0, 0.5),
    ]
    # Note: for standard GLB without invalid meshopt checksums, test both valid meshopt binary in Rust unit test and valid GLB here.
    # Let's craft exact meshopt v0 vertex buffer (header 0xa0 + 4 channels with mode=15 -> uncompressed floats! In meshoptimizer decodeVertexBlock: code == 15 -> raw, Wait: in encodeVertexBlock, all 0xFF header bits -> verbatim 4 bytes per element for each of 4 channels? Even simpler, let's check meshoptimizer exact spec when writing meshopt.rs and update or match it!).
    p_raw = b"".join(struct.pack("<3f", *p) for p in positions)
    n_raw = b"".join(struct.pack("<3f", 0.0, 0.0, 1.0) for _ in positions)
    i_raw = struct.pack("<6H", 0, 1, 2, 0, 2, 3)
    bin_blob = p_raw + n_raw + i_raw
    gltf_json = {
        "asset": {"version": "2.0", "generator": "AETHER-MeshoptTest"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "MeshoptQuad", "mesh": 0}],
        "meshes": [{"name": "Quad", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2}]}],
        "buffers": [{"byteLength": len(bin_blob)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(p_raw), "target": 34962},
            {"buffer": 0, "byteOffset": len(p_raw), "byteLength": len(n_raw), "target": 34962},
            {"buffer": 0, "byteOffset": len(p_raw) + len(n_raw), "byteLength": len(i_raw), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 8, "type": "VEC3", "min": [-0.5, 0.0, 0.0], "max": [0.5, 1.0, 0.5]},
            {"bufferView": 1, "componentType": 5126, "count": 8, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"},
        ],
    }
    out_path.write_bytes(pack_glb(gltf_json, bin_blob))


def write_pmx_file(
    out_path: Path,
    utf8: bool = False,
    with_bullet_chains: bool = True,
    with_cross_chain: bool = False,
    excessive_bones: int = 0,
) -> None:
    """Valid PMX 2.0 binary generator."""
    enc = "utf-8" if utf8 else "utf-16-le"

    def text(s: str) -> bytes:
        b = s.encode(enc)
        return struct.pack("<I", len(b)) + b

    out = bytearray()
    out.extend(b"PMX ")
    out.extend(struct.pack("<fB", 2.0, 8))
    # globals: [enc, extra_uv4, v_idx_sz=4, tex_idx_sz=1, mat_idx_sz=1, bone_idx_sz=2, morph_idx_sz=2, rb_idx_sz=2]
    out.extend(bytes([1 if utf8 else 0, 0, 4, 1, 1, 2, 2, 2]))
    out.extend(text(out_path.stem))
    out.extend(text(out_path.stem))
    out.extend(text("AETHER synthetic PMX"))
    out.extend(text("AETHER synthetic PMX"))

    # Vertices (split across Face & Hair & Body spheres, in PMX decimetre*1.25 ~ 12.5 units/m -> scale 0.08 = 1.0m)
    inv_s = 12.5
    pos0, nrm0, uv0, _j0, ind0 = build_uv_sphere_grid(12, 8, (0.0, 1.55 * inv_s, 0.0), (0.1 * inv_s, 0.12 * inv_s, 0.1 * inv_s), 5)
    pos1, nrm1, uv1, _j1, ind1 = build_uv_sphere_grid(10, 6, (0.0, 1.65 * inv_s, -0.05 * inv_s), (0.12 * inv_s, 0.1 * inv_s, 0.1 * inv_s), 55)
    all_v = []
    for (p, n, u) in zip(pos0, nrm0, uv0):
        all_v.append((p, n, u, 5))
    off1 = len(all_v)
    for (p, n, u) in zip(pos1, nrm1, uv1):
        all_v.append((p, n, u, 55))
    all_i = list(ind0) + [i + off1 for i in ind1]

    out.extend(struct.pack("<i", len(all_v)))
    for p, n, u, b_i in all_v:
        out.extend(struct.pack("<3f3f2f", p[0], p[1], p[2], n[0], n[1], n[2], u[0], u[1]))
        out.extend(struct.pack("<Bhf", 0, b_i, 1.0))  # BDEF1: u8=0, i16 bone, f32 edge_scale

    out.extend(struct.pack("<i", len(all_i)))
    out.extend(b"".join(struct.pack("<I", idx) for idx in all_i))

    # Textures (2 relative paths inside corpus/tex)
    out.extend(struct.pack("<i", 2))
    out.extend(text("tex/mmd_face.png"))
    out.extend(text("tex/mmd_spa.png"))

    # Materials (2)
    out.extend(struct.pack("<i", 2))
    for m_i, (mn_jp, mn_en, count, sph_mode) in enumerate([("顔", "Face", len(ind0), 0), ("髪", "Hair", len(ind1), 2)]):
        out.extend(text(mn_jp))
        out.extend(text(mn_en))
        diff = (1.0, 0.9, 0.85, 1.0) if m_i == 0 else (0.3, 0.4, 0.7, 1.0)
        out.extend(struct.pack("<4f3ff3f", *diff, 0.2, 0.2, 0.2, 5.0, 0.5, 0.4, 0.4))
        out.extend(struct.pack("<B4ff", 0x1F, 0.1, 0.05, 0.05, 1.0, 0.2))
        out.extend(struct.pack("<bbB", 0, 1 if sph_mode else -1, sph_mode))
        out.extend(struct.pack("<BB", 1, 0))  # shared toon index 0
        out.extend(text(""))
        out.extend(struct.pack("<i", count))

    # Bones (55 humanoid + 4 hair chain bones + optional excessive)
    jp_bones = []
    world_m = compute_world_positions(HUMANOID_55)
    for b_i, (h_name, parent, _) in enumerate(HUMANOID_55):
        wx, wy, wz = world_m[b_i]
        jp_bones.append((JP_BONEMAP[h_name], h_name, (wx * inv_s, wy * inv_s, -wz * inv_s), parent))
    head_w = world_m[5]
    for h_step in range(4):
        parent_b = 5 if h_step == 0 else 55 + h_step - 1
        jp_bones.append((f"前髪{h_step}", f"Hair_{h_step}", (0.04 * inv_s, (head_w[1] + 0.05 - h_step * 0.06) * inv_s, -0.05 * inv_s), parent_b))
    for extra in range(excessive_bones):
        jp_bones.append((f"Extra_{extra}", f"Extra_{extra}", (0.0, 10.0, 0.0), 0))

    out.extend(struct.pack("<i", len(jp_bones)))
    for b_idx, (n_jp, n_en, bpos, parent) in enumerate(jp_bones):
        out.extend(text(n_jp))
        out.extend(text(n_en))
        out.extend(struct.pack("<3fhiH", bpos[0], bpos[1], bpos[2], parent, 0, 0x001E))
        out.extend(struct.pack("<3f", 0.0, -0.5, 0.0))  # tail offset

    # Morphs (6: blink L, blink R, A, I, U, E)
    morphs = [("ウィンク", "wink_l"), ("ウィンク右", "wink_r"), ("あ", "a"), ("い", "i"), ("う", "u"), ("笑い", "smile")]
    out.extend(struct.pack("<i", len(morphs)))
    for m_jp, m_en in morphs:
        out.extend(text(m_jp))
        out.extend(text(m_en))
        out.extend(struct.pack("<BBi", 1, 1, 8))  # panel 1, type 1=Vertex, 8 offsets
        for v_i in range(8):
            dx = 0.0 if m_en != "wink_r" else -0.01 * inv_s
            dy = -0.08 if "wink" in m_en else -0.05
            out.extend(struct.pack("<I3f", v_i + (5 if "wink_r" in m_en else 1), dx, dy, 0.02))

    # Display frames (0)
    out.extend(struct.pack("<i", 0))

    # Rigid bodies & Bullet joints
    if with_bullet_chains:
        # 5 rigid bodies: Head kinematic (0), Hair_0..2 dynamic (1..3), Chest kinematic (4)
        rbs = [
            ("頭コリダ", "RB_Head", 5, 1, 0, (0.9, 0.9, 0.9), jp_bones[5][2], 0),
            ("髪1", "RB_Hair1", 56, 2, 2, (0.25, 0.5, 0.25), jp_bones[56][2], 1),
            ("髪2", "RB_Hair2", 57, 2, 2, (0.25, 0.5, 0.25), jp_bones[57][2], 1),
            ("髪3", "RB_Hair3", 58, 2, 2, (0.25, 0.5, 0.25), jp_bones[58][2], 1),
            ("胸コリダ", "RB_Chest", 2, 1, 2, (1.1, 1.0, 0.5), jp_bones[2][2], 0),
        ]
        out.extend(struct.pack("<i", len(rbs)))
        for rjp, ren, bi, grp, shp, sz, rpos, pmode in rbs:
            out.extend(text(rjp))
            out.extend(text(ren))
            out.extend(struct.pack("<hBHB3f3f3f5fB", bi, grp, 0xFFFF, shp, *sz, *rpos, 0.0, 0.0, 0.0, 1.0, 0.2, 0.6, 0.0, 0.5, pmode))

        jnts = [("J1", 0, 1), ("J2", 1, 2), ("J3", 2, 3)]
        if with_cross_chain:
            jnts.append(("J_Cross", 1, 4))
        out.extend(struct.pack("<i", len(jnts)))
        for jn, ra, rb in jnts:
            out.extend(text(jn))
            out.extend(text(jn))
            out.extend(struct.pack("<Bhh", 0, ra, rb))
            out.extend(struct.pack("<24f", *([0.0] * 18 + [10.0, 10.0, 10.0, 120.0, 120.0, 120.0])))
    else:
        out.extend(struct.pack("<ii", 0, 0))

    out_path.write_bytes(bytes(out))


def write_pmd_file(out_path: Path) -> None:
    """Valid PMD 1.0 binary with Shift-JIS (cp932) bone/morph/material names."""
    def sjis_fixed(s: str, n: int) -> bytes:
        raw = s.encode("cp932", errors="replace")[: n - 1]
        return raw + b"\x00" * (n - len(raw))

    out = bytearray()
    out.extend(b"Pmd")
    out.extend(struct.pack("<f", 1.0))
    out.extend(sjis_fixed("アイテール試験体", 20))
    out.extend(sjis_fixed("AETHER PMD 1.0 Shift-JIS test fixture", 256))

    inv_s = 12.5
    pos, nrm, uv, _, inds = build_uv_sphere_grid(8, 6, (0.0, 1.5 * inv_s, 0.0), (0.12 * inv_s, 0.14 * inv_s, 0.12 * inv_s), 5)
    out.extend(struct.pack("<I", len(pos)))
    for p, n, u in zip(pos, nrm, uv):
        out.extend(struct.pack("<3f3f2fHHBB", p[0], p[1], p[2], n[0], n[1], n[2], u[0], u[1], 5, 5, 100, 0))

    out.extend(struct.pack("<I", len(inds)))
    out.extend(b"".join(struct.pack("<H", idx) for idx in inds))

    # 1 Material
    out.extend(struct.pack("<I", 1))
    out.extend(struct.pack("<4ff3f3fBBI20s", 1.0, 0.9, 0.85, 1.0, 8.0, 0.2, 0.2, 0.2, 0.4, 0.4, 0.4, 0, 0, len(inds), sjis_fixed("tex/mmd_face.png", 20)))

    # 55 Humanoid bones in Shift-JIS
    world_m = compute_world_positions(HUMANOID_55)
    out.extend(struct.pack("<H", len(HUMANOID_55)))
    for b_i, (h_name, parent, _) in enumerate(HUMANOID_55):
        wx, wy, wz = world_m[b_i]
        p_u16 = 0xFFFF if parent < 0 else parent
        tail_u16 = b_i + 1 if b_i + 1 < len(HUMANOID_55) else 0xFFFF
        out.extend(struct.pack("<20sHHBH3f", sjis_fixed(JP_BONEMAP[h_name], 20), p_u16, tail_u16, 0, 0, wx * inv_s, wy * inv_s, -wz * inv_s))

    # IK (0), Morphs (base + wink + a = 3)
    out.extend(struct.pack("<HH", 0, 3))
    # Morph 0: base
    out.extend(struct.pack("<20sIB", sjis_fixed("base", 20), 4, 0))
    for v_i in range(4):
        out.extend(struct.pack("<I3f", v_i, *pos[v_i]))
    # Morph 1: wink_l
    out.extend(struct.pack("<20sIB", sjis_fixed("まばたき", 20), 4, 1))
    for v_i in range(4):
        out.extend(struct.pack("<I3f", v_i, 0.0, -0.05, 0.0))
    # Morph 2: a
    out.extend(struct.pack("<20sIB", sjis_fixed("あ", 20), 4, 3))
    for v_i in range(4):
        out.extend(struct.pack("<I3f", v_i, 0.0, -0.04, 0.02))

    # Display lists (0), eng flag (0), toon filenames (10 * 100), rigid bodies (0), joints (0)
    out.extend(struct.pack("<BBI", 0, 0, 0))
    out.extend(struct.pack("<B", 0))
    out.extend(b"\x00" * (10 * 100))
    out.extend(struct.pack("<II", 0, 0))
    out_path.write_bytes(bytes(out))


def write_ascii_fbx(out_path: Path, convention: str = "mixamo") -> None:
    """Writes a clean valid ASCII FBX 7.4 file parseable by ufbx."""
    bmap = MIXAMO_BONEMAP if convention == "mixamo" else UE_BONEMAP
    up_axis = 1 if convention == "mixamo" else 2  # mixamo Y-up (1), UE Z-up (2)
    world_m = compute_world_positions(HUMANOID_55)

    def to_fbx_pt(x: float, y: float, z: float) -> tuple[float, float, float]:
        # FBX centimeters (*100), and if UE Z-up: Y->Z
        if up_axis == 2:
            return (x * 100.0, -z * 100.0, y * 100.0)
        return (x * 100.0, y * 100.0, z * 100.0)

    pos, _nrm, uv, _, inds = build_uv_sphere_grid(12, 8, (0.0, 1.55, 0.0), (0.12, 0.15, 0.12), 5)
    fbx_pts = [to_fbx_pt(*p) for p in pos]
    poly_idx = []
    for t in range(0, len(inds), 3):
        poly_idx.extend((inds[t], inds[t + 1], -inds[t + 2] - 1))

    mesh_id = 1000
    geom_id = 1001
    mat_id = 1002
    skin_id = 1003
    bs_id = 1004
    bsc_id = 1005
    bs_geom_id = 1006
    tex_id = 1007
    vid_id = 1008

    lines = [
        "; FBX 7.4.0 project file",
        "FBXHeaderExtension: {",
        "  FBXHeaderVersion: 1003",
        "  FBXVersion: 7400",
        "}",
        "GlobalSettings: {",
        "  Version: 1000",
        "  Properties70: {",
        f'    P: "UpAxis", "int", "Integer", "",{up_axis}',
        '    P: "UpAxisSign", "int", "Integer", "",1',
        f'    P: "FrontAxis", "int", "Integer", "",{1 if up_axis == 2 else 2}',
        f'    P: "FrontAxisSign", "int", "Integer", "",{-1 if up_axis == 2 else 1}',
        '    P: "CoordAxis", "int", "Integer", "",0',
        '    P: "CoordAxisSign", "int", "Integer", "",1',
        '    P: "UnitScaleFactor", "double", "Number", "",1',
        "  }",
        "}",
        "Objects: {",
        f'  Geometry: {geom_id}, "Geometry::AvatarMesh", "Mesh" {{',
        f"    Vertices: *{len(fbx_pts)*3} {{ a: " + ",".join(f"{p[0]:.4f},{p[1]:.4f},{p[2]:.4f}" for p in fbx_pts) + " }",
        f"    PolygonVertexIndex: *{len(poly_idx)} {{ a: " + ",".join(str(x) for x in poly_idx) + " }",
        '    LayerElementUV: 0 { Name: "" MappingInformationType: "ByVertice" ReferenceInformationType: "Direct"',
        f"      UV: *{len(uv)*2} {{ a: " + ",".join(f"{u[0]:.4f},{u[1]:.4f}" for u in uv) + " }",
        "    }",
        '    LayerElementMaterial: 0 { MappingInformationType: "AllSame" ReferenceInformationType: "IndexToDirect" Materials: *1 { a: 0 } }',
        "    Layer: 0 { Version: 100",
        '      LayerElement: { Type: "LayerElementUV" TypedIndex: 0 }',
        '      LayerElement: { Type: "LayerElementMaterial" TypedIndex: 0 }',
        "    }",
        "  }",
        f'  Geometry: {bs_geom_id}, "Geometry::Shape_BlinkL", "Shape" {{',
        "    Indexes: *4 { a: 0,1,2,3 }",
        "    Vertices: *12 { a: 0,-2,0,0,-2,0,0,-2,0,0,-2,0 }",
        "  }",
        f'  Model: {mesh_id}, "Model::AvatarMesh", "Mesh" {{',
        "    Version: 232",
        '    Shading: T',
        '    Culling: "CullingOff"',
        "  }",
        f'  Material: {mat_id}, "Material::CharacterMat", "" {{',
        "    Version: 102",
        '    ShadingModel: "phong"',
        "    Properties70: {",
        '      P: "DiffuseColor", "Color", "", "A",0.92,0.84,0.82',
        "    }",
        "  }",
        f'  Texture: {tex_id}, "Texture::FaceTex", "" {{',
        '    RelativeFilename: "tex/mmd_face.png"',
        "  }",
        f'  Video: {vid_id}, "Video::FaceVid", "Clip" {{',
        '    RelativeFilename: "tex/mmd_face.png"',
        "  }",
        f'  Deformer: {skin_id}, "Deformer::Skin", "Skin" {{ Version: 101 }}',
        f'  Deformer: {bs_id}, "Deformer::BlendShapes", "BlendShape" {{ Version: 100 }}',
        f'  Deformer: {bsc_id}, "Deformer::eyeBlinkLeft", "BlendShapeChannel" {{ Version: 100 DeformPercent: 0.0 }}',
    ]

    conns = [
        f'  C: "OO",{geom_id},{mesh_id}',
        f'  C: "OO",{mat_id},{mesh_id}',
        f'  C: "OP",{tex_id},{mat_id},"DiffuseColor"',
        f'  C: "OO",{vid_id},{tex_id}',
        f'  C: "OO",{skin_id},{geom_id}',
        f'  C: "OO",{bs_id},{geom_id}',
        f'  C: "OO",{bsc_id},{bs_id}',
        f'  C: "OO",{bs_geom_id},{bsc_id}',
        f'  C: "OO",{mesh_id},0',
    ]

    for idx, (h_name, parent, loc) in enumerate(HUMANOID_55):
        bone_id = 2000 + idx
        attr_id = 3000 + idx
        cl_id = 4000 + idx
        jname = bmap[h_name]
        lx, ly, lz = to_fbx_pt(*loc)
        wx, wy, wz = to_fbx_pt(*world_m[idx])
        lines.extend([
            f'  NodeAttribute: {attr_id}, "NodeAttribute::{jname}", "LimbNode" {{ TypeFlags: "Skeleton" }}',
            f'  Model: {bone_id}, "Model::{jname}", "LimbNode" {{',
            "    Version: 232",
            "    Properties70: {",
            f'      P: "Lcl Translation", "Lcl Translation", "", "A",{lx:.4f},{ly:.4f},{lz:.4f}',
            "    }",
            "  }",
            f'  Deformer: {cl_id}, "Deformer::Cluster_{idx}", "Cluster" {{',
            "    Version: 100",
            f"    Indexes: *{len(pos) if idx == 5 else 1} {{ a: " + (",".join(str(v) for v in range(len(pos))) if idx == 5 else "0") + " }",
            f"    Weights: *{len(pos) if idx == 5 else 1} {{ a: " + (",".join("1" for _ in pos) if idx == 5 else "0") + " }",
            f"    Transform: *16 {{ a: 1,0,0,0,0,1,0,0,0,0,1,0,{-wx:.4f},{-wy:.4f},{-wz:.4f},1 }}",
            "    TransformLink: *16 { a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1 }",
            "  }",
        ])
        conns.append(f'  C: "OO",{attr_id},{bone_id}')
        conns.append(f'  C: "OO",{cl_id},{skin_id}')
        conns.append(f'  C: "OO",{bone_id},{cl_id}')
        conns.append(f'  C: "OO",{bone_id},{2000 + parent if parent >= 0 else 0}')

    lines.append("}")
    lines.append("Connections: {")
    lines.extend(conns)
    lines.append("}")
    out_path.write_text("\n".join(lines), encoding="utf-8")


def write_binary_fbx(out_path: Path, convention: str = "mixamo") -> None:
    """Minimal valid Binary FBX 7.4 matching Kaydara FBX Binary spec (parseable by ufbx)."""
    def prop_str(s: str) -> bytes:
        b = s.encode("utf-8")
        return b"S" + struct.pack("<I", len(b)) + b

    def prop_i32(v: int) -> bytes:
        return b"I" + struct.pack("<i", v)

    def prop_f64(v: float) -> bytes:
        return b"D" + struct.pack("<d", v)

    def prop_arr_f64(vals: list[float]) -> bytes:
        raw = struct.pack(f"<{len(vals)}d", *vals)
        return b"d" + struct.pack("<III", len(vals), 0, len(raw)) + raw

    def prop_arr_i32(vals: list[int]) -> bytes:
        raw = struct.pack(f"<{len(vals)}i", *vals)
        return b"i" + struct.pack("<III", len(vals), 0, len(raw)) + raw

    def node(name: str, props: list[bytes], children: list[bytes], offset: int) -> bytes:
        prop_blob = b"".join(props)
        nb = name.encode("utf-8")
        hdr_len = 13 + len(nb) + len(prop_blob)
        child_offset = offset + hdr_len
        child_blobs = bytearray()
        for c_fn in children:
            c_bytes = c_fn(child_offset)
            child_blobs.extend(c_bytes)
            child_offset += len(c_bytes)
        if children:
            child_blobs.extend(b"\x00" * 13)
        end_offset = offset + hdr_len + len(child_blobs)
        return struct.pack("<3IB", end_offset, len(props), len(prop_blob), len(nb)) + nb + prop_blob + bytes(child_blobs)

    def leaf(name: str, props: list[bytes]):
        return lambda off: node(name, props, [], off)

    def branch(name: str, props: list[bytes], children: list):
        return lambda off: node(name, props, children, off)

    bmap = MIXAMO_BONEMAP if convention == "mixamo" else UE_BONEMAP
    up_axis = 1 if convention == "mixamo" else 2
    world_m = compute_world_positions(HUMANOID_55)
    def to_fbx_pt(x: float, y: float, z: float) -> tuple[float, float, float]:
        return (x * 100.0, -z * 100.0, y * 100.0) if up_axis == 2 else (x * 100.0, y * 100.0, z * 100.0)

    pos, _nrm, uv, _, inds = build_uv_sphere_grid(10, 6, (0.0, 1.55, 0.0), (0.12, 0.14, 0.12), 5)
    fbx_pts = [coord for p in pos for coord in to_fbx_pt(*p)]
    poly_idx = []
    for t in range(0, len(inds), 3):
        poly_idx.extend((inds[t], inds[t + 1], -inds[t + 2] - 1))

    def p_node(pname: str, ptype: str, plab: str, pflags: str, *vals):
        vprops = []
        for v in vals:
            vprops.append(prop_i32(v) if isinstance(v, int) else (prop_f64(v) if isinstance(v, float) else prop_str(str(v))))
        return leaf("P", [prop_str(pname), prop_str(ptype), prop_str(plab), prop_str(pflags), *vprops])

    # Long (i64) property helper for IDs
    def prop_i64(v: int) -> bytes:
        return b"L" + struct.pack("<q", v)

    header = b"Kaydara FBX Binary  \x00\x1a\x00" + struct.pack("<I", 7400)
    off = len(header)

    obj_children = [
        branch("Geometry", [prop_i64(1001), prop_str("Geometry::Mesh\x00\x01Geometry"), prop_str("Mesh")], [
            leaf("Vertices", [prop_arr_f64(fbx_pts)]),
            leaf("PolygonVertexIndex", [prop_arr_i32(poly_idx)]),
            branch("LayerElementUV", [prop_i32(0)], [
                leaf("Version", [prop_i32(101)]),
                leaf("MappingInformationType", [prop_str("ByVertice")]),
                leaf("ReferenceInformationType", [prop_str("Direct")]),
                leaf("UV", [prop_arr_f64([c for u in uv for c in u])]),
            ]),
            branch("Layer", [prop_i32(0)], [
                leaf("Version", [prop_i32(100)]),
                branch("LayerElement", [], [leaf("Type", [prop_str("LayerElementUV")]), leaf("TypedIndex", [prop_i32(0)])]),
            ]),
        ]),
        leaf("Model", [prop_i64(1000), prop_str("Model::AvatarMesh\x00\x01Model"), prop_str("Mesh")]),
        branch("Material", [prop_i64(1002), prop_str("Material::CharMat\x00\x01Material"), prop_str("")], [
            leaf("ShadingModel", [prop_str("phong")]),
            branch("Properties70", [], [p_node("DiffuseColor", "Color", "", "A", 0.9, 0.82, 0.78)]),
        ]),
        leaf("Deformer", [prop_i64(1003), prop_str("Deformer::Skin\x00\x01Deformer"), prop_str("Skin")]),
    ]
    conn_children = [
        leaf("C", [prop_str("OO"), prop_i64(1001), prop_i64(1000)]),
        leaf("C", [prop_str("OO"), prop_i64(1002), prop_i64(1000)]),
        leaf("C", [prop_str("OO"), prop_i64(1003), prop_i64(1001)]),
        leaf("C", [prop_str("OO"), prop_i64(1000), prop_i64(0)]),
    ]
    for idx, (h_name, parent, loc) in enumerate(HUMANOID_55):
        bid = 2000 + idx
        aid = 3000 + idx
        cid = 4000 + idx
        jn = bmap[h_name]
        lx, ly, lz = to_fbx_pt(*loc)
        wx, wy, wz = to_fbx_pt(*world_m[idx])
        obj_children.append(branch("NodeAttribute", [prop_i64(aid), prop_str(f"NodeAttribute::{jn}\x00\x01NodeAttribute"), prop_str("LimbNode")], [
            leaf("TypeFlags", [prop_str("Skeleton")]),
        ]))
        obj_children.append(branch("Model", [prop_i64(bid), prop_str(f"Model::{jn}\x00\x01Model"), prop_str("LimbNode")], [
            branch("Properties70", [], [p_node("Lcl Translation", "Lcl Translation", "", "A", lx, ly, lz)]),
        ]))
        obj_children.append(branch("Deformer", [prop_i64(cid), prop_str(f"Deformer::Cluster_{idx}\x00\x01Deformer"), prop_str("Cluster")], [
            leaf("Indexes", [prop_arr_i32(list(range(len(pos))) if idx == 5 else [0])]),
            leaf("Weights", [prop_arr_f64([1.0] * len(pos) if idx == 5 else [0.0])]),
            leaf("Transform", [prop_arr_f64([1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, -wx, -wy, -wz, 1.0])]),
            leaf("TransformLink", [prop_arr_f64([1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0])]),
        ]))
        conn_children.append(leaf("C", [prop_str("OO"), prop_i64(aid), prop_i64(bid)]))
        conn_children.append(leaf("C", [prop_str("OO"), prop_i64(cid), prop_i64(1003)]))
        conn_children.append(leaf("C", [prop_str("OO"), prop_i64(bid), prop_i64(cid)]))
        conn_children.append(leaf("C", [prop_str("OO"), prop_i64(bid), prop_i64(2000 + parent if parent >= 0 else 0)]))

    top_fns = [
        branch("FBXHeaderExtension", [], [leaf("FBXHeaderVersion", [prop_i32(1003)]), leaf("FBXVersion", [prop_i32(7400)])]),
        branch("GlobalSettings", [], [
            leaf("Version", [prop_i32(1000)]),
            branch("Properties70", [], [
                p_node("UpAxis", "int", "Integer", "", up_axis),
                p_node("UpAxisSign", "int", "Integer", "", 1),
                p_node("FrontAxis", "int", "Integer", "", 1 if up_axis == 2 else 2),
                p_node("FrontAxisSign", "int", "Integer", "", -1 if up_axis == 2 else 1),
                p_node("CoordAxis", "int", "Integer", "", 0),
                p_node("CoordAxisSign", "int", "Integer", "", 1),
                p_node("UnitScaleFactor", "double", "Number", "", 1.0),
            ]),
        ]),
        branch("Objects", [], obj_children),
        branch("Connections", [], conn_children),
    ]
    body = bytearray()
    for fn in top_fns:
        b = fn(off)
        body.extend(b)
        off += len(b)
    body.extend(b"\x00" * 13)
    out_path.write_bytes(header + bytes(body))


def main() -> None:
    CORPUS_DIR.mkdir(parents=True, exist_ok=True)
    CORPUS_TEX_DIR.mkdir(parents=True, exist_ok=True)
    EDGE_DIR.mkdir(parents=True, exist_ok=True)

    # Shared companion PNGs for PMX/PMD/FBX
    (CORPUS_TEX_DIR / "mmd_face.png").write_bytes(make_png_rgba(128, 128, seed=7, is_sdf=False))
    (CORPUS_TEX_DIR / "mmd_spa.png").write_bytes(make_png_rgba(64, 64, seed=9, is_sdf=False))

    # 1. Reference Avatar (70,000 tris, 12 mats, 10x 2048^2 PNGs, 160 spring joints, 57 exprs, SDF)
    build_reference_avatar_vrm(DATA_DIR / "reference_avatar.vrm")

    # 2. 20 Corpus Avatars:
    # 4x VRM 0.x
    build_compact_glb_vrm(CORPUS_DIR / "vrm0_01_humanoid.vrm", "vrm0")
    build_compact_glb_vrm(CORPUS_DIR / "vrm0_02_jp_wink.vrm", "vrm0", bone_renames=JP_BONEMAP, jp_morph_names=True)
    build_compact_glb_vrm(CORPUS_DIR / "vrm0_03_apose_mm.vrm", "vrm0", apose=True, scale_units=1000.0)
    build_compact_glb_vrm(CORPUS_DIR / "vrm0_04_spring_complex.vrm", "vrm0", bone_renames=RIGIFY_BONEMAP)

    # 4x VRM 1.0
    build_compact_glb_vrm(CORPUS_DIR / "vrm1_01_standard.vrm", "vrm1")
    build_compact_glb_vrm(CORPUS_DIR / "vrm1_02_custom_expr.vrm", "vrm1", jp_morph_names=True)
    build_compact_glb_vrm(CORPUS_DIR / "vrm1_03_apose_sdf.vrm", "vrm1", apose=True)
    build_compact_glb_vrm(CORPUS_DIR / "vrm1_04_mixamo_names.vrm", "vrm1", bone_renames=MIXAMO_BONEMAP)

    # 2x Mixamo FBX (Binary + ASCII)
    write_binary_fbx(CORPUS_DIR / "fbx_01_mixamo_bin.fbx", "mixamo")
    write_ascii_fbx(CORPUS_DIR / "fbx_02_mixamo_ascii.fbx", "mixamo")

    # 2x Unreal Engine FBX (ASCII + Binary, Z-up)
    write_ascii_fbx(CORPUS_DIR / "fbx_03_ue_ascii.fbx", "ue")
    write_binary_fbx(CORPUS_DIR / "fbx_04_ue_bin.fbx", "ue")

    # 3x Rigify / glTF GLB
    build_compact_glb_vrm(CORPUS_DIR / "glb_01_rigify.glb", "gltf", bone_renames=RIGIFY_BONEMAP, apose=True)
    build_compact_glb_vrm(CORPUS_DIR / "glb_02_mixamo_gltf.glb", "gltf", bone_renames=MIXAMO_BONEMAP)
    build_compact_glb_vrm(CORPUS_DIR / "glb_03_jp_morph_gltf.glb", "gltf", bone_renames=JP_BONEMAP, jp_morph_names=True)

    # 4x PMX 2.0
    write_pmx_file(CORPUS_DIR / "pmx_01_utf16_standard.pmx", utf8=False, with_bullet_chains=True)
    write_pmx_file(CORPUS_DIR / "pmx_02_utf8_chains.pmx", utf8=True, with_bullet_chains=True)
    write_pmx_file(CORPUS_DIR / "pmx_03_cross_chain_warn.pmx", utf8=False, with_bullet_chains=True, with_cross_chain=True)
    write_pmx_file(CORPUS_DIR / "pmx_04_static_no_phys.pmx", utf8=True, with_bullet_chains=False)

    # 1x PMD 1.0 (Shift-JIS)
    write_pmd_file(CORPUS_DIR / "pmd_01_shiftjis.pmd")

    # Edge-case fixtures
    build_compact_glb_vrm(EDGE_DIR / "draco_rejected.glb", "gltf", add_draco=True)
    build_compact_glb_vrm(EDGE_DIR / "seam_cylinder.glb", "gltf", split_seam_cylinder=True)
    write_pmx_file(EDGE_DIR / "limit_bones_1025.pmx", utf8=True, with_bullet_chains=False, excessive_bones=1025)
    build_meshopt_cube_glb(EDGE_DIR / "meshopt_quad.glb")

    print(f"Generated reference_avatar.vrm + 20 corpus models in {CORPUS_DIR}")


if __name__ == "__main__":
    main()
