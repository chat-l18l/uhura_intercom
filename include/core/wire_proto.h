#ifndef UHURA_CORE_WIRE_PROTO_H
#define UHURA_CORE_WIRE_PROTO_H

#include "core/types.h"

/* =========================================================================
 * 'UHUR' 20-Byte Binary Wire Protocol (Big-Endian / Portable x86_64 & aarch64)
 * ========================================================================= */
enum {
    UHURA_WIRE_MAGIC_0 = 0x55U, /* 'U' */
    UHURA_WIRE_MAGIC_1 = 0x48U, /* 'H' */
    UHURA_WIRE_MAGIC_2 = 0x55U, /* 'U' */
    UHURA_WIRE_MAGIC_3 = 0x52U, /* 'R' */
    UHURA_WIRE_VERSION = 0x01U
};

typedef enum {
    PACKET_KIND_AUDIO      = 1,
    PACKET_KIND_TALK_START = 2,
    PACKET_KIND_TALK_END   = 3,
    PACKET_KIND_HEARTBEAT  = 4
} PacketKind;

typedef enum {
    WIRE_FLAG_NONE            = 0x00U,
    WIRE_FLAG_ALL_CALL        = 0x01U,
    WIRE_FLAG_DUPLEX_RESERVED = 0x02U
} WireHeaderFlag;

typedef struct {
    uint8_t bits;
} WireFlags;

typedef enum {
    WIRE_OK = 0,
    WIRE_ERR_TOO_SHORT,
    WIRE_ERR_BAD_MAGIC,
    WIRE_ERR_UNSUPPORTED_VERSION,
    WIRE_ERR_INVALID_KIND,
    WIRE_ERR_INVALID_STATION,
    WIRE_ERR_PAYLOAD_TOO_LARGE,
    WIRE_ERR_LENGTH_MISMATCH,
    WIRE_ERR_INVALID_PAYLOAD
} WireError;

/* Parameter Object Pattern (Rule 7) */
typedef struct {
    PacketKind       kind;
    StationId        sender;
    WireFlags        flags;
    TalkSessionId    session;
    SeqNum           seq;
    TargetPeerMask   targets;
    ByteSlice_Loaned payload; /* Non-empty for AUDIO; {NULL, 0} for control packets */
} WireSerializeParams;

/*
 * Short-lived parsed view into a raw UDP datagram buffer.
 * Marked _Loaned because .payload borrows directly from raw_datagram (Rule 2).
 */
typedef struct {
    PacketKind       kind;
    StationId        sender;
    WireFlags        flags;
    TalkSessionId    session;
    SeqNum           seq;
    TargetPeerMask   targets;
    ByteSlice_Loaned payload;
} WirePacketView_Loaned;

NODISCARD WireError WireProto_serialize(
    WireDatagram_Owned *out_datagram,
    WireSerializeParams params
);

NODISCARD WireError WireProto_parse(
    ByteSlice_Loaned raw_datagram,
    WirePacketView_Loaned *out_view
);

#endif /* UHURA_CORE_WIRE_PROTO_H */
