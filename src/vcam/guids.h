#pragma once

#include <guiddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Frozen Phase 5 DirectShow Virtual Camera filter CLSID (Appendix D: AETHER-TS-001 v1.0).
// {7A5E14B2-4F8C-4D31-9E2A-61B38F0C42D5}
DEFINE_GUID(CLSID_AetherVirtualCam, 0x7a5e14b2, 0x4f8c, 0x4d31, 0x9e, 0x2a, 0x61, 0xb3, 0x8f, 0x0c, 0x42, 0xd5);

#define AETHER_VCAM_FRIENDLY_NAME_W L"Aether 2.5D Virtual Cam (Alpha)"
#define AETHER_VCAM_FRIENDLY_NAME_A "Aether 2.5D Virtual Cam (Alpha)"
#define AETHER_VCAM_SHM_NAME_W L"Local\\AetherVCam_v1"
#define AETHER_VCAM_CLSID_STR_W L"{7A5E14B2-4F8C-4D31-9E2A-61B38F0C42D5}"

#ifdef __cplusplus
}
#endif
