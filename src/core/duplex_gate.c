#include "core/duplex_gate.h"

#include <string.h>

/*
 * Pure integer 64-entry sine lookup table (peak 4800 / 32767 ≈ -16.7 dBFS)
 * Zero floating-point or libm dependencies.
 */
static const int16_t SINE_TABLE_64[64] = {
       0,   470,   936,  1393,  1837,  2264,  2667,  3044,
    3394,  3712,  3991,  4231,  4435,  4595,  4708,  4777,
    4800,  4777,  4708,  4595,  4435,  4231,  3991,  3712,
    3394,  3044,  2667,  2264,  1837,  1393,   936,   470,
       0,  -470,  -936, -1393, -1837, -2264, -2667, -3044,
   -3394, -3712, -3991, -4231, -4435, -4595, -4708, -4777,
   -4800, -4777, -4708, -4595, -4435, -4231, -3991, -3712,
   -3394, -3044, -2667, -2264, -1837, -1393,  -936,  -470
};

enum {
    CHIME_CALL_T1_SAMPLES   = 2640U,
    CHIME_CALL_TOTAL        = 6720U,  /* 140 ms two-tone Enterprise whistle */
    CHIME_CARE_TOTAL        = 9600U,  /* 200 ms prominent three-note Care Alert */
    CHIME_OPEN_TICKS        = 12U,    /* 120 ms pre-listen chime before mic opens */
    CHIME_OPEN_TOTAL        = 5760U,
    CHIME_CLOSE_TOTAL       = 3840U,  /* 80 ms gentle descending close tone */
    PHASE_STEP_C5_Q32       = 46841989U, /* ~523 Hz */
    PHASE_STEP_G5_Q32       = 70149874U, /* ~784 Hz */
    PHASE_STEP_C6_Q32       = 93683978U  /* ~1047 Hz */
};

static void start_chime(DuplexGate_Owned *ctx, ChimePattern pattern, uint16_t total_samples) {
    assert(ctx != NULL);
    if (ctx->chime_mode != CHIME_ENABLED) {
        return;
    }
    ctx->active_chime            = pattern;
    ctx->chime_samples_total     = total_samples;
    ctx->chime_samples_remaining = total_samples;
    ctx->chime_phase_q32         = 0U;
}

static uint32_t chime_step_for_progress(ChimePattern pat, uint16_t elapsed) {
    if (pat == CHIME_PATTERN_CARE_ALERT) {
        if (elapsed < 3200U) {
            return (uint32_t)PHASE_STEP_G5_Q32;
        }
        if (elapsed < 6400U) {
            return (uint32_t)PHASE_STEP_C6_Q32;
        }
        return (uint32_t)PHASE_STEP_G5_Q32;
    }
    if (pat == CHIME_PATTERN_LISTEN_OPEN) {
        return (elapsed < 2880U) ? (uint32_t)PHASE_STEP_C5_Q32 : (uint32_t)PHASE_STEP_G5_Q32;
    }
    if (pat == CHIME_PATTERN_LISTEN_CLOSED) {
        return (elapsed < 1920U) ? (uint32_t)PHASE_STEP_G5_Q32 : (uint32_t)PHASE_STEP_C5_Q32;
    }
    /* CHIME_PATTERN_CALL_INCOMING */
    return (elapsed < (uint16_t)CHIME_CALL_T1_SAMPLES)
           ? (uint32_t)PHASE_STEP_G5_Q32
           : (uint32_t)PHASE_STEP_C6_Q32;
}

static void overlay_chime_samples(DuplexGate_Owned *ctx, PcmMonoSliceMut_Loaned speaker_out) {
    if (ctx->chime_samples_remaining == 0U) {
        return;
    }

    for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
        if (ctx->chime_samples_remaining == 0U) {
            ctx->active_chime = CHIME_PATTERN_NONE;
            break;
        }
        const uint16_t elapsed =
            (uint16_t)(ctx->chime_samples_total - ctx->chime_samples_remaining);
        const uint32_t step    = chime_step_for_progress(ctx->active_chime, elapsed);
        const size_t lut_idx   = (size_t)((ctx->chime_phase_q32 >> 26U) & 0x3FU);
        const int32_t mixed =
            (int32_t)speaker_out.samples[i] + (int32_t)SINE_TABLE_64[lut_idx];

        if (mixed > INT16_MAX) {
            speaker_out.samples[i] = INT16_MAX;
        } else if (mixed < INT16_MIN) {
            speaker_out.samples[i] = INT16_MIN;
        } else {
            speaker_out.samples[i] = (int16_t)mixed;
        }

        ctx->chime_phase_q32 += step;
        ctx->chime_samples_remaining = (uint16_t)(ctx->chime_samples_remaining - 1U);
    }
}

