#ifndef UHURA_CORE_INTERCOM_CORE_H
#define UHURA_CORE_INTERCOM_CORE_H

#include "core/duplex_gate.h"
#include "core/jitter_buffer.h"
#include "core/opus_codec.h"
#include "core/types.h"
#include "core/wire_proto.h"

/* =========================================================================
 * Top-Level Pure Intercom Orchestrator (Rule 1, Rule 4, Rule 6 & Rule 7)
 *
 * Completely platform-free: accepts 10 ms PCM frames, PTT commands, and raw
 * UDP slices; produces mixed speaker PCM frames and queued UDP WireDatagrams
 * annotated with destination TargetPeerMask for unicast fan-out.
 * ========================================================================= */
enum {
    UHURA_TX_OUTBOX_CAPACITY      = 16U,
    UHURA_HEARTBEAT_INTERVAL_US   = 5000000ULL, /* 5 seconds (keeps WireGuard alive) */
    UHURA_PEER_ONLINE_TIMEOUT_US  = 16000000ULL /* 16 seconds (> 3 missed heartbeats) */
};

typedef enum {
    CORE_OK = 0,
    CORE_ERR_INVALID_STATION,
    CORE_ERR_CODEC_INIT,
    CORE_ERR_DUPLICATE_PEER,
    CORE_ERR_OUTBOX_EMPTY
} CoreError;

typedef struct {
    TargetPeerMask     recipients; /* Configured peers to send this UDP datagram to */
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
    StationId       station;
    char            name[UHURA_STATION_NAME_MAX];
    MonotonicTimeUs last_seen;
    bool            is_configured;
    bool            ever_seen;
} PeerInfo;

typedef struct {
    StationId          sender;
    JitterBuffer_Owned jitter;
    OpusDecoder_Owned  decoder;
    MonotonicTimeUs    last_packet_time;
    bool               is_bound;
} RxStreamSlot_Owned;

typedef struct {
    StationId       local_station;
    BitrateBps      opus_bitrate;
    InbandFecMode   fec_mode;
    SlotCount       jitter_prefetch;
    SlotCount       hangover_frames;
    ChimeMode       chime_mode;
} IntercomConfigParams;

typedef struct {
    StationId station;
    const char *name; /* Copied safely into fixed-size PeerInfo.name */
} PeerRegisterParams;

typedef struct {
    PcmMonoSlice_Loaned mic_pcm;
    MonotonicTimeUs     now;
} IntercomTickParams;

typedef struct {
    ByteSlice_Loaned raw_datagram;
    MonotonicTimeUs  now;
} IntercomRxDatagramParams;

typedef struct {
    StationId          local_station;
    TargetPeerMask     configured_peers;
    TargetPeerMask     selected_targets;
    TalkSessionId      tx_session;
    SeqNum             tx_seq;
    MonotonicTimeUs    last_heartbeat_tx;
    PeerInfo           peers[UHURA_MAX_PEERS];
    DuplexGate_Owned   duplex_gate;
    OpusEncoder_Owned  encoder;
    RxStreamSlot_Owned rx_streams[UHURA_MAX_RX_STREAMS];
    TxOutbox_Owned     tx_outbox;
    uint32_t           rx_dropped_packets;
    bool               is_initialized;
} IntercomCore_Owned;

NODISCARD CoreError IntercomCore_init(
    IntercomCore_Owned *ctx,
    IntercomConfigParams params
);

NODISCARD CoreError IntercomCore_register_peer(
    IntercomCore_Owned *ctx,
    PeerRegisterParams params
);

void IntercomCore_set_targets(
    IntercomCore_Owned *ctx,
    TargetPeerMask targets
);

NODISCARD TargetPeerMask IntercomCore_selected_targets(const IntercomCore_Owned *ctx);

void IntercomCore_set_ptt(
    IntercomCore_Owned *ctx,
    PttButtonState button
);

void IntercomCore_on_rx_datagram(
    IntercomCore_Owned *ctx,
    IntercomRxDatagramParams params
);

/*
 * Advances the entire audio & protocol engine by 10 ms (480 samples):
 * - Decodes & mixes up to 4 simultaneous incoming talkers (with FEC/PLC)
 * - Runs PTT DuplexGate (+ incoming whistle chime)
 * - Encodes & queues outgoing AUDIO packet if transmitting
 * - Queues periodic HEARTBEAT packets for WireGuard/peer liveness
 */
void IntercomCore_tick_10ms(
    IntercomCore_Owned * restrict ctx,
    IntercomTickParams params,
    PcmFrame_Owned * restrict out_speaker_pcm
);

/* Moves the oldest queued outgoing UDP datagram + destination mask into *out_tx */
NODISCARD CoreError IntercomCore_pop_tx_move(
    IntercomCore_Owned * restrict ctx,
    TxEnvelope_Owned * restrict out_tx
);

NODISCARD bool IntercomCore_is_peer_online(
    const IntercomCore_Owned *ctx,
    StationId peer,
    MonotonicTimeUs now
);

/* Returns bitmask of stations currently speaking to this node */
NODISCARD TargetPeerMask IntercomCore_active_talkers(const IntercomCore_Owned *ctx);

#endif /* UHURA_CORE_INTERCOM_CORE_H */
