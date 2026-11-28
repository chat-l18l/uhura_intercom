#include "core/intercom_core.h"
#include "platform/linux_hal.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* =========================================================================
 * Main Linux Intercom Node Runner
 * Strictly zero global variables (Rule 4): uses signalfd + timerfd + poll()
 * and a single AppRuntime_Owned in main().
 * ========================================================================= */
typedef struct {
    IntercomCore_Owned core;
    UdpTransport_Owned udp;
    AudioHal_Owned     audio;
    CliControl_Owned   cli;
    FileDesc           timer_fd;
    FileDesc           signal_fd;
    char               local_name[UHURA_STATION_NAME_MAX];
    char               bind_ip[UHURA_IP_STRING_MAX];
    char               cap_dev[UHURA_DEVICE_NAME_MAX];
    char               play_dev[UHURA_DEVICE_NAME_MAX];
    UdpPort            bind_port;
    PttButtonState     ptt_state;
    CallKind           ptt_call_kind;
    uint32_t           synth_phase_q32;
    uint32_t           synth_tick_counter;
} AppRuntime_Owned;

/* Sine lookup for headless --no-audio test tone (440 Hz A4) */
static const int16_t TEST_SINE_32[32] = {
       0,   978,  1919,  2786,  3546,  4170,  4634,  4919,
    5016,  4919,  4634,  4170,  3546,  2786,  1919,   978,
       0,  -978, -1919, -2786, -3546, -4170, -4634, -4919,
   -5016, -4919, -4634, -4170, -3546, -2786, -1919,  -978
};

static MonotonicTimeUs monotonic_now_us(void) {
    struct timespec ts = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t sec_us  = (uint64_t)ts.tv_sec * 1000000ULL;
    const uint64_t nsec_us = (uint64_t)ts.tv_nsec / 1000ULL;
    return (MonotonicTimeUs){ .us = sec_us + nsec_us };
}

static void copy_cstr_bounded(char *dst, size_t cap, const char *src) {
    assert(dst != NULL);
    assert(cap > 0U);
    memset(dst, 0, cap);
    if (src == NULL) {
        return;
    }
    for (size_t i = 0U; i < (cap - 1U); ++i) {
        if (src[i] == '\0') {
            break;
        }
        dst[i] = src[i];
    }
}

static bool parse_u16_strict(const char *s, uint16_t *out_val) {
    if ((s == NULL) || (s[0] == '\0') || (out_val == NULL)) {
        return false;
    }
    char *endp = NULL;
    const unsigned long val = strtoul(s, &endp, 10);
    if ((endp == s) || (*endp != '\0') || (val > 65535UL)) {
        return false;
    }
    *out_val = (uint16_t)val;
    return true;
}

/* Parses "--peer ID:Name:IP:Port" (e.g. "2:Ma-Woonkamer:10.8.0.2:4711") */
static bool parse_and_add_peer(AppRuntime_Owned *app, const char *spec) {
    assert(app != NULL);
    if (spec == NULL) {
        return false;
    }

    char buf[160];
    copy_cstr_bounded(buf, sizeof(buf), spec);

    char *c1 = strchr(buf, ':');
    if (c1 == NULL) {
        return false;
    }
    *c1 = '\0';

    char *name_part = c1 + 1;
    char *c2 = strchr(name_part, ':');
    if (c2 == NULL) {
        return false;
    }
    *c2 = '\0';

    char *ip_and_port = c2 + 1;
    char *c_last = strrchr(ip_and_port, ':');
    if (c_last == NULL) {
        return false;
    }
    *c_last = '\0';
    const char *ip_part   = ip_and_port;
    const char *port_part = c_last + 1;

    uint16_t id_u16   = 0U;
    uint16_t port_u16 = 0U;
    if (!parse_u16_strict(buf, &id_u16) || !parse_u16_strict(port_part, &port_u16)) {
        return false;
    }
    if ((id_u16 < 1U) || (id_u16 > (uint16_t)UHURA_MAX_PEERS) || (port_u16 == 0U)) {
        return false;
    }

    const StationId st = { .id = (uint8_t)id_u16 };
    if (IntercomCore_register_peer(&app->core, (PeerRegisterParams){
            .station = st,
            .name    = name_part
        }) != CORE_OK) {
        return false;
    }

    if (UdpTransport_set_peer_endpoint(&app->udp, (UdpPeerEndpointParams){
            .station = st,
            .ip_str  = ip_part,
            .port    = { .port = port_u16 }
        }) != HAL_OK) {
        return false;
    }

    return true;
}

