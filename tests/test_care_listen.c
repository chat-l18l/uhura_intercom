#include "core/intercom_core.h"

#include <stdio.h>

#define TEST_CHECK(cond)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static PcmFrame_Owned make_square_tone(int16_t amplitude, size_t half_period) {
    PcmFrame_Owned f = {0};
    PcmFrame_init_silence(&f);
    if (amplitude == 0) {
        return f;
    }
    for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
        f.samples[i] = (((i / half_period) & 1U) == 0U) ? amplitude : (int16_t)(-amplitude);
    }
    return f;
}

static int64_t abs_energy(const PcmFrame_Owned *f) {
    int64_t sum = 0;
    for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
        const int32_t v = f->samples[i];
        sum += (v < 0) ? -v : v;
    }
    return sum;
}

/* Routes queued packets among 4 simulated nodes and optionally captures LISTEN_CONFIRM */
static void route_network(
    IntercomCore_Owned *nodes[4],
    MonotonicTimeUs now,
    WireDatagram_Owned *captured_confirm
) {
    /* Two passes so Challenge -> Confirm roundtrip completes in the same simulated step */
    for (size_t pass = 0U; pass < 2U; ++pass) {
        for (size_t s = 0U; s < 4U; ++s) {
            TxEnvelope_Owned env = {0};
            for (size_t g = 0U; g < (size_t)UHURA_TX_OUTBOX_CAPACITY; ++g) {
                if (IntercomCore_pop_tx_move(nodes[s], &env) != CORE_OK) {
                    break;
                }

                if (captured_confirm != NULL && !captured_confirm->is_valid) {
                    WirePacketView_Loaned view = {0};
                    if ((WireProto_parse(WireDatagram_loan(&env.datagram), &view) == WIRE_OK) &&
                        (view.kind == PACKET_KIND_LISTEN_CONFIRM)) {
                        *captured_confirm = env.datagram; /* Save copy to test replay attack! */
                    }
                }

                for (size_t r = 0U; r < 4U; ++r) {
                    if (TargetPeerMask_contains(env.recipients, nodes[r]->local_station)) {
                        IntercomCore_on_rx_datagram(nodes[r], (IntercomRxDatagramParams){
                            .raw_datagram = WireDatagram_loan(&env.datagram),
                            .now          = now
                        });
                    }
                }
            }
        }
    }
}

