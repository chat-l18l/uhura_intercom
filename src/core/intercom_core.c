#include "core/intercom_core.h"

#include <string.h>

enum {
    AUDIBLE_SOUND_PEAK_THRESHOLD = 450U
};

static void copy_station_name(char dst[UHURA_STATION_NAME_MAX], const char *src) {
    assert(dst != NULL);
    memset(dst, 0, (size_t)UHURA_STATION_NAME_MAX);
    if (src == NULL) {
        return;
    }
    for (size_t i = 0U; i < ((size_t)UHURA_STATION_NAME_MAX - 1U); ++i) {
        if (src[i] == '\0') {
            break;
        }
        dst[i] = src[i];
    }
}

/* SplitMix64 mixing function for deterministic ChallengeNonce generation & verification */
static uint64_t splitmix64(uint64_t x) {
    uint64_t z = x + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

static ChallengeNonce generate_fresh_nonce(
    IntercomCore_Owned *ctx,
    StationId challenger,
    MonotonicTimeUs now
) {
    assert(ctx != NULL);
    ctx->nonce_counter += 1ULL;
    const uint64_t seed =
        now.us ^
        (ctx->nonce_counter * 0xD6E8FEB86659FD93ULL) ^
        ((uint64_t)ctx->local_station.id << 48U) ^
        ((uint64_t)challenger.id << 40U);
    uint64_t mixed = splitmix64(seed);
    if (mixed == 0ULL) {
        mixed = 1ULL;
    }
    return (ChallengeNonce){ .val = mixed };
}

static ChallengeNonce compute_challenge_response(
    ChallengeNonce challenge,
    TalkSessionId session,
    StationId caller_station,
    StationId resident_station
) {
    const uint64_t domain_bind =
        challenge.val ^
        ((uint64_t)session.id << 16U) ^
        ((uint64_t)caller_station.id << 8U) ^
        (uint64_t)resident_station.id ^
        0x5548555243415245ULL; /* "UHURCARE" domain separator */
    return (ChallengeNonce){ .val = splitmix64(domain_bind) };
}

static bool is_peer_authorized_for_listen(const IntercomCore_Owned *ctx, StationId peer) {
    assert(ctx != NULL);
    if (!StationId_is_valid(peer)) {
        return false;
    }
    const PeerInfo *info = &ctx->peers[peer.id - 1U];
    return info->is_configured && (info->listen_perm == LISTEN_PERM_ALLOWED);
}

static uint16_t measure_frame_peak(const PcmFrame_Owned *frame) {
    assert(frame != NULL);
    uint16_t peak = 0U;
    for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
        const int32_t v = (int32_t)frame->samples[i];
        const uint16_t mag = (v < 0) ? (uint16_t)(-v) : (uint16_t)v;
        if (mag > peak) {
            peak = mag;
        }
    }
    return peak;
}

static void restore_normal_capture_and_volume(IntercomCore_Owned *ctx) {
    assert(ctx != NULL);
    AudioDsp_set_capture_mode(&ctx->audio_dsp, CAPTURE_MODE_NORMAL_VOICE);
    OpusEncoder_set_capture_mode(&ctx->encoder, CAPTURE_MODE_NORMAL_VOICE);
    ctx->authorized_listener = (StationId){ .id = 0U };
}

static void outbox_push_move(
    TxOutbox_Owned * restrict outbox,
    TargetPeerMask recipients,
    WireDatagram_Owned * restrict dgram
) {
    assert(outbox != NULL);
    assert(dgram != NULL);
    assert(dgram->is_valid);

    if (recipients.bits == 0U) {
        *dgram = (WireDatagram_Owned){0};
        return;
    }

    if (outbox->count >= (uint16_t)UHURA_TX_OUTBOX_CAPACITY) {
        outbox->items[outbox->tail] = (TxEnvelope_Owned){0};
        outbox->tail  = (uint16_t)((outbox->tail + 1U) % (uint16_t)UHURA_TX_OUTBOX_CAPACITY);
        outbox->count = (uint16_t)(outbox->count - 1U);
    }

    TxEnvelope_Owned *slot = &outbox->items[outbox->head];
    slot->recipients = recipients;
    WireDatagram_move(&slot->datagram, dgram);
    slot->is_valid = true;

    outbox->head  = (uint16_t)((outbox->head + 1U) % (uint16_t)UHURA_TX_OUTBOX_CAPACITY);
    outbox->count = (uint16_t)(outbox->count + 1U);
}

