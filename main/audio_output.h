#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Инициализация аудио-выхода.
 * Вызывается один раз после M5.begin().
 */
void audio_output_init(void);

/**
 * @brief Запуск воспроизведения.
 */
void audio_output_start(void);

/**
 * @brief Остановка воспроизведения.
 */
void audio_output_stop(void);

/**
 * @brief Запись PCM.
 *
 * ВАЖНО:
 * samples должны быть interleaved stereo 16-bit.
 * frames = количество фреймов на канал.
 * Для стерео буфер должен содержать frames * 2 семплов.
 *
 * @param samples PCM данные, int16_t, stereo interleaved
 * @param frames количество фреймов на канал
 * @param sample_rate частота дискретизации
 * @param channels должно быть 2
 * @return ESP_OK при успехе
 */
esp_err_t audio_output_write_pcm(const int16_t* samples,
                                 size_t frames,
                                 uint32_t sample_rate,
                                 uint8_t channels);

/**
 * @brief Периодический опрос детекта наушников.
 * Вызывать из ui_update() или другого не критичного цикла.
 * Внутри есть throttle 200 мс и debounce.
 */
void audio_output_poll(void);

/**
 * @brief Установка громкости.
 * @param volume_percent 0..100
 */
void audio_output_set_volume(uint8_t volume_percent);

/**
 * @brief Текущий активный выход.
 * @return true, если сейчас наушники; false, если динамик
 */
bool audio_output_is_headphone(void);

#ifdef __cplusplus
}
#endif
