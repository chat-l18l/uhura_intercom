#ifndef UHURA_PLATFORM_LINUX_HAL_H
#define UHURA_PLATFORM_LINUX_HAL_H

#include "core/intercom_core.h"
#include "core/spsc_pcm_ring.h"
#include "core/types.h"

#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <termios.h>

/* =========================================================================
 * Thin Linux OS / Hardware Abstraction Layer (Rule 6: Isolated in platform/)
 * Zero globals, zero heap allocation, strict _Owned / _Loaned contracts.
 * ========================================================================= */
enum {
    UHURA_DEVICE_NAME_MAX = 64U,
    UHURA_IP_STRING_MAX   = 64U
};

typedef struct {
    int fd;
} FileDesc;

typedef enum {
    HAL_OK = 0,
    HAL_ERR_SOCKET_OPEN,
    HAL_ERR_SOCKET_BIND,
    HAL_ERR_INVALID_ADDRESS,
    HAL_ERR_SEND_FAILED,
    HAL_ERR_WOULD_BLOCK,
    HAL_ERR_RECV_FAILED,
    HAL_ERR_AUDIO_OPEN,
    HAL_ERR_AUDIO_PARAMS,
    HAL_ERR_AUDIO_THREAD,
    HAL_ERR_TTY_SETUP
} HalError;

/* -------------------------------------------------------------------------
 * 1. Non-blocking UDP Unicast Socket + Static Peer Endpoint Table
 * ------------------------------------------------------------------------- */
typedef struct {
    struct sockaddr_storage addr;
    socklen_t               addr_len;
    bool                    is_configured;
} UdpEndpoint;

typedef struct {
    FileDesc    sock;
    sa_family_t family;
    UdpEndpoint peer_endpoints[UHURA_MAX_PEERS];
    bool        is_open;
} UdpTransport_Owned;

typedef struct {
    const char *bind_ip; /* NULL or "0.0.0.0" or "::" or WireGuard IP */
    UdpPort     bind_port;
} UdpBindParams;

typedef struct {
    StationId   station;
    const char *ip_str;
    UdpPort     port;
} UdpPeerEndpointParams;

typedef struct {
    TargetPeerMask   recipients;
    ByteSlice_Loaned datagram;
} UdpFanoutSendParams;

NODISCARD HalError UdpTransport_open(UdpTransport_Owned *ctx, UdpBindParams params);
void UdpTransport_close(UdpTransport_Owned *ctx);
NODISCARD FileDesc UdpTransport_poll_fd(const UdpTransport_Owned *ctx);

NODISCARD HalError UdpTransport_set_peer_endpoint(
    UdpTransport_Owned *ctx,
    UdpPeerEndpointParams params
);

NODISCARD HalError UdpTransport_send_fanout(
    const UdpTransport_Owned *ctx,
    UdpFanoutSendParams params
);

/* Reads one pending UDP datagram into *out_dgram; returns HAL_ERR_WOULD_BLOCK if empty */
NODISCARD HalError UdpTransport_recv_next(
    const UdpTransport_Owned *ctx,
    WireDatagram_Owned *out_dgram
);

/* -------------------------------------------------------------------------
 * 2. Real-Time ALSA Audio I/O Thread (Bridged via Lock-Free SPSC Rings)
 * ------------------------------------------------------------------------- */
typedef enum {
    AUDIO_MODE_ALSA     = 0,
    AUDIO_MODE_SYNTH_NO_HW = 1 /* Headless test mode without sound card */
} AudioBackendMode;

typedef struct {
    const char      *capture_device;  /* e.g. "default" or "plughw:0,0" */
    const char      *playback_device; /* e.g. "default" or "plughw:0,0" */
    DurationMs       alsa_latency;    /* e.g. 30 ms */
    AudioBackendMode mode;
} AudioHalConfigParams;

typedef struct {
    SpscPcmRing_Owned capture_ring;
    SpscPcmRing_Owned playback_ring;
    void             *alsa_capture_handle;  /* snd_pcm_t* opaque outside audio_alsa.c */
    void             *alsa_playback_handle; /* snd_pcm_t* opaque outside audio_alsa.c */
    pthread_t         worker_thread;
    atomic_bool       stop_requested;
    AudioBackendMode  mode;
    bool              thread_started;
    bool              is_open;
} AudioHal_Owned;

NODISCARD HalError AudioHal_open_and_start(
    AudioHal_Owned *ctx,
    AudioHalConfigParams params
);

void AudioHal_stop_and_close(AudioHal_Owned *ctx);

/* -------------------------------------------------------------------------
 * 3. Interactive Terminal PTT & Station Selector (Raw Non-Blocking Termios)
 * ------------------------------------------------------------------------- */
typedef enum {
    CLI_CMD_NONE = 0,
    CLI_CMD_TOGGLE_PTT,
    CLI_CMD_TOGGLE_CARE_PTT,
    CLI_CMD_RELEASE_PTT,
    CLI_CMD_ABORT_LISTEN,
    CLI_CMD_VOL_DOWN,
    CLI_CMD_VOL_UP,
    CLI_CMD_TOGGLE_STATION,
    CLI_CMD_SELECT_ALL,
    CLI_CMD_CLEAR_TARGETS,
    CLI_CMD_QUIT
} CliCommandKind;

typedef struct {
    CliCommandKind kind;
    StationId      station_arg; /* Valid when kind == CLI_CMD_TOGGLE_STATION */
} CliCommand;

typedef struct {
    struct termios orig_termios;
    FileDesc       tty_fd;
    bool           raw_mode_active;
    bool           is_open;
} CliControl_Owned;

typedef struct {
    const IntercomCore_Owned *core;
    const char               *local_name;
    MonotonicTimeUs           now;
} CliStatusRenderParams;

NODISCARD HalError CliControl_open(CliControl_Owned *ctx);
void CliControl_close(CliControl_Owned *ctx);
NODISCARD FileDesc CliControl_poll_fd(const CliControl_Owned *ctx);
NODISCARD CliCommand CliControl_read_command(const CliControl_Owned *ctx);
void CliControl_render_status(const CliControl_Owned *ctx, CliStatusRenderParams params);

#endif /* UHURA_PLATFORM_LINUX_HAL_H */