static void queue_control_packet_flagged(
    IntercomCore_Owned *ctx,
    PacketKind kind,
    TargetPeerMask recipients,
    WireFlags extra_flags
) {
    assert(ctx != NULL);
    if (recipients.bits == 0U) {
        return;
    }

    const bool is_all_call =
        (ctx->configured_peers.bits != 0U) &&
        ((recipients.bits & ctx->configured_peers.bits) == ctx->configured_peers.bits);

    uint8_t flag_bits = extra_flags.bits;
    if (is_all_call) {
        flag_bits = (uint8_t)(flag_bits | (uint8_t)WIRE_FLAG_ALL_CALL);
    }

    WireDatagram_Owned dgram = {0};
    const WireError err = WireProto_serialize(&dgram, (WireSerializeParams){
        .kind    = kind,
        .sender  = ctx->local_station,
        .flags   = { .bits = flag_bits },
        .session = ctx->tx_session,
        .seq     = ctx->tx_seq,
        .targets = recipients,
        .payload = { .data = NULL, .len = 0U }
    });
    if (err == WIRE_OK) {
        outbox_push_move(&ctx->tx_outbox, recipients, &dgram);
    }
}

static RxStreamSlot_Owned *find_or_bind_rx_slot(
    IntercomCore_Owned *ctx,
    StationId sender,
    MonotonicTimeUs now
) {
    assert(ctx != NULL);

    for (size_t i = 0U; i < (size_t)UHURA_MAX_RX_STREAMS; ++i) {
        if (ctx->rx_streams[i].is_bound && (ctx->rx_streams[i].sender.id == sender.id)) {
            ctx->rx_streams[i].last_packet_time = now;
            return &ctx->rx_streams[i];
        }
    }

    for (size_t i = 0U; i < (size_t)UHURA_MAX_RX_STREAMS; ++i) {
        if (!ctx->rx_streams[i].is_bound || !JitterBuffer_is_active(&ctx->rx_streams[i].jitter)) {
            RxStreamSlot_Owned *slot = &ctx->rx_streams[i];
            JitterBuffer_reset(&slot->jitter);
            OpusDecoder_reset(&slot->decoder);
            slot->sender            = sender;
            slot->last_packet_time  = now;
            slot->recent_peak       = 0U;
            slot->has_audible_sound = false;
            slot->is_bound          = true;
            return slot;
        }
    }

    size_t oldest_idx = 0U;
    uint64_t oldest_time = ctx->rx_streams[0].last_packet_time.us;
    for (size_t i = 1U; i < (size_t)UHURA_MAX_RX_STREAMS; ++i) {
        if (ctx->rx_streams[i].last_packet_time.us < oldest_time) {
            oldest_time = ctx->rx_streams[i].last_packet_time.us;
            oldest_idx  = i;
        }
    }

    if ((now.us > oldest_time) && ((now.us - oldest_time) > 500000ULL)) {
        RxStreamSlot_Owned *slot = &ctx->rx_streams[oldest_idx];
        JitterBuffer_reset(&slot->jitter);
        OpusDecoder_reset(&slot->decoder);
        slot->sender            = sender;
        slot->last_packet_time  = now;
        slot->recent_peak       = 0U;
        slot->has_audible_sound = false;
        slot->is_bound          = true;
        return slot;
    }

    return NULL;
}

NODISCARD CoreError IntercomCore_init(
    IntercomCore_Owned *ctx,
    IntercomConfigParams params
) {
    assert(ctx != NULL);
    memset(ctx, 0, sizeof(*ctx));

    if (!StationId_is_valid(params.local_station)) {
        return CORE_ERR_INVALID_STATION;
    }

    ctx->local_station        = params.local_station;
    ctx->tx_session           = (TalkSessionId){ .id = 0U };
    ctx->tx_seq               = (SeqNum){ .num = 0U };
    ctx->care_listen_duration = (SlotCount){ .count = (uint16_t)UHURA_CARE_LISTEN_MAX_TICKS };

    AudioDsp_init(&ctx->audio_dsp, (VolumePercent){ .pct = 100U });

    const BitrateBps bitrate = (params.opus_bitrate.bps == 0U)
                               ? (BitrateBps){ .bps = (uint32_t)UHURA_DEFAULT_BITRATE_BPS }
                               : params.opus_bitrate;

    const CodecError enc_err = OpusEncoder_init(&ctx->encoder, (OpusEncoderConfigParams){
        .bitrate  = bitrate,
        .fec_mode = params.fec_mode
    });
    if (enc_err != CODEC_OK) {
        return CORE_ERR_CODEC_INIT;
    }

    const SlotCount prefetch = (params.jitter_prefetch.count == 0U)
                               ? (SlotCount){ .count = (uint16_t)UHURA_DEFAULT_PREFETCH_SLOTS }
                               : params.jitter_prefetch;

    for (size_t i = 0U; i < (size_t)UHURA_MAX_RX_STREAMS; ++i) {
        JitterBuffer_init(&ctx->rx_streams[i].jitter, (JitterConfigParams){
            .target_prefetch = prefetch,
            .max_plc_frames  = { .count = (uint16_t)UHURA_MAX_CONSECUTIVE_PLC }
        });
        const CodecError dec_err = OpusDecoder_init(&ctx->rx_streams[i].decoder);
        if (dec_err != CODEC_OK) {
            return CORE_ERR_CODEC_INIT;
        }
    }

    DuplexGate_init(&ctx->duplex_gate, (DuplexGateConfigParams){
        .hangover_frames = params.hangover_frames,
        .chime_mode      = params.chime_mode
    });

    ctx->is_initialized = true;
    return CORE_OK;
}

