#include "platform/win32_hal.h"

#include <string.h>

static DWORD WINAPI win_audio_worker(LPVOID raw_arg) {
    assert(raw_arg != NULL);
    WinAudioHal_Owned *ctx = (WinAudioHal_Owned *)raw_arg;

    (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    while (!atomic_load_explicit(&ctx->stop_requested, memory_order_acquire)) {
        (void)WaitForSingleObject(ctx->audio_event, 15U);

        if (atomic_load_explicit(&ctx->stop_requested, memory_order_acquire)) {
            break;
        }

        /* 1. Drain completed 10 ms microphone capture blocks -> capture_ring */
        if (ctx->has_capture_hw) {
            for (size_t i = 0U; i < (size_t)UHURA_WINMM_BUF_COUNT; ++i) {
                WAVEHDR *hdr = &ctx->in_hdrs[i];
                if ((hdr->dwFlags & WHDR_DONE) != 0U) {
                    PcmFrame_Owned cap = {0};
                    PcmFrame_init_silence(&cap);
                    if (hdr->dwBytesRecorded == (DWORD)UHURA_PCM_FRAME_BYTES) {
                        memcpy(cap.samples, ctx->in_pcm[i], (size_t)UHURA_PCM_FRAME_BYTES);
                    }
                    (void)SpscPcmRing_push_move(&ctx->capture_ring, &cap);

                    hdr->dwBytesRecorded = 0U;
                    (void)waveInAddBuffer(ctx->wave_in, hdr, (UINT)sizeof(WAVEHDR));
                }
            }
        }

        /* 2. Refill completed 10 ms speaker playback blocks <- playback_ring */
        if (ctx->has_playback_hw) {
            for (size_t i = 0U; i < (size_t)UHURA_WINMM_BUF_COUNT; ++i) {
                WAVEHDR *hdr = &ctx->out_hdrs[i];
                if ((hdr->dwFlags & WHDR_DONE) != 0U) {
                    PcmFrame_Owned play = {0};
                    if (SpscPcmRing_pop_move(&ctx->playback_ring, &play) == RING_OK) {
                        memcpy(ctx->out_pcm[i], play.samples, (size_t)UHURA_PCM_FRAME_BYTES);
                    } else {
                        memset(ctx->out_pcm[i], 0, (size_t)UHURA_PCM_FRAME_BYTES);
                    }
                    (void)waveOutWrite(ctx->wave_out, hdr, (UINT)sizeof(WAVEHDR));
                }
            }
        }
    }

    return 0U;
}

NODISCARD WinHalError WinAudioHal_open_and_start(
    WinAudioHal_Owned *ctx,
    WinAudioConfigParams params
) {
    assert(ctx != NULL);
    memset(ctx, 0, sizeof(*ctx));

    SpscPcmRing_init(&ctx->capture_ring);
    SpscPcmRing_init(&ctx->playback_ring);
    atomic_init(&ctx->stop_requested, false);
    ctx->mode = params.mode;

    if (params.mode == WIN_AUDIO_SYNTH_NO_HW) {
        ctx->is_open = true;
        return WIN_HAL_OK;
    }

    ctx->audio_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (ctx->audio_event == NULL) {
        return WIN_HAL_ERR_AUDIO_OPEN;
    }

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 1U;
    wfx.nSamplesPerSec  = (DWORD)UHURA_SAMPLE_RATE_HZ;
    wfx.wBitsPerSample  = 16U;
    wfx.nBlockAlign     = 2U;
    wfx.nAvgBytesPerSec = (DWORD)UHURA_SAMPLE_RATE_HZ * 2U;

    /* Open default Windows 11 speaker (backed by WASAPI auto-converting engine) */
    if (waveOutOpen(
            &ctx->wave_out,
            WAVE_MAPPER,
            &wfx,
            (DWORD_PTR)ctx->audio_event,
            0U,
            CALLBACK_EVENT) == MMSYSERR_NOERROR) {
        ctx->has_playback_hw = true;
        for (size_t i = 0U; i < (size_t)UHURA_WINMM_BUF_COUNT; ++i) {
            memset(ctx->out_pcm[i], 0, (size_t)UHURA_PCM_FRAME_BYTES);
            ctx->out_hdrs[i].lpData         = (LPSTR)ctx->out_pcm[i];
            ctx->out_hdrs[i].dwBufferLength = (DWORD)UHURA_PCM_FRAME_BYTES;
            (void)waveOutPrepareHeader(ctx->wave_out, &ctx->out_hdrs[i], (UINT)sizeof(WAVEHDR));
            (void)waveOutWrite(ctx->wave_out, &ctx->out_hdrs[i], (UINT)sizeof(WAVEHDR));
        }
    }

    /* Open default Windows 11 microphone (gracefully optional if PC has speakers only) */
    if (waveInOpen(
            &ctx->wave_in,
            WAVE_MAPPER,
            &wfx,
            (DWORD_PTR)ctx->audio_event,
            0U,
            CALLBACK_EVENT) == MMSYSERR_NOERROR) {
        ctx->has_capture_hw = true;
        for (size_t i = 0U; i < (size_t)UHURA_WINMM_BUF_COUNT; ++i) {
            memset(ctx->in_pcm[i], 0, (size_t)UHURA_PCM_FRAME_BYTES);
            ctx->in_hdrs[i].lpData         = (LPSTR)ctx->in_pcm[i];
            ctx->in_hdrs[i].dwBufferLength = (DWORD)UHURA_PCM_FRAME_BYTES;
            (void)waveInPrepareHeader(ctx->wave_in, &ctx->in_hdrs[i], (UINT)sizeof(WAVEHDR));
            (void)waveInAddBuffer(ctx->wave_in, &ctx->in_hdrs[i], (UINT)sizeof(WAVEHDR));
        }
        (void)waveInStart(ctx->wave_in);
    }

    ctx->is_open = true;

    ctx->worker_thread = CreateThread(NULL, 65536U, win_audio_worker, ctx, 0U, NULL);
    if (ctx->worker_thread == NULL) {
        WinAudioHal_stop_and_close(ctx);
        return WIN_HAL_ERR_AUDIO_THREAD;
    }

    ctx->thread_started = true;
    return WIN_HAL_OK;
}

void WinAudioHal_stop_and_close(WinAudioHal_Owned *ctx) {
    assert(ctx != NULL);
    if (!ctx->is_open) {
        return;
    }

    atomic_store_explicit(&ctx->stop_requested, true, memory_order_release);
    if (ctx->audio_event != NULL) {
        (void)SetEvent(ctx->audio_event);
    }

    if (ctx->thread_started && (ctx->worker_thread != NULL)) {
        (void)WaitForSingleObject(ctx->worker_thread, 2000U);
        (void)CloseHandle(ctx->worker_thread);
        ctx->worker_thread  = NULL;
        ctx->thread_started = false;
    }

    if (ctx->has_capture_hw) {
        (void)waveInReset(ctx->wave_in);
        for (size_t i = 0U; i < (size_t)UHURA_WINMM_BUF_COUNT; ++i) {
            (void)waveInUnprepareHeader(ctx->wave_in, &ctx->in_hdrs[i], (UINT)sizeof(WAVEHDR));
        }
        (void)waveInClose(ctx->wave_in);
        ctx->has_capture_hw = false;
    }

    if (ctx->has_playback_hw) {
        (void)waveOutReset(ctx->wave_out);
        for (size_t i = 0U; i < (size_t)UHURA_WINMM_BUF_COUNT; ++i) {
            (void)waveOutUnprepareHeader(ctx->wave_out, &ctx->out_hdrs[i], (UINT)sizeof(WAVEHDR));
        }
        (void)waveOutClose(ctx->wave_out);
        ctx->has_playback_hw = false;
    }

    if (ctx->audio_event != NULL) {
        (void)CloseHandle(ctx->audio_event);
        ctx->audio_event = NULL;
    }

    ctx->is_open = false;
}
