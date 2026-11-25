#include "core/jitter_buffer.h"

#include <string.h>

enum {
    JITTER_INDEX_MASK = UHURA_JITTER_CAPACITY - 1U
};

static size_t seq_to_slot_index(SeqNum seq) {
    return (size_t)(seq.num & (uint32_t)JITTER_INDEX_MASK);
}

static void clear_all_slots(JitterBuffer_Owned *ctx) {
    assert(ctx != NULL);
    memset(ctx->slots, 0, sizeof(ctx->slots));
    ctx->occupied_count = (SlotCount){ .count = 0U };
}

void JitterBuffer_init(JitterBuffer_Owned *ctx, JitterConfigParams params) {
    assert(ctx != NULL);
    memset(ctx, 0, sizeof(*ctx));

    uint16_t prefetch = params.target_prefetch.count;
    if (prefetch == 0U) {
        prefetch = 1U;
    } else if (prefetch > (uint16_t)(UHURA_JITTER_CAPACITY / 2U)) {
        prefetch = (uint16_t)(UHURA_JITTER_CAPACITY / 2U);
    }

    uint16_t max_plc = params.max_plc_frames.count;
    if (max_plc == 0U) {
        max_plc = (uint16_t)UHURA_MAX_CONSECUTIVE_PLC;
    }

    ctx->target_prefetch = (SlotCount){ .count = prefetch };
    ctx->max_plc_frames  = (SlotCount){ .count = max_plc };
    ctx->state           = JB_STATE_IDLE;
    ctx->is_initialized  = true;
}

void JitterBuffer_reset(JitterBuffer_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    clear_all_slots(ctx);
    ctx->state            = JB_STATE_IDLE;
    ctx->next_play_seq    = (SeqNum){ .num = 0U };
    ctx->highest_seq_seen = (SeqNum){ .num = 0U };
    ctx->prefetch_ticks   = (SlotCount){ .count = 0U };
    ctx->consecutive_plc  = (SlotCount){ .count = 0U };
    ctx->talk_end_marked  = false;
}

void JitterBuffer_on_talk_start(JitterBuffer_Owned *ctx, TalkSessionId session) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if ((ctx->state == JB_STATE_IDLE) || (ctx->active_session.id != session.id)) {
        JitterBuffer_reset(ctx);
        ctx->active_session = session;
    }
}

void JitterBuffer_on_talk_end(JitterBuffer_Owned *ctx, TalkSessionId session) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if ((ctx->state != JB_STATE_IDLE) && (ctx->active_session.id == session.id)) {
        ctx->talk_end_marked = true;
        if (ctx->occupied_count.count == 0U) {
            JitterBuffer_reset(ctx);
        } else if (ctx->state == JB_STATE_PREFETCHING) {
            /* Short tap on PTT: flush remaining buffered audio immediately */
            ctx->state = JB_STATE_PLAYING;
        }
    }
}

NODISCARD JitterPushResult JitterBuffer_push_move(
    JitterBuffer_Owned * restrict ctx,
    EncodedFrame_Owned * restrict frame_in
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    assert(frame_in != NULL);
    assert(frame_in->is_valid);

    JitterPushResult result = JITTER_PUSH_STORED;
    const TalkSessionId incoming_session = frame_in->session;
    const SeqNum incoming_seq = frame_in->seq;

    if ((ctx->state == JB_STATE_IDLE) || (incoming_session.id != ctx->active_session.id)) {
        const bool switched_session =
            (ctx->state != JB_STATE_IDLE) && (incoming_session.id != ctx->active_session.id);

        JitterBuffer_reset(ctx);
        ctx->active_session   = incoming_session;
        ctx->next_play_seq    = incoming_seq;
        ctx->highest_seq_seen = incoming_seq;
        ctx->state            = (ctx->target_prefetch.count <= 1U)
                                ? JB_STATE_PLAYING
                                : JB_STATE_PREFETCHING;
        result = switched_session ? JITTER_PUSH_NEW_SESSION : JITTER_PUSH_STORED;
    } else {
        /* Same session: check if late, duplicate, or far ahead */
        if (incoming_seq.num < ctx->next_play_seq.num) {
            *frame_in = (EncodedFrame_Owned){0};
            return JITTER_PUSH_LATE_OR_DUPLICATE;
        }

        const uint32_t ahead = incoming_seq.num - ctx->next_play_seq.num;
        if (ahead >= (uint32_t)UHURA_JITTER_CAPACITY) {
            /* Large burst jump after network stall: resync window cleanly */
            clear_all_slots(ctx);
            ctx->next_play_seq    = incoming_seq;
            ctx->highest_seq_seen = incoming_seq;
            ctx->consecutive_plc  = (SlotCount){ .count = 0U };
            result = JITTER_PUSH_RESYNCED;
        }
    }

    const size_t idx = seq_to_slot_index(incoming_seq);
    if (ctx->slots[idx].is_valid && (ctx->slots[idx].seq.num == incoming_seq.num)) {
        /* Duplicate UDP packet: discard incoming copy */
        *frame_in = (EncodedFrame_Owned){0};
        return JITTER_PUSH_LATE_OR_DUPLICATE;
    }

    if (!ctx->slots[idx].is_valid) {
        ctx->occupied_count.count = (uint16_t)(ctx->occupied_count.count + 1U);
    }
    EncodedFrame_move(&ctx->slots[idx], frame_in);

    if (incoming_seq.num > ctx->highest_seq_seen.num) {
        ctx->highest_seq_seen = incoming_seq;
    }
    if (incoming_seq.num < ctx->next_play_seq.num) {
        ctx->next_play_seq = incoming_seq;
    }

    if (ctx->state == JB_STATE_PREFETCHING) {
        const uint32_t span = (ctx->highest_seq_seen.num - ctx->next_play_seq.num) + 1U;
        if (span >= (uint32_t)ctx->target_prefetch.count) {
            ctx->state = JB_STATE_PLAYING;
        }
    }

    assert(!frame_in->is_valid);
    return result;
}