NODISCARD CoreError IntercomCore_register_peer(
    IntercomCore_Owned *ctx,
    PeerRegisterParams params
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if (!StationId_is_valid(params.station) ||
        (params.station.id == ctx->local_station.id)) {
        return CORE_ERR_INVALID_STATION;
    }

    const size_t idx = (size_t)(params.station.id - 1U);
    if (ctx->peers[idx].is_configured) {
        return CORE_ERR_DUPLICATE_PEER;
    }

    ctx->peers[idx].station       = params.station;
    ctx->peers[idx].listen_perm   = LISTEN_PERM_DENIED; /* Hard default: NEVER allow remote mic */
    ctx->peers[idx].is_configured = true;
    ctx->peers[idx].ever_seen     = false;
    ctx->peers[idx].last_seen     = (MonotonicTimeUs){ .us = 0ULL };
    copy_station_name(ctx->peers[idx].name, params.name);

    ctx->configured_peers = TargetPeerMask_with_station(ctx->configured_peers, params.station);

    if (ctx->selected_targets.bits == 0U) {
        ctx->selected_targets = TargetPeerMask_from_station(params.station);
    }

    return CORE_OK;
}

NODISCARD CoreError IntercomCore_set_peer_listen_permission(
    IntercomCore_Owned *ctx,
    PeerPermissionParams params
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if (!StationId_is_valid(params.station)) {
        return CORE_ERR_INVALID_STATION;
    }
    PeerInfo *peer = &ctx->peers[params.station.id - 1U];
    if (!peer->is_configured) {
        return CORE_ERR_UNKNOWN_PEER;
    }

    peer->listen_perm = params.permission;

    /* If revoking permission while this peer had an active window, terminate immediately! */
    if ((params.permission == LISTEN_PERM_DENIED) &&
        (ctx->authorized_listener.id == params.station.id)) {
        IntercomCore_abort_care_listen(ctx);
    }

    return CORE_OK;
}

void IntercomCore_set_care_listen_duration(
    IntercomCore_Owned *ctx,
    SlotCount duration_ticks
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    uint16_t clamped = duration_ticks.count;
    if ((clamped == 0U) || (clamped > (uint16_t)UHURA_CARE_LISTEN_MAX_TICKS)) {
        clamped = (uint16_t)UHURA_CARE_LISTEN_MAX_TICKS;
    }
    ctx->care_listen_duration = (SlotCount){ .count = clamped };
}

void IntercomCore_set_user_volume(
    IntercomCore_Owned *ctx,
    VolumePercent volume
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    AudioDsp_set_user_volume(&ctx->audio_dsp, volume);
}

NODISCARD VolumePercent IntercomCore_effective_volume(const IntercomCore_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    return AudioDsp_effective_volume(&ctx->audio_dsp);
}

void IntercomCore_set_targets(
    IntercomCore_Owned *ctx,
    TargetPeerMask targets
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    ctx->selected_targets = (TargetPeerMask){
        .bits = (uint16_t)(targets.bits & ctx->configured_peers.bits)
    };
}

NODISCARD TargetPeerMask IntercomCore_selected_targets(const IntercomCore_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    return ctx->selected_targets;
}

void IntercomCore_set_ptt(
    IntercomCore_Owned *ctx,
    PttButtonState button
) {
    IntercomCore_set_ptt_command(ctx, (PttCommandParams){
        .button    = button,
        .call_kind = CALL_KIND_NORMAL_PTT
    });
}

