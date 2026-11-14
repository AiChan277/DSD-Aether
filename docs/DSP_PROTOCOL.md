# AETHER-2.5D External Audio-DSP Synchronisation Protocol (`REQ-OUT-030`)

- **Shared Memory Section**: `Local\AetherDspSyncSharedMem` (`4096 B` page-aligned view, `AetherDspSyncPacket` at byte offset `0`).
- **Magic / Version**: `magic_header = 0x41455448` (`'AETH'`), `protocol_version = 1`.
- **Contract Header**: [`include/aether_ffi_contract.h`](../include/aether_ffi_contract.h) (`128 B`, `align(64)`, cache-line partitioned).
- **OSC Telemetry Endpoint**: UDP `127.0.0.1:9001` (`f32` `/aether/telemetry/latency_ms` per frame; `f32` `/aether/telemetry/fps` & `i32` `/aether/tracking/ep` at `4 Hz`).

## 1. Cache-Line Partitioning & Seqlock Ownership

`AetherDspSyncPacket` occupies two adjacent 64-byte cache lines with **strict single-writer isolation** and **zero blocking OS locks**:

1. **Cache Line 0 (`bytes 0..63`) — Written ONLY by Aether (T4)**:
   - Seqlock counter: `sequence_number` (odd while writing, even when quiescent, incremented by `+1` before and `+1` after payload stores with acquire/release fences).
   - Fields: `magic_header`, `total_video_latency_ms` (`(t_present - t_capture) / 1000.0`), `recommended_dsp_delay_ms`, `video_present_qpc_us`, `protocol_version`, `video_fps_milli` (e.g. `60000`), `latency_jitter_ms` (`p95 - p50` over last `120` frames), `video_flags` (`bit 0` Spout active, `bit 1` VCam active, `bit 2` NDI active, `bit 3` tracking lost).
   - Delay recommendation law (`§11.5`):
     $$\text{target} = \text{clamp}(\text{EMA}_{0.1}(\Delta t_{\text{video}}) - \text{dsp\_reported\_latency\_ms} + \text{user\_offset\_ms},\ 0,\ 250)$$
     Slew-limited to `±2.0 ms/s` (`0.002 ms` per millisecond of `dt`) so the external audio delay line never steps audibly.

2. **Cache Line 1 (`bytes 64..127`) — Written ONLY by External Audio DSP**:
   - Seqlock counter: `dsp_sequence_number` (odd while writing, `+2` per complete publish).
   - Fields: `dsp_input_rms_db` (`dBFS`), `dsp_viseme_weights[5]` (`[A, I, U, E, O]` clamped to `[0.0, 1.0]`), `dsp_reported_latency_ms`, `dsp_write_qpc_us` (QPC microseconds at write), `dsp_flags` (`bit 0` `voice_active`, `bit 1` `delay_applied`, `bit 2` `visemes_valid`).
   - Staleness cutoff: data older than `250 ms` (`now_qpc_us - dsp_write_qpc_us > 250000`) is ignored and mouth ownership hands back cleanly to camera tracking over `60 ms` (`§9.7`).

## 2. Reference C11 Reader / Writer (`DSP Side`)

```c
#include "aether_ffi_contract.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <stdint.h>
#include <string.h>

static uint64_t aether_qpc_us(void) {
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000ULL) / (uint64_t)freq.QuadPart);
}

/* DSP writes Cache Line 1 (visemes + DSP latency) without blocking video */
void aether_dsp_publish(AetherDspSyncPacket* pkt,
                        float rms_db,
                        const float visemes[5],
                        float dsp_latency_ms,
                        uint32_t flags) {
    LONG seq = (LONG)InterlockedCompareExchange((volatile LONG*)&pkt->dsp_sequence_number, 0, 0);
    if ((seq & 1) != 0) {
        ++seq; /* recover if previous writer crashed mid-write */
    }
    InterlockedExchange((volatile LONG*)&pkt->dsp_sequence_number, seq + 1);
    MemoryBarrier();

    pkt->dsp_input_rms_db = rms_db;
    memcpy(pkt->dsp_viseme_weights, visemes, sizeof(float) * 5);
    pkt->dsp_reported_latency_ms = dsp_latency_ms;
    pkt->dsp_write_qpc_us = aether_qpc_us();
    pkt->dsp_flags = flags;

    MemoryBarrier();
    InterlockedExchange((volatile LONG*)&pkt->dsp_sequence_number, seq + 2);
}

/* DSP reads Cache Line 0 (recommended delay + video latency) wait-free (max 4 retries) */
int aether_dsp_read_video(const AetherDspSyncPacket* pkt, AetherDspSyncPacket* out_copy) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        uint32_t s1 = (uint32_t)InterlockedCompareExchange(
            (volatile LONG*)&((AetherDspSyncPacket*)pkt)->sequence_number, 0, 0);
        if ((s1 & 1u) != 0u) {
            continue;
        }
        MemoryBarrier();
        memcpy(out_copy, pkt, 64); /* Cache line 0 only */
        MemoryBarrier();
        uint32_t s2 = (uint32_t)InterlockedCompareExchange(
            (volatile LONG*)&((AetherDspSyncPacket*)pkt)->sequence_number, 0, 0);
        if (s1 == s2 && out_copy->magic_header == AETHER_DSP_MAGIC) {
            return 1;
        }
    }
    return 0;
}
```
