#ifndef UHURA_CORE_INTERCOM_CORE_H
#define UHURA_CORE_INTERCOM_CORE_H

#include "core/audio_dsp.h"
#include "core/duplex_gate.h"
#include "core/jitter_buffer.h"
#include "core/opus_codec.h"
#include "core/types.h"
#include "core/wire_proto.h"

/* =========================================================================
 * Top-Level Pure Intercom Orchestrator (Rule 1, Rule 4, Rule 6 & Rule 7)
 *
 * Includes:
 * - Multi-station PTT fan-out + 4-stream RX mixing
 * - Authorized "Roep & Luister" Care Call with:
 *     * Local per-peer permission matrix (default LISTEN_PERM_DENIED)
 *     * Anti-replay 64-bit ChallengeNonce handshake
 *     * Emergency 100% Volume Override during incoming Care Call
 *     * Bounded <= 30s Far-Field (+24 dB, OPUS_SIGNAL_AUTO) return window
 *     * Per-room audible sound detection for multi-box homes
 * ========================================================================= */
enum {
    UHURA_TX_OUTBOX_CAPACITY       = 16U,
    UHURA_HEARTBEAT_INTERVAL_US    = 5000000ULL,  /* 5 seconds */
    UHURA_PEER_ONLINE_TIMEOUT_US   = 16000000ULL, /* 16 seconds */
    UHURA_CHALLENGE_TIMEOUT_US     = 2000000ULL,  /* 2 seconds max RTT for challenge */
    UHURA_CARE_OVERRIDE_HOLD_US    = 3000000ULL   /* Hold 100% volume until jitter drains */
};

typedef enum {
    CORE_OK = 0,
    CORE_ERR_INVALID_STATION,
    CORE_ERR_UNKNOWN_PEER,
    CORE_ERR_CODEC_INIT,
    CORE_ERR_DUPLICATE_PEER,
    CORE_ERR_OUTBOX_EMPTY
} CoreError;

typedef enum {
    LISTEN_PERM_DENIED  = 0, /* Default: remote box can NEVER open our microphone */
    LISTEN_PERM_ALLOWED = 1  /* Explicitly authorized on resident's box (e.g. son/daughter) */
} PeerListenPermission;

typedef enum {
    CALL_KIND_NORMAL_PTT  = 0,
    CALL_KIND_CARE_LISTEN = 1 /* Emergency volume + request 30s listen window */
} CallKind;

typedef struct {
    TargetPeerMask     recipients;
    WireDatagram_Owned datagram;
    bool               is_valid;
} TxEnvelope_Owned;

typedef struct {
    TxEnvelope_Owned items[UHURA_TX_OUTBOX_CAPACITY];
    uint16_t         head;
    uint16_t         tail;
    uint16_t         count;
} TxOutbox_Owned;

typedef struct {
    StationId            station;
    char                 name[UHURA_STATION_NAME_MAX];
    MonotonicTimeUs      last_seen;
    PeerListenPermission listen_perm;
    bool                 is_configured;
    bool                 ever_seen;
} PeerInfo;

typedef struct {
    StationId          sender;
    JitterBuffer_Owned jitter;
    OpusDecoder_Owned  decoder;
    MonotonicTimeUs    last_packet_time;
    uint16_t           recent_peak;
    bool               has_audible_sound;
    bool               is_bound;
} RxStreamSlot_Owned;

typedef struct {
    StationId       challenger;
    TalkSessionId   session;
    ChallengeNonce  nonce;
    MonotonicTimeUs expires_at;
    bool            is_pending;
} PendingListenChallenge;

typedef struct {
    StationId     local_station;
    BitrateBps    opus_bitrate;
    InbandFecMode fec_mode;
    SlotCount     jitter_prefetch;
    SlotCount     hangover_frames;
    ChimeMode     chime_mode;
} IntercomConfigParams;

typedef struct {
    StationId   station;
    const char *name;
} PeerRegisterParams;

typedef struct {
    StationId            station;
    PeerListenPermission permission;
} PeerPermissionParams;

typedef struct {
    PttButtonState button;
    CallKind       call_kind;
} PttCommandParams;

typedef struct {
    PcmMonoSlice_Loaned mic_pcm;
    MonotonicTimeUs     now;
} IntercomTickParams;