void IntercomCore_set_ptt_command(
    IntercomCore_Owned *ctx,
    PttCommandParams params
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if ((params.button == PTT_PRESSED) && (ctx->selected_targets.bits == 0U)) {
        return;
    }

    /* Local PTT press cancels any active remote Care-Listen window cleanly */
    if ((params.button == PTT_PRESSED) &&
        (DuplexGate_state(&ctx->duplex_gate) == DUPLEX_STATE_AUTO_REPLY_TX)) {
        restore_normal_capture_and_volume(ctx);
    }

    const PttTransition tr = DuplexGate_set_ptt(&ctx->duplex_gate, params.button);
    if (tr == PTT_TRANSITION_STARTED) {
        ctx->tx_session.id      += 1U;
        ctx->tx_seq.num          = 0U;
        ctx->active_tx_call_kind = params.call_kind;

        if (params.call_kind == CALL_KIND_CARE_LISTEN) {
            ctx->care_call_targets = ctx->selected_targets;
            ctx->care_call_session = ctx->tx_session;
            queue_control_packet_flagged(
                ctx,
                PACKET_KIND_CARE_CALL_START,
                ctx->selected_targets,
                (WireFlags){ .bits = (uint8_t)WIRE_FLAG_CARE_CALL }
            );
        } else {
            queue_control_packet_flagged(
                ctx,
                PACKET_KIND_TALK_START,
                ctx->selected_targets,
                (WireFlags){ .bits = (uint8_t)WIRE_FLAG_NONE }
            );
        }
    } else if (tr == PTT_TRANSITION_ENDED) {
        const WireFlags end_flags = {
            .bits = (ctx->active_tx_call_kind == CALL_KIND_CARE_LISTEN)
                    ? (uint8_t)WIRE_FLAG_CARE_CALL
                    : (uint8_t)WIRE_FLAG_NONE
        };
        queue_control_packet_flagged(ctx, PACKET_KIND_TALK_END, ctx->selected_targets, end_flags);
        queue_control_packet_flagged(ctx, PACKET_KIND_TALK_END, ctx->selected_targets, end_flags);
        ctx->active_tx_call_kind = CALL_KIND_NORMAL_PTT;
    }
}

void IntercomCore_abort_care_listen(IntercomCore_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    /* Case 1: Resident aborting active 30s mic window on their own box */
    if (DuplexGate_state(&ctx->duplex_gate) == DUPLEX_STATE_AUTO_REPLY_TX) {
        const StationId listener = ctx->authorized_listener;
        DuplexGate_stop_auto_reply(&ctx->duplex_gate);
        restore_normal_capture_and_volume(ctx);
        ctx->pending_challenge = (PendingListenChallenge){0};

        if (StationId_is_valid(listener)) {
            const TargetPeerMask mask = TargetPeerMask_from_station(listener);
            queue_control_packet_flagged(
                ctx,
                PACKET_KIND_TALK_END,
                mask,
                (WireFlags){ .bits = (uint8_t)WIRE_FLAG_NONE }
            );
        }
        return;
    }

    /* Case 2: Caregiver sending LISTEN_ABORT to close remote window early */
    if (ctx->care_call_targets.bits != 0U) {
        queue_control_packet_flagged(
            ctx,
            PACKET_KIND_LISTEN_ABORT,
            ctx->care_call_targets,
            (WireFlags){ .bits = (uint8_t)WIRE_FLAG_CARE_CALL }
        );
        ctx->care_call_targets = TargetPeerMask_none();
    }
}

static void handle_incoming_listen_challenge(
    IntercomCore_Owned *ctx,
    const WirePacketView_Loaned *pkt
) {
    assert(ctx != NULL);
    assert(pkt != NULL);

    /* Only respond if WE initiated this exact Care-Call session to this peer */
    if (!TargetPeerMask_contains(ctx->care_call_targets, pkt->sender) ||
        (pkt->session.id != ctx->care_call_session.id)) {
        return;
    }

    ChallengeNonce challenge = {0};
    if (WireProto_parse_nonce(pkt->payload, &challenge) != WIRE_OK) {
        return;
    }

    const ChallengeNonce response = compute_challenge_response(
        challenge,
        pkt->session,
        ctx->local_station,
        pkt->sender
    );

    const TargetPeerMask reply_target = TargetPeerMask_from_station(pkt->sender);
    WireDatagram_Owned confirm_dgram = {0};
    if (WireProto_serialize_nonce(&confirm_dgram, (WireNonceSerializeParams){
            .kind    = PACKET_KIND_LISTEN_CONFIRM,
            .sender  = ctx->local_station,
            .session = pkt->session,
            .targets = reply_target,
            .nonce   = response
        }) == WIRE_OK) {
        outbox_push_move(&ctx->tx_outbox, reply_target, &confirm_dgram);
    }
}

