#pragma once

#include "config.h"
#include <stdint.h>
#include <stdbool.h>

void wifi_connect(const char* ssid, const char* pass);
void wifi_reconnect(const char* ssid, const char* pass);
bool wifi_is_connected();
bool wifi_test_connection(const char* ssid, const char* pass);
wifi_test_result_t wifi_get_last_error();

/**
 * @brief Ожидание подключения к Wi-Fi
 * @param timeout_ms Таймаут в миллисекундах
 * @return true если подключено
 */
bool wifi_wait_connected(uint32_t timeout_ms);

bool wifi_is_testing();

/**
 * @brief Получить RSSI текущей точки доступа
 * @return RSSI в dBm, или 0 если не подключено
 */
int wifi_get_rssi(void);

/**
 * @brief Запустить SNTP синхронизацию времени
 */
void wifi_start_sntp(void);

typedef struct {
    char ssid[33];
    int8_t rssi;
} wifi_scan_record_t;

/**
 * @brief Запускает сканирование Wi-Fi сетей (блокирующее или асинхронное)
 * @return Количество найденных сетей или -1 при ошибке
 */
int wifi_scan_networks(void);

/**
 * @brief Получает количество результатов последнего сканирования
 */
int wifi_scan_get_count(void);

/**
 * @brief Получает запись из результатов сканирования по индексу
 */
const wifi_scan_record_t* wifi_scan_get_record(int index);

// ============================================================
// Known Networks (Auto-connect & Auto-save)
// ============================================================
#define MAX_KNOWN_NETWORKS 10

typedef struct {
    char ssid[33];
    char pass[65];
    bool auto_connect;
} known_network_t;

bool wifi_known_networks_load(void);
bool wifi_known_networks_save(void);
bool wifi_known_networks_add(const char* ssid, const char* pass);
bool wifi_known_networks_remove(int index);
int wifi_known_networks_get_count(void);
const known_network_t* wifi_known_networks_get(int index);
bool wifi_try_auto_connect(void);
