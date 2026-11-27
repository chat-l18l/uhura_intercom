#ifndef UHURA_CORE_OPUS_CODEC_H
#define UHURA_CORE_OPUS_CODEC_H

#include "core/types.h"

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
  #include <stdalign.h>
#endif

/* =========================================================================
 * Zero-Malloc Opus Codec Wrappers (Rule 1 & Rule 2)
 * Uses opus_encoder_init / opus_decoder_init on statically aligned storage.
 * ========================================================================= */
enum {
    UHURA_OPUS_ENC_STATE_BYTES = 65536U, /* 64 KB static storage (>= opus_encoder_get_size(1)) */
    UHURA_OPUS_DEC_STATE_BYTES = 40960U, /* 40 KB static storage (>= opus_decoder_get_size(1)) */
    UHURA_DEFAULT_BITRATE_BPS  = 32000U
};

typedef enum {
    FEC_DISABLED = 0,
    FEC_ENABLED  = 1
} InbandFecMode;

typedef enum {
    CODEC_OK = 0,
    CODEC_ERR_STATE_TOO_SMALL,
    CODEC_ERR_INVALID_CONFIG,
    CODEC_ERR_LIBOPUS_INIT,
    CODEC_ERR_ENCODE_FAILED,
    CODEC_ERR_DECODE_FAILED
} CodecError;

typedef struct {
    alignas(max_align_t) uint8_t state_storage[UHURA_OPUS_ENC_STATE_BYTES];
    bool is_initialized;
} OpusEncoder_Owned;

typedef struct {
    alignas(max_align_t) uint8_t state_storage[UHURA_OPUS_DEC_STATE_BYTES];
    bool is_initialized;
} OpusDecoder_Owned;

/* Parameter Object Pattern (Rule 7) */
typedef struct {
    BitrateBps    bitrate;
    InbandFecMode fec_mode;
} OpusEncoderConfigParams;

typedef struct {
    PcmMonoSlice_Loaned pcm_in;
    TalkSessionId       session;
    SeqNum              seq;
} OpusEncodeParams;

NODISCARD CodecError OpusEncoder_init(
    OpusEncoder_Owned *ctx,
    OpusEncoderConfigParams params
);

/* Switches dynamically between voice-optimized PTT and non-speech-preserving Care mode */
void OpusEncoder_set_capture_mode(
    OpusEncoder_Owned *ctx,
    CaptureMode mode
);

NODISCARD CodecError OpusEncoder_encode_frame(
    OpusEncoder_Owned *ctx,
    OpusEncodeParams params,
    EncodedFrame_Owned *out_encoded
);

NODISCARD CodecError OpusDecoder_init(OpusDecoder_Owned *ctx);

void OpusDecoder_reset(OpusDecoder_Owned *ctx);

NODISCARD CodecError OpusDecoder_decode_packet(
    OpusDecoder_Owned *ctx,
    ByteSlice_Loaned opus_packet,
    PcmFrame_Owned *out_pcm
);

NODISCARD CodecError OpusDecoder_decode_fec_from_next(
    OpusDecoder_Owned *ctx,
    ByteSlice_Loaned next_opus_packet,
    PcmFrame_Owned *out_pcm
);

NODISCARD CodecError OpusDecoder_decode_plc(
    OpusDecoder_Owned *ctx,
    PcmFrame_Owned *out_pcm
);

#endif /* UHURA_CORE_OPUS_CODEC_H */