static void handle_incoming_listen_confirm(
    IntercomCore_Owned *ctx,
    const WirePacketView_Loaned *pkt,
    MonotonicTimeUs now
) {
    assert(ctx != NULL);
    assert(pkt != NULL);

    if (!ctx->pending_challenge.is_pending ||
        (pkt->sender.id != ctx->pending_challenge.challenger.id) ||
        (pkt->session.id != ctx->pending_challenge.session.id) ||
        (now.us > ctx->pending_challenge.expires_at.us) ||
        !is_peer_authorized_for_listen(ctx, pkt->sender)) {
        ctx->unauthorized_listen_attempts += 1U;
        return;
    }

    ChallengeNonce provided = {0};
    if (WireProto_parse_nonce(pkt->payload, &provided) != WIRE_OK) {
        ctx->unauthorized_listen_attempts += 1U;
        return;
    }

    const ChallengeNonce expected = compute_challenge_response(
        ctx->pending_challenge.nonce,
        ctx->pending_challenge.session,
        pkt->sender,
        ctx->local_station
    );

    if (provided.val != expected.val) {
        ctx->unauthorized_listen_attempts += 1U;
        return;
    }

    /* Valid live confirmation! Consume challenge immediately (one-time anti-replay!) */
    ctx->pending_challenge = (PendingListenChallenge){0};

    ctx->authorized_listener = pkt->sender;
    ctx->tx_session.id      += 1U;
    ctx->tx_seq.num          = 0U;

    AudioDsp_set_capture_mode(&ctx->audio_dsp, CAPTURE_MODE_FAR_FIELD_CARE);
    OpusEncoder_set_capture_mode(&ctx->encoder, CAPTURE_MODE_FAR_FIELD_CARE);
    DuplexGate_start_auto_reply(&ctx->duplex_gate, ctx->care_listen_duration);

    assert(is_peer_authorized_for_listen(ctx, ctx->authorized_listener));
}

void IntercomCore_on_rx_datagram(
    IntercomCore_Owned *ctx,
    IntercomRxDatagramParams params
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    WirePacketView_Loaned pkt = {0};
    const WireError parse_err = WireProto_parse(params.raw_datagram, &pkt);
    if (parse_err != WIRE_OK) {
        ctx->rx_dropped_packets += 1U;
        return;
    }

    if (pkt.sender.id == ctx->local_station.id) {
        return;
    }

    const size_t sender_idx = (size_t)(pkt.sender.id - 1U);
    if (ctx->peers[sender_idx].is_configured) {
        ctx->peers[sender_idx].last_seen = params.now;
        ctx->peers[sender_idx].ever_seen = true;
    }

    if (pkt.kind == PACKET_KIND_HEARTBEAT) {
        return;
    }

    if (!TargetPeerMask_contains(pkt.targets, ctx->local_station)) {
        return;
    }

    if (pkt.kind == PACKET_KIND_LISTEN_CHALLENGE) {
        handle_incoming_listen_challenge(ctx, &pkt);
        return;
    }

    if (pkt.kind == PACKET_KIND_LISTEN_CONFIRM) {
        handle_incoming_listen_confirm(ctx, &pkt, params.now);
        return;
    }

    if (pkt.kind == PACKET_KIND_LISTEN_ABORT) {
        if ((DuplexGate_state(&ctx->duplex_gate) == DUPLEX_STATE_AUTO_REPLY_TX) &&
            (ctx->authorized_listener.id == pkt.sender.id)) {
            IntercomCore_abort_care_listen(ctx);
        }
        return;
    }

    /* Check Care-Call flag & enforce strict local PeerListenPermission */
    const bool has_care_flag =
        (pkt.kind == PACKET_KIND_CARE_CALL_START) ||
        ((pkt.flags.bits & (uint8_t)WIRE_FLAG_CARE_CALL) != 0U);
    const bool sender_authorized = is_peer_authorized_for_listen(ctx, pkt.sender);

    if (has_care_flag && !sender_authorized) {
        ctx->unauthorized_listen_attempts += 1U;
    }

    const bool effective_care_call = has_care_flag && sender_authorized;
    if (effective_care_call) {
        /* Emergency 100% Volume Override during authorized Care Call! */
        AudioDsp_set_playback_gain_mode(&ctx->audio_dsp, GAIN_MODE_EMERGENCY_FULL);
        ctx->care_volume_override_until = (MonotonicTimeUs){
            .us = params.now.us + (uint64_t)UHURA_CARE_OVERRIDE_HOLD_US
        };

        /* If caregiver speaks again during the 30s listen window, pause mic so resident hears! */
        if ((pkt.kind != PACKET_KIND_TALK_END) &&
            (DuplexGate_state(&ctx->duplex_gate) == DUPLEX_STATE_AUTO_REPLY_TX) &&
            (ctx->authorized_listener.id == pkt.sender.id)) {
            DuplexGate_stop_auto_reply(&ctx->duplex_gate);
            restore_normal_capture_and_volume(ctx);
        }
    }

    RxStreamSlot_Owned *slot = find_or_bind_rx_slot(ctx, pkt.sender, params.now);
    if (slot == NULL) {
        ctx->rx_dropped_packets += 1U;
        return;
    }

    if ((pkt.kind == PACKET_KIND_TALK_START) || (pkt.kind == PACKET_KIND_CARE_CALL_START)) {
        const bool was_active = JitterBuffer_is_active(&slot->jitter);
        JitterBuffer_on_talk_start(&slot->jitter, pkt.session);
        if (!was_active) {
            OpusDecoder_reset(&slot->decoder);
            if (effective_care_call) {
                DuplexGate_trigger_care_alert_chime(&ctx->duplex_gate);
            } else {
                DuplexGate_trigger_incoming_chime(&ctx->duplex_gate);
            }
        }
        return;
    }

    if (pkt.kind == PACKET_KIND_TALK_END) {
        JitterBuffer_on_talk_end(&slot->jitter, pkt.session);

        if (effective_care_call) {
            /* Deduplicate repeated UDP TALK_END packets for the same talk session */
            const bool already_challenged =
                (ctx->last_challenged_sender.id == pkt.sender.id) &&
                (ctx->last_challenged_session.id == pkt.session.id);

            if (!already_challenged) {
                ctx->last_challenged_sender  = pkt.sender;
                ctx->last_challenged_session = pkt.session;

                const ChallengeNonce nonce = generate_fresh_nonce(ctx, pkt.sender, params.now);
                ctx->pending_challenge = (PendingListenChallenge){
                    .challenger = pkt.sender,
                    .session    = pkt.session,
                    .nonce      = nonce,
                    .expires_at = { .us = params.now.us + (uint64_t)UHURA_CHALLENGE_TIMEOUT_US },
                    .is_pending = true
                };

                const TargetPeerMask ch_target = TargetPeerMask_from_station(pkt.sender);
                WireDatagram_Owned ch_dgram = {0};
                if (WireProto_serialize_nonce(&ch_dgram, (WireNonceSerializeParams){
                        .kind    = PACKET_KIND_LISTEN_CHALLENGE,
                        .sender  = ctx->local_station,
                        .session = pkt.session,
                        .targets = ch_target,
                        .nonce   = nonce
                    }) == WIRE_OK) {
                    outbox_push_move(&ctx->tx_outbox, ch_target, &ch_dgram);
                }
            }
        }
        return;
    }

    /* PACKET_KIND_AUDIO */
    EncodedFrame_Owned frame = {0};
    memcpy(frame.bytes, pkt.payload.data, pkt.payload.len);
    frame.len      = (uint16_t)pkt.payload.len;
    frame.session  = pkt.session;
    frame.seq      = pkt.seq;
    frame.is_valid = true;

    const bool was_idle = !JitterBuffer_is_active(&slot->jitter);
    const JitterPushResult push_res = JitterBuffer_push_move(&slot->jitter, &frame);
    if (was_idle || (push_res == JITTER_PUSH_NEW_SESSION)) {
        OpusDecoder_reset(&slot->decoder);
    }
}

