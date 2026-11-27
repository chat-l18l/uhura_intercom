#ifndef UHURA_CORE_DUPLEX_GATE_H
#define UHURA_CORE_DUPLEX_GATE_H

#include "core/types.h"

/* =========================================================================
 * PTT Duplex Gate, Care-Listen 30s Auto-Reply, Chimes & Phase-2 AEC Hook
 *
 * States:
 * - DUPLEX_STATE_LISTENING:      Speaker active, mic muted.
 * - DUPLEX_STATE_TRANSMITTING:   Manual PTT held; speaker muted, mic hot.
 * - DUPLEX_STATE_HANGOVER:       30 ms room-reverb mute after PTT release.
 * - DUPLEX_STATE_AUTO_REPLY_TX:  Bounded 30s Care-Listen window; plays short
 *   pre-open tone first (while mic still muted so +24 dB AGC never echoes its
 *   own chime!), then mutes speaker and streams mic for <= 3000 ticks (30.0 s).
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
    DUPLEX_STATE_HANGOVER,
    DUPLEX_STATE_AUTO_REPLY_TX
} DuplexState;

typedef enum {
    PTT_TRANSITION_NONE = 0,
    PTT_TRANSITION_STARTED,
    PTT_TRANSITION_ENDED
} PttTransition;

typedef enum {
    CHIME_PATTERN_NONE = 0,
    CHIME_PATTERN_CALL_INCOMING,
    CHIME_PATTERN_CARE_ALERT,
    CHIME_PATTERN_LISTEN_OPEN,
    CHIME_PATTERN_LISTEN_CLOSED
} ChimePattern;

typedef struct {
    SlotCount hangover_frames;
    ChimeMode chime_mode;
} DuplexGateConfigParams;

typedef struct {
    PcmMonoSlice_Loaned    far_end_speaker_in; /* Mixed incoming intercom audio */
    PcmMonoSlice_Loaned    near_end_mic_in;    /* Conditioned microphone capture */
    PcmMonoSliceMut_Loaned speaker_out;        /* Filtered/gated output to speaker */
    PcmMonoSliceMut_Loaned tx_mic_out;         /* Filtered/gated output to Opus encoder */
} DuplexAudioFrameParams;

typedef struct {
    DuplexState  state;
    SlotCount    hangover_config;
    SlotCount    hangover_remaining;
    SlotCount    auto_reply_pre_chime_ticks; /* Speaker announces before +24dB mic opens */
    SlotCount    auto_reply_ticks_remaining; /* Bounded <= UHURA_CARE_LISTEN_MAX_TICKS */
    ChimeMode    chime_mode;
    ChimePattern active_chime;
    uint16_t     chime_samples_remaining;
    uint16_t     chime_samples_total;
    uint32_t     chime_phase_q32;
    bool         auto_reply_just_expired;
    bool         is_initialized;
} DuplexGate_Owned;

void DuplexGate_init(DuplexGate_Owned *ctx, DuplexGateConfigParams params);

NODISCARD PttTransition DuplexGate_set_ptt(DuplexGate_Owned *ctx, PttButtonState button);

/* Starts bounded Care-Listen auto-reply window (clamped to <= 3000 ticks = 30.0 s) */
void DuplexGate_start_auto_reply(DuplexGate_Owned *ctx, SlotCount duration_ticks);

/* Immediately cancels an active Care-Listen window (e.g. local button press or ABORT) */
void DuplexGate_stop_auto_reply(DuplexGate_Owned *ctx);

/* Triggers standard incoming-call whistle or louder Care-Call alert chime */
void DuplexGate_trigger_incoming_chime(DuplexGate_Owned *ctx);
void DuplexGate_trigger_care_alert_chime(DuplexGate_Owned *ctx);

/*
 * Processes one 10 ms (480-sample) audio tick.
 * Returns true if tx_mic_out contains active audio to be encoded and transmitted.
 */
NODISCARD bool DuplexGate_process_10ms(
    DuplexGate_Owned *ctx,
    DuplexAudioFrameParams params
);

/* Consumes one-shot flag indicating the 30s Care-Listen timer just finished */
NODISCARD bool DuplexGate_take_auto_reply_expired(DuplexGate_Owned *ctx);

NODISCARD DuplexState DuplexGate_state(const DuplexGate_Owned *ctx);
NODISCARD bool DuplexGate_is_mic_hot(const DuplexGate_Owned *ctx);
NODISCARD SlotCount DuplexGate_auto_reply_remaining(const DuplexGate_Owned *ctx);

#endif /* UHURA_CORE_DUPLEX_GATE_H */
