#include "core/opus_codec.h"

#include <opus.h>
#include <string.h>

static OpusEncoder *as_opus_enc(OpusEncoder_Owned *ctx) {
    assert(ctx != NULL);
    return (OpusEncoder *)(void *)ctx->state_storage;
}

static OpusDecoder *as_opus_dec(OpusDecoder_Owned *ctx) {
    assert(ctx != NULL);
    return (OpusDecoder *)(void *)ctx->state_storage;
}

NODISCARD CodecError OpusEncoder_init(
    OpusEncoder_Owned *ctx,
    OpusEncoderConfigParams params
) {
    assert(ctx != NULL);
    memset(ctx, 0, sizeof(*ctx));

    if ((params.bitrate.bps < 8000U) || (params.bitrate.bps > 128000U)) {
        return CODEC_ERR_INVALID_CONFIG;
    }

    const int req_size = opus_encoder_get_size(1);
    assert(req_size > 0);
    if ((req_size <= 0) || ((size_t)req_size > sizeof(ctx->state_storage))) {
        return CODEC_ERR_STATE_TOO_SMALL;
    }

    OpusEncoder *enc = as_opus_enc(ctx);
    const int init_rc = opus_encoder_init(
        enc,
        (opus_int32)UHURA_SAMPLE_RATE_HZ,
        1,
        OPUS_APPLICATION_VOIP
    );
    if (init_rc != OPUS_OK) {
        return CODEC_ERR_LIBOPUS_INIT;
    }

    (void)opus_encoder_ctl(enc, OPUS_SET_BITRATE((opus_int32)params.bitrate.bps));
    (void)opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    (void)opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
    (void)opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC((params.fec_mode == FEC_ENABLED) ? 1 : 0));
    (void)opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC((params.fec_mode == FEC_ENABLED) ? 10 : 0));

    ctx->is_initialized = true;
    return CODEC_OK;
}

NODISCARD CodecError OpusEncoder_encode_frame(
    OpusEncoder_Owned *ctx,
    OpusEncodeParams params,
    EncodedFrame_Owned *out_encoded
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    assert(out_encoded != NULL);
    assert(params.pcm_in.samples != NULL);
    assert(params.pcm_in.frames.count == (uint16_t)UHURA_SAMPLES_PER_FRAME);

    *out_encoded = (EncodedFrame_Owned){0};

    OpusEncoder *enc = as_opus_enc(ctx);
    const opus_int32 nbytes = opus_encode(
        enc,
        params.pcm_in.samples,
        (int)UHURA_SAMPLES_PER_FRAME,
        out_encoded->bytes,
        (opus_int32)UHURA_MAX_OPUS_BYTES
    );

    if ((nbytes <= 0) || ((size_t)nbytes > (size_t)UHURA_MAX_OPUS_BYTES)) {
        return CODEC_ERR_ENCODE_FAILED;
    }

    out_encoded->len     = (uint16_t)nbytes;
    out_encoded->session = params.session;
    out_encoded->seq     = params.seq;
    out_encoded->is_valid = true;
    return CODEC_OK;
}

NODISCARD CodecError OpusDecoder_init(OpusDecoder_Owned *ctx) {
    assert(ctx != NULL);
    memset(ctx, 0, sizeof(*ctx));

    const int req_size = opus_decoder_get_size(1);
    assert(req_size > 0);
    if ((req_size <= 0) || ((size_t)req_size > sizeof(ctx->state_storage))) {
        return CODEC_ERR_STATE_TOO_SMALL;
    }

    OpusDecoder *dec = as_opus_dec(ctx);
    const int init_rc = opus_decoder_init(dec, (opus_int32)UHURA_SAMPLE_RATE_HZ, 1);
    if (init_rc != OPUS_OK) {
        return CODEC_ERR_LIBOPUS_INIT;
    }

    ctx->is_initialized = true;
    return CODEC_OK;
}

void OpusDecoder_reset(OpusDecoder_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    OpusDecoder *dec = as_opus_dec(ctx);
    (void)opus_decoder_ctl(dec, OPUS_RESET_STATE);
}

NODISCARD CodecError OpusDecoder_decode_packet(
    OpusDecoder_Owned *ctx,
    ByteSlice_Loaned opus_packet,
    PcmFrame_Owned *out_pcm
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    assert(out_pcm != NULL);

    *out_pcm = (PcmFrame_Owned){0};
    if ((opus_packet.data == NULL) || (opus_packet.len == 0U) ||
        (opus_packet.len > (size_t)UHURA_MAX_OPUS_BYTES)) {
        return CODEC_ERR_DECODE_FAILED;
    }

    OpusDecoder *dec = as_opus_dec(ctx);
    const int decoded_samples = opus_decode(
        dec,
        opus_packet.data,
        (opus_int32)opus_packet.len,
        out_pcm->samples,
        (int)UHURA_SAMPLES_PER_FRAME,
        0
    );

    if (decoded_samples != (int)UHURA_SAMPLES_PER_FRAME) {
        return CODEC_ERR_DECODE_FAILED;
    }

    out_pcm->is_valid = true;
    return CODEC_OK;
}

NODISCARD CodecError OpusDecoder_decode_fec_from_next(
    OpusDecoder_Owned *ctx,
    ByteSlice_Loaned next_opus_packet,
    PcmFrame_Owned *out_pcm
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    assert(out_pcm != NULL);

    *out_pcm = (PcmFrame_Owned){0};
    if ((next_opus_packet.data == NULL) || (next_opus_packet.len == 0U) ||
        (next_opus_packet.len > (size_t)UHURA_MAX_OPUS_BYTES)) {
        return CODEC_ERR_DECODE_FAILED;
    }

    OpusDecoder *dec = as_opus_dec(ctx);
    const int decoded_samples = opus_decode(
        dec,
        next_opus_packet.data,
        (opus_int32)next_opus_packet.len,
        out_pcm->samples,
        (int)UHURA_SAMPLES_PER_FRAME,
        1
    );

    if (decoded_samples != (int)UHURA_SAMPLES_PER_FRAME) {
        return CODEC_ERR_DECODE_FAILED;
    }

    out_pcm->is_valid = true;
    return CODEC_OK;
}

NODISCARD CodecError OpusDecoder_decode_plc(
    OpusDecoder_Owned *ctx,
    PcmFrame_Owned *out_pcm
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    assert(out_pcm != NULL);

    *out_pcm = (PcmFrame_Owned){0};

    OpusDecoder *dec = as_opus_dec(ctx);
    const int decoded_samples = opus_decode(
        dec,
        NULL,
        0,
        out_pcm->samples,
        (int)UHURA_SAMPLES_PER_FRAME,
        0
    );

    if (decoded_samples != (int)UHURA_SAMPLES_PER_FRAME) {
        return CODEC_ERR_DECODE_FAILED;
    }

    out_pcm->is_valid = true;
    return CODEC_OK;
}
