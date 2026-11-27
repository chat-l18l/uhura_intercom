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

static void write_be_u64(uint8_t *dst, uint64_t val) {
    assert(dst != NULL);
    write_be_u32(&dst[0], (uint32_t)((val >> 32U) & 0xFFFFFFFFULL));
    write_be_u32(&dst[4], (uint32_t)(val & 0xFFFFFFFFULL));
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

static uint64_t read_be_u64(const uint8_t *src) {
    assert(src != NULL);
    const uint64_t hi = (uint64_t)read_be_u32(&src[0]);
    const uint64_t lo = (uint64_t)read_be_u32(&src[4]);
    return (hi << 32U) | lo;
}

static bool is_valid_packet_kind(uint8_t raw_kind) {
    return (raw_kind >= (uint8_t)PACKET_KIND_AUDIO) &&
           (raw_kind <= (uint8_t)PACKET_KIND_LISTEN_ABORT);
}

static bool is_valid_payload_for_kind(PacketKind kind, size_t payload_len, const uint8_t *data) {
    if (kind == PACKET_KIND_AUDIO) {
        return (payload_len > 0U) &&
               (payload_len <= (size_t)UHURA_MAX_OPUS_BYTES) &&
               (data != NULL);
    }
    if ((kind == PACKET_KIND_LISTEN_CHALLENGE) || (kind == PACKET_KIND_LISTEN_CONFIRM)) {
        return (payload_len == (size_t)UHURA_CHALLENGE_NONCE_BYTES) && (data != NULL);
    }
    return (payload_len == 0U);
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
    if (!is_valid_payload_for_kind(params.kind, params.payload.len, params.payload.data)) {
        return WIRE_ERR_INVALID_PAYLOAD;
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

NODISCARD WireError WireProto_serialize_nonce(
    WireDatagram_Owned *out_datagram,
    WireNonceSerializeParams params
) {
    assert(out_datagram != NULL);
    if ((params.kind != PACKET_KIND_LISTEN_CHALLENGE) &&
        (params.kind != PACKET_KIND_LISTEN_CONFIRM)) {
        *out_datagram = (WireDatagram_Owned){0};
        return WIRE_ERR_INVALID_KIND;
    }

    uint8_t nonce_bytes[UHURA_CHALLENGE_NONCE_BYTES];
    write_be_u64(nonce_bytes, params.nonce.val);

    return WireProto_serialize(out_datagram, (WireSerializeParams){
        .kind    = params.kind,
        .sender  = params.sender,
        .flags   = { .bits = (uint8_t)WIRE_FLAG_CARE_CALL },
        .session = params.session,
        .seq     = { .num = 0U },
        .targets = params.targets,
        .payload = { .data = nonce_bytes, .len = sizeof(nonce_bytes) }
    });
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
    const uint8_t *payload_ptr =
        (payload_len > 0U) ? &raw_datagram.data[UHURA_WIRE_HEADER_BYTES] : NULL;

    if (!is_valid_payload_for_kind(kind, (size_t)payload_len, payload_ptr)) {
        return WIRE_ERR_INVALID_PAYLOAD;
    }

    out_view->kind    = kind;
    out_view->sender  = sender;
    out_view->flags   = (WireFlags){ .bits = hdr[7] };
    out_view->session = (TalkSessionId){ .id = read_be_u32(&hdr[8]) };
    out_view->seq     = (SeqNum){ .num = read_be_u32(&hdr[12]) };
    out_view->targets = (TargetPeerMask){ .bits = read_be_u16(&hdr[16]) };
    out_view->payload = (ByteSlice_Loaned){
        .data = payload_ptr,
        .len  = (size_t)payload_len
    };

    return WIRE_OK;
}

NODISCARD WireError WireProto_parse_nonce(
    ByteSlice_Loaned payload,
    ChallengeNonce *out_nonce
) {
    assert(out_nonce != NULL);
    *out_nonce = (ChallengeNonce){ .val = 0ULL };

    if ((payload.data == NULL) || (payload.len != (size_t)UHURA_CHALLENGE_NONCE_BYTES)) {
        return WIRE_ERR_INVALID_PAYLOAD;
    }

    out_nonce->val = read_be_u64(payload.data);
    return WIRE_OK;
}
