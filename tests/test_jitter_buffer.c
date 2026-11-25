#include "core/jitter_buffer.h"

#include <stdio.h>

#define TEST_CHECK(cond)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static EncodedFrame_Owned make_dummy_frame(TalkSessionId session, SeqNum seq, uint8_t tag) {
    EncodedFrame_Owned f = {0};
    f.bytes[0] = tag;
    f.len      = 4U;
    f.session  = session;
    f.seq      = seq;
    f.is_valid = true;
    return f;
}

int main(void) {
    JitterBuffer_Owned jb;
    JitterBuffer_init(&jb, (JitterConfigParams){
        .target_prefetch = { .count = 2U },
        .max_plc_frames  = { .count = 3U }
    });

    const TalkSessionId sess1 = { .id = 10U };

    /* 1. Out-of-order arrival: seq 0, then seq 2, then seq 1 */
    EncodedFrame_Owned f0 = make_dummy_frame(sess1, (SeqNum){ .num = 0U }, 0xA0U);
    EncodedFrame_Owned f2 = make_dummy_frame(sess1, (SeqNum){ .num = 2U }, 0xA2U);
    EncodedFrame_Owned f1 = make_dummy_frame(sess1, (SeqNum){ .num = 1U }, 0xA1U);

    TEST_CHECK(JitterBuffer_push_move(&jb, &f0) == JITTER_PUSH_STORED);
    TEST_CHECK(!f0.is_valid);
    TEST_CHECK(JitterBuffer_push_move(&jb, &f2) == JITTER_PUSH_STORED);
    TEST_CHECK(JitterBuffer_push_move(&jb, &f1) == JITTER_PUSH_STORED);

    /* Pop 0, 1, 2 in exact order */
    EncodedFrame_Owned out = {0};
    TEST_CHECK(JitterBuffer_pop_10ms(&jb, &out) == JITTER_POP_FRAME);
    TEST_CHECK(out.seq.num == 0U && out.bytes[0] == 0xA0U);

    TEST_CHECK(JitterBuffer_pop_10ms(&jb, &out) == JITTER_POP_FRAME);
    TEST_CHECK(out.seq.num == 1U && out.bytes[0] == 0xA1U);

    TEST_CHECK(JitterBuffer_pop_10ms(&jb, &out) == JITTER_POP_FRAME);
    TEST_CHECK(out.seq.num == 2U && out.bytes[0] == 0xA2U);

    /* 2. Packet 3 lost, Packet 4 present -> must return JITTER_POP_FEC_NEXT then JITTER_POP_FRAME */
    EncodedFrame_Owned f4 = make_dummy_frame(sess1, (SeqNum){ .num = 4U }, 0xA4U);
    TEST_CHECK(JitterBuffer_push_move(&jb, &f4) == JITTER_PUSH_STORED);

    TEST_CHECK(JitterBuffer_pop_10ms(&jb, &out) == JITTER_POP_FEC_NEXT);
    TEST_CHECK(out.seq.num == 4U && out.bytes[0] == 0xA4U);

    TEST_CHECK(JitterBuffer_pop_10ms(&jb, &out) == JITTER_POP_FRAME);
    TEST_CHECK(out.seq.num == 4U && out.bytes[0] == 0xA4U);

    /* 3. Total loss: max_plc_frames = 3 -> 3x PLC then IDLE */
    TEST_CHECK(JitterBuffer_pop_10ms(&jb, &out) == JITTER_POP_MISSING_PLC);
    TEST_CHECK(JitterBuffer_pop_10ms(&jb, &out) == JITTER_POP_MISSING_PLC);
    TEST_CHECK(JitterBuffer_pop_10ms(&jb, &out) == JITTER_POP_MISSING_PLC);
    TEST_CHECK(JitterBuffer_pop_10ms(&jb, &out) == JITTER_POP_SILENCE_IDLE);
    TEST_CHECK(!JitterBuffer_is_active(&jb));

    return 0;
}
