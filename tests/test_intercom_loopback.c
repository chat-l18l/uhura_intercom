#include "core/intercom_core.h"

#include <stdio.h>

#define TEST_CHECK(cond)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static const int16_t VOICE_TONE_16[16] = {
       0,  3000,  5600,  7400,  8000,  7400,  5600,  3000,
       0, -3000, -5600, -7400, -8000, -7400, -5600, -3000
};

static PcmFrame_Owned make_voice_frame(uint32_t *phase_step_idx, uint32_t stride) {
    PcmFrame_Owned f = {0};
    PcmFrame_init_silence(&f);
    for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
        f.samples[i] = VOICE_TONE_16[(*phase_step_idx) & 0x0FU];
        *phase_step_idx += stride;
    }
    return f;
}

static int64_t frame_abs_energy(const PcmFrame_Owned *f) {
    int64_t e = 0;
    for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
        const int32_t v = f->samples[i];
        e += (v < 0) ? -v : v;
    }
    return e;
}

static void route_outbox_to_stations(
    IntercomCore_Owned *sender,
    IntercomCore_Owned *nodes[3],
    MonotonicTimeUs now,
    bool drop_audio_seq_4
) {
    TxEnvelope_Owned env = {0};
    for (size_t guard = 0U; guard < (size_t)UHURA_TX_OUTBOX_CAPACITY; ++guard) {
        if (IntercomCore_pop_tx_move(sender, &env) != CORE_OK) {
            break;
        }
        WirePacketView_Loaned view = {0};
        if (WireProto_parse(WireDatagram_loan(&env.datagram), &view) == WIRE_OK) {
            if (drop_audio_seq_4 && (view.kind == PACKET_KIND_AUDIO) && (view.seq.num == 4U)) {
                continue; /* Simulate lost WireGuard UDP packet */
            }
        }
        for (size_t n = 0U; n < 3U; ++n) {
            if (TargetPeerMask_contains(env.recipients, nodes[n]->local_station)) {
                IntercomCore_on_rx_datagram(nodes[n], (IntercomRxDatagramParams){
                    .raw_datagram = WireDatagram_loan(&env.datagram),
                    .now          = now
                });
            }
        }
    }
}

