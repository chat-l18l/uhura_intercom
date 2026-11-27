#include "core/audio_dsp.h"

#include <stdio.h>

#define TEST_CHECK(cond)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static uint16_t frame_peak(const PcmFrame_Owned *f) {
    uint16_t pk = 0U;
    for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
        const int32_t v = f->samples[i];
        const uint16_t m = (v < 0) ? (uint16_t)(-v) : (uint16_t)v;
        if (m > pk) {
            pk = m;
        }
    }
    return pk;
}

int main(void) {
    AudioDsp_Owned dsp;
    AudioDsp_init(&dsp, (VolumePercent){ .pct = 15U });

    /* 1. 150 Hz High-Pass must strip constant DC offset */
    PcmFrame_Owned dc = {0};
    for (size_t tick = 0U; tick < 25U; ++tick) {
        PcmFrame_init_silence(&dc);
        for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
            dc.samples[i] = 12000;
        }
        AudioDsp_process_capture_10ms(&dsp, PcmFrame_loan_mut(&dc));
    }
    /* After 250 ms of pure DC, output must have decayed close to zero */
    TEST_CHECK(frame_peak(&dc) < 100U);

    /* 2. Far-Field Care AGC: Noise-Floor Cap keeps silent room quiet */
    AudioDsp_set_capture_mode(&dsp, CAPTURE_MODE_FAR_FIELD_CARE);
    PcmFrame_Owned quiet_room = {0};
    for (size_t tick = 0U; tick < 15U; ++tick) {
        PcmFrame_init_silence(&quiet_room);
        for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
            quiet_room.samples[i] = ((i & 1U) == 0U) ? 25 : -25;
        }
        AudioDsp_process_capture_10ms(&dsp, PcmFrame_loan_mut(&quiet_room));
    }
    /* Must NOT be boosted by 16x (400); capped at <= 2x (+6 dB) */
    TEST_CHECK(frame_peak(&quiet_room) <= 70U);

    /* 3. Far-Field Care AGC: Faint door knock / distant voice (+/- 450) IS boosted strongly */
    PcmFrame_Owned distant_knock = {0};
    for (size_t tick = 0U; tick < 15U; ++tick) {
        PcmFrame_init_silence(&distant_knock);
        for (size_t i = 0U; i < (size_t)UHURA_SAMPLES_PER_FRAME; ++i) {
            distant_knock.samples[i] = ((i % 24U) < 12U) ? 450 : -450;
        }
        AudioDsp_process_capture_10ms(&dsp, PcmFrame_loan_mut(&distant_knock));
    }
    TEST_CHECK(frame_peak(&distant_knock) > 4000U);

    /* 4. Emergency Volume Override: overrides 0% night mute to 100%, then restores 0% */
    AudioDsp_set_user_volume(&dsp, (VolumePercent){ .pct = 0U });
    PcmFrame_Owned spk = {0};
    PcmFrame_init_silence(&spk);
    spk.samples[10] = 10000;
    AudioDsp_process_playback_10ms(&dsp, PcmFrame_loan_mut(&spk));
    TEST_CHECK(spk.samples[10] == 0);

    AudioDsp_set_playback_gain_mode(&dsp, GAIN_MODE_EMERGENCY_FULL);
    spk.samples[10] = 10000;
    AudioDsp_process_playback_10ms(&dsp, PcmFrame_loan_mut(&spk));
    TEST_CHECK(spk.samples[10] == 10000);

    AudioDsp_set_playback_gain_mode(&dsp, GAIN_MODE_USER_LEVEL);
    spk.samples[10] = 10000;
    AudioDsp_process_playback_10ms(&dsp, PcmFrame_loan_mut(&spk));
    TEST_CHECK(spk.samples[10] == 0);

    return 0;
}
