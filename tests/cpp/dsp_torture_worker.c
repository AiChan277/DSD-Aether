#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "aether_ffi_contract.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * External C11/Win32 separate-process torture test client implementing the exact
 * Cache Line 1 seqlock protocol documented in docs/DSP_PROTOCOL.md.
 */

static uint64_t aether_qpc_us_c(void) {
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000LL) / freq.QuadPart);
}

static uint32_t read_seq_cl1(const volatile AetherDspSyncPacket* p) {
    uint32_t s = (uint32_t)InterlockedCompareExchange((volatile LONG*)&p->dsp_sequence_number, 0, 0);
    MemoryBarrier();
    return s;
}

static uint32_t read_seq_cl0(const volatile AetherDspSyncPacket* p) {
    uint32_t s = (uint32_t)InterlockedCompareExchange((volatile LONG*)&p->sequence_number, 0, 0);
    MemoryBarrier();
    return s;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <section_name> <writer|reader|cl0reader> <iterations>\n", argv[0]);
        return 2;
    }

    const char* section_name = argv[1];
    const char* role = argv[2];
    const uint64_t iterations = _strtoui64(argv[3], NULL, 10);

    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, section_name);
    if (map == NULL) {
        fprintf(stderr, "OpenFileMappingA failed: %lu\n", GetLastError());
        return 3;
    }

    volatile AetherDspSyncPacket* pkt =
        (volatile AetherDspSyncPacket*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AetherDspSyncPacket));
    if (pkt == NULL) {
        CloseHandle(map);
        fprintf(stderr, "MapViewOfFile failed: %lu\n", GetLastError());
        return 4;
    }

    if (strcmp(role, "writer") == 0) {
        uint32_t seq = read_seq_cl1(pkt);
        if ((seq & 1u) != 0u) {
            ++seq;
        }
        for (uint64_t i = 0; i < iterations; ++i) {
            const uint32_t k = (uint32_t)(i + 1u);
            const float base = (float)(k % 1000u) * 0.001f;

            InterlockedExchange((volatile LONG*)&pkt->dsp_sequence_number, (LONG)(seq + 1u));
            MemoryBarrier();

            pkt->dsp_input_rms_db = -60.0f + (float)(k % 60u);
            pkt->dsp_viseme_weights[0] = base;
            pkt->dsp_viseme_weights[1] = 1.0f - base;
            pkt->dsp_viseme_weights[2] = (float)((k >> 2u) & 255u) / 255.0f;
            pkt->dsp_viseme_weights[3] = (float)((k >> 4u) & 255u) / 255.0f;
            pkt->dsp_viseme_weights[4] = (float)(k % 1000u);
            pkt->dsp_reported_latency_ms = 5.0f + (float)(k % 17u);
            pkt->dsp_write_qpc_us = aether_qpc_us_c() | 1ULL;
            pkt->dsp_flags = 0x07u;

            MemoryBarrier();
            seq += 2u;
            InterlockedExchange((volatile LONG*)&pkt->dsp_sequence_number, (LONG)seq);
        }
        /* Mark done sentinel bit in _reserved1[0..3] with seq still clean even */
        InterlockedExchange((volatile LONG*)&pkt->_reserved1[0], 1L);
    } else if (strcmp(role, "reader") == 0) {
        uint64_t torn = 0;
        uint64_t valid_reads = 0;
        for (uint64_t i = 0; i < iterations; ++i) {
            uint32_t s0 = read_seq_cl1(pkt);
            if ((s0 & 1u) != 0u || s0 == 0u) {
                continue;
            }
            const float v0 = pkt->dsp_viseme_weights[0];
            const float v1 = pkt->dsp_viseme_weights[1];
            const float v4 = pkt->dsp_viseme_weights[4];
            const uint64_t qpc = pkt->dsp_write_qpc_us;
            const uint32_t flags = pkt->dsp_flags;
            MemoryBarrier();
            uint32_t s1 = read_seq_cl1(pkt);
            if (s0 == s1) {
                const uint32_t k_mod = (uint32_t)v4;
                const float expect_base = (float)(k_mod % 1000u) * 0.001f;
                if (fabsf(v0 - expect_base) > 1e-4f || fabsf((v0 + v1) - 1.0f) > 1e-4f || flags != 0x07u ||
                    qpc == 0ULL) {
                    ++torn;
                    if (torn == 1) {
                        fprintf(stderr, "Torn CL1 read s=%u v0=%f v1=%f v4=%f flags=%u\n", s0, v0, v1, v4, flags);
                    }
                }
                ++valid_reads;
            }
        }
        if (torn > 0 || valid_reads == 0) {
            UnmapViewOfFile((LPCVOID)pkt);
            CloseHandle(map);
            fprintf(stderr, "Reader failed: torn=%llu valid=%llu\n", (unsigned long long)torn,
                    (unsigned long long)valid_reads);
            return 5;
        }
    } else if (strcmp(role, "cl0reader") == 0) {
        uint64_t valid_reads = 0;
        for (uint64_t i = 0; i < iterations; ++i) {
            uint32_t s0 = read_seq_cl0(pkt);
            if ((s0 & 1u) != 0u || s0 == 0u) {
                continue;
            }
            const uint32_t magic = pkt->magic_header;
            const float lat = pkt->total_video_latency_ms;
            const float delay = pkt->recommended_dsp_delay_ms;
            const uint64_t qpc = pkt->video_present_qpc_us;
            MemoryBarrier();
            uint32_t s1 = read_seq_cl0(pkt);
            if (s0 == s1) {
                if (magic != (uint32_t)AETHER_DSP_MAGIC || delay < 0.0f || delay > 250.0f || lat < 0.0f ||
                    qpc == 0ULL) {
                    UnmapViewOfFile((LPCVOID)pkt);
                    CloseHandle(map);
                    return 6;
                }
                ++valid_reads;
            }
        }
        if (valid_reads == 0) {
            UnmapViewOfFile((LPCVOID)pkt);
            CloseHandle(map);
            return 7;
        }
    } else {
        UnmapViewOfFile((LPCVOID)pkt);
        CloseHandle(map);
        return 8;
    }

    UnmapViewOfFile((LPCVOID)pkt);
    CloseHandle(map);
    return 0;
}
