#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Инициализация хранилища (NVS)
 * Должна быть вызвана один раз при старте приложения
 */
void storage_init();

/**
 * @brief Сохранение учетных данных Wi-Fi в NVS
 * @param ssid Имя сети (максимум 32 символа + '\0')
 * @param pass Пароль (максимум 64 символа + '\0')
 */
void storage_save_wifi(const char* ssid, const char* pass);

/**
 * @brief Загрузка учетных данных Wi-Fi из NVS
 * @param ssid Буфер для имени сети (должен быть не меньше 33 байт)
 * @param ssid_len Размер буфера ssid
 * @param pass Буфер для пароля (должен быть не меньше 65 байт)
 * @param pass_len Размер буфера pass
 * @return true если данные успешно загружены, false если данных нет
 */
bool storage_load_wifi(char* ssid, size_t ssid_len, char* pass, size_t pass_len);

/**
 * @brief Сохранение индекса последней выбранной станции
 * @param station_idx Индекс станции (0-14)
 */
void storage_save_station(int station_idx);

/**
 * @brief Загрузка индекса последней выбранной станции
 * @return Индекс станции (0 по умолчанию, если данных нет)
 */
int storage_load_station();

/**
 * @brief Сохранение ID последней выбранной станции (устойчиво к обновлению кاتалога)
 * @param station_id Уникальный ID станции
 */
void storage_save_last_station_id(uint32_t station_id);

/**
 * @brief Загрузка ID последней выбранной станции
 * @return ID станции (0 по умолчанию, если данных нет)
 */
uint32_t storage_load_last_station_id(void);
