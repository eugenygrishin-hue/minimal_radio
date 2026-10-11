#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Инициализация и монтирование SD-карты.
 * @return true если SD доступна и смонтирована.
 */
bool sd_manager_init(void);

/**
 * @brief Проверка, смонтирована ли SD-карта.
 */
bool sd_manager_is_mounted(void);

/**
 * @brief Точка монтирования SD-карты.
 */
const char* sd_manager_mount_point(void);

/**
 * @brief Создать директорию, если её нет.
 */
bool sd_manager_ensure_dir(const char* path);

/**
 * @brief Записать текстовый файл.
 */
bool sd_manager_write_text(const char* path, const char* text);

/**
 * @brief Прочитать текстовый файл.
 *
 * Возвращает выделенный буфер. Вызывающий код обязан освободить его через free().
 * Если out_len != NULL, туда записывается длина без нулевого терминала.
 */
char* sd_manager_read_text(const char* path, size_t* out_len);

/**
 * @brief Свободное место на SD-карте в байтах.
 */
uint64_t sd_manager_free_bytes(void);

/**
 * @brief Простой self-test: запись/чтение/удаление файла.
 */
bool sd_manager_self_test(void);

#ifdef __cplusplus
}
#endif
