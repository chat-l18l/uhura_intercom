#include "platform/linux_hal.h"

#include <alsa/asoundlib.h>
#include <string.h>

static void *alsa_worker_entry(void *raw_arg) {
    assert(raw_arg != NULL);
    AudioHal_Owned *ctx = (AudioHal_Owned *)raw_arg;
    snd_pcm_t *cap_pcm  = (snd_pcm_t *)ctx->alsa_capture_handle;
    snd_pcm_t *play_pcm = (snd_pcm_t *)ctx->alsa_playback_handle;
    assert(cap_pcm != NULL);
    assert(play_pcm != NULL);

    (void)snd_pcm_prepare(cap_pcm);
    (void)snd_pcm_prepare(play_pcm);

    /* Pre-fill 2 silent 10 ms periods on playback to prevent immediate startup EPIPE */
    PcmFrame_Owned silence = {0};
    PcmFrame_init_silence(&silence);
    for (size_t pre = 0U; pre < 2U; ++pre) {
        (void)snd_pcm_writei(
            play_pcm,
            silence.samples,
            (snd_pcm_uframes_t)UHURA_SAMPLES_PER_FRAME
        );
    }

    while (!atomic_load_explicit(&ctx->stop_requested, memory_order_acquire)) {
        /* 1. Capture exactly 480 samples (10 ms @ 48 kHz) from microphone */
        PcmFrame_Owned cap_frame = {0};
        PcmFrame_init_silence(&cap_frame);

        snd_pcm_sframes_t rframes = snd_pcm_readi(
            cap_pcm,
            cap_frame.samples,
            (snd_pcm_uframes_t)UHURA_SAMPLES_PER_FRAME
        );
        if (rframes < 0) {
            rframes = snd_pcm_recover(cap_pcm, (int)rframes, 1);
            if (rframes < 0) {
                PcmFrame_init_silence(&cap_frame);
            }
        }
        (void)SpscPcmRing_push_move(&ctx->capture_ring, &cap_frame);

        /* 2. Pop 480 samples from playback ring (or silence on underrun) -> speaker */
        PcmFrame_Owned play_frame = {0};
        const RingStatus pop_st = SpscPcmRing_pop_move(&ctx->playback_ring, &play_frame);
        if (pop_st != RING_OK) {
            PcmFrame_init_silence(&play_frame);
        }

        const snd_pcm_sframes_t wframes = snd_pcm_writei(
            play_pcm,
            play_frame.samples,
            (snd_pcm_uframes_t)UHURA_SAMPLES_PER_FRAME
        );
        if (wframes < 0) {
            (void)snd_pcm_recover(play_pcm, (int)wframes, 1);
        }
    }

    return NULL;
}

NODISCARD static HalError open_alsa_stream(
    const char *device_name,
    snd_pcm_stream_t stream_dir,
    DurationMs latency,
    snd_pcm_t **out_pcm
) {
    assert(out_pcm != NULL);
    *out_pcm = NULL;

    const char *dev = ((device_name == NULL) || (device_name[0] == '\0'))
                      ? "default"
                      : device_name;

    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, dev, stream_dir, 0) < 0) {
        return HAL_ERR_AUDIO_OPEN;
    }

    const unsigned int latency_us =
        (latency.ms < 15U) ? 30000U : (unsigned int)(latency.ms * 1000U);

    /*
     * snd_pcm_set_params configures 48 kHz S16 mono without calling
     * snd_pcm_hw_params_malloc, and enables soft_resample=1 for Pi/Jetson USB audio.
     */
    const int rc = snd_pcm_set_params(
        pcm,
        SND_PCM_FORMAT_S16,
        SND_PCM_ACCESS_RW_INTERLEAVED,
        1U,
        (unsigned int)UHURA_SAMPLE_RATE_HZ,
        1,
        latency_us
    );
    if (rc < 0) {
        (void)snd_pcm_close(pcm);
        return HAL_ERR_AUDIO_PARAMS;
    }

    *out_pcm = pcm;
    return HAL_OK;
}

NODISCARD HalError AudioHal_open_and_start(
    AudioHal_Owned *ctx,
    AudioHalConfigParams params
) {
    assert(ctx != NULL);
    memset(ctx, 0, sizeof(*ctx));

    SpscPcmRing_init(&ctx->capture_ring);
    SpscPcmRing_init(&ctx->playback_ring);
    atomic_init(&ctx->stop_requested, false);
    ctx->mode = params.mode;

    if (params.mode == AUDIO_MODE_SYNTH_NO_HW) {
        ctx->is_open = true;
        return HAL_OK;
    }

    snd_pcm_t *cap_pcm  = NULL;
    snd_pcm_t *play_pcm = NULL;

    HalError err = open_alsa_stream(
        params.capture_device,
        SND_PCM_STREAM_CAPTURE,
        params.alsa_latency,
        &cap_pcm
    );
    if (err != HAL_OK) {
        return err;
    }

    err = open_alsa_stream(
        params.playback_device,
        SND_PCM_STREAM_PLAYBACK,
        params.alsa_latency,
        &play_pcm
    );
    if (err != HAL_OK) {
        (void)snd_pcm_close(cap_pcm);
        return err;
    }

    ctx->alsa_capture_handle  = cap_pcm;
    ctx->alsa_playback_handle = play_pcm;
    ctx->is_open              = true;

    if (pthread_create(&ctx->worker_thread, NULL, alsa_worker_entry, ctx) != 0) {
        (void)snd_pcm_close(play_pcm);
        (void)snd_pcm_close(cap_pcm);
        memset(ctx, 0, sizeof(*ctx));
        return HAL_ERR_AUDIO_THREAD;
    }

    ctx->thread_started = true;
    return HAL_OK;
}

void AudioHal_stop_and_close(AudioHal_Owned *ctx) {
    assert(ctx != NULL);
    if (!ctx->is_open) {
        return;
    }

    atomic_store_explicit(&ctx->stop_requested, true, memory_order_release);

    if (ctx->thread_started) {
        (void)pthread_join(ctx->worker_thread, NULL);
        ctx->thread_started = false;
    }

    if (ctx->alsa_capture_handle != NULL) {
        (void)snd_pcm_close((snd_pcm_t *)ctx->alsa_capture_handle);
        ctx->alsa_capture_handle = NULL;
    }
    if (ctx->alsa_playback_handle != NULL) {
        (void)snd_pcm_close((snd_pcm_t *)ctx->alsa_playback_handle);
        ctx->alsa_playback_handle = NULL;
    }

    ctx->is_open = false;
}
