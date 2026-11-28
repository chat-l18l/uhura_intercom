#include "platform/win32_hal.h"

#include <string.h>

static bool parse_win_sockaddr(
    const char *ip_str,
    UdpPort port,
    struct sockaddr_storage *out_storage,
    int *out_len
) {
    assert(out_storage != NULL);
    assert(out_len != NULL);
    memset(out_storage, 0, sizeof(*out_storage));

    const char *effective_ip =
        ((ip_str == NULL) || (ip_str[0] == '\0')) ? "0.0.0.0" : ip_str;

    struct sockaddr_in *sin4 = (struct sockaddr_in *)out_storage;
    if (InetPtonA(AF_INET, effective_ip, &sin4->sin_addr) == 1) {
        sin4->sin_family = AF_INET;
        sin4->sin_port   = htons(port.port);
        *out_len         = (int)sizeof(struct sockaddr_in);
        return true;
    }

    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)out_storage;
    if (InetPtonA(AF_INET6, effective_ip, &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port   = htons(port.port);
        *out_len          = (int)sizeof(struct sockaddr_in6);
        return true;
    }

    return false;
}

NODISCARD WinHalError WinUdpTransport_open(
    WinUdpTransport_Owned *ctx,
    WinUdpBindParams params
) {
    assert(ctx != NULL);
    memset(ctx, 0, sizeof(*ctx));
    ctx->sock.handle = INVALID_SOCKET;

    if (WSAStartup(MAKEWORD(2, 2), &ctx->wsa_data) != 0) {
        return WIN_HAL_ERR_WSA_INIT;
    }
    ctx->wsa_started = true;

    struct sockaddr_storage bind_addr;
    int bind_len = 0;
    if (!parse_win_sockaddr(params.bind_ip, params.bind_port, &bind_addr, &bind_len)) {
        WSACleanup();
        ctx->wsa_started = false;
        return WIN_HAL_ERR_INVALID_ADDRESS;
    }

    const SOCKET s = socket((int)bind_addr.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        ctx->wsa_started = false;
        return WIN_HAL_ERR_SOCKET_OPEN;
    }

    u_long nonblocking = 1UL;
    (void)ioctlsocket(s, (long)FIONBIO, &nonblocking);

    const char reuse = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, (int)sizeof(reuse));

    if (bind(s, (const struct sockaddr *)&bind_addr, bind_len) == SOCKET_ERROR) {
        (void)closesocket(s);
        WSACleanup();
        ctx->wsa_started = false;
        return WIN_HAL_ERR_SOCKET_BIND;
    }

    ctx->sock.handle = s;
    ctx->family      = (ADDRESS_FAMILY)bind_addr.ss_family;
    ctx->is_open     = true;
    return WIN_HAL_OK;
}

void WinUdpTransport_close(WinUdpTransport_Owned *ctx) {
    assert(ctx != NULL);
    if (ctx->sock.handle != INVALID_SOCKET) {
        (void)closesocket(ctx->sock.handle);
        ctx->sock.handle = INVALID_SOCKET;
    }
    if (ctx->wsa_started) {
        WSACleanup();
        ctx->wsa_started = false;
    }
    ctx->is_open = false;
}

NODISCARD WinHalError WinUdpTransport_set_peer_endpoint(
    WinUdpTransport_Owned *ctx,
    WinUdpPeerParams params
) {
    assert(ctx != NULL);
    assert(ctx->is_open);

    if (!StationId_is_valid(params.station) ||
        (params.ip_str == NULL) ||
        (params.port.port == 0U)) {
        return WIN_HAL_ERR_INVALID_ADDRESS;
    }

    struct sockaddr_storage peer_addr;
    int peer_len = 0;
    if (!parse_win_sockaddr(params.ip_str, params.port, &peer_addr, &peer_len)) {
        return WIN_HAL_ERR_INVALID_ADDRESS;
    }
    if ((ADDRESS_FAMILY)peer_addr.ss_family != ctx->family) {
        return WIN_HAL_ERR_INVALID_ADDRESS;
    }

    const size_t idx = (size_t)(params.station.id - 1U);
    ctx->peer_endpoints[idx].addr          = peer_addr;
    ctx->peer_endpoints[idx].addr_len      = peer_len;
    ctx->peer_endpoints[idx].is_configured = true;
    return WIN_HAL_OK;
}

NODISCARD WinHalError WinUdpTransport_send_fanout(
    const WinUdpTransport_Owned *ctx,
    WinUdpFanoutParams params
) {
    assert(ctx != NULL);
    assert(ctx->is_open);
    assert(params.datagram.data != NULL);
    assert(params.datagram.len > 0U);

    WinHalError status = WIN_HAL_OK;

    for (uint8_t id = 1U; id <= (uint8_t)UHURA_MAX_PEERS; ++id) {
        const StationId st = { .id = id };
        if (!TargetPeerMask_contains(params.recipients, st)) {
            continue;
        }

        const WinUdpEndpoint *ep = &ctx->peer_endpoints[id - 1U];
        if (!ep->is_configured) {
            continue;
        }

        const int sent = sendto(
            ctx->sock.handle,
            (const char *)params.datagram.data,
            (int)params.datagram.len,
            0,
            (const struct sockaddr *)&ep->addr,
            ep->addr_len
        );
        if (sent == SOCKET_ERROR) {
            status = WIN_HAL_ERR_SEND_FAILED;
        }
    }

    return status;
}

NODISCARD WinHalError WinUdpTransport_recv_next(
    const WinUdpTransport_Owned *ctx,
    WireDatagram_Owned *out_dgram
) {
    assert(ctx != NULL);
    assert(ctx->is_open);
    assert(out_dgram != NULL);

    *out_dgram = (WireDatagram_Owned){0};

    char scratch[UHURA_MAX_WIRE_BYTES + 16U];
    const int nread = recvfrom(
        ctx->sock.handle,
        scratch,
        (int)sizeof(scratch),
        0,
        NULL,
        NULL
    );

    if (nread == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return WIN_HAL_ERR_WOULD_BLOCK;
        }
        return WIN_HAL_ERR_RECV_FAILED;
    }

    if ((nread < (int)UHURA_WIRE_HEADER_BYTES) ||
        (nread > (int)UHURA_MAX_WIRE_BYTES)) {
        return WIN_HAL_ERR_RECV_FAILED;
    }

    memcpy(out_dgram->bytes, scratch, (size_t)nread);
    out_dgram->len      = (uint16_t)nread;
    out_dgram->is_valid = true;
    return WIN_HAL_OK;
}