static void print_usage(const char *prog) {
    (void)fprintf(
        stderr,
        "Gebruik: %s --id <1..16> [--name <Naam>] [--bind <IP>] [--port <Poort>]\n"
        "            --peer <ID:Naam:IP:Poort> [--peer ...] [--allow-listen <ID>]\n"
        "            [--volume <0..100>] [--capture-dev <ALSA>] [--playback-dev <ALSA>]\n"
        "            [--no-chime] [--no-audio]\n\n"
        "Voorbeeld (Kastje bij Moeder: geeft ALLEEN post #1 Edwin en #4 Zus luisterrechten):\n"
        "  %s --id 2 --name Ma-Woonkamer --port 4712 --volume 60 \\\n"
        "     --peer 1:Edwin:10.8.0.1:4711 --allow-listen 1 \\\n"
        "     --peer 4:Zus:10.8.0.4:4711   --allow-listen 4 \\\n"
        "     --peer 5:Buurvrouw:10.8.0.5:4711\n",
        prog,
        prog
    );
}

NODISCARD static FileDesc create_10ms_timerfd(void) {
    const int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) {
        return (FileDesc){ .fd = -1 };
    }

    struct itimerspec its = {
        .it_interval = { .tv_sec = 0, .tv_nsec = 10000000L }, /* 10 ms */
        .it_value    = { .tv_sec = 0, .tv_nsec = 10000000L }
    };
    if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
        (void)close(tfd);
        return (FileDesc){ .fd = -1 };
    }
    return (FileDesc){ .fd = tfd };
}

NODISCARD static FileDesc create_stop_signalfd(void) {
    sigset_t mask;
    (void)sigemptyset(&mask);
    (void)sigaddset(&mask, SIGINT);
    (void)sigaddset(&mask, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        return (FileDesc){ .fd = -1 };
    }

    const int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    return (FileDesc){ .fd = sfd };
}

static void drain_tx_outbox(AppRuntime_Owned *app) {
    TxEnvelope_Owned env = {0};
    for (size_t guard = 0U; guard < (size_t)UHURA_TX_OUTBOX_CAPACITY; ++guard) {
        if (IntercomCore_pop_tx_move(&app->core, &env) != CORE_OK) {
            break;
        }
        (void)UdpTransport_send_fanout(&app->udp, (UdpFanoutSendParams){
            .recipients = env.recipients,
            .datagram   = WireDatagram_loan(&env.datagram)
        });
    }
}

static void fill_synth_mic_frame(AppRuntime_Owned *app, PcmFrame_Owned *out_mic) {
    PcmFrame_init_silence(out_mic);
    app->synth_tick_counter += 1U;

    const DuplexState st = DuplexGate_state(&app->core.duplex_gate);
    if (st == DUPLEX_STATE_TRANSMITTING) {
        /* Continuous 440 Hz tone during manual PTT in --no-audio test mode */
        const uint32_t step_440_q32 = 39370534U;
        for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
            const size_t idx    = (size_t)((app->synth_phase_q32 >> 27U) & 0x1FU);
            out_mic->samples[i] = TEST_SINE_32[idx];
            app->synth_phase_q32 += step_440_q32;
        }
    } else if (st == DUPLEX_STATE_AUTO_REPLY_TX) {
        /* Simulated faint door-knock pulse (80 ms every 1.0 s) in --no-audio test mode */
        if ((app->synth_tick_counter % 100U) < 8U) {
            const uint32_t step_220_q32 = 19685267U;
            for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
                const size_t idx    = (size_t)((app->synth_phase_q32 >> 27U) & 0x1FU);
                out_mic->samples[i] = (int16_t)(TEST_SINE_32[idx] >> 3); /* Faint distant level */
                app->synth_phase_q32 += step_220_q32;
            }
        }
    }
}

