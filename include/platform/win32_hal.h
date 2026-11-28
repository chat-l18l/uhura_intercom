#ifndef UHURA_PLATFORM_WIN32_HAL_H
#define UHURA_PLATFORM_WIN32_HAL_H

#include "core/intercom_core.h"
#include "core/spsc_pcm_ring.h"
#include "core/types.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <stdatomic.h>

/* =========================================================================
 * Thin Windows 11 Native HAL (Winsock2 + WinMM/WASAPI + Win32 GDI)
 * Zero external DLLs, zero heap allocations, zero mutable globals.
 * ========================================================================= */
enum {
    UHURA_WINMM_BUF_COUNT = 4U, /* 4 x 10 ms = 40 ms low-latency audio ring */
    UHURA_WIN_IP_STR_MAX  = 64U
};

typedef struct {
    SOCKET handle;
} WinSocketHandle;

typedef enum {
    WIN_HAL_OK = 0,
    WIN_HAL_ERR_WSA_INIT,
    WIN_HAL_ERR_SOCKET_OPEN,
    WIN_HAL_ERR_SOCKET_BIND,
    WIN_HAL_ERR_INVALID_ADDRESS,
    WIN_HAL_ERR_SEND_FAILED,
    WIN_HAL_ERR_WOULD_BLOCK,
    WIN_HAL_ERR_RECV_FAILED,
    WIN_HAL_ERR_AUDIO_OPEN,
    WIN_HAL_ERR_AUDIO_THREAD
} WinHalError;

/* -------------------------------------------------------------------------
 * 1. Non-Blocking Winsock2 UDP Unicast Transport
 * ------------------------------------------------------------------------- */
typedef struct {
    struct sockaddr_storage addr;
    int                     addr_len;
    bool                    is_configured;
} WinUdpEndpoint;

typedef struct {
    WSADATA         wsa_data;
    WinSocketHandle sock;
    ADDRESS_FAMILY  family;
    WinUdpEndpoint  peer_endpoints[UHURA_MAX_PEERS];
    bool            wsa_started;
    bool            is_open;
} WinUdpTransport_Owned;

typedef struct {
    const char *bind_ip;
    UdpPort     bind_port;
} WinUdpBindParams;

typedef struct {
    StationId   station;
    const char *ip_str;
    UdpPort     port;
} WinUdpPeerParams;

typedef struct {
    TargetPeerMask   recipients;
    ByteSlice_Loaned datagram;
} WinUdpFanoutParams;

NODISCARD WinHalError WinUdpTransport_open(
    WinUdpTransport_Owned *ctx,
    WinUdpBindParams params
);

void WinUdpTransport_close(WinUdpTransport_Owned *ctx);

NODISCARD WinHalError WinUdpTransport_set_peer_endpoint(
    WinUdpTransport_Owned *ctx,
    WinUdpPeerParams params
);

NODISCARD WinHalError WinUdpTransport_send_fanout(
    const WinUdpTransport_Owned *ctx,
    WinUdpFanoutParams params
);

NODISCARD WinHalError WinUdpTransport_recv_next(
    const WinUdpTransport_Owned *ctx,
    WireDatagram_Owned *out_dgram
);

/* -------------------------------------------------------------------------
 * 2. Low-Latency Event-Driven 10 ms Audio I/O (48 kHz Mono int16_t)
 * ------------------------------------------------------------------------- */
typedef enum {
    WIN_AUDIO_HARDWARE    = 0,
    WIN_AUDIO_SYNTH_NO_HW = 1
} WinAudioMode;

typedef struct {
    WinAudioMode mode;
} WinAudioConfigParams;

typedef struct {
    SpscPcmRing_Owned capture_ring;
    SpscPcmRing_Owned playback_ring;
    HWAVEIN           wave_in;
    HWAVEOUT          wave_out;
    HANDLE            audio_event;
    HANDLE            worker_thread;
    WAVEHDR           in_hdrs[UHURA_WINMM_BUF_COUNT];
    WAVEHDR           out_hdrs[UHURA_WINMM_BUF_COUNT];
    int16_t           in_pcm[UHURA_WINMM_BUF_COUNT][UHURA_SAMPLES_PER_FRAME];
    int16_t           out_pcm[UHURA_WINMM_BUF_COUNT][UHURA_SAMPLES_PER_FRAME];
    atomic_bool       stop_requested;
    WinAudioMode      mode;
    bool              has_capture_hw;
    bool              has_playback_hw;
    bool              thread_started;
    bool              is_open;
} WinAudioHal_Owned;

NODISCARD WinHalError WinAudioHal_open_and_start(
    WinAudioHal_Owned *ctx,
    WinAudioConfigParams params
);

void WinAudioHal_stop_and_close(WinAudioHal_Owned *ctx);

#endif /* UHURA_PLATFORM_WIN32_HAL_H */
