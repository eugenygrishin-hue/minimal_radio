#pragma once

#include "freertos/ringbuf.h"
#include <stdint.h>

/**
 * @brief Инициализация аудио-плеера
 * @param ringbuf Указатель на RingBuffer для получения аудиоданных
 */
void audio_init(RingbufHandle_t ringbuf);

/**
 * @brief Запуск задачи декодера
 */
void audio_start();

/**
 * @brief Установка громкости
 * @param vol Громкость от 0 до 100
 */
void audio_set_volume(int vol);

/**
 * @brief Переключение режима mute
 */
void audio_toggle_mute();

/**
 * @brief Получение текущего уровня буфера декодера
 * @return Количество байт в буфере декодера
 */
uint32_t audio_get_buffer_level();

/**
 * @brief Получение статистики буфера
 * @param starve Указатель для количества случаев нехватки данных
 * @param overflow Указатель для количества переполнений
 */
void audio_get_stats(uint32_t* starve, uint32_t* overflow);

/**
 * @brief Получение текущей громкости
 * @return Громкость от 0 до 100
 */
int audio_get_volume();

/**
 * @brief Проверка режима mute
 * @return true если mute активен
 */
bool audio_is_muted();

// ============================================================
// Headphone Detection & Output Switching
// ============================================================

/**
 * @brief Проверяет, подключены ли наушники.
 * @return true если наушники подключены
 */
bool audio_is_headphone_connected(void);

/**
 * @brief Переключает аудиовыход между наушниками и динамиком.
 * @param headphone true = наушники, false = динамик
 */
void audio_set_output_headphone(bool headphone);

/**
 * @brief Возвращает текущий режим выхода.
 * @return true если наушники, false если динамик
 */
bool audio_is_headphone_output(void);

