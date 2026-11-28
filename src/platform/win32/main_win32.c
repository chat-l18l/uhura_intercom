#include "core/intercom_core.h"
#include "platform/win32_hal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Native Windows 11 Intercom GUI ("Virtueel Kastje" — < 500 KB standalone)
 *
 * - Zero global variables: WinAppRuntime_Owned* bound to HWND GWLP_USERDATA.
 * - Zero heap allocations: static config tables + stack/frame-bounded buffers.
 * - Reads uhura.txt from the same directory as uhura.exe (plus CLI overrides).
 * ========================================================================= */
enum {
    WIN_UI_WIDTH         = 460,
    WIN_UI_HEIGHT        = 510,
    WIN_TIMER_ID         = 1U,
    WIN_TIMER_MS         = 5U,
    MAX_CONFIG_PEERS     = 16U,
    PEER_SPEC_MAX_LEN    = 140U
};

typedef enum {
    HELD_BTN_NONE = 0,
    HELD_BTN_NORMAL_PTT,
    HELD_BTN_CARE_PTT
} HeldTalkButton;

typedef struct {
    StationId local_id;
    char      local_name[UHURA_STATION_NAME_MAX];
    char      bind_ip[UHURA_WIN_IP_STR_MAX];
    UdpPort   bind_port;
    VolumePercent initial_volume;
    ChimeMode     chime_mode;
    WinAudioMode  audio_mode;
    char      peer_specs[MAX_CONFIG_PEERS][PEER_SPEC_MAX_LEN];
    size_t    peer_spec_count;
    StationId allow_listen_ids[MAX_CONFIG_PEERS];
    size_t    allow_listen_count;
} StartupConfig;

typedef struct {
    IntercomCore_Owned    core;
    WinUdpTransport_Owned udp;
    WinAudioHal_Owned     audio;
    HWND                  hwnd;
    HFONT                 font_title;
    HFONT                 font_body;
    HFONT                 font_bold;
    char                  local_name[UHURA_STATION_NAME_MAX];
    LARGE_INTEGER         qpc_freq;
    uint64_t              next_tick_us;
    HeldTalkButton        held_button;
    bool                  space_ptt_held;
    uint32_t              synth_phase_q32;
    uint32_t              synth_ticks;
    uint32_t              ui_refresh_div;
} WinAppRuntime_Owned;

static const int16_t WIN_SINE_32[32] = {
       0,   978,  1919,  2786,  3546,  4170,  4634,  4919,
    5016,  4919,  4634,  4170,  3546,  2786,  1919,   978,
       0,  -978, -1919, -2786, -3546, -4170, -4634, -4919,
   -5016, -4919, -4634, -4170, -3546, -2786, -1919,  -978
};

