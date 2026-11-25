#include "core/duplex_gate.h"

#include <string.h>

/*
 * Pure integer 64-entry sine lookup table (amplitude peak 3600 / 32767 ≈ -19 dBFS)
 * Avoids floating-point / libm dependencies for the two-tone Enterprise chime.
 */
static const int16_t SINE_TABLE_64[64] = {
       0,   353,   702,  1045,  1378,  1698,  2000,  2283,
    2546,  2784,  2993,  3173,  3326,  3446,  3531,  3583,
    3600,  3583,  3531,  3446,  3326,  3173,  2993,  2784,
    2546,  2283,  2000,  1698,  1378,  1045,   702,   353,
       0,  -353,  -702, -1045, -1378, -1698, -2000, -2283,
   -2546, -2784, -2993, -3173, -3326, -3446, -3531, -3583,
   -3600, -3583, -3531, -3446, -3326, -3173, -2993, -2784,
   -2546, -2283, -2000, -1698, -1378, -1045,  -702,  -353
};

enum {
    /* 140 ms total: 55 ms G5 (~784 Hz) followed by 85 ms C6 (~1047 Hz) @ 48 kHz */
    CHIME_TONE1_SAMPLES = 2640U,
    CHIME_TOTAL_SAMPLES = 6720U,
    PHASE_STEP_G5_Q32   = 70149874U, /* round(784.0 / 48000.0 * 2^32) */
    PHASE_STEP_C6_Q32   = 93683978U  /* round(1047.0 / 48000.0 * 2^32) */
};

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
            ctx->state                   = DUPLEX_STATE_TRANSMITTING;
            ctx->hangover_remaining      = (SlotCount){ .count = 0U };
            ctx->chime_samples_remaining = 0U;
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

void DuplexGate_trigger_incoming_chime(DuplexGate_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if ((ctx->chime_mode == CHIME_ENABLED) && (ctx->state == DUPLEX_STATE_LISTENING)) {
        ctx->chime_samples_total     = (uint16_t)CHIME_TOTAL_SAMPLES;
        ctx->chime_samples_remaining = (uint16_t)CHIME_TOTAL_SAMPLES;
        ctx->chime_phase_q32         = 0U;
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
        /* Half-duplex TX: mute local speaker to prevent acoustic feedback loop */
        memset(params.speaker_out.samples, 0, (size_t)UHURA_PCM_FRAME_BYTES);
        memcpy(params.tx_mic_out.samples,
               params.near_end_mic_in.samples,
               (size_t)UHURA_PCM_FRAME_BYTES);
        return true;
    }

    /* Not transmitting: zero microphone outgoing buffer */
    memset(params.tx_mic_out.samples, 0, (size_t)UHURA_PCM_FRAME_BYTES);

    if (ctx->state == DUPLEX_STATE_HANGOVER) {
        /* Keep speaker muted briefly while room acoustic tail decays */
        memset(params.speaker_out.samples, 0, (size_t)UHURA_PCM_FRAME_BYTES);
        if (ctx->hangover_remaining.count > 0U) {
            ctx->hangover_remaining.count = (uint16_t)(ctx->hangover_remaining.count - 1U);
        }
        if (ctx->hangover_remaining.count == 0U) {
            ctx->state = DUPLEX_STATE_LISTENING;
        }
        return false;
    }

    /* DUPLEX_STATE_LISTENING: pass far-end intercom audio to speaker */
    memcpy(params.speaker_out.samples,
           params.far_end_speaker_in.samples,
           (size_t)UHURA_PCM_FRAME_BYTES);

    /* Overlay two-tone Enterprise whistle chime if active (bounded loop: 480 samples) */
    if (ctx->chime_samples_remaining > 0U) {
        for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
            if (ctx->chime_samples_remaining == 0U) {
                break;
            }
            const uint16_t elapsed =
                (uint16_t)(ctx->chime_samples_total - ctx->chime_samples_remaining);
            const uint32_t step = (elapsed < (uint16_t)CHIME_TONE1_SAMPLES)
                                  ? (uint32_t)PHASE_STEP_G5_Q32
                                  : (uint32_t)PHASE_STEP_C6_Q32;

            const size_t lut_idx = (size_t)((ctx->chime_phase_q32 >> 26U) & 0x3FU);
            const int32_t mixed =
                (int32_t)params.speaker_out.samples[i] + (int32_t)SINE_TABLE_64[lut_idx];

            if (mixed > INT16_MAX) {
                params.speaker_out.samples[i] = INT16_MAX;
            } else if (mixed < INT16_MIN) {
                params.speaker_out.samples[i] = INT16_MIN;
            } else {
                params.speaker_out.samples[i] = (int16_t)mixed;
            }

            ctx->chime_phase_q32 += step;
            ctx->chime_samples_remaining = (uint16_t)(ctx->chime_samples_remaining - 1U);
        }
    }

    return false;
}

NODISCARD DuplexState DuplexGate_state(const DuplexGate_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    return ctx->state;
}
