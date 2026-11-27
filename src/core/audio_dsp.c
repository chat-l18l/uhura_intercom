#include "core/audio_dsp.h"

#include <string.h>

enum {
    /* alpha = 1 / (1 + 2*pi*150/48000) ≈ 0.98074 -> 32137 in Q15 */
    HP150_ALPHA_Q15           = 32137,
    AGC_UNITY_Q8              = 256U,   /* 1.0x (0 dB) */
    AGC_NORMAL_MAX_Q8         = 768U,   /* 3.0x (+9.5 dB) */
    AGC_FAR_SILENCE_CAP_Q8    = 512U,   /* 2.0x (+6.0 dB) max in silent room */
    AGC_FAR_ACTIVE_MAX_Q8     = 4096U,  /* 16.0x (+24.1 dB) for distant voice/knocks */
    AGC_NORMAL_TARGET_PEAK    = 9000U,
    AGC_FAR_TARGET_PEAK       = 12000U,
    AGC_NORMAL_FLOOR_PEAK     = 200U,
    AGC_FAR_NOISE_FLOOR_PEAK  = 90U,    /* Threshold separating silent room from distant knock */
    SOFT_LIMIT_KNEE           = 24000,
    SOFT_LIMIT_CEILING        = 32000
};

static int16_t soft_limit_i32(int32_t sample) {
    const bool neg = (sample < 0);
    int32_t mag = neg ? -sample : sample;

    if (mag > SOFT_LIMIT_KNEE) {
        mag = SOFT_LIMIT_KNEE + ((mag - SOFT_LIMIT_KNEE) >> 2);
        if (mag > SOFT_LIMIT_CEILING) {
            mag = SOFT_LIMIT_CEILING;
        }
    }

    return neg ? (int16_t)(-mag) : (int16_t)mag;
}

static VolumePercent clamp_volume(VolumePercent v) {
    return (VolumePercent){ .pct = (v.pct > 100U) ? 100U : v.pct };
}

void AudioDsp_init(AudioDsp_Owned *ctx, VolumePercent initial_volume) {
    assert(ctx != NULL);
    memset(ctx, 0, sizeof(*ctx));

    ctx->agc_gain_q8        = (uint16_t)AGC_UNITY_Q8;
    ctx->capture_mode       = CAPTURE_MODE_NORMAL_VOICE;
    ctx->user_volume        = clamp_volume(initial_volume);
    ctx->playback_gain_mode = GAIN_MODE_USER_LEVEL;
    ctx->is_initialized     = true;
}

void AudioDsp_set_capture_mode(AudioDsp_Owned *ctx, CaptureMode mode) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if (ctx->capture_mode != mode) {
        ctx->capture_mode = mode;
        ctx->agc_gain_q8  = (uint16_t)AGC_UNITY_Q8;
    }
}

NODISCARD CaptureMode AudioDsp_capture_mode(const AudioDsp_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    return ctx->capture_mode;
}

void AudioDsp_set_user_volume(AudioDsp_Owned *ctx, VolumePercent volume) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    ctx->user_volume = clamp_volume(volume);
}

NODISCARD VolumePercent AudioDsp_user_volume(const AudioDsp_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    return ctx->user_volume;
}

NODISCARD VolumePercent AudioDsp_effective_volume(const AudioDsp_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);

    if (ctx->playback_gain_mode == GAIN_MODE_EMERGENCY_FULL) {
        return (VolumePercent){ .pct = 100U };
    }
    return ctx->user_volume;
}

void AudioDsp_set_playback_gain_mode(AudioDsp_Owned *ctx, PlaybackGainMode mode) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    ctx->playback_gain_mode = mode;
}

NODISCARD PlaybackGainMode AudioDsp_playback_gain_mode(const AudioDsp_Owned *ctx) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    return ctx->playback_gain_mode;
}

