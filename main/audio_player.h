#pragma once

#include "freertos/ringbuf.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(RingbufHandle_t ringbuf);
void audio_start(void);

void audio_set_volume(int vol);
void audio_toggle_mute(void);

uint32_t audio_get_buffer_level(void);
void audio_get_stats(uint32_t* starve, uint32_t* overflow);

int audio_get_volume(void);
bool audio_is_muted(void);

/**
 * @brief Принудительно сбрасывает внутренний декодерный буфер.
 * Вызывать при переключении станции, ПОСЛЕ http_stream_drain_ringbuf().
 */
void audio_player_force_reset(void);

/**
 * @brief true, когда decode_task вышел из rebuffering и начал выводить PCM.
 * Используется как триггер fade in при переключении станции.
 */
bool audio_player_is_outputting(void);

#ifdef __cplusplus
}
#endif