int main(void) {
    /*
     * 4 Stations:
     * #1 = Edwin (Authorized on both of Mother's boxes!)
     * #2 = Ma-Woonkamer (Downstairs, User Volume = 0% muted, empty quiet room)
     * #3 = Ma-Overloop  (Upstairs near bathroom, User Volume = 10%, faint knocking!)
     * #4 = Buurman      (Normal Kring peer, NOT authorized to listen!)
     */
    IntercomCore_Owned edwin;
    IntercomCore_Owned ma_beneden;
    IntercomCore_Owned ma_boven;
    IntercomCore_Owned buurman;

    const IntercomConfigParams base = {
        .local_station   = { .id = 1U },
        .opus_bitrate    = { .bps = 32000U },
        .fec_mode        = FEC_ENABLED,
        .jitter_prefetch = { .count = 2U },
        .hangover_frames = { .count = 2U },
        .chime_mode      = CHIME_ENABLED
    };

    IntercomConfigParams c1 = base; c1.local_station.id = 1U;
    IntercomConfigParams c2 = base; c2.local_station.id = 2U;
    IntercomConfigParams c3 = base; c3.local_station.id = 3U;
    IntercomConfigParams c4 = base; c4.local_station.id = 4U;

    TEST_CHECK(IntercomCore_init(&edwin, c1) == CORE_OK);
    TEST_CHECK(IntercomCore_init(&ma_beneden, c2) == CORE_OK);
    TEST_CHECK(IntercomCore_init(&ma_boven, c3) == CORE_OK);
    TEST_CHECK(IntercomCore_init(&buurman, c4) == CORE_OK);

    IntercomCore_Owned *nodes[4] = { &edwin, &ma_beneden, &ma_boven, &buurman };

    /* Register peers everywhere */
    for (size_t i = 0U; i < 4U; ++i) {
        for (uint8_t id = 1U; id <= 4U; ++id) {
            if (nodes[i]->local_station.id != id) {
                TEST_CHECK(IntercomCore_register_peer(nodes[i], (PeerRegisterParams){
                    .station = { .id = id }, .name = "Node"
                }) == CORE_OK);
            }
        }
    }

    /* Grant LISTEN_PERM_ALLOWED ONLY to Edwin (#1) on Mother's two boxes */
    TEST_CHECK(IntercomCore_set_peer_listen_permission(&ma_beneden, (PeerPermissionParams){
        .station = { .id = 1U }, .permission = LISTEN_PERM_ALLOWED
    }) == CORE_OK);
    TEST_CHECK(IntercomCore_set_peer_listen_permission(&ma_boven, (PeerPermissionParams){
        .station = { .id = 1U }, .permission = LISTEN_PERM_ALLOWED
    }) == CORE_OK);

    /* Set short 25-tick (250 ms) listen duration for fast deterministic testing */
    IntercomCore_set_care_listen_duration(&ma_beneden, (SlotCount){ .count = 25U });
    IntercomCore_set_care_listen_duration(&ma_boven, (SlotCount){ .count = 25U });

    /* Mother turned down her boxes for the evening: Downstairs 0%, Upstairs 10% */
    IntercomCore_set_user_volume(&ma_beneden, (VolumePercent){ .pct = 0U });
    IntercomCore_set_user_volume(&ma_boven, (VolumePercent){ .pct = 10U });

    uint64_t sim_us = 1000000ULL;
    const PcmFrame_Owned silence     = make_square_tone(0, 12U);
    const PcmFrame_Owned quiet_hiss  = make_square_tone(20, 12U);  /* Empty living room */
    const PcmFrame_Owned faint_knock = make_square_tone(550, 16U); /* Knocking upstairs! */
    const PcmFrame_Owned loud_voice  = make_square_tone(6000, 12U);

    /* =====================================================================
     * TEST 1: Unauthorized peer (Buurman #4) attempts a Care-Listen call!
     * Must NOT override Mother's 0% volume and must NEVER open her mic!
     * ===================================================================== */
    IntercomCore_set_targets(&buurman, TargetPeerMask_from_station((StationId){ .id = 2U }));
    IntercomCore_set_ptt_command(&buurman, (PttCommandParams){
        .button = PTT_PRESSED, .call_kind = CALL_KIND_CARE_LISTEN
    });
    route_network(nodes, (MonotonicTimeUs){ .us = sim_us }, NULL);

    int64_t ma_beneden_unauth_spk = 0;
    for (size_t t = 0U; t < 6U; ++t) {
        sim_us += 10000ULL;
        const MonotonicTimeUs now = { .us = sim_us };
        PcmFrame_Owned spk = {0};
        IntercomCore_tick_10ms(&buurman, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&loud_voice), .now = now
        }, &spk);
        route_network(nodes, now, NULL);

        IntercomCore_tick_10ms(&ma_beneden, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&silence), .now = now
        }, &spk);
        ma_beneden_unauth_spk += abs_energy(&spk);
    }

    IntercomCore_set_ptt_command(&buurman, (PttCommandParams){
        .button = PTT_RELEASED, .call_kind = CALL_KIND_CARE_LISTEN
    });
    route_network(nodes, (MonotonicTimeUs){ .us = sim_us }, NULL);

    TEST_CHECK(ma_beneden.unauthorized_listen_attempts > 0U);
    TEST_CHECK(ma_beneden_unauth_spk == 0); /* 0% night mute was respected! */
    TEST_CHECK(DuplexGate_state(&ma_beneden.duplex_gate) == DUPLEX_STATE_LISTENING);
    TEST_CHECK(!DuplexGate_is_mic_hot(&ma_beneden.duplex_gate));

    /* =====================================================================
     * TEST 2: Authorized Edwin (#1) calls BOTH Ma-Beneden (#2) & Ma-Boven (#3)
     * ===================================================================== */
    const TargetPeerMask both_ma = TargetPeerMask_with_station(
        TargetPeerMask_from_station((StationId){ .id = 2U }),
        (StationId){ .id = 3U }
    );
    IntercomCore_set_targets(&edwin, both_ma);
    IntercomCore_set_ptt_command(&edwin, (PttCommandParams){
        .button = PTT_PRESSED, .call_kind = CALL_KIND_CARE_LISTEN
    });
    route_network(nodes, (MonotonicTimeUs){ .us = sim_us }, NULL);

    /* Emergency Volume Override MUST jump Ma-Beneden from 0% to 100%! */
    TEST_CHECK(IntercomCore_effective_volume(&ma_beneden).pct == 100U);
    TEST_CHECK(IntercomCore_effective_volume(&ma_boven).pct == 100U);

    int64_t ma_beneden_emerg_spk = 0;
    for (size_t t = 0U; t < 8U; ++t) {
        sim_us += 10000ULL;
        const MonotonicTimeUs now = { .us = sim_us };
        PcmFrame_Owned spk_e = {0}, spk_b = {0}, spk_u = {0};

        IntercomCore_tick_10ms(&edwin, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&loud_voice), .now = now
        }, &spk_e);
        route_network(nodes, now, NULL);

        IntercomCore_tick_10ms(&ma_beneden, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&quiet_hiss), .now = now
        }, &spk_b);
        IntercomCore_tick_10ms(&ma_boven, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&faint_knock), .now = now
        }, &spk_u);

        ma_beneden_emerg_spk += abs_energy(&spk_b);
    }
    TEST_CHECK(ma_beneden_emerg_spk > 100000); /* Played loud despite 0% user volume! */

    /* Edwin releases PTT -> Challenge/Confirm completes -> both open Auto-Reply! */
    WireDatagram_Owned captured_confirm = {0};
    IntercomCore_set_ptt_command(&edwin, (PttCommandParams){
        .button = PTT_RELEASED, .call_kind = CALL_KIND_CARE_LISTEN
    });
    route_network(nodes, (MonotonicTimeUs){ .us = sim_us }, &captured_confirm);

    TEST_CHECK(captured_confirm.is_valid);
    TEST_CHECK(DuplexGate_state(&ma_beneden.duplex_gate) == DUPLEX_STATE_AUTO_REPLY_TX);
    TEST_CHECK(DuplexGate_state(&ma_boven.duplex_gate) == DUPLEX_STATE_AUTO_REPLY_TX);
    TEST_CHECK(DuplexGate_is_mic_hot(&ma_beneden.duplex_gate));
    TEST_CHECK(DuplexGate_is_mic_hot(&ma_boven.duplex_gate));

    /* Run 42 ticks (12 pre-chime + 25 active listen + 5 drain):
     * - Edwin MUST hear the boosted knocking from Upstairs (#3)!
     * - Audible room detection MUST flag Upstairs (#3) and NOT empty Downstairs (#2)!
     * - Buurman (#4) MUST hear ZERO audio! */
    int64_t edwin_heard_energy   = 0;
    int64_t buurman_heard_energy = 0;
    bool upstairs_flagged_audible   = false;
    bool downstairs_flagged_audible = false;

    for (size_t t = 0U; t < 42U; ++t) {
        sim_us += 10000ULL;
        const MonotonicTimeUs now = { .us = sim_us };
        PcmFrame_Owned spk_e = {0}, spk_b = {0}, spk_u = {0}, spk_n = {0};

        IntercomCore_tick_10ms(&ma_beneden, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&quiet_hiss), .now = now
        }, &spk_b);
        IntercomCore_tick_10ms(&ma_boven, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&faint_knock), .now = now
        }, &spk_u);
        route_network(nodes, now, NULL);

        IntercomCore_tick_10ms(&edwin, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&silence), .now = now
        }, &spk_e);
        IntercomCore_tick_10ms(&buurman, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&silence), .now = now
        }, &spk_n);

        edwin_heard_energy   += abs_energy(&spk_e);
        buurman_heard_energy += abs_energy(&spk_n);

        const TargetPeerMask aud = IntercomCore_audible_talkers(&edwin);
        if (TargetPeerMask_contains(aud, (StationId){ .id = 3U })) {
            upstairs_flagged_audible = true;
        }
        if (TargetPeerMask_contains(aud, (StationId){ .id = 2U })) {
            downstairs_flagged_audible = true;
        }
    }

    TEST_CHECK(edwin_heard_energy > 500000);
    TEST_CHECK(buurman_heard_energy == 0); /* Exclusive privacy to Edwin! */
    TEST_CHECK(upstairs_flagged_audible);
    TEST_CHECK(!downstairs_flagged_audible);

    /* Both boxes MUST have closed their microphones automatically! */
    TEST_CHECK(DuplexGate_state(&ma_beneden.duplex_gate) == DUPLEX_STATE_LISTENING);
    TEST_CHECK(DuplexGate_state(&ma_boven.duplex_gate) == DUPLEX_STATE_LISTENING);
    TEST_CHECK(!DuplexGate_is_mic_hot(&ma_boven.duplex_gate));

    /* =====================================================================
     * TEST 3: Replay Attack! Re-sending the captured valid LISTEN_CONFIRM
     * (addressed to Ma-Beneden #2) MUST be rejected because the one-time
     * ChallengeNonce was already consumed!
     * ===================================================================== */
    const uint32_t prev_unauth = ma_beneden.unauthorized_listen_attempts;
    IntercomCore_on_rx_datagram(&ma_beneden, (IntercomRxDatagramParams){
        .raw_datagram = WireDatagram_loan(&captured_confirm),
        .now          = { .us = sim_us + 10000ULL }
    });
    TEST_CHECK(ma_beneden.unauthorized_listen_attempts == (prev_unauth + 1U));
    TEST_CHECK(DuplexGate_state(&ma_beneden.duplex_gate) == DUPLEX_STATE_LISTENING);
    TEST_CHECK(!DuplexGate_is_mic_hot(&ma_beneden.duplex_gate));

    /* =====================================================================
     * TEST 4: Multiple Authorized Family Members (e.g. Edwin #1 AND Zus #5)!
     * Both siblings independently hold LISTEN_PERM_ALLOWED on Mother's box
     * and Zus (#5) can also open a 30s Care-Listen window while Buurman (#4)
     * remains denied.
     * ===================================================================== */
    IntercomCore_Owned zus;
    IntercomConfigParams c5 = base;
    c5.local_station.id = 5U;
    TEST_CHECK(IntercomCore_init(&zus, c5) == CORE_OK);
    TEST_CHECK(IntercomCore_register_peer(&zus, (PeerRegisterParams){
        .station = { .id = 2U }, .name = "Ma-Woonkamer"
    }) == CORE_OK);
    TEST_CHECK(IntercomCore_register_peer(&ma_beneden, (PeerRegisterParams){
        .station = { .id = 5U }, .name = "Zus"
    }) == CORE_OK);
    TEST_CHECK(IntercomCore_set_peer_listen_permission(&ma_beneden, (PeerPermissionParams){
        .station = { .id = 5U }, .permission = LISTEN_PERM_ALLOWED
    }) == CORE_OK);

    /* Both Edwin (#1) and Zus (#5) are now simultaneously authorized! */
    TEST_CHECK(ma_beneden.peers[0].listen_perm == LISTEN_PERM_ALLOWED);
    TEST_CHECK(ma_beneden.peers[4].listen_perm == LISTEN_PERM_ALLOWED);
    TEST_CHECK(ma_beneden.peers[3].listen_perm == LISTEN_PERM_DENIED); /* Buurman #4 still denied */

    IntercomCore_set_targets(&zus, TargetPeerMask_from_station((StationId){ .id = 2U }));
    IntercomCore_set_ptt_command(&zus, (PttCommandParams){
        .button = PTT_PRESSED, .call_kind = CALL_KIND_CARE_LISTEN
    });
    IntercomCore_set_ptt_command(&zus, (PttCommandParams){
        .button = PTT_RELEASED, .call_kind = CALL_KIND_CARE_LISTEN
    });

    /* Route Zus <-> Ma-Beneden Challenge/Confirm */
    sim_us += 20000ULL;
    for (size_t pass = 0U; pass < 3U; ++pass) {
        TxEnvelope_Owned env = {0};
        while (IntercomCore_pop_tx_move(&zus, &env) == CORE_OK) {
            IntercomCore_on_rx_datagram(&ma_beneden, (IntercomRxDatagramParams){
                .raw_datagram = WireDatagram_loan(&env.datagram),
                .now          = { .us = sim_us }
            });
        }
        while (IntercomCore_pop_tx_move(&ma_beneden, &env) == CORE_OK) {
            IntercomCore_on_rx_datagram(&zus, (IntercomRxDatagramParams){
                .raw_datagram = WireDatagram_loan(&env.datagram),
                .now          = { .us = sim_us }
            });
        }
    }

    TEST_CHECK(DuplexGate_state(&ma_beneden.duplex_gate) == DUPLEX_STATE_AUTO_REPLY_TX);
    TEST_CHECK(ma_beneden.authorized_listener.id == 5U); /* Streaming exclusively to Zus! */

    return 0;
}
