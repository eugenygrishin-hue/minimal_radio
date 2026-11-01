#include "audio_player.h"
#include "audio_output.h"
#include "config.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#include "mp3dec.h"

#include <cstring>
#include <cstdio>

static const char* TAG = "AUDIO";

static RingbufHandle_t s_ringbuf = NULL;

static uint32_t g_dec_buf = 0;
static volatile bool g_playback_started = false;

static int g_current_volume = 75;
static bool g_is_muted = false;

static void decode_play_task(void* pvParameters)
{
    HMP3Decoder mp3_decoder = MP3InitDecoder();

    uint8_t* inbuf = (uint8_t*)heap_caps_malloc(
        DEC_BUF_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    short* outbuf = (short*)heap_caps_malloc(
        2304 * sizeof(short),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    /*
     * Статические буферы, чтобы не грузить стек задачи.
     * stereo_tmp: максимум 1152 стерео-фрейма = 2304 семпла.
     * silence_stereo: 512 стерео-фрейма = 1024 семпла.
     */
    static short stereo_tmp[2304];
    static short silence_stereo[1024];

    if (mp3_decoder == NULL || inbuf == NULL || outbuf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate audio decoder resources");
        if (inbuf) heap_caps_free(inbuf);
        if (outbuf) heap_caps_free(outbuf);
        if (mp3_decoder) MP3FreeDecoder(mp3_decoder);
        vTaskDelete(NULL);
        return;
    }

    int bytes_in_buffer = 0;

    auto do_prefill = [&]() {
        while (bytes_in_buffer < PREFILL_TARGET) {
            size_t size = 0;
            void* data = xRingbufferReceive(s_ringbuf, &size, pdMS_TO_TICKS(2000));

            if (data != NULL) {
                if (bytes_in_buffer + (int)size <= DEC_BUF_SIZE) {
                    memcpy(inbuf + bytes_in_buffer, data, size);
                    bytes_in_buffer += (int)size;
                }
                vRingbufferReturnItem(s_ringbuf, data);
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    };

    do_prefill();

    g_playback_started = true;
    audio_output_start();

    while (1) {
        /* 1. Underrun / rebuffering */
        if (bytes_in_buffer < REBUF_THRESHOLD) {
            ESP_LOGW(TAG, "Buffer underrun! Rebuffering...");
            g_playback_started = false;

            size_t size = 0;
            void* data = NULL;
            while ((data = xRingbufferReceive(s_ringbuf, &size, 0)) != NULL) {
                vRingbufferReturnItem(s_ringbuf, data);
            }

            bytes_in_buffer = 0;
            memset(inbuf, 0, DEC_BUF_SIZE);

            do_prefill();
            g_playback_started = true;
            continue;
        }

        /* 2. Дозаполнение буфера */
        while (bytes_in_buffer < PREFILL_TARGET) {
            size_t size = 0;
            TickType_t timeout = (bytes_in_buffer < 4096)
                                 ? pdMS_TO_TICKS(500)
                                 : pdMS_TO_TICKS(20);

            void* data = xRingbufferReceive(s_ringbuf, &size, timeout);

            if (data != NULL) {
                if (bytes_in_buffer + (int)size <= DEC_BUF_SIZE) {
                    memcpy(inbuf + bytes_in_buffer, data, size);
                    bytes_in_buffer += (int)size;
                }
                vRingbufferReturnItem(s_ringbuf, data);
            } else {
                break;
            }
        }

        /* 3. Защита от тишины при низком буфере */
        if (bytes_in_buffer < 8192) {
            /*
             * silence_stereo содержит 1024 short = 512 стерео-фрейм.
             */
            audio_output_write_pcm(silence_stereo, 512, 44100, 2);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        g_dec_buf = (uint32_t)bytes_in_buffer;

        uint8_t* rp = inbuf;
        int bytes_left = bytes_in_buffer;
        int frames_pass = 0;

        /* 4. Декодирование */
        while (bytes_left > 0 && frames_pass < FRAMES_PER_PASS) {
            int sync_off = MP3FindSyncWord(rp, bytes_left);

            if (sync_off < 0) {
                rp++;
                bytes_left--;
                continue;
            }

            if (sync_off > 0) {
                rp += sync_off;
                bytes_left -= sync_off;
            }

            uint8_t* frame_start = rp;
            int frame_bytes = bytes_left;

            int ret = MP3Decode(mp3_decoder, &rp, &bytes_left, outbuf, 0);

            if (ret == ERR_MP3_INDATA_UNDERFLOW) {
                break;
            }

            if (ret != ERR_MP3_NONE) {
                rp = frame_start + 1;
                bytes_left = frame_bytes - 1;
                continue;
            }

            MP3FrameInfo fi;
            MP3GetLastFrameInfo(mp3_decoder, &fi);

            if (fi.samprate == 0 || fi.nChans == 0 || fi.outputSamps == 0) {
                rp = frame_start + 1;
                bytes_left = frame_bytes - 1;
                continue;
            }

            /*
             * КРИТИЧНО:
             * audio_output_write_pcm ожидает frames = фреймы на канал.
             *
             * LibHelix:
             * - mono: outputSamps = frames
             * - stereo: outputSamps = frames * 2
             */
            size_t frames = 0;

            if (fi.nChans == 2) {
                frames = (size_t)fi.outputSamps / 2;
            } else {
                frames = (size_t)fi.outputSamps;
            }

            if (frames == 0) {
                continue;
            }

            /* Защита от выхода за пределы outbuf / stereo_tmp */
            if (frames > 1152) {
                frames = 1152;
            }

            if (fi.nChans == 1) {
                /* Моно -> стерео */
                for (size_t i = 0; i < frames; ++i) {
                    stereo_tmp[i * 2 + 0] = outbuf[i];
                    stereo_tmp[i * 2 + 1] = outbuf[i];
                }

                audio_output_write_pcm(stereo_tmp,
                                       frames,
                                       (uint32_t)fi.samprate,
                                       2);
            } else {
                /* Уже стерео interleaved */
                audio_output_write_pcm(outbuf,
                                       frames,
                                       (uint32_t)fi.samprate,
                                       2);
            }

            frames_pass++;
        }

        /* 5. Сдвигаем остаток */
        if (bytes_left > 0 && bytes_left < bytes_in_buffer) {
            memmove(inbuf, rp, bytes_left);
        } else if (bytes_left == 0) {
            bytes_left = 0;
        }

        bytes_in_buffer = bytes_left;

        vTaskDelay(1);
    }
}

void audio_init(RingbufHandle_t ringbuf)
{
    s_ringbuf = ringbuf;

    audio_output_init();

    ESP_LOGI(TAG, "Audio player initialized");
}

void audio_start(void)
{
    xTaskCreatePinnedToCore(
        decode_play_task,
        "audio_decode",
        8192,
        NULL,
        10,
        NULL,
        0
    );

    ESP_LOGI(TAG, "Audio decoder task started");
}

void audio_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;

    g_current_volume = vol;

    if (!g_is_muted) {
        audio_output_set_volume((uint8_t)g_current_volume);
    }
}

void audio_toggle_mute(void)
{
    g_is_muted = !g_is_muted;

    if (g_is_muted) {
        audio_output_set_volume(0);
    } else {
        audio_output_set_volume((uint8_t)g_current_volume);
    }
}

uint32_t audio_get_buffer_level(void)
{
    return g_dec_buf;
}

void audio_get_stats(uint32_t* starve, uint32_t* overflow)
{
    if (starve) *starve = 0;
    if (overflow) *overflow = 0;
}

int audio_get_volume(void)
{
    return g_current_volume;
}

bool audio_is_muted(void)
{
    return g_is_muted;
}