NODISCARD JitterPopStatus JitterBuffer_pop_10ms(
    JitterBuffer_Owned * restrict ctx,
    EncodedFrame_Owned * restrict out_frame
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    assert(out_frame != NULL);

    *out_frame = (EncodedFrame_Owned){0};

    if (ctx->state == JB_STATE_IDLE) {
        return JITTER_POP_SILENCE_IDLE;
    }

    if (ctx->state == JB_STATE_PREFETCHING) {
        ctx->prefetch_ticks.count = (uint16_t)(ctx->prefetch_ticks.count + 1U);
        if (ctx->prefetch_ticks.count < ctx->target_prefetch.count) {
            return JITTER_POP_SILENCE_IDLE;
        }
        ctx->state = JB_STATE_PLAYING;
    }

    /* JB_STATE_PLAYING */
    const SeqNum cur_seq = ctx->next_play_seq;
    const size_t cur_idx = seq_to_slot_index(cur_seq);

    if (ctx->slots[cur_idx].is_valid && (ctx->slots[cur_idx].seq.num == cur_seq.num)) {
        EncodedFrame_move(out_frame, &ctx->slots[cur_idx]);
        assert(ctx->occupied_count.count > 0U);
        ctx->occupied_count.count = (uint16_t)(ctx->occupied_count.count - 1U);
        ctx->next_play_seq.num    = cur_seq.num + 1U;
        ctx->consecutive_plc      = (SlotCount){ .count = 0U };

        if (ctx->talk_end_marked && (ctx->occupied_count.count == 0U)) {
            JitterBuffer_reset(ctx);
        }
        return JITTER_POP_FRAME;
    }

    /* Current frame is missing. If TALK_END arrived and buffer is empty, finish cleanly. */
    if (ctx->talk_end_marked && (ctx->occupied_count.count == 0U)) {
        JitterBuffer_reset(ctx);
        return JITTER_POP_SILENCE_IDLE;
    }

    /* Check if packet N+1 is already present so Opus can run in-band FEC! */
    const SeqNum next_seq = { .num = cur_seq.num + 1U };
    const size_t next_idx = seq_to_slot_index(next_seq);
    if (ctx->slots[next_idx].is_valid && (ctx->slots[next_idx].seq.num == next_seq.num)) {
        /* Copy N+1 for FEC recovery while keeping N+1 in its slot for the next 10 ms tick */
        *out_frame = ctx->slots[next_idx];
        ctx->next_play_seq.num   = cur_seq.num + 1U;
        ctx->consecutive_plc.count = (uint16_t)(ctx->consecutive_plc.count + 1U);
        return JITTER_POP_FEC_NEXT;
    }

    /* Neither N nor N+1 available: emit bounded PLC, then return to IDLE */
    ctx->consecutive_plc.count = (uint16_t)(ctx->consecutive_plc.count + 1U);
    ctx->next_play_seq.num     = cur_seq.num + 1U;

    if (ctx->consecutive_plc.count > ctx->max_plc_frames.count) {
        JitterBuffer_reset(ctx);
        return JITTER_POP_SILENCE_IDLE;
    }

    return JITTER_POP_MISSING_PLC;
}

NODISCARD bool JitterBuffer_is_active(const JitterBuffer_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    return ctx->state != JB_STATE_IDLE;
}
