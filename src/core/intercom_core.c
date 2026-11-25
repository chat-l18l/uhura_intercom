#include "core/intercom_core.h"

#include <string.h>

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
        /* Drop oldest control/audio packet if caller has not drained outbox */
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

static void queue_control_packet(
    IntercomCore_Owned *ctx,
    PacketKind kind,
    TargetPeerMask recipients
) {
    assert(ctx != NULL);
    if (recipients.bits == 0U) {
        return;
    }

    const bool is_all_call =
        (ctx->configured_peers.bits != 0U) &&
        ((recipients.bits & ctx->configured_peers.bits) == ctx->configured_peers.bits);

    WireDatagram_Owned dgram = {0};
    const WireError err = WireProto_serialize(&dgram, (WireSerializeParams){
        .kind    = kind,
        .sender  = ctx->local_station,
        .flags   = { .bits = is_all_call ? (uint8_t)WIRE_FLAG_ALL_CALL : (uint8_t)WIRE_FLAG_NONE },
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

    /* 1. Existing slot bound to this sender */
    for (size_t i = 0U; i < (size_t)UHURA_MAX_RX_STREAMS; ++i) {
        if (ctx->rx_streams[i].is_bound && (ctx->rx_streams[i].sender.id == sender.id)) {
            ctx->rx_streams[i].last_packet_time = now;
            return &ctx->rx_streams[i];
        }
    }

    /* 2. Free or idle slot */
    for (size_t i = 0U; i < (size_t)UHURA_MAX_RX_STREAMS; ++i) {
        if (!ctx->rx_streams[i].is_bound || !JitterBuffer_is_active(&ctx->rx_streams[i].jitter)) {
            RxStreamSlot_Owned *slot = &ctx->rx_streams[i];
            JitterBuffer_reset(&slot->jitter);
            OpusDecoder_reset(&slot->decoder);
            slot->sender           = sender;
            slot->last_packet_time = now;
            slot->is_bound         = true;
            return slot;
        }
    }

    /* 3. All 4 concurrent slots busy: steal the stalest slot if >500 ms inactive */
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
        slot->sender           = sender;
        slot->last_packet_time = now;
        slot->is_bound         = true;
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

    ctx->local_station = params.local_station;
    ctx->tx_session    = (TalkSessionId){ .id = 0U };
    ctx->tx_seq        = (SeqNum){ .num = 0U };

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
    ctx->peers[idx].is_configured = true;
    ctx->peers[idx].ever_seen     = false;
    ctx->peers[idx].last_seen     = (MonotonicTimeUs){ .us = 0ULL };
    copy_station_name(ctx->peers[idx].name, params.name);

    ctx->configured_peers = TargetPeerMask_with_station(ctx->configured_peers, params.station);

    /* Default convenience: add newly registered peer to active selection if none set yet */
    if (ctx->selected_targets.bits == 0U) {
        ctx->selected_targets = TargetPeerMask_from_station(params.station);
    }

    return CORE_OK;
}

void IntercomCore_set_targets(
    IntercomCore_Owned *ctx,
    TargetPeerMask targets
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    /* Mask against configured peers so we never target non-existent stations */
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
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    /* Ignore PTT press when no target stations are selected */
    if ((button == PTT_PRESSED) && (ctx->selected_targets.bits == 0U)) {
        return;
    }

    const PttTransition tr = DuplexGate_set_ptt(&ctx->duplex_gate, button);
    if (tr == PTT_TRANSITION_STARTED) {
        ctx->tx_session.id += 1U;
        ctx->tx_seq.num     = 0U;
        queue_control_packet(ctx, PACKET_KIND_TALK_START, ctx->selected_targets);
    } else if (tr == PTT_TRANSITION_ENDED) {
        /* Send TALK_END twice for resilience over UDP / WireGuard */
        queue_control_packet(ctx, PACKET_KIND_TALK_END, ctx->selected_targets);
        queue_control_packet(ctx, PACKET_KIND_TALK_END, ctx->selected_targets);
    }
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

    /* Ignore self-looped packets */
    if (pkt.sender.id == ctx->local_station.id) {
        return;
    }

    /* Update peer online presence */
    const size_t sender_idx = (size_t)(pkt.sender.id - 1U);
    if (ctx->peers[sender_idx].is_configured) {
        ctx->peers[sender_idx].last_seen = params.now;
        ctx->peers[sender_idx].ever_seen = true;
    }

    if (pkt.kind == PACKET_KIND_HEARTBEAT) {
        return;
    }

    /* Filter out talk packets not addressed to our station */
    if (!TargetPeerMask_contains(pkt.targets, ctx->local_station)) {
        return;
    }

    RxStreamSlot_Owned *slot = find_or_bind_rx_slot(ctx, pkt.sender, params.now);
    if (slot == NULL) {
        ctx->rx_dropped_packets += 1U;
        return;
    }

    if (pkt.kind == PACKET_KIND_TALK_START) {
        const bool was_active = JitterBuffer_is_active(&slot->jitter);
        JitterBuffer_on_talk_start(&slot->jitter, pkt.session);
        if (!was_active) {
            OpusDecoder_reset(&slot->decoder);
            DuplexGate_trigger_incoming_chime(&ctx->duplex_gate);
        }
        return;
    }

    if (pkt.kind == PACKET_KIND_TALK_END) {
        JitterBuffer_on_talk_end(&slot->jitter, pkt.session);
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

    /* 1. Decode & mix all active RX talker streams (bounded loop: max 4 streams) */
    PcmFrame_Owned mixed_rx = {0};
    PcmFrame_init_silence(&mixed_rx);

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
            /* JITTER_POP_SILENCE_IDLE */
            if (!JitterBuffer_is_active(&slot->jitter)) {
                slot->is_bound = false;
            }
        }

        if ((dec_err == CODEC_OK) && decoded_pcm.is_valid) {
            PcmFrame_mix_into(&mixed_rx, PcmFrame_loan(&decoded_pcm));
        }
    }

    /* 2. Run PTT DuplexGate (+ chime overlay; Phase-2 AEC slot) */
    PcmFrame_init_silence(out_speaker_pcm);
    PcmFrame_Owned tx_mic_pcm = {0};
    PcmFrame_init_silence(&tx_mic_pcm);

    const bool should_transmit = DuplexGate_process_10ms(
        &ctx->duplex_gate,
        (DuplexAudioFrameParams){
            .far_end_speaker_in = PcmFrame_loan(&mixed_rx),
            .near_end_mic_in    = params.mic_pcm,
            .speaker_out        = PcmFrame_loan_mut(out_speaker_pcm),
            .tx_mic_out         = PcmFrame_loan_mut(&tx_mic_pcm)
        }
    );

    /* 3. Encode once & fan-out to selected target peers when PTT is active */
    if (should_transmit && (ctx->selected_targets.bits != 0U)) {
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
            const bool is_all_call =
                (ctx->configured_peers.bits != 0U) &&
                ((ctx->selected_targets.bits & ctx->configured_peers.bits) ==
                 ctx->configured_peers.bits);

            WireDatagram_Owned dgram = {0};
            const WireError wire_err = WireProto_serialize(&dgram, (WireSerializeParams){
                .kind    = PACKET_KIND_AUDIO,
                .sender  = ctx->local_station,
                .flags   = {
                    .bits = is_all_call ? (uint8_t)WIRE_FLAG_ALL_CALL : (uint8_t)WIRE_FLAG_NONE
                },
                .session = encoded.session,
                .seq     = encoded.seq,
                .targets = ctx->selected_targets,
                .payload = EncodedFrame_loan(&encoded)
            });

            if (wire_err == WIRE_OK) {
                outbox_push_move(&ctx->tx_outbox, ctx->selected_targets, &dgram);
            }
            ctx->tx_seq.num += 1U;
        }
    }

    /* 4. Periodic heartbeat to keep WireGuard tunnel & peer presence alive */
    if (ctx->configured_peers.bits != 0U) {
        const bool first_hb = (ctx->last_heartbeat_tx.us == 0ULL);
        const bool elapsed  = (params.now.us >= ctx->last_heartbeat_tx.us) &&
                              ((params.now.us - ctx->last_heartbeat_tx.us) >=
                               (uint64_t)UHURA_HEARTBEAT_INTERVAL_US);

        if (first_hb || elapsed) {
            ctx->last_heartbeat_tx = params.now;
            queue_control_packet(ctx, PACKET_KIND_HEARTBEAT, ctx->configured_peers);
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
