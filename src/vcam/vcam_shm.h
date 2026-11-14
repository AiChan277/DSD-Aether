#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AETHER_VCAM_MAGIC 0x4D435641u /* 'AVCM' */
#define AETHER_VCAM_VERSION 1u
#define AETHER_VCAM_SLOTS 3u
#define AETHER_VCAM_MAX_WIDTH 1920u
#define AETHER_VCAM_MAX_HEIGHT 1080u

#define AETHER_VCAM_FOURCC_ARGB32 0x41524742u /* BGRA / ARGB32 with alpha */
#define AETHER_VCAM_FOURCC_RGB32 0x32334252u  /* RGB32 */
#define AETHER_VCAM_FOURCC_NV12 0x3231564Eu   /* NV12 */

#define AETHER_VCAM_PIN_FILTER_ACTIVE (1u << 0)
#define AETHER_VCAM_PIN_RGB_CONNECTED (1u << 1)
#define AETHER_VCAM_PIN_NV12_CONNECTED (1u << 2)

// Header sits at offset 0 of Local\AetherVCam_v1 (4096 B page reserved before Slot 0)
#define AETHER_VCAM_HEADER_BYTES 4096u
// Each slot holds up to 1920x1080x4 BGRA (8,294,400 B, padded to 64 KiB -> 8,323,072 B)
// plus 1920x1080x3/2 NV12 (3,110,400 B), total rounded to 64 KiB alignment (11,468,800 B).
#define AETHER_VCAM_NV12_OFFSET_IN_SLOT (8323072u)
#define AETHER_VCAM_SLOT_STRIDE_BYTES (11468800u)
#define AETHER_VCAM_SECTION_BYTES (AETHER_VCAM_HEADER_BYTES + AETHER_VCAM_SLOTS * AETHER_VCAM_SLOT_STRIDE_BYTES)

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
typedef struct __declspec(align(64)) AetherVCamHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t active_fourcc;
    uint32_t fps_milli;
    volatile long write_seq; /* odd while updating slots/metadata, +2 per published frame */
    uint32_t latest_slot;
    volatile long active_pin_flags; /* AETHER_VCAM_PIN_* bitmask set by DirectShow filter */
    uint32_t slot_stride_bytes;
    uint32_t bgra_byte_size;
    uint32_t nv12_offset_in_slot;
    uint32_t bottom_up;
    uint32_t _pad0;
    uint64_t producer_qpc_us;
    uint64_t slot_qpc_us[3];
    uint64_t slot_frame_seq[3];
    uint8_t _reserved[16];
} AetherVCamHeader;
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifdef __cplusplus
static_assert(sizeof(AetherVCamHeader) == 128, "AetherVCamHeader must be 128 bytes");
static_assert(alignof(AetherVCamHeader) == 64, "AetherVCamHeader must be 64-byte aligned");
static_assert((AETHER_VCAM_HEADER_BYTES % 4096u) == 0u);
static_assert((AETHER_VCAM_NV12_OFFSET_IN_SLOT % 65536u) == 0u);
static_assert((AETHER_VCAM_SLOT_STRIDE_BYTES % 65536u) == 0u);
#endif

#ifdef __cplusplus
}
#endif