int main(void) {
    /* 3 Enterprise stations: 1=Brug, 2=Machinekamer, 3=Kantine */
    IntercomCore_Owned brug;
    IntercomCore_Owned machinekamer;
    IntercomCore_Owned kantine;

    const IntercomConfigParams base_cfg = {
        .local_station   = { .id = 1U },
        .opus_bitrate    = { .bps = 32000U },
        .fec_mode        = FEC_ENABLED,
        .jitter_prefetch = { .count = 2U },
        .hangover_frames = { .count = 2U },
        .chime_mode      = CHIME_ENABLED
    };

    IntercomConfigParams cfg1 = base_cfg;
    IntercomConfigParams cfg2 = base_cfg;
    IntercomConfigParams cfg3 = base_cfg;
    cfg1.local_station.id = 1U;
    cfg2.local_station.id = 2U;
    cfg3.local_station.id = 3U;

    TEST_CHECK(IntercomCore_init(&brug, cfg1) == CORE_OK);
    TEST_CHECK(IntercomCore_init(&machinekamer, cfg2) == CORE_OK);
    TEST_CHECK(IntercomCore_init(&kantine, cfg3) == CORE_OK);

    TEST_CHECK(IntercomCore_register_peer(&brug, (PeerRegisterParams){
        .station = { .id = 2U }, .name = "Machinekamer"
    }) == CORE_OK);
    TEST_CHECK(IntercomCore_register_peer(&brug, (PeerRegisterParams){
        .station = { .id = 3U }, .name = "Kantine"
    }) == CORE_OK);
    TEST_CHECK(IntercomCore_register_peer(&machinekamer, (PeerRegisterParams){
        .station = { .id = 1U }, .name = "Brug"
    }) == CORE_OK);
    TEST_CHECK(IntercomCore_register_peer(&kantine, (PeerRegisterParams){
        .station = { .id = 1U }, .name = "Brug"
    }) == CORE_OK);

    IntercomCore_Owned *all_nodes[3] = { &brug, &machinekamer, &kantine };

    /* Scenario A: Brug calls ONLY Machinekamer (Kantine must stay completely silent!) */
    IntercomCore_set_targets(&brug, TargetPeerMask_from_station((StationId){ .id = 2U }));
    IntercomCore_set_ptt(&brug, PTT_PRESSED);

    uint64_t sim_us     = 1000000ULL;
    uint32_t brug_phase = 0U;
    int64_t energy_mk   = 0;
    int64_t energy_kan  = 0;
    int64_t energy_brug_spk = 0;

    PcmFrame_Owned silence = {0};
    PcmFrame_init_silence(&silence);

    route_outbox_to_stations(&brug, all_nodes, (MonotonicTimeUs){ .us = sim_us }, false);

    for (size_t tick = 0U; tick < 12U; ++tick) {
        sim_us += 10000ULL;
        const MonotonicTimeUs now = { .us = sim_us };

        PcmFrame_Owned mic_brug = make_voice_frame(&brug_phase, 1U);
        PcmFrame_Owned spk_brug = {0};
        PcmFrame_Owned spk_mk   = {0};
        PcmFrame_Owned spk_kan  = {0};

        IntercomCore_tick_10ms(&brug, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&mic_brug), .now = now
        }, &spk_brug);
        route_outbox_to_stations(&brug, all_nodes, now, true);

        IntercomCore_tick_10ms(&machinekamer, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&silence), .now = now
        }, &spk_mk);
        route_outbox_to_stations(&machinekamer, all_nodes, now, false);

        IntercomCore_tick_10ms(&kantine, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&silence), .now = now
        }, &spk_kan);
        route_outbox_to_stations(&kantine, all_nodes, now, false);

        energy_brug_spk += frame_abs_energy(&spk_brug);
        energy_mk       += frame_abs_energy(&spk_mk);
        energy_kan      += frame_abs_energy(&spk_kan);
    }

    IntercomCore_set_ptt(&brug, PTT_RELEASED);
    route_outbox_to_stations(&brug, all_nodes, (MonotonicTimeUs){ .us = sim_us }, false);

    /* Brug's own speaker MUST be muted while transmitting (half-duplex PTT gate) */
    TEST_CHECK(energy_brug_spk == 0);
    /* Machinekamer received audio + Enterprise whistle chime */
    TEST_CHECK(energy_mk > 200000);
    /* Kantine was not targeted -> zero speaker energy */
    TEST_CHECK(energy_kan == 0);

    /* Scenario B: Brug selects BOTH Machinekamer AND Kantine (All-Call) */
    IntercomCore_set_targets(&brug, TargetPeerMask_all());
    IntercomCore_set_ptt(&brug, PTT_PRESSED);
    route_outbox_to_stations(&brug, all_nodes, (MonotonicTimeUs){ .us = sim_us }, false);

    int64_t energy_mk_all  = 0;
    int64_t energy_kan_all = 0;

    for (size_t tick = 0U; tick < 10U; ++tick) {
        sim_us += 10000ULL;
        const MonotonicTimeUs now = { .us = sim_us };

        PcmFrame_Owned mic_brug = make_voice_frame(&brug_phase, 1U);
        PcmFrame_Owned spk_brug = {0};
        PcmFrame_Owned spk_mk   = {0};
        PcmFrame_Owned spk_kan  = {0};

        IntercomCore_tick_10ms(&brug, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&mic_brug), .now = now
        }, &spk_brug);
        route_outbox_to_stations(&brug, all_nodes, now, false);

        IntercomCore_tick_10ms(&machinekamer, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&silence), .now = now
        }, &spk_mk);
        IntercomCore_tick_10ms(&kantine, (IntercomTickParams){
            .mic_pcm = PcmFrame_loan(&silence), .now = now
        }, &spk_kan);

        energy_mk_all  += frame_abs_energy(&spk_mk);
        energy_kan_all += frame_abs_energy(&spk_kan);
    }

    TEST_CHECK(energy_mk_all > 200000);
    TEST_CHECK(energy_kan_all > 200000);
    TEST_CHECK(IntercomCore_is_peer_online(
        &machinekamer,
        (StationId){ .id = 1U },
        (MonotonicTimeUs){ .us = sim_us }
    ));

    return 0;
}
