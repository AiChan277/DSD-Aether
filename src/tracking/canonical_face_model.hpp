#pragma once

#include <array>
#include <cstdint>

namespace aether::tracking {

#include "tracking/canonical_face_data.inc"

inline constexpr uint32_t kM3SubsetCount = 146U;

/**
 * Exact 146 landmark subset required by `M3_arkit52_blendshapes` (`AETHER-TS-001` §8.3, ADR-0008),
 * matching official Google MediaPipe `face_blendshapes_graph.cc` (`kLandmarksSubsetIdxs`).
 */
inline constexpr std::array<uint16_t, kM3SubsetCount> kM3LandmarkSubsetIndices = {{
    0,   1,   4,   5,   6,   7,   8,   10,  13,  14,  17,  21,  33,  37,  39,  40,  46,  52,  53,  54,  55,
    58,  61,  63,  65,  66,  67,  70,  78,  80,  81,  82,  84,  87,  88,  91,  93,  95,  103, 105, 107, 109,
    127, 132, 133, 136, 144, 145, 146, 148, 149, 150, 152, 153, 154, 155, 157, 158, 159, 160, 161, 162, 163,
    168, 172, 173, 176, 178, 181, 185, 191, 195, 197, 234, 246, 249, 251, 263, 267, 269, 270, 276, 282, 283,
    284, 285, 288, 291, 293, 295, 296, 297, 300, 308, 310, 311, 312, 314, 317, 318, 321, 323, 324, 332, 334,
    336, 338, 356, 361, 362, 365, 373, 374, 375, 377, 378, 379, 380, 381, 382, 384, 385, 386, 387, 388, 389,
    390, 397, 398, 400, 402, 405, 409, 415, 454, 466, 468, 469, 470, 471, 472, 473, 474, 475, 476, 477,
}};

/**
 * Maps `AETHER-TS-001` Appendix B blendshape slot (`0..51`) -> MediaPipe M3 output index (`0..51`, or -1 for
 * `tongueOut` which is derived from inner lip aperture/protrusion heuristics when unavailable from M3).
 *
 * MediaPipe M3 output order:
 *  0=_neutral, 1=browDownLeft, 2=browDownRight, 3=browInnerUp, 4=browOuterUpLeft, 5=browOuterUpRight,
 *  6=cheekPuff, 7=cheekSquintLeft, 8=cheekSquintRight, 9=eyeBlinkLeft, 10=eyeBlinkRight, 11=eyeLookDownLeft,
 *  12=eyeLookDownRight, 13=eyeLookInLeft, 14=eyeLookInRight, 15=eyeLookOutLeft, 16=eyeLookOutRight,
 *  17=eyeLookUpLeft, 18=eyeLookUpRight, 19=eyeSquintLeft, 20=eyeSquintRight, 21=eyeWideLeft, 22=eyeWideRight,
 *  23=jawForward, 24=jawLeft, 25=jawRight, 26=jawOpen, 27=mouthClose, 28=mouthDimpleLeft, 29=mouthDimpleRight,
 *  30=mouthFrownLeft, 31=mouthFrownRight, 32=mouthFunnel, 33=mouthLeft, 34=mouthRight, 35=mouthLowerDownLeft,
 *  36=mouthLowerDownRight, 37=mouthPressLeft, 38=mouthPressRight, 39=mouthPucker, 40=mouthRollLower,
 *  41=mouthRollUpper, 42=mouthShrugLower, 43=mouthShrugUpper, 44=mouthSmileLeft, 45=mouthSmileRight,
 *  46=mouthStretchLeft, 47=mouthStretchRight, 48=mouthUpperUpLeft, 49=mouthUpperUpRight,
 *  50=noseSneerLeft, 51=noseSneerRight.
 */
inline constexpr std::array<int8_t, 52> kAppendixBFromMediaPipeM3 = {{
    9,  // 0: eyeBlinkLeft
    11, // 1: eyeLookDownLeft
    13, // 2: eyeLookInLeft
    15, // 3: eyeLookOutLeft
    17, // 4: eyeLookUpLeft
    19, // 5: eyeSquintLeft
    21, // 6: eyeWideLeft
    10, // 7: eyeBlinkRight
    12, // 8: eyeLookDownRight
    14, // 9: eyeLookInRight
    16, // 10: eyeLookOutRight
    18, // 11: eyeLookUpRight
    20, // 12: eyeSquintRight
    22, // 13: eyeWideRight
    23, // 14: jawForward
    24, // 15: jawLeft
    25, // 16: jawRight
    26, // 17: jawOpen
    27, // 18: mouthClose
    32, // 19: mouthFunnel
    39, // 20: mouthPucker
    33, // 21: mouthLeft
    34, // 22: mouthRight
    44, // 23: mouthSmileLeft
    45, // 24: mouthSmileRight
    30, // 25: mouthFrownLeft
    31, // 26: mouthFrownRight
    28, // 27: mouthDimpleLeft
    29, // 28: mouthDimpleRight
    46, // 29: mouthStretchLeft
    47, // 30: mouthStretchRight
    40, // 31: mouthRollLower
    41, // 32: mouthRollUpper
    42, // 33: mouthShrugLower
    43, // 34: mouthShrugUpper
    37, // 35: mouthPressLeft
    38, // 36: mouthPressRight
    35, // 37: mouthLowerDownLeft
    36, // 38: mouthLowerDownRight
    48, // 39: mouthUpperUpLeft
    49, // 40: mouthUpperUpRight
    1,  // 41: browDownLeft
    2,  // 42: browDownRight
    3,  // 43: browInnerUp
    4,  // 44: browOuterUpLeft
    5,  // 45: browOuterUpRight
    6,  // 46: cheekPuff
    7,  // 47: cheekSquintLeft
    8,  // 48: cheekSquintRight
    50, // 49: noseSneerLeft
    51, // 50: noseSneerRight
    -1, // 51: tongueOut (not emitted by M3; kept 0.0f cleanly)
}};

} // namespace aether::tracking
