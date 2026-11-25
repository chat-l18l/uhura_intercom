#ifndef UHURA_CORE_JITTER_BUFFER_H
#define UHURA_CORE_JITTER_BUFFER_H

#include "core/types.h"

/* =========================================================================
 * Fixed-Capacity Sequence-Indexed Jitter Buffer (Rule 1, Rule 2 & Rule 4)
 * Handles WireGuard jitter, out-of-order UDP arrival, new TalkSessionId
 * transitions, in-band FEC lookahead, and bounded PLC decay.
 * ========================================================================= */
enum {
    UHURA_DEFAULT_PREFETCH_SLOTS = 2U,  /* 20 ms target jitter depth */
    UHURA_MAX_CONSECUTIVE_PLC    = 8U   /* 80 ms max PLC before returning to idle */
};

typedef enum {
    JB_STATE_IDLE = 0,
    JB_STATE_PREFETCHING,
    JB_STATE_PLAYING
} JitterState;

typedef enum {
    JITTER_PUSH_STORED = 0,
    JITTER_PUSH_NEW_SESSION,
    JITTER_PUSH_LATE_OR_DUPLICATE,
    JITTER_PUSH_RESYNCED
} JitterPushResult;

typedef enum {
    JITTER_POP_FRAME = 0,     /* Exact frame moved into *out_frame */
    JITTER_POP_FEC_NEXT,      /* Frame missing, but N+1 copied to *out_frame for FEC */
    JITTER_POP_MISSING_PLC,   /* Frame missing, run Opus PLC */
    JITTER_POP_SILENCE_IDLE   /* Pre-buffering or idle; output silence */
} JitterPopStatus;

typedef struct {
    SlotCount target_prefetch;
    SlotCount max_plc_frames;
} JitterConfigParams;

typedef struct {
    EncodedFrame_Owned slots[UHURA_JITTER_CAPACITY];
    JitterState        state;
    TalkSessionId      active_session;
    SeqNum             next_play_seq;
    SeqNum             highest_seq_seen;
    SlotCount          occupied_count;
    SlotCount          target_prefetch;
    SlotCount          max_plc_frames;
    SlotCount          prefetch_ticks;
    SlotCount          consecutive_plc;
    bool               talk_end_marked;
    bool               is_initialized;
} JitterBuffer_Owned;

void JitterBuffer_init(JitterBuffer_Owned *ctx, JitterConfigParams params);
void JitterBuffer_reset(JitterBuffer_Owned *ctx);

/* Marks incoming TALK_START so a session is primed before audio arrives */
void JitterBuffer_on_talk_start(JitterBuffer_Owned *ctx, TalkSessionId session);

/* Marks incoming TALK_END so stream stops cleanly once queued frames drain */
void JitterBuffer_on_talk_end(JitterBuffer_Owned *ctx, TalkSessionId session);

/*
 * Consumes ownership of *frame_in (*frame_in = {0}) in all cases so callers
 * never leak or accidentally reuse a pushed frame (Rule 2).
 */
NODISCARD JitterPushResult JitterBuffer_push_move(
    JitterBuffer_Owned * restrict ctx,
    EncodedFrame_Owned * restrict frame_in
);

NODISCARD JitterPopStatus JitterBuffer_pop_10ms(
    JitterBuffer_Owned * restrict ctx,
    EncodedFrame_Owned * restrict out_frame
);

NODISCARD bool JitterBuffer_is_active(const JitterBuffer_Owned *ctx);

#endif /* UHURA_CORE_JITTER_BUFFER_H */
