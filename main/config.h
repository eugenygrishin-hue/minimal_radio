#pragma once

#include <stdint.h>

// === ПИНЫ И АДРЕСА ===
#define I2S_BCK_PIN         GPIO_NUM_0
#define I2S_WS_PIN          GPIO_NUM_6
#define I2S_DOUT_PIN        GPIO_NUM_13
#define I2S_DIN_PIN         GPIO_NUM_14
#define I2S_MCLK_PIN        GPIO_NUM_7
#define ES8388_ADDR         0x10
#define STM32_ADDR          0x33
#define HMI_I2C_ADDR        0x41

// === НАСТРОЙКИ WI-FI ПО УМОЛЧАНИЮ ===
#define DEFAULT_WIFI_SSID   "Elektron-38463"
#define DEFAULT_WIFI_PASS   "138ff5395"

// === ПАРАМЕТРЫ АУДИО И БУФЕРОВ ===
#define RINGBUF_SIZE        (256 * 1024)
#define DEC_BUF_SIZE        (128 * 1024)
#define PREFILL_TARGET      (115 * 1024)
#define REBUF_THRESHOLD     (16 * 1024)
#define FRAMES_PER_PASS     10

// === СТРУКТУРЫ ДАННЫХ ===
typedef struct {
    const char *name;
    const char *url;
} radio_station_t;

typedef struct {
    const char *category;
    const radio_station_t *stations;
    int count;
} radio_category_t;

// === ОБЪЯВЛЕНИЯ ВНЕШНИХ ПЕРЕМЕННЫХ (определены в config.cpp) ===
extern const radio_category_t g_categories[];
extern const int g_category_count;
extern const int g_total_stations;

// === ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ===
const char* get_station_url(int index);

/**
 * @brief Результат теста подключения Wi-Fi
 */
typedef enum {
    WIFI_TEST_NOT_STARTED = 0,
    WIFI_TEST_IN_PROGRESS,
    WIFI_TEST_SUCCESS,
    WIFI_TEST_FAILED_AUTH,      // Неверный пароль
    WIFI_TEST_FAILED_NOT_FOUND, // Сеть не найдена
    WIFI_TEST_FAILED_TIMEOUT,   // Таймаут
    WIFI_TEST_FAILED_OTHER      // Другая ошибка
} wifi_test_result_t;