void IntercomCore_tick_10ms(
    IntercomCore_Owned * restrict ctx,
    IntercomTickParams params,
    PcmFrame_Owned * restrict out_speaker_pcm
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    assert(out_speaker_pcm != NULL);
    assert(params.mic_pcm.samples != NULL);
    assert(params.mic_pcm.frames.count == (uint16_t)UHURA_SAMPLES_PER_FRAME);

    /* Expire stale unconfirmed challenge */
    if (ctx->pending_challenge.is_pending &&
        (params.now.us > ctx->pending_challenge.expires_at.us)) {
        ctx->pending_challenge = (PendingListenChallenge){0};
    }

    /* 1. Decode & mix active RX streams + measure per-room sound activity */
    PcmFrame_Owned mixed_rx = {0};
    PcmFrame_init_silence(&mixed_rx);
    bool any_rx_active = false;

    for (size_t i = 0U; i < (size_t)UHURA_MAX_RX_STREAMS; ++i) {
        RxStreamSlot_Owned *slot = &ctx->rx_streams[i];
        if (!slot->is_bound) {
            continue;
        }

        EncodedFrame_Owned popped = {0};
        const JitterPopStatus pop_status = JitterBuffer_pop_10ms(&slot->jitter, &popped);

        PcmFrame_Owned decoded_pcm = {0};
        CodecError dec_err = CODEC_ERR_DECODE_FAILED;

        if (pop_status == JITTER_POP_FRAME) {
            dec_err = OpusDecoder_decode_packet(
                &slot->decoder,
                EncodedFrame_loan(&popped),
                &decoded_pcm
            );
        } else if (pop_status == JITTER_POP_FEC_NEXT) {
            dec_err = OpusDecoder_decode_fec_from_next(
                &slot->decoder,
                EncodedFrame_loan(&popped),
                &decoded_pcm
            );
        } else if (pop_status == JITTER_POP_MISSING_PLC) {
            dec_err = OpusDecoder_decode_plc(&slot->decoder, &decoded_pcm);
        } else {
            slot->recent_peak       = 0U;
            slot->has_audible_sound = false;
            if (!JitterBuffer_is_active(&slot->jitter)) {
                slot->is_bound = false;
            }
        }

        if ((dec_err == CODEC_OK) && decoded_pcm.is_valid) {
            any_rx_active = true;
            const uint16_t pk = measure_frame_peak(&decoded_pcm);
            slot->recent_peak       = pk;
            slot->has_audible_sound = (pk >= (uint16_t)AUDIBLE_SOUND_PEAK_THRESHOLD);
            PcmFrame_mix_into(&mixed_rx, PcmFrame_loan(&decoded_pcm));
        }
    }

    /* Release Emergency Volume Override once Care Call audio has drained & window is closed */
    if ((AudioDsp_playback_gain_mode(&ctx->audio_dsp) == GAIN_MODE_EMERGENCY_FULL) &&
        !any_rx_active &&
        (DuplexGate_state(&ctx->duplex_gate) != DUPLEX_STATE_AUTO_REPLY_TX) &&
        (params.now.us > ctx->care_volume_override_until.us)) {
        AudioDsp_set_playback_gain_mode(&ctx->audio_dsp, GAIN_MODE_USER_LEVEL);
    }

    /* 2. Condition incoming microphone frame (150 Hz High-Pass + Normal/Far-Field AGC) */
    PcmFrame_Owned conditioned_mic = PcmFrame_from_slice(params.mic_pcm);
    AudioDsp_process_capture_10ms(&ctx->audio_dsp, PcmFrame_loan_mut(&conditioned_mic));

    /* 3. Run PTT / Auto-Reply DuplexGate */
    PcmFrame_init_silence(out_speaker_pcm);
    PcmFrame_Owned tx_mic_pcm = {0};
    PcmFrame_init_silence(&tx_mic_pcm);

    const StationId auto_listener_before = ctx->authorized_listener;
    const bool is_auto_reply_before =
        (DuplexGate_state(&ctx->duplex_gate) == DUPLEX_STATE_AUTO_REPLY_TX);

    const bool should_transmit = DuplexGate_process_10ms(
        &ctx->duplex_gate,
        (DuplexAudioFrameParams){
            .far_end_speaker_in = PcmFrame_loan(&mixed_rx),
            .near_end_mic_in    = PcmFrame_loan(&conditioned_mic),
            .speaker_out        = PcmFrame_loan_mut(out_speaker_pcm),
            .tx_mic_out         = PcmFrame_loan_mut(&tx_mic_pcm)
        }
    );

    /* Apply User Volume or 100% Emergency Override to speaker output */
    AudioDsp_process_playback_10ms(&ctx->audio_dsp, PcmFrame_loan_mut(out_speaker_pcm));

    /* Determine exclusive recipient mask (30s Care-Listen ONLY goes to authorized_listener!) */
    TargetPeerMask tx_recipients = ctx->selected_targets;
    if (is_auto_reply_before) {
        assert(StationId_is_valid(auto_listener_before));
        assert(is_peer_authorized_for_listen(ctx, auto_listener_before));
        tx_recipients = TargetPeerMask_from_station(auto_listener_before);
    }

    /* 4. Encode once & queue outgoing AUDIO packet */
    if (should_transmit && (tx_recipients.bits != 0U)) {
        EncodedFrame_Owned encoded = {0};
        const CodecError enc_err = OpusEncoder_encode_frame(
            &ctx->encoder,
            (OpusEncodeParams){
                .pcm_in  = PcmFrame_loan(&tx_mic_pcm),
                .session = ctx->tx_session,
                .seq     = ctx->tx_seq
            },
            &encoded
        );

        if ((enc_err == CODEC_OK) && encoded.is_valid) {
            uint8_t flag_bits = 0U;
            if (!is_auto_reply_before &&
                (ctx->configured_peers.bits != 0U) &&
                ((tx_recipients.bits & ctx->configured_peers.bits) ==
                 ctx->configured_peers.bits)) {
                flag_bits |= (uint8_t)WIRE_FLAG_ALL_CALL;
            }
            if (!is_auto_reply_before && (ctx->active_tx_call_kind == CALL_KIND_CARE_LISTEN)) {
                flag_bits |= (uint8_t)WIRE_FLAG_CARE_CALL;
            }

            WireDatagram_Owned dgram = {0};
            const WireError wire_err = WireProto_serialize(&dgram, (WireSerializeParams){
                .kind    = PACKET_KIND_AUDIO,
                .sender  = ctx->local_station,
                .flags   = { .bits = flag_bits },
                .session = encoded.session,
                .seq     = encoded.seq,
                .targets = tx_recipients,
                .payload = EncodedFrame_loan(&encoded)
            });

            if (wire_err == WIRE_OK) {
                outbox_push_move(&ctx->tx_outbox, tx_recipients, &dgram);
            }
            ctx->tx_seq.num += 1U;
        }
    }

    /* Clean up if the bounded 30s Care-Listen window just reached 0 ticks */
    if (DuplexGate_take_auto_reply_expired(&ctx->duplex_gate)) {
        restore_normal_capture_and_volume(ctx);
        AudioDsp_set_playback_gain_mode(&ctx->audio_dsp, GAIN_MODE_USER_LEVEL);
        if (StationId_is_valid(auto_listener_before)) {
            const TargetPeerMask end_mask = TargetPeerMask_from_station(auto_listener_before);
            queue_control_packet_flagged(
                ctx,
                PACKET_KIND_TALK_END,
                end_mask,
                (WireFlags){ .bits = (uint8_t)WIRE_FLAG_NONE }
            );
        }
    }

    /* 5. Periodic heartbeat */
    if (ctx->configured_peers.bits != 0U) {
        const bool first_hb = (ctx->last_heartbeat_tx.us == 0ULL);
        const bool elapsed  = (params.now.us >= ctx->last_heartbeat_tx.us) &&
                              ((params.now.us - ctx->last_heartbeat_tx.us) >=
                               (uint64_t)UHURA_HEARTBEAT_INTERVAL_US);

        if (first_hb || elapsed) {
            ctx->last_heartbeat_tx = params.now;
            queue_control_packet_flagged(
                ctx,
                PACKET_KIND_HEARTBEAT,
                ctx->configured_peers,
                (WireFlags){ .bits = (uint8_t)WIRE_FLAG_NONE }
            );
        }
    }
}

