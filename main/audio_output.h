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

/**
 * @brief Запускает программный fade out.
 * @param duration_ms  Длительность (50-300 мс).
 */
void audio_output_fade_out(uint32_t duration_ms);

/**
 * @brief Запускает программный fade in.
 */
void audio_output_fade_in(uint32_t duration_ms);

/**
 * @brief true если fade ещё идёт.
 */
bool audio_output_is_fading(void);

/**
 * @brief true если fade out завершён (factor == 0).
 */
bool audio_output_fade_out_done(void);

void audio_output_fade_out(uint32_t duration_ms);
void audio_output_fade_in(uint32_t duration_ms);
bool audio_output_is_fading(void);
bool audio_output_fade_out_done(void);
void audio_output_set_fade_factor(int32_t factor);

#ifdef __cplusplus
}
#endif
