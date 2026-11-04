#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void audio_output_init(void);
void audio_output_start(void);
void audio_output_stop(void);

esp_err_t audio_output_write_pcm(const int16_t* samples,
                                 size_t frames,
                                 uint32_t sample_rate,
                                 uint8_t channels);

void audio_output_poll(void);
void audio_output_set_volume(uint8_t volume_percent);

bool audio_output_is_headphone(void);

uint8_t audio_output_get_volume(void);
uint8_t audio_output_get_headphone_volume(void);
uint8_t audio_output_get_speaker_volume(void);

void audio_output_save_volumes_now(void);

#ifdef __cplusplus
}
#endif
