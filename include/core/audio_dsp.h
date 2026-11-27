#ifndef UHURA_CORE_AUDIO_DSP_H
#define UHURA_CORE_AUDIO_DSP_H

#include "core/types.h"

/* =========================================================================
 * Fixed-Point Audio Conditioning & Emergency Volume Override (Rule 1 & Rule 4)
 *
 * 1. 150 Hz IIR High-Pass Filter: removes cabinet rumble & plosives.
 * 2. Dual-Profile AGC + Soft Limiter:
 *    - CAPTURE_MODE_NORMAL_VOICE: gentle speech leveling (up to +9.5 dB).
 *    - CAPTURE_MODE_FAR_FIELD_CARE: +24 dB boost for distant speech / door
 *      knocks at 6m, with a Noise-Floor Gain Cap (+6 dB max in quiet rooms)
 *      so silent rooms in a multi-box home do not amplify hiss.
 * 3. Playback Volume + Emergency Override (100% full output during Care Calls).
 * ========================================================================= */
typedef enum {
    GAIN_MODE_USER_LEVEL     = 0,
    GAIN_MODE_EMERGENCY_FULL = 1
} PlaybackGainMode;

typedef struct {
    int32_t          hp_x_prev;
    int32_t          hp_y_prev_q15;
    uint16_t         agc_gain_q8; /* 256 = 1.0x (0 dB), 4096 = 16.0x (+24 dB) */
    CaptureMode      capture_mode;
    VolumePercent    user_volume;
    PlaybackGainMode playback_gain_mode;
    bool             is_initialized;
} AudioDsp_Owned;

void AudioDsp_init(AudioDsp_Owned *ctx, VolumePercent initial_volume);

void AudioDsp_set_capture_mode(AudioDsp_Owned *ctx, CaptureMode mode);
NODISCARD CaptureMode AudioDsp_capture_mode(const AudioDsp_Owned *ctx);

void AudioDsp_set_user_volume(AudioDsp_Owned *ctx, VolumePercent volume);
NODISCARD VolumePercent AudioDsp_user_volume(const AudioDsp_Owned *ctx);
NODISCARD VolumePercent AudioDsp_effective_volume(const AudioDsp_Owned *ctx);

void AudioDsp_set_playback_gain_mode(AudioDsp_Owned *ctx, PlaybackGainMode mode);
NODISCARD PlaybackGainMode AudioDsp_playback_gain_mode(const AudioDsp_Owned *ctx);

/* Conditions 10 ms (480 samples) of microphone audio in-place */
void AudioDsp_process_capture_10ms(
    AudioDsp_Owned *ctx,
    PcmMonoSliceMut_Loaned mic_inout
);

/* Scales 10 ms (480 samples) of speaker audio with User or Emergency Override volume */
void AudioDsp_process_playback_10ms(
    const AudioDsp_Owned *ctx,
    PcmMonoSliceMut_Loaned speaker_inout
);

#endif /* UHURA_CORE_AUDIO_DSP_H */