void DuplexGate_init(DuplexGate_Owned *ctx, DuplexGateConfigParams params) {
    assert(ctx != NULL);
    memset(ctx, 0, sizeof(*ctx));

    ctx->state           = DUPLEX_STATE_LISTENING;
    ctx->hangover_config = params.hangover_frames;
    ctx->chime_mode      = params.chime_mode;
    ctx->is_initialized  = true;
}

NODISCARD PttTransition DuplexGate_set_ptt(DuplexGate_Owned *ctx, PttButtonState button) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if (button == PTT_PRESSED) {
        if (ctx->state != DUPLEX_STATE_TRANSMITTING) {
            /* Manual PTT overrides any active Care-Listen window cleanly */
            ctx->state                      = DUPLEX_STATE_TRANSMITTING;
            ctx->hangover_remaining         = (SlotCount){ .count = 0U };
            ctx->auto_reply_pre_chime_ticks = (SlotCount){ .count = 0U };
            ctx->auto_reply_ticks_remaining = (SlotCount){ .count = 0U };
            ctx->chime_samples_remaining    = 0U;
            return PTT_TRANSITION_STARTED;
        }
        return PTT_TRANSITION_NONE;
    }

    /* PTT_RELEASED */
    if (ctx->state == DUPLEX_STATE_TRANSMITTING) {
        if (ctx->hangover_config.count > 0U) {
            ctx->state              = DUPLEX_STATE_HANGOVER;
            ctx->hangover_remaining = ctx->hangover_config;
        } else {
            ctx->state = DUPLEX_STATE_LISTENING;
        }
        return PTT_TRANSITION_ENDED;
    }

    return PTT_TRANSITION_NONE;
}

void DuplexGate_start_auto_reply(DuplexGate_Owned *ctx, SlotCount duration_ticks) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    uint16_t clamped = duration_ticks.count;
    if ((clamped == 0U) || (clamped > (uint16_t)UHURA_CARE_LISTEN_MAX_TICKS)) {
        clamped = (uint16_t)UHURA_CARE_LISTEN_MAX_TICKS;
    }

    /* Do not override if resident is actively holding local PTT */
    if (ctx->state == DUPLEX_STATE_TRANSMITTING) {
        return;
    }

    ctx->state                      = DUPLEX_STATE_AUTO_REPLY_TX;
    ctx->auto_reply_ticks_remaining = (SlotCount){ .count = clamped };
    ctx->auto_reply_just_expired    = false;

    if (ctx->chime_mode == CHIME_ENABLED) {
        ctx->auto_reply_pre_chime_ticks = (SlotCount){ .count = (uint16_t)CHIME_OPEN_TICKS };
        start_chime(ctx, CHIME_PATTERN_LISTEN_OPEN, (uint16_t)CHIME_OPEN_TOTAL);
    } else {
        ctx->auto_reply_pre_chime_ticks = (SlotCount){ .count = 0U };
    }

    assert(ctx->auto_reply_ticks_remaining.count <= (uint16_t)UHURA_CARE_LISTEN_MAX_TICKS);
}

void DuplexGate_stop_auto_reply(DuplexGate_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if (ctx->state == DUPLEX_STATE_AUTO_REPLY_TX) {
        ctx->state                      = DUPLEX_STATE_LISTENING;
        ctx->auto_reply_pre_chime_ticks = (SlotCount){ .count = 0U };
        ctx->auto_reply_ticks_remaining = (SlotCount){ .count = 0U };
        start_chime(ctx, CHIME_PATTERN_LISTEN_CLOSED, (uint16_t)CHIME_CLOSE_TOTAL);
    }
}

void DuplexGate_trigger_incoming_chime(DuplexGate_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if (ctx->state == DUPLEX_STATE_LISTENING) {
        start_chime(ctx, CHIME_PATTERN_CALL_INCOMING, (uint16_t)CHIME_CALL_TOTAL);
    }
}

void DuplexGate_trigger_care_alert_chime(DuplexGate_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if (ctx->state == DUPLEX_STATE_LISTENING) {
        start_chime(ctx, CHIME_PATTERN_CARE_ALERT, (uint16_t)CHIME_CARE_TOTAL);
    }
}

