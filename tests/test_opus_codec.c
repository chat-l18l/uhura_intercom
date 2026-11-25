#include "core/opus_codec.h"

#include <stdio.h>

#define TEST_CHECK(cond)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static const int16_t SINE_32[32] = {
       0,  1564,  3068,  4453,  5667,  6663,  7404,  7860,
    8016,  7860,  7404,  6663,  5667,  4453,  3068,  1564,
       0, -1564, -3068, -4453, -5667, -6663, -7404, -7860,
   -8016, -7860, -7404, -6663, -5667, -4453, -3068, -1564
};

int main(void) {
    OpusEncoder_Owned enc;
    OpusDecoder_Owned dec;

    TEST_CHECK(OpusEncoder_init(&enc, (OpusEncoderConfigParams){
        .bitrate  = { .bps = 32000U },
        .fec_mode = FEC_ENABLED
    }) == CODEC_OK);

    TEST_CHECK(OpusDecoder_init(&dec) == CODEC_OK);

    uint32_t phase = 0U;
    int64_t total_abs_energy = 0;

    for (uint32_t seq = 0U; seq < 10U; ++seq) {
        PcmFrame_Owned pcm_in = {0};
        PcmFrame_init_silence(&pcm_in);
        for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
            pcm_in.samples[i] = SINE_32[(phase >> 27U) & 0x1FU];
            phase += 39370534U;
        }

        EncodedFrame_Owned pkt = {0};
        TEST_CHECK(OpusEncoder_encode_frame(&enc, (OpusEncodeParams){
            .pcm_in  = PcmFrame_loan(&pcm_in),
            .session = { .id = 1U },
            .seq     = { .num = seq }
        }, &pkt) == CODEC_OK);

        TEST_CHECK(pkt.is_valid);
        TEST_CHECK(pkt.len > 0U && pkt.len <= (uint16_t)UHURA_MAX_OPUS_BYTES);

        PcmFrame_Owned pcm_out = {0};
        if (seq == 5U) {
            /* Simulate packet 5 lost: run PLC */
            TEST_CHECK(OpusDecoder_decode_plc(&dec, &pcm_out) == CODEC_OK);
        } else if (seq == 6U) {
            /* Verify FEC decode API on packet 6 as well as normal decode */
            PcmFrame_Owned fec_out = {0};
            TEST_CHECK(OpusDecoder_decode_fec_from_next(
                &dec,
                EncodedFrame_loan(&pkt),
                &fec_out
            ) == CODEC_OK);
            TEST_CHECK(fec_out.is_valid);
            TEST_CHECK(OpusDecoder_decode_packet(&dec, EncodedFrame_loan(&pkt), &pcm_out) == CODEC_OK);
        } else {
            TEST_CHECK(OpusDecoder_decode_packet(&dec, EncodedFrame_loan(&pkt), &pcm_out) == CODEC_OK);
        }

        TEST_CHECK(pcm_out.is_valid);
        for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
            const int32_t s = pcm_out.samples[i];
            total_abs_energy += (s < 0) ? -s : s;
        }
    }

    /* Decoded speech/tone over 100 ms must contain substantial signal energy */
    TEST_CHECK(total_abs_energy > 500000);
    return 0;
}
