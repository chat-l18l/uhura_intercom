#include "core/wire_proto.h"

#include <stdio.h>
#include <string.h>

#define TEST_CHECK(cond)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            return 1;                                                               \
        }                                                                           \
    } while (0)

int main(void) {
    /* 1. Verify 'UHUR' magic bytes and 20-byte big-endian header layout */
    const uint8_t sample_opus[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x42, 0x99 };
    const TargetPeerMask targets = TargetPeerMask_with_station(
        TargetPeerMask_from_station((StationId){ .id = 2U }),
        (StationId){ .id = 3U }
    );

    WireDatagram_Owned dgram = {0};
    const WireError ser_err = WireProto_serialize(&dgram, (WireSerializeParams){
        .kind    = PACKET_KIND_AUDIO,
        .sender  = { .id = 1U },
        .flags   = { .bits = (uint8_t)WIRE_FLAG_NONE },
        .session = { .id = 0x01020304U },
        .seq     = { .num = 0x0000002AU },
        .targets = targets,
        .payload = { .data = sample_opus, .len = sizeof(sample_opus) }
    });

    TEST_CHECK(ser_err == WIRE_OK);
    TEST_CHECK(dgram.is_valid);
    TEST_CHECK(dgram.len == 26U);
    TEST_CHECK(dgram.bytes[0] == 'U');
    TEST_CHECK(dgram.bytes[1] == 'H');
    TEST_CHECK(dgram.bytes[2] == 'U');
    TEST_CHECK(dgram.bytes[3] == 'R');

    /* 2. Roundtrip parse */
    WirePacketView_Loaned parsed = {0};
    const WireError parse_err = WireProto_parse(WireDatagram_loan(&dgram), &parsed);
    TEST_CHECK(parse_err == WIRE_OK);
    TEST_CHECK(parsed.kind == PACKET_KIND_AUDIO);
    TEST_CHECK(parsed.sender.id == 1U);
    TEST_CHECK(parsed.session.id == 0x01020304U);
    TEST_CHECK(parsed.seq.num == 42U);
    TEST_CHECK(TargetPeerMask_contains(parsed.targets, (StationId){ .id = 2U }));
    TEST_CHECK(TargetPeerMask_contains(parsed.targets, (StationId){ .id = 3U }));
    TEST_CHECK(!TargetPeerMask_contains(parsed.targets, (StationId){ .id = 4U }));
    TEST_CHECK(parsed.payload.len == sizeof(sample_opus));
    TEST_CHECK(memcmp(parsed.payload.data, sample_opus, sizeof(sample_opus)) == 0);

    /* 3. Verify move invalidates source (Rule 2) */
    WireDatagram_Owned moved = {0};
    WireDatagram_move(&moved, &dgram);
    TEST_CHECK(!dgram.is_valid);
    TEST_CHECK(moved.is_valid);

    /* 4. Reject non-'UHUR' magic (e.g. old 'ICOM' header) */
    moved.bytes[0] = 'I';
    moved.bytes[1] = 'C';
    moved.bytes[2] = 'O';
    moved.bytes[3] = 'M';
    TEST_CHECK(WireProto_parse(WireDatagram_loan(&moved), &parsed) == WIRE_ERR_BAD_MAGIC);

    /* 5. Control packet (HEARTBEAT) must have 0 payload */
    WireDatagram_Owned hb = {0};
    TEST_CHECK(WireProto_serialize(&hb, (WireSerializeParams){
        .kind    = PACKET_KIND_HEARTBEAT,
        .sender  = { .id = 3U },
        .flags   = { .bits = 0U },
        .session = { .id = 0U },
        .seq     = { .num = 0U },
        .targets = TargetPeerMask_all(),
        .payload = { .data = NULL, .len = 0U }
    }) == WIRE_OK);
    TEST_CHECK(hb.len == 20U);
    TEST_CHECK(WireProto_parse(WireDatagram_loan(&hb), &parsed) == WIRE_OK);
    TEST_CHECK(parsed.kind == PACKET_KIND_HEARTBEAT);
    TEST_CHECK(parsed.payload.len == 0U);

    /* 6. Reject invalid station IDs and malformed payloads */
    TEST_CHECK(WireProto_serialize(&hb, (WireSerializeParams){
        .kind    = PACKET_KIND_AUDIO,
        .sender  = { .id = 1U },
        .flags   = { .bits = 0U },
        .session = { .id = 1U },
        .seq     = { .num = 0U },
        .targets = TargetPeerMask_all(),
        .payload = { .data = NULL, .len = 0U }
    }) == WIRE_ERR_INVALID_PAYLOAD);

    TEST_CHECK(WireProto_serialize(&hb, (WireSerializeParams){
        .kind    = PACKET_KIND_HEARTBEAT,
        .sender  = { .id = 17U },
        .flags   = { .bits = 0U },
        .session = { .id = 0U },
        .seq     = { .num = 0U },
        .targets = TargetPeerMask_all(),
        .payload = { .data = NULL, .len = 0U }
    }) == WIRE_ERR_INVALID_STATION);

    return 0;
}