typedef struct {
    ByteSlice_Loaned raw_datagram;
    MonotonicTimeUs  now;
} IntercomRxDatagramParams;

typedef struct {
    StationId              local_station;
    TargetPeerMask         configured_peers;
    TargetPeerMask         selected_targets;
    TalkSessionId          tx_session;
    SeqNum                 tx_seq;
    CallKind               active_tx_call_kind;
    TargetPeerMask         care_call_targets;
    TalkSessionId          care_call_session;
    StationId              authorized_listener; /* Exclusive recipient during 30s window */
    StationId              last_challenged_sender;
    TalkSessionId          last_challenged_session;
    PendingListenChallenge pending_challenge;
    SlotCount              care_listen_duration; /* Default 3000 ticks = 30.0 s */
    uint64_t               nonce_counter;
    MonotonicTimeUs        care_volume_override_until;
    MonotonicTimeUs        last_heartbeat_tx;
    PeerInfo               peers[UHURA_MAX_PEERS];
    AudioDsp_Owned         audio_dsp;
    DuplexGate_Owned       duplex_gate;
    OpusEncoder_Owned      encoder;
    RxStreamSlot_Owned     rx_streams[UHURA_MAX_RX_STREAMS];
    TxOutbox_Owned         tx_outbox;
    uint32_t               rx_dropped_packets;
    uint32_t               unauthorized_listen_attempts;
    bool                   is_initialized;
} IntercomCore_Owned;

NODISCARD CoreError IntercomCore_init(
    IntercomCore_Owned *ctx,
    IntercomConfigParams params
);

NODISCARD CoreError IntercomCore_register_peer(
    IntercomCore_Owned *ctx,
    PeerRegisterParams params
);

/* Grants or revokes local permission for a specific peer to open the 30s Care-Listen window */
NODISCARD CoreError IntercomCore_set_peer_listen_permission(
    IntercomCore_Owned *ctx,
    PeerPermissionParams params
);

/* Configures bounded Care-Listen duration (clamped to 1..3000 ticks = max 30.0 s) */
void IntercomCore_set_care_listen_duration(
    IntercomCore_Owned *ctx,
    SlotCount duration_ticks
);

void IntercomCore_set_user_volume(
    IntercomCore_Owned *ctx,
    VolumePercent volume
);

NODISCARD VolumePercent IntercomCore_effective_volume(const IntercomCore_Owned *ctx);

void IntercomCore_set_targets(
    IntercomCore_Owned *ctx,
    TargetPeerMask targets
);

NODISCARD TargetPeerMask IntercomCore_selected_targets(const IntercomCore_Owned *ctx);

/* Normal PTT convenience wrapper */
void IntercomCore_set_ptt(
    IntercomCore_Owned *ctx,
    PttButtonState button
);

/* PTT with explicit CallKind (CALL_KIND_NORMAL_PTT vs CALL_KIND_CARE_LISTEN) */
void IntercomCore_set_ptt_command(
    IntercomCore_Owned *ctx,
    PttCommandParams params
);

/* Immediately terminates any active 30s Care-Listen window (local or remote) */
void IntercomCore_abort_care_listen(IntercomCore_Owned *ctx);

void IntercomCore_on_rx_datagram(
    IntercomCore_Owned *ctx,
    IntercomRxDatagramParams params
);

void IntercomCore_tick_10ms(
    IntercomCore_Owned * restrict ctx,
    IntercomTickParams params,
    PcmFrame_Owned * restrict out_speaker_pcm
);

NODISCARD CoreError IntercomCore_pop_tx_move(
    IntercomCore_Owned * restrict ctx,
    TxEnvelope_Owned * restrict out_tx
);

NODISCARD bool IntercomCore_is_peer_online(
    const IntercomCore_Owned *ctx,
    StationId peer,
    MonotonicTimeUs now
);

/* Bitmask of stations with an active RX stream */
NODISCARD TargetPeerMask IntercomCore_active_talkers(const IntercomCore_Owned *ctx);

/* Bitmask of stations currently producing audible sound/knocks above room silence */
NODISCARD TargetPeerMask IntercomCore_audible_talkers(const IntercomCore_Owned *ctx);

#endif /* UHURA_CORE_INTERCOM_CORE_H */
