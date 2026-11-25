#ifndef UHURA_CORE_DUPLEX_GATE_H
#define UHURA_CORE_DUPLEX_GATE_H

#include "core/types.h"

/* =========================================================================
 * PTT Duplex Gate, Bosun-Whistle Chime & Phase-2 AEC Hook (Rule 4 & Rule 7)
 *
 * Phase 1: Half-duplex PTT state machine (mutes local speaker while talking +
 *          30 ms room-reverb hangover on PTT release).
 * Phase 2: Exact same 10 ms frame signature hosts Acoustic Echo Cancellation
 *          using aligned speaker_reference and mic_capture_inout slices.
 * ========================================================================= */
enum {
    UHURA_DEFAULT_HANGOVER_FRAMES = 3U /* 30 ms cooldown after releasing PTT */
};

typedef enum {
    PTT_RELEASED = 0,
    PTT_PRESSED  = 1
} PttButtonState;

typedef enum {
    CHIME_DISABLED = 0,
    CHIME_ENABLED  = 1
} ChimeMode;

typedef enum {
    DUPLEX_STATE_LISTENING = 0,
    DUPLEX_STATE_TRANSMITTING,
    DUPLEX_STATE_HANGOVER
} DuplexState;

typedef enum {
    PTT_TRANSITION_NONE = 0,
    PTT_TRANSITION_STARTED,
    PTT_TRANSITION_ENDED
} PttTransition;

typedef struct {
    SlotCount hangover_frames;
    ChimeMode chime_mode;
} DuplexGateConfigParams;

typedef struct {
    PcmMonoSlice_Loaned    far_end_speaker_in; /* Mixed incoming intercom audio */
    PcmMonoSlice_Loaned    near_end_mic_in;    /* Raw microphone capture */
    PcmMonoSliceMut_Loaned speaker_out;        /* Filtered/gated output to ALSA speaker */
    PcmMonoSliceMut_Loaned tx_mic_out;         /* Filtered/gated output to Opus encoder */
} DuplexAudioFrameParams;

typedef struct {
    DuplexState state;
    SlotCount   hangover_config;
    SlotCount   hangover_remaining;
    ChimeMode   chime_mode;
    uint16_t    chime_samples_remaining;
    uint16_t    chime_samples_total;
    uint32_t    chime_phase_q32;
    bool        is_initialized;
} DuplexGate_Owned;

void DuplexGate_init(DuplexGate_Owned *ctx, DuplexGateConfigParams params);

NODISCARD PttTransition DuplexGate_set_ptt(DuplexGate_Owned *ctx, PttButtonState button);

/* Triggers the classic two-tone Enterprise incoming-call whistle when listening */
void DuplexGate_trigger_incoming_chime(DuplexGate_Owned *ctx);

/*
 * Processes one 10 ms (480-sample) audio tick.
 * Returns true if tx_mic_out contains active speech to be encoded and transmitted.
 */
NODISCARD bool DuplexGate_process_10ms(
    DuplexGate_Owned *ctx,
    DuplexAudioFrameParams params
);

NODISCARD DuplexState DuplexGate_state(const DuplexGate_Owned *ctx);

#endif /* UHURA_CORE_DUPLEX_GATE_H */
