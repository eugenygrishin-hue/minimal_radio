#pragma once
#include <stdint.h>
#include <stddef.h>
#include "catalog_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M3U_PARSER_MAX_STATIONS 1000

/**
 * @brief Генерирует стабильный 32-битный ID на основе URL (FNV-1a hash)
 * Используется для дедупликации и привязки избранного.
 */
uint32_t catalog_generate_station_id(const char* url);

/**
 * @brief Парсит M3U файл и заполняет массив станций.
 * @param filepath Путь к файлу (например, "/sd/radio/import.m3u")
 * @param default_group Имя группы по умолчанию (например, имя файла без .m3u)
 * @param out_stations Массив для записи станций (должен быть предварительно выделен)
 * @param max_stations Максимальное количество станций в массиве
 * @return Количество успешно распарсенных станций
 */
int m3u_parse_file(const char* filepath, const char* default_group, catalog_station_t* out_stations, int max_stations);

#ifdef __cplusplus
}
#endif
