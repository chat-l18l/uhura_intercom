#include "platform/linux_hal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/ip.h>
#include <string.h>
#include <unistd.h>

static bool parse_numeric_sockaddr(
    const char *ip_str,
    UdpPort port,
    struct sockaddr_storage *out_storage,
    socklen_t *out_len
) {
    assert(out_storage != NULL);
    assert(out_len != NULL);
    memset(out_storage, 0, sizeof(*out_storage));

    const char *effective_ip = ((ip_str == NULL) || (ip_str[0] == '\0')) ? "0.0.0.0" : ip_str;

    /* Try IPv4 first (standard for LAN & WireGuard wg0) */
    struct sockaddr_in *sin4 = (struct sockaddr_in *)out_storage;
    if (inet_pton(AF_INET, effective_ip, &sin4->sin_addr) == 1) {
        sin4->sin_family = AF_INET;
        sin4->sin_port   = htons(port.port);
        *out_len         = (socklen_t)sizeof(struct sockaddr_in);
        return true;
    }

    /* Try IPv6 (e.g. fd00:: WireGuard ULA) */
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)out_storage;
    if (inet_pton(AF_INET6, effective_ip, &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port   = htons(port.port);
        *out_len          = (socklen_t)sizeof(struct sockaddr_in6);
        return true;
    }

    return false;
}

NODISCARD HalError UdpTransport_open(UdpTransport_Owned *ctx, UdpBindParams params) {
    assert(ctx != NULL);
    memset(ctx, 0, sizeof(*ctx));
    ctx->sock.fd = -1;

    struct sockaddr_storage bind_addr;
    socklen_t bind_len = 0U;
    if (!parse_numeric_sockaddr(params.bind_ip, params.bind_port, &bind_addr, &bind_len)) {
        return HAL_ERR_INVALID_ADDRESS;
    }

    const int fd = socket(
        (int)bind_addr.ss_family,
        SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0
    );
    if (fd < 0) {
        return HAL_ERR_SOCKET_OPEN;
    }

    const int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, (socklen_t)sizeof(reuse));

    /* Set Expedited Forwarding (DSCP EF = 0xB8) TOS hint for low-latency VoIP */
    if (bind_addr.ss_family == AF_INET) {
        const int tos_ef = 0xB8;
        (void)setsockopt(fd, IPPROTO_IP, IP_TOS, &tos_ef, (socklen_t)sizeof(tos_ef));
    }

    if (bind(fd, (const struct sockaddr *)&bind_addr, bind_len) < 0) {
        (void)close(fd);
        return HAL_ERR_SOCKET_BIND;
    }

    ctx->sock.fd = fd;
    ctx->family  = bind_addr.ss_family;
    ctx->is_open = true;
    return HAL_OK;
}

void UdpTransport_close(UdpTransport_Owned *ctx) {
    assert(ctx != NULL);
    if (ctx->is_open && (ctx->sock.fd >= 0)) {
        (void)close(ctx->sock.fd);
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->sock.fd = -1;
}

NODISCARD FileDesc UdpTransport_poll_fd(const UdpTransport_Owned *ctx) {
    assert(ctx != NULL);
    return ctx->sock;
}

NODISCARD HalError UdpTransport_set_peer_endpoint(
    UdpTransport_Owned *ctx,
    UdpPeerEndpointParams params
) {
    assert(ctx != NULL);
    assert(ctx->is_open);

    if (!StationId_is_valid(params.station) || (params.ip_str == NULL) || (params.port.port == 0U)) {
        return HAL_ERR_INVALID_ADDRESS;
    }

    struct sockaddr_storage peer_addr;
    socklen_t peer_len = 0U;
    if (!parse_numeric_sockaddr(params.ip_str, params.port, &peer_addr, &peer_len)) {
        return HAL_ERR_INVALID_ADDRESS;
    }

    if (peer_addr.ss_family != ctx->family) {
        return HAL_ERR_INVALID_ADDRESS;
    }

    const size_t idx = (size_t)(params.station.id - 1U);
    ctx->peer_endpoints[idx].addr          = peer_addr;
    ctx->peer_endpoints[idx].addr_len      = peer_len;
    ctx->peer_endpoints[idx].is_configured = true;
    return HAL_OK;
}

NODISCARD HalError UdpTransport_send_fanout(
    const UdpTransport_Owned *ctx,
    UdpFanoutSendParams params
) {
    assert(ctx != NULL);
    assert(ctx->is_open);
    assert(params.datagram.data != NULL);
    assert(params.datagram.len > 0U);

    HalError status = HAL_OK;

    /* Bounded loop over all 16 possible stations (NASA Power of 10) */
    for (uint8_t id = 1U; id <= (uint8_t)UHURA_MAX_PEERS; ++id) {
        const StationId st = { .id = id };
        if (!TargetPeerMask_contains(params.recipients, st)) {
            continue;
        }

        const UdpEndpoint *ep = &ctx->peer_endpoints[id - 1U];
        if (!ep->is_configured) {
            continue;
        }

        const ssize_t sent = sendto(
            ctx->sock.fd,
            params.datagram.data,
            params.datagram.len,
            MSG_DONTWAIT,
            (const struct sockaddr *)&ep->addr,
            ep->addr_len
        );
        if (sent < 0) {
            status = HAL_ERR_SEND_FAILED;
        }
    }

    return status;
}

NODISCARD HalError UdpTransport_recv_next(
    const UdpTransport_Owned *ctx,
    WireDatagram_Owned *out_dgram
) {
    assert(ctx != NULL);
    assert(ctx->is_open);
    assert(out_dgram != NULL);

    *out_dgram = (WireDatagram_Owned){0};

    uint8_t scratch[UHURA_MAX_WIRE_BYTES + 16U];
    const ssize_t nread = recvfrom(
        ctx->sock.fd,
        scratch,
        sizeof(scratch),
        MSG_DONTWAIT,
        NULL,
        NULL
    );

    if (nread < 0) {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
            return HAL_ERR_WOULD_BLOCK;
        }
        return HAL_ERR_RECV_FAILED;
    }

    if ((nread < (ssize_t)UHURA_WIRE_HEADER_BYTES) ||
        (nread > (ssize_t)UHURA_MAX_WIRE_BYTES)) {
        return HAL_ERR_RECV_FAILED;
    }

    memcpy(out_dgram->bytes, scratch, (size_t)nread);
    out_dgram->len      = (uint16_t)nread;
    out_dgram->is_valid = true;
    return HAL_OK;
}
