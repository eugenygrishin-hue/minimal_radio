#include "m3u_parser.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const char* TAG = "M3U_PARSER";

// Реализация хэша FNV-1a (32-bit)
uint32_t catalog_generate_station_id(const char* url) {
    uint32_t hash = 2166136261u;
    for (const char* p = url; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 16777619u;
    }
    return hash;
}

int m3u_parse_file(const char* filepath, const char* default_group, catalog_station_t* out_stations, int max_stations) {
    FILE* f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open M3U file: %s", filepath);
        return 0;
    }

    char line[512];
    int count = 0;
    char current_name[CATALOG_MAX_NAME_LEN + 1] = "Unknown";
    bool has_extinf = false;

    char active_group[CATALOG_MAX_CATEGORY_LEN + 1];
    if (default_group && default_group[0] != '\0') {
        strncpy(active_group, default_group, sizeof(active_group) - 1);
        active_group[sizeof(active_group) - 1] = '\0';
    } else {
        strcpy(active_group, "Imported");
    }

    while (fgets(line, sizeof(line), f) && count < max_stations) {
        // Обрезаем пробелы и переносы строк в конце
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' ' || line[len-1] == '\t')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        if (strncmp(line, "#EXTINF:", 8) == 0) {
            has_extinf = true;
            strcpy(current_name, "Unknown");

            // 1. Ищем group-title="..."
            char final_group[CATALOG_MAX_CATEGORY_LEN + 1];
            char* gt = strstr(line, "group-title=\"");
            if (gt) {
                gt += 13;
                char* gt_end = strchr(gt, '"');
                if (gt_end) {
                    size_t g_len = gt_end - gt;
                    if (g_len > 0 && g_len < sizeof(final_group)) {
                        strncpy(final_group, gt, g_len);
                        final_group[g_len] = '\0';
                    } else {
                        strcpy(final_group, active_group);
                    }
                } else {
                    strcpy(final_group, active_group);
                }
            } else {
                strcpy(final_group, active_group);
            }

            // 2. Ищем имя после последней запятой
            char* comma = strrchr(line, ',');
            if (comma) {
                strncpy(current_name, comma + 1, sizeof(current_name) - 1);
                current_name[sizeof(current_name) - 1] = '\0';
            }

            // Сохраняем группу во временное поле (мы используем name как носитель, но лучше иметь отдельное,
            // однако для простоты пока сохраним группу в name, а при merge решим, или добавим поле в struct)
            // Для совместимости с текущим catalog_station_t, мы пока сохраним имя, а группу передадим в add_station
            // Но так как out_stations - это catalog_station_t, мы временно положим имя.
            // *Уточнение*: мы будем передавать группу отдельно в merge, или добавим поле в struct (см. Шаг 2).
            strncpy(out_stations[count].name, current_name, sizeof(out_stations[count].name) - 1);
            out_stations[count].name[sizeof(out_stations[count].name) - 1] = '\0';

            // Временно используем bitrate_kbps как хранение индекса группы, но это грязно.
            // Лучше добавим поле `char group[48]` в catalog_station_t на Шаге 2.
            // Пока оставим name, а группу обработаем в парсере иначе, или просто добавим поле.
            // Добавим поле `group` в catalog_station_t ниже.

        } else if (line[0] != '#') {
            // Это строка с URL
            if (has_extinf || count == 0) {
                strncpy(out_stations[count].url, line, sizeof(out_stations[count].url) - 1);
                out_stations[count].url[sizeof(out_stations[count].url) - 1] = '\0';

                // Генерируем стабильный ID
                out_stations[count].id = catalog_generate_station_id(out_stations[count].url);
                out_stations[count].bitrate_kbps = 0; // Будет определен позже или при merge

                // Базовая валидация: URL должен начинаться с http:// или https://
                if (strncmp(out_stations[count].url, "http://", 7) == 0 ||
                    strncmp(out_stations[count].url, "https://", 8) == 0) {
                    count++;
                } else {
                    ESP_LOGW(TAG, "Skipping invalid URL: %s", out_stations[count].url);
                }
            }
            has_extinf = false; // Сброс для следующей итерации
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "Parsed %d valid stations from %s", count, filepath);
    return count;
}
