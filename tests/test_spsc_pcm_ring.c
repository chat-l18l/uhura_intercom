#include "core/spsc_pcm_ring.h"

#include <stdio.h>

#define TEST_CHECK(cond)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            return 1;                                                               \
        }                                                                           \
    } while (0)

int main(void) {
    SpscPcmRing_Owned ring;
    SpscPcmRing_init(&ring);
    TEST_CHECK(SpscPcmRing_available_slots(&ring).count == 0U);

    /* 1. Fill to capacity (16 frames) and verify RING_FULL preserves source ownership */
    for (uint16_t i = 0U; i < (uint16_t)UHURA_SPSC_RING_CAPACITY; ++i) {
        PcmFrame_Owned f = {0};
        PcmFrame_init_silence(&f);
        f.samples[0] = (int16_t)(100 + (int16_t)i);
        TEST_CHECK(SpscPcmRing_push_move(&ring, &f) == RING_OK);
        TEST_CHECK(!f.is_valid);
    }
    TEST_CHECK(SpscPcmRing_available_slots(&ring).count == (uint16_t)UHURA_SPSC_RING_CAPACITY);

    PcmFrame_Owned extra = {0};
    PcmFrame_init_silence(&extra);
    extra.samples[0] = 999;
    TEST_CHECK(SpscPcmRing_push_move(&ring, &extra) == RING_FULL);
    TEST_CHECK(extra.is_valid);

    /* 2. Drain and verify FIFO order across wraparound (64 cycles) */
    for (uint16_t i = 0U; i < (uint16_t)UHURA_SPSC_RING_CAPACITY; ++i) {
        PcmFrame_Owned out = {0};
        TEST_CHECK(SpscPcmRing_pop_move(&ring, &out) == RING_OK);
        TEST_CHECK(out.is_valid);
        TEST_CHECK(out.samples[0] == (int16_t)(100 + (int16_t)i));
    }

    PcmFrame_Owned empty_out = {0};
    TEST_CHECK(SpscPcmRing_pop_move(&ring, &empty_out) == RING_EMPTY);
    TEST_CHECK(!empty_out.is_valid);

    for (uint16_t cycle = 0U; cycle < 64U; ++cycle) {
        PcmFrame_Owned in = {0};
        PcmFrame_init_silence(&in);
        in.samples[479] = (int16_t)cycle;
        TEST_CHECK(SpscPcmRing_push_move(&ring, &in) == RING_OK);

        PcmFrame_Owned out = {0};
        TEST_CHECK(SpscPcmRing_pop_move(&ring, &out) == RING_OK);
        TEST_CHECK(out.samples[479] == (int16_t)cycle);
    }

    /* 3. Verify saturating int16 mixer clamps instead of overflowing */
    PcmFrame_Owned acc = {0};
    PcmFrame_Owned add = {0};
    PcmFrame_init_silence(&acc);
    PcmFrame_init_silence(&add);
    acc.samples[0] = 25000;
    add.samples[0] = 20000;
    acc.samples[1] = -25000;
    add.samples[1] = -20000;

    PcmFrame_mix_into(&acc, PcmFrame_loan(&add));
    TEST_CHECK(acc.samples[0] == INT16_MAX);
    TEST_CHECK(acc.samples[1] == INT16_MIN);

    return 0;
}
