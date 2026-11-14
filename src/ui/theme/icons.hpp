#pragma once

namespace aether::ui::icons {

// UTF-8 Private Use Area (U+E001..U+E028) glyphs baked in assets/fonts/AetherIcons-Regular.ttf
// on a uniform 24-unit stroke grid (AETHER-TS-001 §12.3.5, ADR-0013).
inline constexpr const char* kSearch = "\xEE\x80\x81";        // U+E001
inline constexpr const char* kPlus = "\xEE\x80\x82";          // U+E002
inline constexpr const char* kEye = "\xEE\x80\x83";           // U+E003
inline constexpr const char* kEyeOff = "\xEE\x80\x84";        // U+E004
inline constexpr const char* kLock = "\xEE\x80\x85";          // U+E005
inline constexpr const char* kUnlock = "\xEE\x80\x86";        // U+E006
inline constexpr const char* kChevronRight = "\xEE\x80\x87";  // U+E007
inline constexpr const char* kChevronDown = "\xEE\x80\x88";   // U+E008
inline constexpr const char* kPlay = "\xEE\x80\x89";          // U+E009
inline constexpr const char* kPause = "\xEE\x80\x8A";         // U+E00A
inline constexpr const char* kSliders = "\xEE\x80\x8B";       // U+E00B
inline constexpr const char* kCamera = "\xEE\x80\x8C";        // U+E00C
inline constexpr const char* kSun = "\xEE\x80\x8D";           // U+E00D
inline constexpr const char* kCpu = "\xEE\x80\x8E";           // U+E00E
inline constexpr const char* kFolder = "\xEE\x80\x8F";        // U+E00F
inline constexpr const char* kSave = "\xEE\x80\x90";          // U+E010
inline constexpr const char* kUndo = "\xEE\x80\x91";          // U+E011
inline constexpr const char* kRedo = "\xEE\x80\x92";          // U+E012
inline constexpr const char* kSettings = "\xEE\x80\x93";      // U+E013
inline constexpr const char* kKeyboard = "\xEE\x80\x94";      // U+E014
inline constexpr const char* kCheck = "\xEE\x80\x95";         // U+E015
inline constexpr const char* kAlertTriangle = "\xEE\x80\x96"; // U+E016
inline constexpr const char* kErrorCircle = "\xEE\x80\x97";   // U+E017
inline constexpr const char* kRadioOn = "\xEE\x80\x98";       // U+E018
inline constexpr const char* kMaximize = "\xEE\x80\x99";      // U+E019
inline constexpr const char* kLayers = "\xEE\x80\x9A";        // U+E01A
inline constexpr const char* kBone = "\xEE\x80\x9B";          // U+E01B
inline constexpr const char* kBox = "\xEE\x80\x9C";           // U+E01C
inline constexpr const char* kRefresh = "\xEE\x80\x9D";       // U+E01D
inline constexpr const char* kMic = "\xEE\x80\x9E";           // U+E01E
inline constexpr const char* kNodes = "\xEE\x80\x9F";         // U+E01F
inline constexpr const char* kTerminal = "\xEE\x80\xA0";      // U+E020
inline constexpr const char* kClose = "\xEE\x80\xA1";         // U+E021
inline constexpr const char* kInfo = "\xEE\x80\xA2";          // U+E022
inline constexpr const char* kLink = "\xEE\x80\xA3";          // U+E023

// Domain-specific glyphs on matching 24-unit uniform stroke (§12.3.5):
inline constexpr const char* kDomainNpu = "\xEE\x80\xA4";      // U+E024 (NPU chip)
inline constexpr const char* kDomainSdf = "\xEE\x80\xA5";      // U+E025 (Face SDF sphere-split)
inline constexpr const char* kDomainBlas = "\xEE\x80\xA6";     // U+E026 (BVH/BLAS diamond hierarchy)
inline constexpr const char* kDomainSpout = "\xEE\x80\xA7";    // U+E027 (Shared stream frame)
inline constexpr const char* kDomainFlatness = "\xEE\x80\xA8"; // U+E028 (2.5D Z-compression planes)

inline constexpr unsigned int kIconRangeMin = 0xE001;
inline constexpr unsigned int kIconRangeMax = 0xE028;

} // namespace aether::ui::icons