NODISCARD bool DuplexGate_process_10ms(
    DuplexGate_Owned *ctx,
    DuplexAudioFrameParams params
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    assert(params.far_end_speaker_in.samples != NULL);
    assert(params.near_end_mic_in.samples != NULL);
    assert(params.speaker_out.samples != NULL);
    assert(params.tx_mic_out.samples != NULL);
    assert(params.far_end_speaker_in.frames.count == (uint16_t)UHURA_SAMPLES_PER_FRAME);
    assert(params.near_end_mic_in.frames.count == (uint16_t)UHURA_SAMPLES_PER_FRAME);
    assert(params.speaker_out.frames.count == (uint16_t)UHURA_SAMPLES_PER_FRAME);
    assert(params.tx_mic_out.frames.count == (uint16_t)UHURA_SAMPLES_PER_FRAME);

    if (ctx->state == DUPLEX_STATE_TRANSMITTING) {
        memset(params.speaker_out.samples, 0, (size_t)UHURA_PCM_FRAME_BYTES);
        memcpy(params.tx_mic_out.samples,
               params.near_end_mic_in.samples,
               (size_t)UHURA_PCM_FRAME_BYTES);
        return true;
    }

    if (ctx->state == DUPLEX_STATE_AUTO_REPLY_TX) {
        assert(ctx->auto_reply_ticks_remaining.count <= (uint16_t)UHURA_CARE_LISTEN_MAX_TICKS);

        /* Phase A: Play "Listen Open" announcement chime BEFORE opening +24 dB mic */
        if (ctx->auto_reply_pre_chime_ticks.count > 0U) {
            memset(params.speaker_out.samples, 0, (size_t)UHURA_PCM_FRAME_BYTES);
            memset(params.tx_mic_out.samples, 0, (size_t)UHURA_PCM_FRAME_BYTES);
            overlay_chime_samples(ctx, params.speaker_out);
            ctx->auto_reply_pre_chime_ticks.count =
                (uint16_t)(ctx->auto_reply_pre_chime_ticks.count - 1U);
            return false;
        }

        /* Phase B: Mute speaker 100% (zero feedback!) and stream Far-Field mic */
        memset(params.speaker_out.samples, 0, (size_t)UHURA_PCM_FRAME_BYTES);
        memcpy(params.tx_mic_out.samples,
               params.near_end_mic_in.samples,
               (size_t)UHURA_PCM_FRAME_BYTES);

        if (ctx->auto_reply_ticks_remaining.count > 0U) {
            ctx->auto_reply_ticks_remaining.count =
                (uint16_t)(ctx->auto_reply_ticks_remaining.count - 1U);
        }

        if (ctx->auto_reply_ticks_remaining.count == 0U) {
            ctx->state                   = DUPLEX_STATE_LISTENING;
            ctx->auto_reply_just_expired = true;
            start_chime(ctx, CHIME_PATTERN_LISTEN_CLOSED, (uint16_t)CHIME_CLOSE_TOTAL);
        }
        return true;
    }

    /* Not transmitting: zero microphone outgoing buffer */
    memset(params.tx_mic_out.samples, 0, (size_t)UHURA_PCM_FRAME_BYTES);

    if (ctx->state == DUPLEX_STATE_HANGOVER) {
        memset(params.speaker_out.samples, 0, (size_t)UHURA_PCM_FRAME_BYTES);
        if (ctx->hangover_remaining.count > 0U) {
            ctx->hangover_remaining.count = (uint16_t)(ctx->hangover_remaining.count - 1U);
        }
        if (ctx->hangover_remaining.count == 0U) {
            ctx->state = DUPLEX_STATE_LISTENING;
        }
        return false;
    }

    /* DUPLEX_STATE_LISTENING */
    memcpy(params.speaker_out.samples,
           params.far_end_speaker_in.samples,
           (size_t)UHURA_PCM_FRAME_BYTES);
    overlay_chime_samples(ctx, params.speaker_out);
    return false;
}

NODISCARD bool DuplexGate_take_auto_reply_expired(DuplexGate_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    const bool expired = ctx->auto_reply_just_expired;
    ctx->auto_reply_just_expired = false;
    return expired;
}

NODISCARD DuplexState DuplexGate_state(const DuplexGate_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    return ctx->state;
}

NODISCARD bool DuplexGate_is_mic_hot(const DuplexGate_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    return (ctx->state == DUPLEX_STATE_TRANSMITTING) ||
           (ctx->state == DUPLEX_STATE_AUTO_REPLY_TX);
}

NODISCARD SlotCount DuplexGate_auto_reply_remaining(const DuplexGate_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if (ctx->state != DUPLEX_STATE_AUTO_REPLY_TX) {
        return (SlotCount){ .count = 0U };
    }
    return ctx->auto_reply_ticks_remaining;
}