NODISCARD CoreError IntercomCore_pop_tx_move(
    IntercomCore_Owned * restrict ctx,
    TxEnvelope_Owned * restrict out_tx
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    assert(out_tx != NULL);

    *out_tx = (TxEnvelope_Owned){0};
    if (ctx->tx_outbox.count == 0U) {
        return CORE_ERR_OUTBOX_EMPTY;
    }

    TxEnvelope_Owned *slot = &ctx->tx_outbox.items[ctx->tx_outbox.tail];
    assert(slot->is_valid);

    out_tx->recipients = slot->recipients;
    WireDatagram_move(&out_tx->datagram, &slot->datagram);
    out_tx->is_valid = true;
    *slot = (TxEnvelope_Owned){0};

    ctx->tx_outbox.tail =
        (uint16_t)((ctx->tx_outbox.tail + 1U) % (uint16_t)UHURA_TX_OUTBOX_CAPACITY);
    ctx->tx_outbox.count = (uint16_t)(ctx->tx_outbox.count - 1U);
    return CORE_OK;
}

NODISCARD bool IntercomCore_is_peer_online(
    const IntercomCore_Owned *ctx,
    StationId peer,
    MonotonicTimeUs now
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if (!StationId_is_valid(peer)) {
        return false;
    }
    const PeerInfo *info = &ctx->peers[peer.id - 1U];
    if (!info->is_configured || !info->ever_seen) {
        return false;
    }
    if (now.us < info->last_seen.us) {
        return true;
    }
    return (now.us - info->last_seen.us) <= (uint64_t)UHURA_PEER_ONLINE_TIMEOUT_US;
}

NODISCARD TargetPeerMask IntercomCore_active_talkers(const IntercomCore_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    TargetPeerMask active = TargetPeerMask_none();
    for (size_t i = 0U; i < (size_t)UHURA_MAX_RX_STREAMS; ++i) {
        if (ctx->rx_streams[i].is_bound && JitterBuffer_is_active(&ctx->rx_streams[i].jitter)) {
            active = TargetPeerMask_with_station(active, ctx->rx_streams[i].sender);
        }
    }
    return active;
}

NODISCARD TargetPeerMask IntercomCore_audible_talkers(const IntercomCore_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    TargetPeerMask audible = TargetPeerMask_none();
    for (size_t i = 0U; i < (size_t)UHURA_MAX_RX_STREAMS; ++i) {
        if (ctx->rx_streams[i].is_bound &&
            JitterBuffer_is_active(&ctx->rx_streams[i].jitter) &&
            ctx->rx_streams[i].has_audible_sound) {
            audible = TargetPeerMask_with_station(audible, ctx->rx_streams[i].sender);
        }
    }
    return audible;
}
