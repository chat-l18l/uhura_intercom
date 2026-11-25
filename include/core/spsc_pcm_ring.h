#ifndef UHURA_CORE_SPSC_PCM_RING_H
#define UHURA_CORE_SPSC_PCM_RING_H

#include "core/types.h"

#include <stdatomic.h>

/* =========================================================================
 * Lock-Free Single-Producer Single-Consumer PCM Frame Ringbuffer
 * Bridges the real-time audio callback thread and the core/network loop
 * without mutexes or heap allocation (Rule 1, Rule 2 & Rule 4).
 * ========================================================================= */
typedef enum {
    RING_OK = 0,
    RING_FULL,
    RING_EMPTY
} RingStatus;

typedef struct {
    PcmFrame_Owned slots[UHURA_SPSC_RING_CAPACITY];
    atomic_size_t  write_idx;
    atomic_size_t  read_idx;
    bool           is_initialized;
} SpscPcmRing_Owned;

void SpscPcmRing_init(SpscPcmRing_Owned *ctx);

/*
 * Moves ownership of *frame_in into the ringbuffer on RING_OK (*frame_in = {0}).
 * On RING_FULL, *frame_in remains valid and owned by the caller.
 */
NODISCARD RingStatus SpscPcmRing_push_move(
    SpscPcmRing_Owned * restrict ctx,
    PcmFrame_Owned * restrict frame_in
);

/*
 * Moves ownership of the oldest queued frame into *frame_out on RING_OK.
 * On RING_EMPTY, *frame_out is cleared to {0}.
 */
NODISCARD RingStatus SpscPcmRing_pop_move(
    SpscPcmRing_Owned * restrict ctx,
    PcmFrame_Owned * restrict frame_out
);

NODISCARD SlotCount SpscPcmRing_available_slots(const SpscPcmRing_Owned *ctx);

#endif /* UHURA_CORE_SPSC_PCM_RING_H */