static void copy_str_safe(char *dst, size_t cap, const char *src) {
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

static char *trim_whitespace(char *s) {
    while ((*s != '\0') && isspace((unsigned char)*s)) {
        ++s;
    }
    size_t len = strlen(s);
    while ((len > 0U) && isspace((unsigned char)s[len - 1U])) {
        s[len - 1U] = '\0';
        --len;
    }
    return s;
}

static bool parse_u16_str(const char *s, uint16_t *out_val) {
    if ((s == NULL) || (s[0] == '\0') || (out_val == NULL)) {
        return false;
    }
    char *endp = NULL;
    const unsigned long v = strtoul(s, &endp, 10);
    if ((endp == s) || (*endp != '\0') || (v > 65535UL)) {
        return false;
    }
    *out_val = (uint16_t)v;
    return true;
}

/* Accepts single ID ("1"), comma/space list ("1, 4"), or repeated lines */
static void append_allow_listen_list(StartupConfig *cfg, const char *list_str) {
    assert(cfg != NULL);
    if (list_str == NULL) {
        return;
    }

    const char *p = list_str;
    for (size_t guard = 0U; (guard < (size_t)MAX_CONFIG_PEERS) && (*p != '\0'); ++guard) {
        while ((*p != '\0') && (isspace((unsigned char)*p) || (*p == ',') || (*p == ';'))) {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        char *endp = NULL;
        const unsigned long v = strtoul(p, &endp, 10);
        if ((endp == p) || (v < 1UL) || (v > (unsigned long)UHURA_MAX_PEERS)) {
            break;
        }
        if (cfg->allow_listen_count < (size_t)MAX_CONFIG_PEERS) {
            cfg->allow_listen_ids[cfg->allow_listen_count].id = (uint8_t)v;
            cfg->allow_listen_count += 1U;
        }
        p = endp;
    }
}

static MonotonicTimeUs win_monotonic_us(const WinAppRuntime_Owned *app) {
    LARGE_INTEGER counter;
    (void)QueryPerformanceCounter(&counter);
    const uint64_t ticks = (uint64_t)counter.QuadPart;
    const uint64_t freq  = (uint64_t)app->qpc_freq.QuadPart;
    const uint64_t sec   = ticks / freq;
    const uint64_t rem   = ticks % freq;
    return (MonotonicTimeUs){ .us = (sec * 1000000ULL) + ((rem * 1000000ULL) / freq) };
}

/* Loads optional uhura.txt located next to uhura.exe */
static void load_uhura_txt_next_to_exe(StartupConfig *cfg) {
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if ((n == 0U) || (n >= (DWORD)(sizeof(path) - 16U))) {
        return;
    }

    char *last_sep = strrchr(path, '\\');
    if (last_sep == NULL) {
        last_sep = strrchr(path, '/');
    }
    if (last_sep != NULL) {
        copy_str_safe(last_sep + 1, sizeof(path) - (size_t)(last_sep + 1 - path), "uhura.txt");
    } else {
        copy_str_safe(path, sizeof(path), "uhura.txt");
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return;
    }

    char line[256];
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        char *trimmed = trim_whitespace(line);
        if ((trimmed[0] == '\0') || (trimmed[0] == '#') || (trimmed[0] == ';')) {
            continue;
        }
        char *eq = strchr(trimmed, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        const char *key = trim_whitespace(trimmed);
        const char *val = trim_whitespace(eq + 1);

        uint16_t tmp_u16 = 0U;
        if (strcmp(key, "id") == 0) {
            if (parse_u16_str(val, &tmp_u16) && (tmp_u16 >= 1U) && (tmp_u16 <= 16U)) {
                cfg->local_id.id = (uint8_t)tmp_u16;
            }
        } else if (strcmp(key, "name") == 0) {
            copy_str_safe(cfg->local_name, sizeof(cfg->local_name), val);
        } else if (strcmp(key, "bind") == 0) {
            copy_str_safe(cfg->bind_ip, sizeof(cfg->bind_ip), val);
        } else if (strcmp(key, "port") == 0) {
            if (parse_u16_str(val, &tmp_u16) && (tmp_u16 > 0U)) {
                cfg->bind_port.port = tmp_u16;
            }
        } else if (strcmp(key, "volume") == 0) {
            if (parse_u16_str(val, &tmp_u16) && (tmp_u16 <= 100U)) {
                cfg->initial_volume.pct = (uint8_t)tmp_u16;
            }
        } else if (strcmp(key, "peer") == 0) {
            if (cfg->peer_spec_count < (size_t)MAX_CONFIG_PEERS) {
                copy_str_safe(
                    cfg->peer_specs[cfg->peer_spec_count],
                    (size_t)PEER_SPEC_MAX_LEN,
                    val
                );
                cfg->peer_spec_count += 1U;
            }
        } else if (strcmp(key, "allow_listen") == 0) {
            append_allow_listen_list(cfg, val);
        }
    }

    (void)fclose(fp);
}

static bool register_peer_spec(WinAppRuntime_Owned *app, const char *spec) {
    char buf[PEER_SPEC_MAX_LEN];
    copy_str_safe(buf, sizeof(buf), spec);

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

    uint16_t id_u16   = 0U;
    uint16_t port_u16 = 0U;
    if (!parse_u16_str(buf, &id_u16) || !parse_u16_str(c_last + 1, &port_u16)) {
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

    return WinUdpTransport_set_peer_endpoint(&app->udp, (WinUdpPeerParams){
        .station = st,
        .ip_str  = ip_and_port,
        .port    = { .port = port_u16 }
    }) == WIN_HAL_OK;
}

static void win_drain_tx(WinAppRuntime_Owned *app) {
    TxEnvelope_Owned env = {0};
    for (size_t g = 0U; g < (size_t)UHURA_TX_OUTBOX_CAPACITY; ++g) {
        if (IntercomCore_pop_tx_move(&app->core, &env) != CORE_OK) {
            break;
        }
        (void)WinUdpTransport_send_fanout(&app->udp, (WinUdpFanoutParams){
            .recipients = env.recipients,
            .datagram   = WireDatagram_loan(&env.datagram)
        });
    }
}

static void fill_win_synth_mic(WinAppRuntime_Owned *app, PcmFrame_Owned *out_mic) {
    PcmFrame_init_silence(out_mic);
    app->synth_ticks += 1U;
    const DuplexState st = DuplexGate_state(&app->core.duplex_gate);
    if (st == DUPLEX_STATE_TRANSMITTING) {
        for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
            out_mic->samples[i] = WIN_SINE_32[(app->synth_phase_q32 >> 27U) & 0x1FU];
            app->synth_phase_q32 += 39370534U;
        }
    } else if ((st == DUPLEX_STATE_AUTO_REPLY_TX) && ((app->synth_ticks % 100U) < 8U)) {
        for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
            out_mic->samples[i] = (int16_t)(WIN_SINE_32[(app->synth_phase_q32 >> 27U) & 0x1FU] >> 3);
            app->synth_phase_q32 += 19685267U;
        }
    }
}

/* =========================================================================
 * UI Hit-Box Geometry & Double-Buffered GDI Rendering
 * ========================================================================= */
static RECT rect_talk_button(void)      { return (RECT){ 18, 322, 442, 382 }; }
static RECT rect_care_button(void)      { return (RECT){ 18, 392, 442, 444 }; }
static RECT rect_stop_mic_button(void)  { return (RECT){ 18, 456, 146, 494 }; }
static RECT rect_vol_down_button(void)  { return (RECT){ 156, 456, 232, 494 }; }
static RECT rect_vol_up_button(void)    { return (RECT){ 240, 456, 316, 494 }; }
static RECT rect_all_call_button(void)  { return (RECT){ 326, 456, 442, 494 }; }

static RECT rect_peer_row(size_t row_idx) {
    const int top = 100 + ((int)row_idx * 35);
    return (RECT){ 18, top, 442, top + 31 };
}

static void draw_filled_rect(HDC hdc, RECT r, COLORREF fill, COLORREF border) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen  = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_br  = SelectObject(hdc, br);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    (void)RoundRect(hdc, r.left, r.top, r.right, r.bottom, 8, 8);
    (void)SelectObject(hdc, old_br);
    (void)SelectObject(hdc, old_pen);
    (void)DeleteObject(br);
    (void)DeleteObject(pen);
}

