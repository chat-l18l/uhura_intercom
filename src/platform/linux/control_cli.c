#include "platform/linux_hal.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

NODISCARD HalError CliControl_open(CliControl_Owned *ctx) {
    assert(ctx != NULL);
    memset(ctx, 0, sizeof(*ctx));
    ctx->tty_fd.fd = -1;

    if (isatty(STDIN_FILENO) == 0) {
        ctx->is_open = true;
        return HAL_OK;
    }

    if (tcgetattr(STDIN_FILENO, &ctx->orig_termios) < 0) {
        return HAL_ERR_TTY_SETUP;
    }

    struct termios raw = ctx->orig_termios;
    raw.c_lflag &= (tcflag_t)(~(ICANON | ECHO));
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
        return HAL_ERR_TTY_SETUP;
    }

    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

    ctx->tty_fd.fd       = STDIN_FILENO;
    ctx->raw_mode_active = true;
    ctx->is_open         = true;
    return HAL_OK;
}

void CliControl_close(CliControl_Owned *ctx) {
    assert(ctx != NULL);
    if (ctx->raw_mode_active) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &ctx->orig_termios);
        (void)fprintf(stdout, "\r\n");
        (void)fflush(stdout);
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->tty_fd.fd = -1;
}

NODISCARD FileDesc CliControl_poll_fd(const CliControl_Owned *ctx) {
    assert(ctx != NULL);
    return ctx->tty_fd;
}

NODISCARD CliCommand CliControl_read_command(const CliControl_Owned *ctx) {
    assert(ctx != NULL);
    CliCommand cmd = { .kind = CLI_CMD_NONE, .station_arg = { .id = 0U } };

    if (!ctx->raw_mode_active || (ctx->tty_fd.fd < 0)) {
        return cmd;
    }

    char ch = '\0';
    const ssize_t n = read(ctx->tty_fd.fd, &ch, 1U);
    if (n <= 0) {
        return cmd;
    }

    if ((ch == ' ') || (ch == 't') || (ch == 'T')) {
        cmd.kind = CLI_CMD_TOGGLE_PTT;
    } else if ((ch == 'w') || (ch == 'W')) {
        cmd.kind = CLI_CMD_TOGGLE_CARE_PTT;
    } else if ((ch == '\n') || (ch == '\r')) {
        cmd.kind = CLI_CMD_RELEASE_PTT;
    } else if ((ch == 'x') || (ch == 'X')) {
        cmd.kind = CLI_CMD_ABORT_LISTEN;
    } else if ((ch == '-') || (ch == '_')) {
        cmd.kind = CLI_CMD_VOL_DOWN;
    } else if ((ch == '+') || (ch == '=')) {
        cmd.kind = CLI_CMD_VOL_UP;
    } else if ((ch >= '1') && (ch <= '9')) {
        cmd.kind        = CLI_CMD_TOGGLE_STATION;
        cmd.station_arg = (StationId){ .id = (uint8_t)(ch - '0') };
    } else if ((ch == 'a') || (ch == 'A')) {
        cmd.kind = CLI_CMD_SELECT_ALL;
    } else if ((ch == 'c') || (ch == 'C')) {
        cmd.kind = CLI_CMD_CLEAR_TARGETS;
    } else if ((ch == 'q') || (ch == 'Q') || (ch == 3)) {
        cmd.kind = CLI_CMD_QUIT;
    }

    return cmd;
}

void CliControl_render_status(const CliControl_Owned *ctx, CliStatusRenderParams params) {
    assert(ctx != NULL);
    assert(params.core != NULL);

    if (!ctx->raw_mode_active) {
        return;
    }

    const DuplexState st = DuplexGate_state(&params.core->duplex_gate);
    char state_buf[44];
    if (st == DUPLEX_STATE_TRANSMITTING) {
        if (params.core->active_tx_call_kind == CALL_KIND_CARE_LISTEN) {
            (void)snprintf(state_buf, sizeof(state_buf), ">>> WELZIJNS-ROEP [ROOD] <<<");
        } else {
            (void)snprintf(state_buf, sizeof(state_buf), ">>> ZENDEN (PTT) [ROOD] <<<");
        }
    } else if (st == DUPLEX_STATE_AUTO_REPLY_TX) {
        const uint16_t rem_ticks = DuplexGate_auto_reply_remaining(&params.core->duplex_gate).count;
        const unsigned int rem_s = (unsigned int)((rem_ticks + 99U) / 100U);
        (void)snprintf(
            state_buf,
            sizeof(state_buf),
            ">>> MIC OPEN %2us->#%u [ROOD] <<<",
            rem_s,
            (unsigned int)params.core->authorized_listener.id
        );
    } else if (st == DUPLEX_STATE_HANGOVER) {
        (void)snprintf(state_buf, sizeof(state_buf), "COOLDOWN");
    } else {
        (void)snprintf(state_buf, sizeof(state_buf), "LUISTEREN");
    }

    const VolumePercent eff_vol = IntercomCore_effective_volume(params.core);
    const bool is_emerg_vol =
        (AudioDsp_playback_gain_mode(&params.core->audio_dsp) == GAIN_MODE_EMERGENCY_FULL);

    const TargetPeerMask sel     = IntercomCore_selected_targets(params.core);
    const TargetPeerMask talkers = IntercomCore_active_talkers(params.core);
    const TargetPeerMask audible = IntercomCore_audible_talkers(params.core);
    const char *self_label       = (params.local_name != NULL) ? params.local_name : "Post";

    (void)fprintf(
        stdout,
        "\r\033[2K[#%u %s] %-31s | Vol:%u%%%s | [",
        (unsigned int)params.core->local_station.id,
        self_label,
        state_buf,
        (unsigned int)eff_vol.pct,
        is_emerg_vol ? "(NOOD!)" : ""
    );

    bool first = true;
    for (uint8_t id = 1U; id <= (uint8_t)UHURA_MAX_PEERS; ++id) {
        const PeerInfo *peer = &params.core->peers[id - 1U];
        if (!peer->is_configured) {
            continue;
        }
        const StationId sid    = { .id = id };
        const bool is_sel      = TargetPeerMask_contains(sel, sid);
        const bool is_online   = IntercomCore_is_peer_online(params.core, sid, params.now);
        const bool is_stream   = TargetPeerMask_contains(talkers, sid);
        const bool is_loud     = TargetPeerMask_contains(audible, sid);
        const bool can_listen  = (peer->listen_perm == LISTEN_PERM_ALLOWED);

        if (!first) {
            (void)fprintf(stdout, " ");
        }
        first = false;

        const char *act_tag = "";
        if (is_loud) {
            act_tag = "(GELUID!)";
        } else if (is_stream) {
            act_tag = "(open)";
        }

        (void)fprintf(
            stdout,
            "%s%u:%s%s%s%s",
            is_sel ? "+" : "-",
            (unsigned int)id,
            (peer->name[0] != '\0') ? peer->name : "Peer",
            can_listen ? "[L]" : "",
            is_online ? "*" : "",
            act_tag
        );
    }

    (void)fprintf(stdout, "] (Spatie=PTT, w=Welzijn+30s, x=StopMic, -/+=Vol, q=Uit)");
    (void)fflush(stdout);
}
