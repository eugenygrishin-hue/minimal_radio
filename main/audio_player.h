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

#ifdef __cplusplus
}
#endif