static void draw_led_circle(HDC hdc, int cx, int cy, int radius, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    HPEN pen  = CreatePen(PS_SOLID, 1, RGB(30, 35, 42));
    HGDIOBJ old_br  = SelectObject(hdc, br);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    (void)Ellipse(hdc, cx - radius, cy - radius, cx + radius, cy + radius);
    (void)SelectObject(hdc, old_br);
    (void)SelectObject(hdc, old_pen);
    (void)DeleteObject(br);
    (void)DeleteObject(pen);
}

static void draw_button_label(HDC hdc, RECT r, COLORREF bg, COLORREF fg, const char *text) {
    draw_filled_rect(hdc, r, bg, RGB(70, 80, 95));
    (void)SetTextColor(hdc, fg);
    RECT tr = r;
    (void)DrawTextA(hdc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void render_intercom_panel(HDC hdc, RECT client, const WinAppRuntime_Owned *app) {
    /* Background */
    HBRUSH bg_brush = CreateSolidBrush(RGB(22, 26, 33));
    (void)FillRect(hdc, &client, bg_brush);
    (void)DeleteObject(bg_brush);
    (void)SetBkMode(hdc, TRANSPARENT);

    const MonotonicTimeUs now   = win_monotonic_us(app);
    const DuplexState st        = DuplexGate_state(&app->core.duplex_gate);
    const bool mic_hot          = DuplexGate_is_mic_hot(&app->core.duplex_gate);
    const VolumePercent eff_vol = IntercomCore_effective_volume(&app->core);
    const bool emerg_vol =
        (AudioDsp_playback_gain_mode(&app->core.audio_dsp) == GAIN_MODE_EMERGENCY_FULL);

    /* 1. Top Status & Privacy LED Banner */
    const RECT banner = { 14, 12, 446, 72 };
    const COLORREF banner_bg = mic_hot ? RGB(185, 28, 28) : RGB(31, 41, 55);
    draw_filled_rect(hdc, banner, banner_bg, mic_hot ? RGB(248, 113, 113) : RGB(55, 65, 81));

    /* Red MIC LED vs Green Standby LED */
    draw_led_circle(hdc, 36, 42, 10, mic_hot ? RGB(255, 80, 80) : RGB(34, 197, 94));

    (void)SelectObject(hdc, app->font_title);
    (void)SetTextColor(hdc, RGB(255, 255, 255));
    char status_line[96];
    if (st == DUPLEX_STATE_TRANSMITTING) {
        if (app->core.active_tx_call_kind == CALL_KIND_CARE_LISTEN) {
            (void)snprintf(status_line, sizeof(status_line), "WELZIJNS-OPROEP — SPREEK NU");
        } else {
            (void)snprintf(status_line, sizeof(status_line), "MICROFOON AAN — SPREEK NU");
        }
    } else if (st == DUPLEX_STATE_AUTO_REPLY_TX) {
        const uint16_t ticks = DuplexGate_auto_reply_remaining(&app->core.duplex_gate).count;
        const unsigned int secs = (unsigned int)((ticks + 99U) / 100U);
        (void)snprintf(status_line, sizeof(status_line), "MIC OPEN (%u sec) — LUISTERT MEE", secs);
    } else {
        (void)snprintf(status_line, sizeof(status_line), "KLAAR OM TE LUISTEREN");
    }
    RECT st_rect = { 56, 16, 438, 44 };
    (void)DrawTextA(hdc, status_line, -1, &st_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    (void)SelectObject(hdc, app->font_body);
    (void)SetTextColor(hdc, emerg_vol ? RGB(253, 224, 71) : RGB(190, 200, 215));
    char sub_line[96];
    (void)snprintf(
        sub_line,
        sizeof(sub_line),
        "Post #%u (%s)   |   Speaker Volume: %u%%%s",
        (unsigned int)app->core.local_station.id,
        app->local_name,
        (unsigned int)eff_vol.pct,
        emerg_vol ? " (NOODVOLUME!)" : ""
    );
    RECT sub_rect = { 56, 43, 438, 68 };
    (void)DrawTextA(hdc, sub_line, -1, &sub_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* 2. Peer List Section */
    (void)SetTextColor(hdc, RGB(156, 163, 175));
    RECT hdr_lbl = { 18, 76, 442, 98 };
    (void)DrawTextA(
        hdc,
        "Bestemmingen (klik om aan/uit te zetten — Groen=Online, Geel=Geluid):",
        -1,
        &hdr_lbl,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE
    );

    const TargetPeerMask selected = IntercomCore_selected_targets(&app->core);
    const TargetPeerMask talkers  = IntercomCore_active_talkers(&app->core);
    const TargetPeerMask audible  = IntercomCore_audible_talkers(&app->core);

    size_t row = 0U;
    for (uint8_t id = 1U; (id <= (uint8_t)UHURA_MAX_PEERS) && (row < 6U); ++id) {
        const PeerInfo *peer = &app->core.peers[id - 1U];
        if (!peer->is_configured) {
            continue;
        }
        const StationId sid   = { .id = id };
        const bool is_sel     = TargetPeerMask_contains(selected, sid);
        const bool is_online  = IntercomCore_is_peer_online(&app->core, sid, now);
        const bool is_loud    = TargetPeerMask_contains(audible, sid);
        const bool is_stream  = TargetPeerMask_contains(talkers, sid);

        const RECT pr = rect_peer_row(row);
        const COLORREF row_bg = is_sel ? RGB(30, 58, 95) : RGB(31, 36, 45);
        const COLORREF row_bd = is_loud ? RGB(250, 204, 21)
                              : (is_sel ? RGB(96, 165, 250) : RGB(55, 65, 81));
        draw_filled_rect(hdc, pr, row_bg, row_bd);

        /* Green Online LED (1 lampje = bereikbaar) */
        draw_led_circle(
            hdc,
            pr.left + 18,
            (pr.top + pr.bottom) / 2,
            6,
            is_online ? RGB(34, 197, 94) : RGB(75, 85, 99)
        );

        /* Station Name + Checkbox */
        (void)SelectObject(hdc, app->font_bold);
        (void)SetTextColor(hdc, RGB(243, 244, 246));
        char row_txt[80];
        (void)snprintf(
            row_txt,
            sizeof(row_txt),
            "[%s]  #%u  %s%s",
            is_sel ? "X" : " ",
            (unsigned int)id,
            peer->name,
            (peer->listen_perm == LISTEN_PERM_ALLOWED) ? "  (Mag 30s luisteren)" : ""
        );
        RECT name_r = { pr.left + 34, pr.top, pr.right - 115, pr.bottom };
        (void)DrawTextA(hdc, row_txt, -1, &name_r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        /* Yellow Sound/Speaking LED (1 lampje = spreekt / geluid!) */
        draw_led_circle(
            hdc,
            pr.right - 95,
            (pr.top + pr.bottom) / 2,
            6,
            is_loud ? RGB(250, 204, 21) : (is_stream ? RGB(14, 165, 233) : RGB(55, 65, 81))
        );
        (void)SelectObject(hdc, app->font_body);
        (void)SetTextColor(hdc, is_loud ? RGB(253, 224, 71) : RGB(148, 163, 184));
        RECT act_r = { pr.right - 82, pr.top, pr.right - 8, pr.bottom };
        (void)DrawTextA(
            hdc,
            is_loud ? "GELUID!" : (is_stream ? "Luistert.." : (is_online ? "Online" : "Offline")),
            -1,
            &act_r,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE
        );

        row += 1U;
    }

    /* 3. Big Action Buttons */
    (void)SelectObject(hdc, app->font_bold);
    const bool normal_active =
        (st == DUPLEX_STATE_TRANSMITTING) && (app->core.active_tx_call_kind == CALL_KIND_NORMAL_PTT);
    const bool care_active =
        (st == DUPLEX_STATE_TRANSMITTING) && (app->core.active_tx_call_kind == CALL_KIND_CARE_LISTEN);

    draw_button_label(
        hdc,
        rect_talk_button(),
        normal_active ? RGB(220, 38, 38) : RGB(22, 101, 52),
        RGB(255, 255, 255),
        normal_active
            ? ">>> BEZIG MET SPREKEN (laat los om te luisteren) <<<"
            : "HOUD INGEDRUKT OM TE SPREKEN  (of Spatie)"
    );

    draw_button_label(
        hdc,
        rect_care_button(),
        care_active ? RGB(220, 38, 38) : RGB(180, 83, 9),
        RGB(255, 255, 255),
        care_active
            ? ">>> LAAT LOS OM 30 SEC MEE TE LUISTEREN <<<"
            : "WELZIJNS-OPROEP  (Houd ingedrukt: Noodvolume + 30s Luisteren)"
    );

    /* 4. Bottom Utility Buttons */
    (void)SelectObject(hdc, app->font_body);
    draw_button_label(hdc, rect_stop_mic_button(), RGB(63, 63, 70), RGB(255, 255, 255), "Stop 30s Mic");
    draw_button_label(hdc, rect_vol_down_button(), RGB(51, 65, 85), RGB(255, 255, 255), "Vol -");
    draw_button_label(hdc, rect_vol_up_button(),   RGB(51, 65, 85), RGB(255, 255, 255), "Vol +");
    draw_button_label(hdc, rect_all_call_button(), RGB(51, 65, 85), RGB(255, 255, 255), "Iedereen");
}

static bool pt_in_rect(RECT r, int x, int y) {
    return (x >= r.left) && (x < r.right) && (y >= r.top) && (y < r.bottom);
}

static void handle_mouse_down(HWND hwnd, WinAppRuntime_Owned *app, int x, int y) {
    if (pt_in_rect(rect_talk_button(), x, y)) {
        app->held_button = HELD_BTN_NORMAL_PTT;
        (void)SetCapture(hwnd);
        IntercomCore_set_ptt_command(&app->core, (PttCommandParams){
            .button    = PTT_PRESSED,
            .call_kind = CALL_KIND_NORMAL_PTT
        });
        win_drain_tx(app);
        (void)InvalidateRect(hwnd, NULL, FALSE);
        return;
    }

    if (pt_in_rect(rect_care_button(), x, y)) {
        app->held_button = HELD_BTN_CARE_PTT;
        (void)SetCapture(hwnd);
        IntercomCore_set_ptt_command(&app->core, (PttCommandParams){
            .button    = PTT_PRESSED,
            .call_kind = CALL_KIND_CARE_LISTEN
        });
        win_drain_tx(app);
        (void)InvalidateRect(hwnd, NULL, FALSE);
        return;
    }

    if (pt_in_rect(rect_stop_mic_button(), x, y)) {
        IntercomCore_abort_care_listen(&app->core);
        win_drain_tx(app);
        (void)InvalidateRect(hwnd, NULL, FALSE);
        return;
    }

    if (pt_in_rect(rect_vol_down_button(), x, y)) {
        const int cur = (int)AudioDsp_user_volume(&app->core.audio_dsp).pct;
        const int nxt = (cur >= 10) ? (cur - 10) : 0;
        IntercomCore_set_user_volume(&app->core, (VolumePercent){ .pct = (uint8_t)nxt });
        (void)InvalidateRect(hwnd, NULL, FALSE);
        return;
    }

    if (pt_in_rect(rect_vol_up_button(), x, y)) {
        const int cur = (int)AudioDsp_user_volume(&app->core.audio_dsp).pct;
        const int nxt = (cur <= 90) ? (cur + 10) : 100;
        IntercomCore_set_user_volume(&app->core, (VolumePercent){ .pct = (uint8_t)nxt });
        (void)InvalidateRect(hwnd, NULL, FALSE);
        return;
    }

    if (pt_in_rect(rect_all_call_button(), x, y)) {
        IntercomCore_set_targets(&app->core, TargetPeerMask_all());
        (void)InvalidateRect(hwnd, NULL, FALSE);
        return;
    }

    /* Check peer row clicks */
    size_t row = 0U;
    for (uint8_t id = 1U; (id <= (uint8_t)UHURA_MAX_PEERS) && (row < 6U); ++id) {
        if (!app->core.peers[id - 1U].is_configured) {
            continue;
        }
        if (pt_in_rect(rect_peer_row(row), x, y)) {
            const StationId sid       = { .id = id };
            const TargetPeerMask cur  = IntercomCore_selected_targets(&app->core);
            const TargetPeerMask next = TargetPeerMask_contains(cur, sid)
                ? TargetPeerMask_without_station(cur, sid)
                : TargetPeerMask_with_station(cur, sid);
            IntercomCore_set_targets(&app->core, next);
            (void)InvalidateRect(hwnd, NULL, FALSE);
            return;
        }
        row += 1U;
    }
}

static void handle_mouse_up(HWND hwnd, WinAppRuntime_Owned *app) {
    if (app->held_button != HELD_BTN_NONE) {
        const CallKind kind = (app->held_button == HELD_BTN_CARE_PTT)
                              ? CALL_KIND_CARE_LISTEN
                              : CALL_KIND_NORMAL_PTT;
        app->held_button = HELD_BTN_NONE;
        (void)ReleaseCapture();
        IntercomCore_set_ptt_command(&app->core, (PttCommandParams){
            .button    = PTT_RELEASED,
            .call_kind = kind
        });
        win_drain_tx(app);
        (void)InvalidateRect(hwnd, NULL, FALSE);
    }
}

static void on_periodic_timer(HWND hwnd, WinAppRuntime_Owned *app) {
    const MonotonicTimeUs now = win_monotonic_us(app);

    /* 1. Drain incoming Winsock2 UDP datagrams */
    for (size_t i = 0U; i < 32U; ++i) {
        WireDatagram_Owned rx_dgram = {0};
        const WinHalError rx_err = WinUdpTransport_recv_next(&app->udp, &rx_dgram);
        if (rx_err == WIN_HAL_ERR_WOULD_BLOCK) {
            break;
        }
        if ((rx_err == WIN_HAL_OK) && rx_dgram.is_valid) {
            IntercomCore_on_rx_datagram(&app->core, (IntercomRxDatagramParams){
                .raw_datagram = WireDatagram_loan(&rx_dgram),
                .now          = now
            });
        }
    }
    win_drain_tx(app);

    /* 2. Run exact 10.00 ms audio ticks locked to QueryPerformanceCounter */
    size_t ticks_ran = 0U;
    while ((now.us >= app->next_tick_us) && (ticks_ran < 4U)) {
        app->next_tick_us += 10000ULL;
        ticks_ran += 1U;

        PcmFrame_Owned mic_frame = {0};
        if ((app->audio.mode == WIN_AUDIO_SYNTH_NO_HW) || !app->audio.has_capture_hw) {
            fill_win_synth_mic(app, &mic_frame);
        } else {
            if (SpscPcmRing_pop_move(&app->audio.capture_ring, &mic_frame) != RING_OK) {
                PcmFrame_init_silence(&mic_frame);
            }
        }

        PcmFrame_Owned spk_frame = {0};
        IntercomCore_tick_10ms(
            &app->core,
            (IntercomTickParams){
                .mic_pcm = PcmFrame_loan(&mic_frame),
                .now     = now
            },
            &spk_frame
        );

        if (app->audio.has_playback_hw) {
            (void)SpscPcmRing_push_move(&app->audio.playback_ring, &spk_frame);
        }

        win_drain_tx(app);
    }

    /* Prevent drift if system slept */
    if ((now.us > app->next_tick_us) && ((now.us - app->next_tick_us) > 50000ULL)) {
        app->next_tick_us = now.us + 10000ULL;
    }

    app->ui_refresh_div += 1U;
    if (app->ui_refresh_div >= 16U) { /* ~12 Hz smooth LED & countdown refresh */
        app->ui_refresh_div = 0U;
        (void)InvalidateRect(hwnd, NULL, FALSE);
    }
}

static LRESULT CALLBACK uhura_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lp;
        (void)SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    WinAppRuntime_Owned *app = (WinAppRuntime_Owned *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (app == NULL) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg) {
        case WM_TIMER:
            on_periodic_timer(hwnd, app);
            return 0;

        case WM_LBUTTONDOWN:
            handle_mouse_down(
                hwnd,
                app,
                (int)(short)LOWORD((DWORD)lp),
                (int)(short)HIWORD((DWORD)lp)
            );
            return 0;

        case WM_LBUTTONUP:
            handle_mouse_up(hwnd, app);
            return 0;

        case WM_KEYDOWN:
            if ((wp == VK_SPACE) && !app->space_ptt_held) {
                app->space_ptt_held = true;
                IntercomCore_set_ptt_command(&app->core, (PttCommandParams){
                    .button    = PTT_PRESSED,
                    .call_kind = CALL_KIND_NORMAL_PTT
                });
                win_drain_tx(app);
                (void)InvalidateRect(hwnd, NULL, FALSE);
            } else if ((wp == 'X') || (wp == VK_ESCAPE)) {
                IntercomCore_abort_care_listen(&app->core);
                win_drain_tx(app);
                (void)InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_KEYUP:
            if ((wp == VK_SPACE) && app->space_ptt_held) {
                app->space_ptt_held = false;
                IntercomCore_set_ptt_command(&app->core, (PttCommandParams){
                    .button    = PTT_RELEASED,
                    .call_kind = CALL_KIND_NORMAL_PTT
                });
                win_drain_tx(app);
                (void)InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_ERASEBKGND:
            return 1; /* Double-buffered in WM_PAINT */

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            (void)GetClientRect(hwnd, &rc);
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;

            HDC mem_dc    = CreateCompatibleDC(hdc);
            HBITMAP bmp   = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ old_b = SelectObject(mem_dc, bmp);

            render_intercom_panel(mem_dc, rc, app);
            (void)BitBlt(hdc, 0, 0, w, h, mem_dc, 0, 0, SRCCOPY);

            (void)SelectObject(mem_dc, old_b);
            (void)DeleteObject(bmp);
            (void)DeleteDC(mem_dc);
            (void)EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            (void)KillTimer(hwnd, (UINT_PTR)WIN_TIMER_ID);
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)SetProcessDPIAware();

    StartupConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_port      = (UdpPort){ .port = 4711U };
    cfg.initial_volume = (VolumePercent){ .pct = 80U };
    cfg.chime_mode     = CHIME_ENABLED;
    cfg.audio_mode     = WIN_AUDIO_HARDWARE;
    copy_str_safe(cfg.local_name, sizeof(cfg.local_name), "Intercom");
    copy_str_safe(cfg.bind_ip, sizeof(cfg.bind_ip), "0.0.0.0");

    /* 1. Load optional uhura.txt from same folder as uhura.exe */
    load_uhura_txt_next_to_exe(&cfg);

    /* 2. Apply command-line overrides if provided */
    for (int i = 1; i < __argc; ++i) {
        const char *arg = __argv[i];
        uint16_t tmp_u16 = 0U;
        if ((strcmp(arg, "--id") == 0) && ((i + 1) < __argc)) {
            if (parse_u16_str(__argv[++i], &tmp_u16) && (tmp_u16 >= 1U) && (tmp_u16 <= 16U)) {
                cfg.local_id.id = (uint8_t)tmp_u16;
            }
        } else if ((strcmp(arg, "--name") == 0) && ((i + 1) < __argc)) {
            copy_str_safe(cfg.local_name, sizeof(cfg.local_name), __argv[++i]);
        } else if ((strcmp(arg, "--bind") == 0) && ((i + 1) < __argc)) {
            copy_str_safe(cfg.bind_ip, sizeof(cfg.bind_ip), __argv[++i]);
        } else if ((strcmp(arg, "--port") == 0) && ((i + 1) < __argc)) {
            if (parse_u16_str(__argv[++i], &tmp_u16) && (tmp_u16 > 0U)) {
                cfg.bind_port.port = tmp_u16;
            }
        } else if ((strcmp(arg, "--volume") == 0) && ((i + 1) < __argc)) {
            if (parse_u16_str(__argv[++i], &tmp_u16) && (tmp_u16 <= 100U)) {
                cfg.initial_volume.pct = (uint8_t)tmp_u16;
            }
        } else if ((strcmp(arg, "--peer") == 0) && ((i + 1) < __argc)) {
            const char *p = __argv[++i];
            if (cfg.peer_spec_count < (size_t)MAX_CONFIG_PEERS) {
                copy_str_safe(cfg.peer_specs[cfg.peer_spec_count], (size_t)PEER_SPEC_MAX_LEN, p);
                cfg.peer_spec_count += 1U;
            }
        } else if ((strcmp(arg, "--allow-listen") == 0) && ((i + 1) < __argc)) {
            append_allow_listen_list(&cfg, __argv[++i]);
        } else if (strcmp(arg, "--no-chime") == 0) {
            cfg.chime_mode = CHIME_DISABLED;
        } else if (strcmp(arg, "--no-audio") == 0) {
            cfg.audio_mode = WIN_AUDIO_SYNTH_NO_HW;
        }
    }

    if (!StationId_is_valid(cfg.local_id)) {
        (void)MessageBoxA(
            NULL,
            "Geen geldig station-nummer gevonden!\n\n"
            "Plaats een bestandje 'uhura.txt' naast uhura.exe met bijvoorbeeld:\n\n"
            "id = 4\n"
            "name = Zus\n"
            "port = 4711\n"
            "peer = 1:Edwin:192.168.1.10:4711\n"
            "peer = 2:Ma-Woonkamer:192.168.1.50:4711\n",
            "UHURA Intercom — Instelling nodig",
            MB_OK | MB_ICONINFORMATION
        );
        return 1;
    }

    WinAppRuntime_Owned app;
    memset(&app, 0, sizeof(app));
    (void)QueryPerformanceFrequency(&app.qpc_freq);
    copy_str_safe(app.local_name, sizeof(app.local_name), cfg.local_name);

    if (IntercomCore_init(&app.core, (IntercomConfigParams){
            .local_station   = cfg.local_id,
            .opus_bitrate    = { .bps = (uint32_t)UHURA_DEFAULT_BITRATE_BPS },
            .fec_mode        = FEC_ENABLED,
            .jitter_prefetch = { .count = (uint16_t)UHURA_DEFAULT_PREFETCH_SLOTS },
            .hangover_frames = { .count = (uint16_t)UHURA_DEFAULT_HANGOVER_FRAMES },
            .chime_mode      = cfg.chime_mode
        }) != CORE_OK) {
        (void)MessageBoxA(NULL, "Kon UHURA audiokern niet initialiseren.", "Fout", MB_OK | MB_ICONERROR);
        return 1;
    }
    IntercomCore_set_user_volume(&app.core, cfg.initial_volume);

    if (WinUdpTransport_open(&app.udp, (WinUdpBindParams){
            .bind_ip   = cfg.bind_ip,
            .bind_port = cfg.bind_port
        }) != WIN_HAL_OK) {
        char msg[160];
        (void)snprintf(
            msg,
            sizeof(msg),
            "Kon UDP-poort %u niet openen (draait uhura.exe al?).",
            (unsigned int)cfg.bind_port.port
        );
        (void)MessageBoxA(NULL, msg, "UHURA Netwerkfout", MB_OK | MB_ICONERROR);
        return 1;
    }

    for (size_t i = 0U; i < cfg.peer_spec_count; ++i) {
        (void)register_peer_spec(&app, cfg.peer_specs[i]);
    }
    for (size_t i = 0U; i < cfg.allow_listen_count; ++i) {
        (void)IntercomCore_set_peer_listen_permission(&app.core, (PeerPermissionParams){
            .station    = cfg.allow_listen_ids[i],
            .permission = LISTEN_PERM_ALLOWED
        });
    }

    (void)timeBeginPeriod(1U);
    (void)WinAudioHal_open_and_start(&app.audio, (WinAudioConfigParams){
        .mode = cfg.audio_mode
    });

    app.font_title = CreateFontA(
        -19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, "Segoe UI"
    );
    app.font_bold = CreateFontA(
        -15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, "Segoe UI"
    );
    app.font_body = CreateFontA(
        -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, "Segoe UI"
    );

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = (UINT)sizeof(wc);
    wc.lpfnWndProc   = uhura_wnd_proc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = L"UhuraIntercomWinClass";
    (void)RegisterClassExW(&wc);

    RECT win_rc = { 0, 0, WIN_UI_WIDTH, WIN_UI_HEIGHT };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    (void)AdjustWindowRect(&win_rc, style, FALSE);

    wchar_t title_w[96];
    (void)_snwprintf(
        title_w,
        sizeof(title_w) / sizeof(title_w[0]),
        L"UHURA Intercom \x2014 %hs (#%u)",
        app.local_name,
        (unsigned int)cfg.local_id.id
    );

    app.next_tick_us = win_monotonic_us(&app).us + 10000ULL;

    app.hwnd = CreateWindowExW(
        0U,
        wc.lpszClassName,
        title_w,
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        win_rc.right - win_rc.left,
        win_rc.bottom - win_rc.top,
        NULL,
        NULL,
        hInstance,
        &app
    );

    if (app.hwnd == NULL) {
        WinAudioHal_stop_and_close(&app.audio);
        (void)timeEndPeriod(1U);
        WinUdpTransport_close(&app.udp);
        return 1;
    }

    (void)SetTimer(app.hwnd, (UINT_PTR)WIN_TIMER_ID, (UINT)WIN_TIMER_MS, NULL);
    (void)ShowWindow(app.hwnd, nCmdShow);
    (void)UpdateWindow(app.hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0U, 0U) > 0) {
        (void)TranslateMessage(&msg);
        (void)DispatchMessageW(&msg);
    }

    if (app.held_button != HELD_BTN_NONE || app.space_ptt_held) {
        IntercomCore_set_ptt(&app.core, PTT_RELEASED);
        win_drain_tx(&app);
    }

    if (app.font_title != NULL) { (void)DeleteObject(app.font_title); }
    if (app.font_bold != NULL)  { (void)DeleteObject(app.font_bold);  }
    if (app.font_body != NULL)  { (void)DeleteObject(app.font_body);  }

    WinAudioHal_stop_and_close(&app.audio);
    (void)timeEndPeriod(1U);
    WinUdpTransport_close(&app.udp);
    return 0;
}