static void adjust_user_volume(AppRuntime_Owned *app, int delta_pct) {
    const int cur = (int)AudioDsp_user_volume(&app->core.audio_dsp).pct;
    int next = cur + delta_pct;
    if (next < 0) {
        next = 0;
    } else if (next > 100) {
        next = 100;
    }
    IntercomCore_set_user_volume(&app->core, (VolumePercent){ .pct = (uint8_t)next });
}

int main(int argc, char **argv) {
    AppRuntime_Owned app;
    memset(&app, 0, sizeof(app));
    app.timer_fd.fd  = -1;
    app.signal_fd.fd = -1;
    app.bind_port    = (UdpPort){ .port = 4711U };
    copy_cstr_bounded(app.local_name, sizeof(app.local_name), "Brug");
    copy_cstr_bounded(app.bind_ip, sizeof(app.bind_ip), "0.0.0.0");
    copy_cstr_bounded(app.cap_dev, sizeof(app.cap_dev), "default");
    copy_cstr_bounded(app.play_dev, sizeof(app.play_dev), "default");

    StationId local_id        = { .id = 0U };
    VolumePercent init_volume = { .pct = 80U };
    ChimeMode chime_mode      = CHIME_ENABLED;
    AudioBackendMode aud_mode = AUDIO_MODE_ALSA;

    /* Pass 1: parse station & audio options */
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--help") == 0) || (strcmp(argv[i], "-h") == 0)) {
            print_usage(argv[0]);
            return 0;
        } else if ((strcmp(argv[i], "--id") == 0) && ((i + 1) < argc)) {
            uint16_t v = 0U;
            if (!parse_u16_strict(argv[++i], &v) || (v < 1U) || (v > (uint16_t)UHURA_MAX_PEERS)) {
                (void)fprintf(stderr, "Ongeldig --id (verwacht 1..16)\n");
                return 1;
            }
            local_id.id = (uint8_t)v;
        } else if ((strcmp(argv[i], "--name") == 0) && ((i + 1) < argc)) {
            copy_cstr_bounded(app.local_name, sizeof(app.local_name), argv[++i]);
        } else if ((strcmp(argv[i], "--bind") == 0) && ((i + 1) < argc)) {
            copy_cstr_bounded(app.bind_ip, sizeof(app.bind_ip), argv[++i]);
        } else if ((strcmp(argv[i], "--port") == 0) && ((i + 1) < argc)) {
            if (!parse_u16_strict(argv[++i], &app.bind_port.port) || (app.bind_port.port == 0U)) {
                (void)fprintf(stderr, "Ongeldige --port\n");
                return 1;
            }
        } else if ((strcmp(argv[i], "--volume") == 0) && ((i + 1) < argc)) {
            uint16_t vol_u16 = 0U;
            if (!parse_u16_strict(argv[++i], &vol_u16) || (vol_u16 > 100U)) {
                (void)fprintf(stderr, "Ongeldig --volume (verwacht 0..100)\n");
                return 1;
            }
            init_volume.pct = (uint8_t)vol_u16;
        } else if ((strcmp(argv[i], "--capture-dev") == 0) && ((i + 1) < argc)) {
            copy_cstr_bounded(app.cap_dev, sizeof(app.cap_dev), argv[++i]);
        } else if ((strcmp(argv[i], "--playback-dev") == 0) && ((i + 1) < argc)) {
            copy_cstr_bounded(app.play_dev, sizeof(app.play_dev), argv[++i]);
        } else if (strcmp(argv[i], "--no-chime") == 0) {
            chime_mode = CHIME_DISABLED;
        } else if (strcmp(argv[i], "--no-audio") == 0) {
            aud_mode = AUDIO_MODE_SYNTH_NO_HW;
        } else if (((strcmp(argv[i], "--peer") == 0) ||
                    (strcmp(argv[i], "--allow-listen") == 0)) && ((i + 1) < argc)) {
            ++i; /* Handled in pass 2 / pass 3 */
        } else {
            (void)fprintf(stderr, "Onbekende optie: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!StationId_is_valid(local_id)) {
        (void)fprintf(stderr, "Fout: --id <1..16> is verplicht.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (IntercomCore_init(&app.core, (IntercomConfigParams){
            .local_station   = local_id,
            .opus_bitrate    = { .bps = (uint32_t)UHURA_DEFAULT_BITRATE_BPS },
            .fec_mode        = FEC_ENABLED,
            .jitter_prefetch = { .count = (uint16_t)UHURA_DEFAULT_PREFETCH_SLOTS },
            .hangover_frames = { .count = (uint16_t)UHURA_DEFAULT_HANGOVER_FRAMES },
            .chime_mode      = chime_mode
        }) != CORE_OK) {
        (void)fprintf(stderr, "Fout: IntercomCore_init mislukt.\n");
        return 1;
    }
    IntercomCore_set_user_volume(&app.core, init_volume);

    if (UdpTransport_open(&app.udp, (UdpBindParams){
            .bind_ip   = app.bind_ip,
            .bind_port = app.bind_port
        }) != HAL_OK) {
        (void)fprintf(stderr, "Fout: kan UDP poort %u op %s niet openen.\n",
                      (unsigned int)app.bind_port.port, app.bind_ip);
        return 1;
    }

    /* Pass 2: register peers (all default to LISTEN_PERM_DENIED) */
    size_t peer_count = 0U;
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--peer") == 0) && ((i + 1) < argc)) {
            const char *spec = argv[++i];
            if (!parse_and_add_peer(&app, spec)) {
                (void)fprintf(stderr, "Fout: ongeldige --peer specificatie: %s\n", spec);
                UdpTransport_close(&app.udp);
                return 1;
            }
            peer_count += 1U;
        }
    }

    /* Pass 3: apply explicit local --allow-listen permissions (supports "1" or "1,4") */
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--allow-listen") == 0) && ((i + 1) < argc)) {
            const char *p = argv[++i];
            bool any_parsed = false;
            for (size_t guard = 0U; (guard < (size_t)UHURA_MAX_PEERS) && (*p != '\0'); ++guard) {
                while ((*p == ' ') || (*p == ',') || (*p == ';')) {
                    ++p;
                }
                if (*p == '\0') {
                    break;
                }
                char *endp = NULL;
                const unsigned long perm_ul = strtoul(p, &endp, 10);
                if ((endp == p) || (perm_ul < 1UL) || (perm_ul > (unsigned long)UHURA_MAX_PEERS)) {
                    any_parsed = false;
                    break;
                }
                if (IntercomCore_set_peer_listen_permission(&app.core, (PeerPermissionParams){
                        .station    = { .id = (uint8_t)perm_ul },
                        .permission = LISTEN_PERM_ALLOWED
                    }) != CORE_OK) {
                    any_parsed = false;
                    break;
                }
                any_parsed = true;
                p = endp;
            }
            if (!any_parsed) {
                (void)fprintf(stderr, "Fout: ongeldige --allow-listen waarde.\n");
                UdpTransport_close(&app.udp);
                return 1;
            }
        }
    }

    if (peer_count == 0U) {
        (void)fprintf(stderr, "Waarschuwing: geen --peer opgegeven (alleen luisteren).\n");
    }

    if (AudioHal_open_and_start(&app.audio, (AudioHalConfigParams){
            .capture_device  = app.cap_dev,
            .playback_device = app.play_dev,
            .alsa_latency    = { .ms = 30U },
            .mode            = aud_mode
        }) != HAL_OK) {
        (void)fprintf(
            stderr,
            "Fout: kon ALSA devices ('%s' / '%s') niet openen. "
            "(Tip: gebruik --no-audio voor test zonder geluidskaart).\n",
            app.cap_dev,
            app.play_dev
        );
        UdpTransport_close(&app.udp);
        return 1;
    }

    (void)CliControl_open(&app.cli);
    app.timer_fd  = create_10ms_timerfd();
    app.signal_fd = create_stop_signalfd();

    if ((app.timer_fd.fd < 0) || (app.signal_fd.fd < 0)) {
        (void)fprintf(stderr, "Fout: kon timerfd/signalfd niet aanmaken.\n");
        CliControl_close(&app.cli);
        AudioHal_stop_and_close(&app.audio);
        UdpTransport_close(&app.udp);
        return 1;
    }

    uint32_t ui_tick_div = 0U;
    bool running         = true;

    while (running) {
        struct pollfd pfds[4];
        pfds[0] = (struct pollfd){ .fd = UdpTransport_poll_fd(&app.udp).fd, .events = POLLIN };
        pfds[1] = (struct pollfd){ .fd = app.timer_fd.fd,                   .events = POLLIN };
        pfds[2] = (struct pollfd){ .fd = app.signal_fd.fd,                  .events = POLLIN };
        pfds[3] = (struct pollfd){ .fd = CliControl_poll_fd(&app.cli).fd,   .events = POLLIN };

        const int ready = poll(pfds, 4U, 100);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        const MonotonicTimeUs now = monotonic_now_us();

        /* 1. Stop signal */
        if ((pfds[2].revents & POLLIN) != 0) {
            struct signalfd_siginfo fdsi;
            (void)read(app.signal_fd.fd, &fdsi, sizeof(fdsi));
            running = false;
            break;
        }

        /* 2. Incoming UDP datagrams (may enqueue Challenge/Confirm replies -> drain outbox!) */
        if ((pfds[0].revents & POLLIN) != 0) {
            for (size_t rx_i = 0U; rx_i < 32U; ++rx_i) {
                WireDatagram_Owned rx_dgram = {0};
                const HalError rx_err = UdpTransport_recv_next(&app.udp, &rx_dgram);
                if (rx_err == HAL_ERR_WOULD_BLOCK) {
                    break;
                }
                if ((rx_err == HAL_OK) && rx_dgram.is_valid) {
                    IntercomCore_on_rx_datagram(&app.core, (IntercomRxDatagramParams){
                        .raw_datagram = WireDatagram_loan(&rx_dgram),
                        .now          = now
                    });
                }
            }
            drain_tx_outbox(&app);
        }

        /* 3. Interactive CLI commands */
        if ((pfds[3].fd >= 0) && ((pfds[3].revents & POLLIN) != 0)) {
            const CliCommand cmd = CliControl_read_command(&app.cli);
            if (cmd.kind == CLI_CMD_QUIT) {
                running = false;
                break;
            } else if (cmd.kind == CLI_CMD_TOGGLE_PTT) {
                app.ptt_state     = (app.ptt_state == PTT_PRESSED) ? PTT_RELEASED : PTT_PRESSED;
                app.ptt_call_kind = CALL_KIND_NORMAL_PTT;
                IntercomCore_set_ptt_command(&app.core, (PttCommandParams){
                    .button    = app.ptt_state,
                    .call_kind = CALL_KIND_NORMAL_PTT
                });
                drain_tx_outbox(&app);
            } else if (cmd.kind == CLI_CMD_TOGGLE_CARE_PTT) {
                if (app.ptt_state == PTT_PRESSED) {
                    app.ptt_state = PTT_RELEASED;
                } else {
                    app.ptt_state     = PTT_PRESSED;
                    app.ptt_call_kind = CALL_KIND_CARE_LISTEN;
                }
                IntercomCore_set_ptt_command(&app.core, (PttCommandParams){
                    .button    = app.ptt_state,
                    .call_kind = app.ptt_call_kind
                });
                drain_tx_outbox(&app);
            } else if (cmd.kind == CLI_CMD_RELEASE_PTT) {
                app.ptt_state = PTT_RELEASED;
                IntercomCore_set_ptt_command(&app.core, (PttCommandParams){
                    .button    = PTT_RELEASED,
                    .call_kind = app.ptt_call_kind
                });
                drain_tx_outbox(&app);
            } else if (cmd.kind == CLI_CMD_ABORT_LISTEN) {
                IntercomCore_abort_care_listen(&app.core);
                drain_tx_outbox(&app);
            } else if (cmd.kind == CLI_CMD_VOL_DOWN) {
                adjust_user_volume(&app, -10);
            } else if (cmd.kind == CLI_CMD_VOL_UP) {
                adjust_user_volume(&app, +10);
            } else if (cmd.kind == CLI_CMD_TOGGLE_STATION) {
                const TargetPeerMask cur = IntercomCore_selected_targets(&app.core);
                const TargetPeerMask next = TargetPeerMask_contains(cur, cmd.station_arg)
                    ? TargetPeerMask_without_station(cur, cmd.station_arg)
                    : TargetPeerMask_with_station(cur, cmd.station_arg);
                IntercomCore_set_targets(&app.core, next);
            } else if (cmd.kind == CLI_CMD_SELECT_ALL) {
                IntercomCore_set_targets(&app.core, TargetPeerMask_all());
            } else if (cmd.kind == CLI_CMD_CLEAR_TARGETS) {
                IntercomCore_set_targets(&app.core, TargetPeerMask_none());
            }
            ui_tick_div = 10U;
        }

        /* 4. 10 ms deterministic audio & protocol tick */
        if ((pfds[1].revents & POLLIN) != 0) {
            uint64_t expirations = 0ULL;
            (void)read(app.timer_fd.fd, &expirations, sizeof(expirations));
            if (expirations > 4ULL) {
                expirations = 4ULL;
            }

            for (uint64_t t = 0ULL; t < expirations; ++t) {
                PcmFrame_Owned mic_frame = {0};
                if (aud_mode == AUDIO_MODE_SYNTH_NO_HW) {
                    fill_synth_mic_frame(&app, &mic_frame);
                } else {
                    if (SpscPcmRing_pop_move(&app.audio.capture_ring, &mic_frame) != RING_OK) {
                        PcmFrame_init_silence(&mic_frame);
                    }
                }

                PcmFrame_Owned spk_frame = {0};
                IntercomCore_tick_10ms(
                    &app.core,
                    (IntercomTickParams){
                        .mic_pcm = PcmFrame_loan(&mic_frame),
                        .now     = now
                    },
                    &spk_frame
                );

                if (aud_mode == AUDIO_MODE_ALSA) {
                    (void)SpscPcmRing_push_move(&app.audio.playback_ring, &spk_frame);
                }

                drain_tx_outbox(&app);
            }

            ui_tick_div += 1U;
            if (ui_tick_div >= 10U) {
                ui_tick_div = 0U;
                CliControl_render_status(&app.cli, (CliStatusRenderParams){
                    .core       = &app.core,
                    .local_name = app.local_name,
                    .now        = now
                });
            }
        }
    }

    if (app.ptt_state == PTT_PRESSED) {
        IntercomCore_set_ptt_command(&app.core, (PttCommandParams){
            .button    = PTT_RELEASED,
            .call_kind = app.ptt_call_kind
        });
        drain_tx_outbox(&app);
    }

    if (app.timer_fd.fd >= 0) {
        (void)close(app.timer_fd.fd);
    }
    if (app.signal_fd.fd >= 0) {
        (void)close(app.signal_fd.fd);
    }
    CliControl_close(&app.cli);
    AudioHal_stop_and_close(&app.audio);
    UdpTransport_close(&app.udp);
    return 0;
}
