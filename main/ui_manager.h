#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

/**
 * @brief События от UI
 */
typedef enum {
    UI_EVENT_NONE = 0,
    UI_EVENT_STATION_CHANGED,
    UI_EVENT_VOLUME_CHANGED,
    UI_EVENT_MUTE_TOGGLED,
    UI_EVENT_WIFI_REVERT,
    UI_EVENT_WIFI_SAVE,
    UI_EVENT_WIFI_TEST,
    UI_EVENT_WIFI_CONNECT,
    UI_EVENT_FAVORITE_TOGGLED,  // <-- НОВОЕ
    UI_EVENT_CATALOG_UPDATE,    // <-- НОВОЕ
	UI_EVENT_WIFI_SCAN_REQUESTED,
	UI_EVENT_WIFI_CONNECT_REQUESTED,
} ui_event_t;

// Режимы UI
typedef enum {
    MODE_PLAYBACK = 0,
    MODE_FAVORITES,
    MODE_CATEGORIES,
    MODE_STATIONS,
    MODE_SETTINGS,
    MODE_WIFI,
    MODE_CATALOG,
    MODE_ABOUT,
    MODE_VOLUME,
    MODE_KNOWN_NETWORKS  // <-- НОВОЕ
} ui_mode_t;

/**
 * @brief Инициализация UI
 */
void ui_init();

/**
 * @brief Обновление UI (вызывается в цикле задачи)
 */
void ui_update();

/**
 * @brief Установка информации для экрана воспроизведения
 */
void ui_set_playback_info(const char* station_name,
                          const char* track_title,
                          uint32_t buffer_level,
                          uint32_t buffer_target,
                          uint32_t starve_count,
                          uint32_t overflow_count);

/**
 * @brief Получение последнего события от UI
 */
ui_event_t ui_get_event();

/**
 * @brief Получение индекса выбранной станции
 */
int ui_get_selected_station();

/**
 * @brief Получение текущей громкости
 */
int ui_get_volume();

/**
 * @brief Получение текущих значений Wi-Fi из полей ввода
 */
void ui_get_wifi_credentials(char* ssid, size_t ssid_len, char* pass, size_t pass_len);

/**
 * @brief Установка результата теста Wi-Fi
 */
void ui_set_wifi_test_result(wifi_test_result_t result);

/**
 * @brief Получение результата теста Wi-Fi
 */
wifi_test_result_t ui_get_wifi_test_result();

/**
 * @brief Установка начальных Wi-Fi кредов (вызывается из main после загрузки из NVS)
 */
void ui_set_wifi_credentials(const char* ssid, const char* pass);

void ui_set_network_info(uint32_t net_fill_bytes, uint32_t ring_capacity_bytes);

void ui_trigger_wifi_scan(void);

void ui_trigger_wifi_connect(void);

/**
 * @brief Показать или скрыть результаты сканирования Wi-Fi на экране
 */
void ui_set_wifi_scan_ready(bool ready);

/**
 * @brief Принудительно переключить режим UI извне (например, из главного цикла)
 */
void ui_set_current_mode(ui_mode_t mode);