void AudioDsp_process_capture_10ms(
    AudioDsp_Owned *ctx,
    PcmMonoSliceMut_Loaned mic_inout
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    assert(mic_inout.samples != NULL);
    assert(mic_inout.frames.count == (uint16_t)UHURA_SAMPLES_PER_FRAME);

    /* Pass 1: 150 Hz 1st-order IIR High-Pass + frame peak measurement */
    uint32_t frame_peak = 0U;
    for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
        const int32_t x = (int32_t)mic_inout.samples[i];
        const int32_t diff_q15 = (x - ctx->hp_x_prev) * 32768;
        const int64_t acc =
            (int64_t)HP150_ALPHA_Q15 * ((int64_t)ctx->hp_y_prev_q15 + (int64_t)diff_q15);
        const int32_t y_q15 = (int32_t)(acc >> 15);
        const int32_t y     = y_q15 >> 15;

        ctx->hp_x_prev     = x;
        ctx->hp_y_prev_q15 = y_q15;

        const int16_t hp_sample = soft_limit_i32(y);
        mic_inout.samples[i] = hp_sample;

        const uint32_t abs_v = (hp_sample < 0)
                               ? (uint32_t)(-(int32_t)hp_sample)
                               : (uint32_t)hp_sample;
        if (abs_v > frame_peak) {
            frame_peak = abs_v;
        }
    }

    /* Pass 2: Compute desired Q8 AGC gain */
    uint32_t target_q8 = (uint32_t)AGC_UNITY_Q8;
    if (ctx->capture_mode == CAPTURE_MODE_FAR_FIELD_CARE) {
        if (frame_peak < (uint32_t)AGC_FAR_NOISE_FLOOR_PEAK) {
            /* Silent room: cap gain at +6 dB so ambient hiss is not amplified */
            target_q8 = (uint32_t)AGC_FAR_SILENCE_CAP_Q8;
        } else {
            target_q8 = ((uint32_t)AGC_FAR_TARGET_PEAK * (uint32_t)AGC_UNITY_Q8) / frame_peak;
            if (target_q8 < (uint32_t)AGC_UNITY_Q8) {
                target_q8 = (uint32_t)AGC_UNITY_Q8;
            } else if (target_q8 > (uint32_t)AGC_FAR_ACTIVE_MAX_Q8) {
                target_q8 = (uint32_t)AGC_FAR_ACTIVE_MAX_Q8;
            }
        }
    } else {
        if (frame_peak >= (uint32_t)AGC_NORMAL_FLOOR_PEAK) {
            target_q8 = ((uint32_t)AGC_NORMAL_TARGET_PEAK * (uint32_t)AGC_UNITY_Q8) / frame_peak;
            if (target_q8 < (uint32_t)AGC_UNITY_Q8) {
                target_q8 = (uint32_t)AGC_UNITY_Q8;
            } else if (target_q8 > (uint32_t)AGC_NORMAL_MAX_Q8) {
                target_q8 = (uint32_t)AGC_NORMAL_MAX_Q8;
            }
        }
    }

    /* Fast attack on loud transients, smooth release on quiet sounds */
    uint32_t cur_q8 = (uint32_t)ctx->agc_gain_q8;
    if (target_q8 < cur_q8) {
        cur_q8 = (cur_q8 + target_q8) >> 1U;
    } else {
        const uint32_t step = (target_q8 - cur_q8) >> 2U;
        cur_q8 += (step > 0U) ? step : ((target_q8 > cur_q8) ? 1U : 0U);
    }
    ctx->agc_gain_q8 = (uint16_t)cur_q8;

    /* Pass 3: Apply AGC gain + soft-knee brickwall limiter */
    for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
        const int32_t amplified = ((int32_t)mic_inout.samples[i] * (int32_t)cur_q8) >> 8;
        mic_inout.samples[i] = soft_limit_i32(amplified);
    }
}

void AudioDsp_process_playback_10ms(
    const AudioDsp_Owned *ctx,
    PcmMonoSliceMut_Loaned speaker_inout
) {
    assert(ctx != NULL);
    assert(ctx->is_initialized);
    assert(speaker_inout.samples != NULL);
    assert(speaker_inout.frames.count == (uint16_t)UHURA_SAMPLES_PER_FRAME);

    const VolumePercent eff = AudioDsp_effective_volume(ctx);
    if (eff.pct == 0U) {
        memset(speaker_inout.samples, 0, (size_t)UHURA_PCM_FRAME_BYTES);
        return;
    }
    if (eff.pct >= 100U) {
        return;
    }

    const int32_t scale = (int32_t)eff.pct;
    for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
        const int32_t scaled = ((int32_t)speaker_inout.samples[i] * scale) / 100;
        speaker_inout.samples[i] = soft_limit_i32(scaled);
    }
}
