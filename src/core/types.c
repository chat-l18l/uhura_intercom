#include "core/types.h"

#include <string.h>

NODISCARD bool StationId_is_valid(StationId station) {
    return (station.id >= 1U) && (station.id <= (uint8_t)UHURA_MAX_PEERS);
}

NODISCARD TargetPeerMask TargetPeerMask_none(void) {
    return (TargetPeerMask){ .bits = 0U };
}

NODISCARD TargetPeerMask TargetPeerMask_all(void) {
    return (TargetPeerMask){ .bits = 0xFFFFU };
}

NODISCARD TargetPeerMask TargetPeerMask_from_station(StationId station) {
    assert(StationId_is_valid(station));
    const uint16_t shift = (uint16_t)(station.id - 1U);
    return (TargetPeerMask){ .bits = (uint16_t)(1U << shift) };
}

NODISCARD TargetPeerMask TargetPeerMask_with_station(TargetPeerMask mask, StationId station) {
    assert(StationId_is_valid(station));
    const TargetPeerMask single = TargetPeerMask_from_station(station);
    return (TargetPeerMask){ .bits = (uint16_t)(mask.bits | single.bits) };
}

NODISCARD TargetPeerMask TargetPeerMask_without_station(TargetPeerMask mask, StationId station) {
    assert(StationId_is_valid(station));
    const TargetPeerMask single = TargetPeerMask_from_station(station);
    const uint16_t inverted = (uint16_t)(~single.bits);
    return (TargetPeerMask){ .bits = (uint16_t)(mask.bits & inverted) };
}

NODISCARD bool TargetPeerMask_contains(TargetPeerMask mask, StationId station) {
    if (!StationId_is_valid(station)) {
        return false;
    }
    const TargetPeerMask single = TargetPeerMask_from_station(station);
    return (mask.bits & single.bits) != 0U;
}

void PcmFrame_init_silence(PcmFrame_Owned *self) {
    assert(self != NULL);
    memset(self->samples, 0, sizeof(self->samples));
    self->is_valid = true;
}

NODISCARD PcmFrame_Owned PcmFrame_from_slice(PcmMonoSlice_Loaned src) {
    assert(src.samples != NULL);
    assert(src.frames.count == (uint16_t)UHURA_SAMPLES_PER_FRAME);

    PcmFrame_Owned out = {0};
    memcpy(out.samples, src.samples, sizeof(out.samples));
    out.is_valid = true;
    return out;
}

NODISCARD PcmMonoSlice_Loaned PcmFrame_loan(const PcmFrame_Owned *self) {
    assert(self != NULL);
    assert(self->is_valid);
    return (PcmMonoSlice_Loaned){
        .samples = self->samples,
        .frames  = { .count = (uint16_t)UHURA_SAMPLES_PER_FRAME }
    };
}

NODISCARD PcmMonoSliceMut_Loaned PcmFrame_loan_mut(PcmFrame_Owned *self) {
    assert(self != NULL);
    assert(self->is_valid);
    return (PcmMonoSliceMut_Loaned){
        .samples = self->samples,
        .frames  = { .count = (uint16_t)UHURA_SAMPLES_PER_FRAME }
    };
}

void PcmFrame_move(PcmFrame_Owned * restrict dst, PcmFrame_Owned * restrict src) {
    assert(dst != NULL);
    assert(src != NULL);
    assert(dst != src);
    assert(src->is_valid);

    *dst = *src;
    *src = (PcmFrame_Owned){0};
    assert(!src->is_valid);
    assert(dst->is_valid);
}

NODISCARD ByteSlice_Loaned EncodedFrame_loan(const EncodedFrame_Owned *self) {
    assert(self != NULL);
    assert(self->is_valid);
    assert(self->len <= (uint16_t)UHURA_MAX_OPUS_BYTES);

    return (ByteSlice_Loaned){
        .data = self->bytes,
        .len  = (size_t)self->len
    };
}

void EncodedFrame_move(EncodedFrame_Owned * restrict dst, EncodedFrame_Owned * restrict src) {
    assert(dst != NULL);
    assert(src != NULL);
    assert(dst != src);
    assert(src->is_valid);

    *dst = *src;
    *src = (EncodedFrame_Owned){0};
    assert(!src->is_valid);
    assert(dst->is_valid);
}

NODISCARD ByteSlice_Loaned WireDatagram_loan(const WireDatagram_Owned *self) {
    assert(self != NULL);
    assert(self->is_valid);
    assert(self->len >= (uint16_t)UHURA_WIRE_HEADER_BYTES);
    assert(self->len <= (uint16_t)UHURA_MAX_WIRE_BYTES);

    return (ByteSlice_Loaned){
        .data = self->bytes,
        .len  = (size_t)self->len
    };
}

void WireDatagram_move(WireDatagram_Owned * restrict dst, WireDatagram_Owned * restrict src) {
    assert(dst != NULL);
    assert(src != NULL);
    assert(dst != src);
    assert(src->is_valid);

    *dst = *src;
    *src = (WireDatagram_Owned){0};
    assert(!src->is_valid);
    assert(dst->is_valid);
}

void PcmFrame_mix_into(PcmFrame_Owned * restrict accumulator, PcmMonoSlice_Loaned contribution) {
    assert(accumulator != NULL);
    assert(accumulator->is_valid);
    assert(contribution.samples != NULL);
    assert(contribution.frames.count == (uint16_t)UHURA_SAMPLES_PER_FRAME);

    /* Bounded loop (NASA Power of 10): exact 480 iterations */
    for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
        const int32_t sum = (int32_t)accumulator->samples[i] + (int32_t)contribution.samples[i];
        if (sum > INT16_MAX) {
            accumulator->samples[i] = INT16_MAX;
        } else if (sum < INT16_MIN) {
            accumulator->samples[i] = INT16_MIN;
        } else {
            accumulator->samples[i] = (int16_t)sum;
        }
    }
}
