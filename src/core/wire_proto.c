#include "core/wire_proto.h"

#include <string.h>

static void write_be_u16(uint8_t *dst, uint16_t val) {
    assert(dst != NULL);
    dst[0] = (uint8_t)((val >> 8U) & 0xFFU);
    dst[1] = (uint8_t)(val & 0xFFU);
}

static void write_be_u32(uint8_t *dst, uint32_t val) {
    assert(dst != NULL);
    dst[0] = (uint8_t)((val >> 24U) & 0xFFU);
    dst[1] = (uint8_t)((val >> 16U) & 0xFFU);
    dst[2] = (uint8_t)((val >> 8U) & 0xFFU);
    dst[3] = (uint8_t)(val & 0xFFU);
}

static uint16_t read_be_u16(const uint8_t *src) {
    assert(src != NULL);
    return (uint16_t)(((uint16_t)src[0] << 8U) | (uint16_t)src[1]);
}

static uint32_t read_be_u32(const uint8_t *src) {
    assert(src != NULL);
    return ((uint32_t)src[0] << 24U) |
           ((uint32_t)src[1] << 16U) |
           ((uint32_t)src[2] << 8U)  |
           (uint32_t)src[3];
}

static bool is_valid_packet_kind(uint8_t raw_kind) {
    return (raw_kind >= (uint8_t)PACKET_KIND_AUDIO) &&
           (raw_kind <= (uint8_t)PACKET_KIND_HEARTBEAT);
}

NODISCARD WireError WireProto_serialize(
    WireDatagram_Owned *out_datagram,
    WireSerializeParams params
) {
    assert(out_datagram != NULL);
    *out_datagram = (WireDatagram_Owned){0};

    if (!is_valid_packet_kind((uint8_t)params.kind)) {
        return WIRE_ERR_INVALID_KIND;
    }
    if (!StationId_is_valid(params.sender)) {
        return WIRE_ERR_INVALID_STATION;
    }
    if (params.payload.len > (size_t)UHURA_MAX_OPUS_BYTES) {
        return WIRE_ERR_PAYLOAD_TOO_LARGE;
    }

    if (params.kind == PACKET_KIND_AUDIO) {
        if ((params.payload.len == 0U) || (params.payload.data == NULL)) {
            return WIRE_ERR_INVALID_PAYLOAD;
        }
    } else {
        if (params.payload.len != 0U) {
            return WIRE_ERR_INVALID_PAYLOAD;
        }
    }

    uint8_t *hdr = out_datagram->bytes;
    hdr[0] = (uint8_t)UHURA_WIRE_MAGIC_0;
    hdr[1] = (uint8_t)UHURA_WIRE_MAGIC_1;
    hdr[2] = (uint8_t)UHURA_WIRE_MAGIC_2;
    hdr[3] = (uint8_t)UHURA_WIRE_MAGIC_3;
    hdr[4] = (uint8_t)UHURA_WIRE_VERSION;
    hdr[5] = (uint8_t)params.kind;
    hdr[6] = params.sender.id;
    hdr[7] = params.flags.bits;

    write_be_u32(&hdr[8], params.session.id);
    write_be_u32(&hdr[12], params.seq.num);
    write_be_u16(&hdr[16], params.targets.bits);
    write_be_u16(&hdr[18], (uint16_t)params.payload.len);

    if (params.payload.len > 0U) {
        memcpy(&out_datagram->bytes[UHURA_WIRE_HEADER_BYTES],
               params.payload.data,
               params.payload.len);
    }

    out_datagram->len = (uint16_t)((size_t)UHURA_WIRE_HEADER_BYTES + params.payload.len);
    out_datagram->is_valid = true;

    assert(out_datagram->len >= (uint16_t)UHURA_WIRE_HEADER_BYTES);
    assert(out_datagram->len <= (uint16_t)UHURA_MAX_WIRE_BYTES);
    return WIRE_OK;
}

NODISCARD WireError WireProto_parse(
    ByteSlice_Loaned raw_datagram,
    WirePacketView_Loaned *out_view
) {
    assert(out_view != NULL);
    *out_view = (WirePacketView_Loaned){0};

    if ((raw_datagram.data == NULL) || (raw_datagram.len < (size_t)UHURA_WIRE_HEADER_BYTES)) {
        return WIRE_ERR_TOO_SHORT;
    }

    const uint8_t *hdr = raw_datagram.data;
    if ((hdr[0] != (uint8_t)UHURA_WIRE_MAGIC_0) ||
        (hdr[1] != (uint8_t)UHURA_WIRE_MAGIC_1) ||
        (hdr[2] != (uint8_t)UHURA_WIRE_MAGIC_2) ||
        (hdr[3] != (uint8_t)UHURA_WIRE_MAGIC_3)) {
        return WIRE_ERR_BAD_MAGIC;
    }

    if (hdr[4] != (uint8_t)UHURA_WIRE_VERSION) {
        return WIRE_ERR_UNSUPPORTED_VERSION;
    }

    const uint8_t raw_kind = hdr[5];
    if (!is_valid_packet_kind(raw_kind)) {
        return WIRE_ERR_INVALID_KIND;
    }

    const StationId sender = { .id = hdr[6] };
    if (!StationId_is_valid(sender)) {
        return WIRE_ERR_INVALID_STATION;
    }

    const uint16_t payload_len = read_be_u16(&hdr[18]);
    if ((size_t)payload_len > (size_t)UHURA_MAX_OPUS_BYTES) {
        return WIRE_ERR_PAYLOAD_TOO_LARGE;
    }

    const size_t expected_total = (size_t)UHURA_WIRE_HEADER_BYTES + (size_t)payload_len;
    if (raw_datagram.len != expected_total) {
        return WIRE_ERR_LENGTH_MISMATCH;
    }

    const PacketKind kind = (PacketKind)raw_kind;
    if ((kind == PACKET_KIND_AUDIO) && (payload_len == 0U)) {
        return WIRE_ERR_INVALID_PAYLOAD;
    }
    if ((kind != PACKET_KIND_AUDIO) && (payload_len != 0U)) {
        return WIRE_ERR_INVALID_PAYLOAD;
    }

    out_view->kind    = kind;
    out_view->sender  = sender;
    out_view->flags   = (WireFlags){ .bits = hdr[7] };
    out_view->session = (TalkSessionId){ .id = read_be_u32(&hdr[8]) };
    out_view->seq     = (SeqNum){ .num = read_be_u32(&hdr[12]) };
    out_view->targets = (TargetPeerMask){ .bits = read_be_u16(&hdr[16]) };
    out_view->payload = (ByteSlice_Loaned){
        .data = (payload_len > 0U) ? &raw_datagram.data[UHURA_WIRE_HEADER_BYTES] : NULL,
        .len  = (size_t)payload_len
    };

    return WIRE_OK;
}
