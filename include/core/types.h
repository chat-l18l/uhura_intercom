#ifndef UHURA_CORE_TYPES_H
#define UHURA_CORE_TYPES_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
  #define NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
  #define NODISCARD __attribute__((warn_unused_result))
#else
  #define NODISCARD
#endif

/* =========================================================================
 * Compile-Time System Constants & Proofs (Rule 1 & Rule 5)
 * ========================================================================= */
enum {
    UHURA_SAMPLE_RATE_HZ       = 48000U,
    UHURA_SAMPLES_PER_FRAME    = 480U,   /* 10 ms @ 48 kHz mono */
    UHURA_PCM_FRAME_BYTES      = 960U,   /* 480 * sizeof(int16_t) */
    UHURA_MAX_OPUS_BYTES       = 256U,
    UHURA_WIRE_HEADER_BYTES    = 20U,
    UHURA_MAX_WIRE_BYTES       = 276U,   /* 20 + 256 bytes */
    UHURA_MAX_PEERS            = 16U,
    UHURA_MAX_RX_STREAMS       = 4U,
    UHURA_JITTER_CAPACITY      = 32U,    /* Power-of-two: 320 ms max window */
    UHURA_SPSC_RING_CAPACITY   = 16U,    /* Power-of-two: 160 ms audio ring */
    UHURA_STATION_NAME_MAX     = 24U
};

static_assert(UHURA_SAMPLES_PER_FRAME * 100U == UHURA_SAMPLE_RATE_HZ,
              "UHURA_SAMPLES_PER_FRAME must represent exactly 10 ms at 48 kHz");
static_assert(UHURA_PCM_FRAME_BYTES == UHURA_SAMPLES_PER_FRAME * sizeof(int16_t),
              "PCM byte size must match 16-bit mono samples per frame");
static_assert(UHURA_MAX_WIRE_BYTES == UHURA_WIRE_HEADER_BYTES + UHURA_MAX_OPUS_BYTES,
              "Max wire datagram size mismatch");
static_assert(UHURA_MAX_WIRE_BYTES <= 512U,
              "Wire datagram must stay well below WireGuard MTU (1280/1420 bytes)");
static_assert((UHURA_JITTER_CAPACITY & (UHURA_JITTER_CAPACITY - 1U)) == 0U,
              "Jitter buffer capacity must be a power of two");
static_assert((UHURA_SPSC_RING_CAPACITY & (UHURA_SPSC_RING_CAPACITY - 1U)) == 0U,
              "SPSC ring capacity must be a power of two");

/* =========================================================================
 * Domain Strong Types (Rule 3: No naked primitives on module boundaries)
 * ========================================================================= */
typedef struct { uint8_t  id;    } StationId;       /* Valid station IDs: 1..16 (0 = none) */
typedef struct { uint16_t bits;  } TargetPeerMask;  /* Bit (id - 1) per destination station */
typedef struct { uint32_t id;    } TalkSessionId;   /* Increments per PTT press */
typedef struct { uint32_t num;   } SeqNum;          /* Packet sequence within talk session */
typedef struct { uint32_t hz;    } SampleRateHz;
typedef struct { uint32_t bps;   } BitrateBps;
typedef struct { uint16_t count; } FrameCount;      /* Number of PCM audio frames (samples) */
typedef struct { uint16_t count; } SlotCount;       /* Number of 10 ms packet/ring slots */
typedef struct { uint32_t ms;    } DurationMs;
typedef struct { uint64_t us;    } MonotonicTimeUs;
typedef struct { uint16_t port;  } UdpPort;

/* =========================================================================
 * Borrowed Views (_Loaned — Rule 2: NEVER store in long-lived _Owned structs)
 * ========================================================================= */
typedef struct {
    const uint8_t *data;
    size_t len;
} ByteSlice_Loaned;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t capacity;
} ByteSliceMut_Loaned;

typedef struct {
    const int16_t *samples;
    FrameCount frames;
} PcmMonoSlice_Loaned;

typedef struct {
    int16_t *samples;
    FrameCount frames;
} PcmMonoSliceMut_Loaned;

/* =========================================================================
 * Owned Fixed-Size Value Buffers (_Owned — Rule 1 & Rule 2)
 * ========================================================================= */
typedef struct {
    int16_t samples[UHURA_SAMPLES_PER_FRAME];
    bool is_valid;
} PcmFrame_Owned;

typedef struct {
    uint8_t bytes[UHURA_MAX_OPUS_BYTES];
    uint16_t len;
    SeqNum seq;
    TalkSessionId session;
    bool is_valid;
} EncodedFrame_Owned;

typedef struct {
    uint8_t bytes[UHURA_MAX_WIRE_BYTES];
    uint16_t len;
    bool is_valid;
} WireDatagram_Owned;

/* =========================================================================
 * StationId & TargetPeerMask Pure Helpers
 * ========================================================================= */
NODISCARD bool StationId_is_valid(StationId station);
NODISCARD TargetPeerMask TargetPeerMask_none(void);
NODISCARD TargetPeerMask TargetPeerMask_all(void);
NODISCARD TargetPeerMask TargetPeerMask_from_station(StationId station);
NODISCARD TargetPeerMask TargetPeerMask_with_station(TargetPeerMask mask, StationId station);
NODISCARD TargetPeerMask TargetPeerMask_without_station(TargetPeerMask mask, StationId station);
NODISCARD bool TargetPeerMask_contains(TargetPeerMask mask, StationId station);

/* =========================================================================
 * Ownership, Borrowing & Move Helpers (Zenoh-pico style)
 * ========================================================================= */
void PcmFrame_init_silence(PcmFrame_Owned *self);
NODISCARD PcmFrame_Owned PcmFrame_from_slice(PcmMonoSlice_Loaned src);
NODISCARD PcmMonoSlice_Loaned PcmFrame_loan(const PcmFrame_Owned *self);
NODISCARD PcmMonoSliceMut_Loaned PcmFrame_loan_mut(PcmFrame_Owned *self);
void PcmFrame_move(PcmFrame_Owned * restrict dst, PcmFrame_Owned * restrict src);

NODISCARD ByteSlice_Loaned EncodedFrame_loan(const EncodedFrame_Owned *self);
void EncodedFrame_move(EncodedFrame_Owned * restrict dst, EncodedFrame_Owned * restrict src);

NODISCARD ByteSlice_Loaned WireDatagram_loan(const WireDatagram_Owned *self);
void WireDatagram_move(WireDatagram_Owned * restrict dst, WireDatagram_Owned * restrict src);

/* Saturating int16 addition for multi-talker PCM mixing */
void PcmFrame_mix_into(PcmFrame_Owned * restrict accumulator, PcmMonoSlice_Loaned contribution);

#endif /* UHURA_CORE_TYPES_H */
