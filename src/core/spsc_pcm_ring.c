#include "core/spsc_pcm_ring.h"

#include <string.h>

enum {
    SPSC_RING_MASK = UHURA_SPSC_RING_CAPACITY - 1U
};

void SpscPcmRing_init(SpscPcmRing_Owned *ctx) {
    assert(ctx != NULL);
    memset(ctx->slots, 0, sizeof(ctx->slots));
    atomic_init(&ctx->write_idx, 0U);
    atomic_init(&ctx->read_idx, 0U);
    ctx->is_initialized = true;
}

NODISCARD RingStatus SpscPcmRing_push_move(
    SpscPcmRing_Owned * restrict ctx,
    PcmFrame_Owned * restrict frame_in
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    assert(frame_in != NULL);
    assert(frame_in->is_valid);

    const size_t w = atomic_load_explicit(&ctx->write_idx, memory_order_relaxed);
    const size_t r = atomic_load_explicit(&ctx->read_idx, memory_order_acquire);

    if ((w - r) >= (size_t)UHURA_SPSC_RING_CAPACITY) {
        return RING_FULL;
    }

    const size_t slot_idx = w & (size_t)SPSC_RING_MASK;
    PcmFrame_move(&ctx->slots[slot_idx], frame_in);

    atomic_store_explicit(&ctx->write_idx, w + 1U, memory_order_release);
    assert(!frame_in->is_valid);
    return RING_OK;
}

NODISCARD RingStatus SpscPcmRing_pop_move(
    SpscPcmRing_Owned * restrict ctx,
    PcmFrame_Owned * restrict frame_out
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    assert(frame_out != NULL);

    *frame_out = (PcmFrame_Owned){0};

    const size_t r = atomic_load_explicit(&ctx->read_idx, memory_order_relaxed);
    const size_t w = atomic_load_explicit(&ctx->write_idx, memory_order_acquire);

    if (w == r) {
        return RING_EMPTY;
    }

    const size_t slot_idx = r & (size_t)SPSC_RING_MASK;
    PcmFrame_move(frame_out, &ctx->slots[slot_idx]);

    atomic_store_explicit(&ctx->read_idx, r + 1U, memory_order_release);
    assert(frame_out->is_valid);
    return RING_OK;
}

NODISCARD SlotCount SpscPcmRing_available_slots(const SpscPcmRing_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    const size_t w = atomic_load_explicit(&ctx->write_idx, memory_order_acquire);
    const size_t r = atomic_load_explicit(&ctx->read_idx, memory_order_acquire);
    const size_t diff = w - r;
    assert(diff <= (size_t)UHURA_SPSC_RING_CAPACITY);

    return (SlotCount){ .count = (uint16_t)diff };
}
