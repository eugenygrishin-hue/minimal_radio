#include "config.h"
#include "storage_manager.h"
#include "wifi_manager.h"
#include "http_stream.h"
#include "audio_player.h"
#include "ui_manager.h"
#include "sd_manager.h"
#include "catalog_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <M5Unified.hpp>
#include <cstring>

static const char* TAG = "MAIN";

#include <stdlib.h>

// Глобальное состояние приложения
static int g_selected_station = 0;
static char g_wifi_ssid[33] = DEFAULT_WIFI_SSID;
static char g_wifi_pass[65] = DEFAULT_WIFI_PASS;

static volatile bool g_restart_stream_after_wifi = false;
static volatile bool g_wifi_test_active = false;

static volatile bool g_wifi_pending_reconnect = false;
static volatile bool g_wifi_reconnect_in_progress = false;
static volatile bool g_wifi_link_seen_down = false;
static uint64_t g_wifi_reconnect_started_ms = 0;

static bool g_sntp_started = false;

static portMUX_TYPE g_wifi_test_mux = portMUX_INITIALIZER_UNLOCKED;


static const char* get_station_url_from_catalog(int index) {
    const catalog_station_t* st = catalog_get_station_by_global_index(index);
    if (st != nullptr && st->url[0] != '\0') {
        return st->url;
    }
    // Fallback на старый config.cpp, если в каталоге чего-то нет
    return get_station_url(index);
}

/**
 * @brief Получение названия станции по индексу
 */
static const char* get_station_name(int index) {
    const catalog_station_t* st = catalog_get_station_by_global_index(index);
    if (st != nullptr) {
        return st->name;
    }
    return "Unknown";
}

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_test_args_t;

static void wifi_test_task(void *pvParameters) {
    wifi_test_args_t *args = (wifi_test_args_t *)pvParameters;

    ESP_LOGI(TAG, "Wi-Fi test task: stopping stream before test");
    http_stream_stop();

    bool success = wifi_test_connection(args->ssid, args->pass);

    if (success) {
        ESP_LOGI(TAG, "Wi-Fi test SUCCESS");
        ui_set_wifi_test_result(WIFI_TEST_SUCCESS);
        g_restart_stream_after_wifi = true;
    } else {
        wifi_test_result_t error = wifi_get_last_error();
        ESP_LOGW(TAG, "Wi-Fi test FAILED, error=%d, restoring saved network", error);
        ui_set_wifi_test_result(error);

        wifi_reconnect(g_wifi_ssid, g_wifi_pass);

        g_restart_stream_after_wifi = false;
        g_wifi_reconnect_in_progress = true;
        g_wifi_link_seen_down = true;
    }

    free(args);

    portENTER_CRITICAL(&g_wifi_test_mux);
    g_wifi_test_active = false;
    portEXIT_CRITICAL(&g_wifi_test_mux);

    vTaskDelete(NULL);
}

/**
 * @brief Точка входа приложения
 */
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Starting GMF Radio...");

    // 1. Инициализация хранилища
    storage_init();

    // 2. Инициализация M5Stack
    auto cfg = M5.config();
    M5.begin(cfg);
    ESP_LOGI(TAG, "M5Stack initialized");

    // 2.1 Инициализация SD
    bool sd_ok = sd_manager_init();
    if (sd_ok) {
        sd_manager_self_test();
    } else {
        ESP_LOGW("RADIO", "SD card unavailable. Will use built-in fallback catalog only.");
    }

    // 2.2 Инициализация Каталога
    catalog_init();
    catalog_log_summary();
    catalog_self_test();

    // 2.2.5 Загрузка избранного
    if (catalog_load_favorites()) {
        ESP_LOGI(TAG, "Favorites loaded: %u stations", (unsigned)catalog_get_favorites_count());
    } else {
        ESP_LOGI(TAG, "No favorites file found or SD unavailable");
    }

    // 2.3 Инициализация UI
    ui_init();

    // 3. Загрузка настроек Wi-Fi
    if (!storage_load_wifi(g_wifi_ssid, sizeof(g_wifi_ssid), g_wifi_pass, sizeof(g_wifi_pass))) {
        strcpy(g_wifi_ssid, DEFAULT_WIFI_SSID);
        strcpy(g_wifi_pass, DEFAULT_WIFI_PASS);
    }
    ui_set_wifi_credentials(g_wifi_ssid, g_wifi_pass);

    // 4. Загрузка последней станции (умная логика: сначала по ID, потом fallback на индекс)
    uint32_t last_st_id = storage_load_last_station_id();
    size_t found_cat = 0, found_st = 0;
    bool found_by_id = false;

    if (last_st_id > 0) {
        found_by_id = catalog_find_station_by_id(last_st_id, &found_cat, &found_st);
    }

    if (found_by_id) {
        g_selected_station = (int)catalog_global_index(found_cat, found_st);
        ESP_LOGI(TAG, "Restored last station by ID: %d", g_selected_station);
    } else {
        // Fallback на старый индекс (если ID не найден или это первый запуск со старыми данными)
        int old_idx = storage_load_station();
        int total = (int)catalog_total_stations();
        if (total <= 0) total = g_total_stations; // fallback на config.cpp если каталог пуст

        if (old_idx >= 0 && old_idx < total) {
            g_selected_station = old_idx;
            ESP_LOGI(TAG, "Restored last station by old index: %d", g_selected_station);
        } else {
            g_selected_station = 0;
            ESP_LOGI(TAG, "Invalid last station, defaulting to 0");
        }
    }

    // 5. Подключение к Wi-Fi (с приоритетом авто-подключения)
    wifi_known_networks_load();
    if (!wifi_try_auto_connect()) {
        ESP_LOGI(TAG, "Auto-connect failed or no known networks, falling back to saved credentials");
        wifi_connect(g_wifi_ssid, g_wifi_pass);
    }

    // Сразу пробуем запустить SNTP, если Wi-Fi поднялся
    if (wifi_is_connected() && !g_sntp_started) {
        wifi_start_sntp();
        g_sntp_started = true;
    }

    // 6. Инициализация аудио
    RingbufHandle_t ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (ringbuf == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return;
    }
    audio_init(ringbuf);
    audio_start();

    // 7. Инициализация HTTP стрима
    http_stream_init(ringbuf);
    http_stream_start(get_station_url_from_catalog(g_selected_station));

    // 8.

    ESP_LOGI(TAG, "All modules initialized, entering main loop");

    // 9. Главный цикл приложения
    while (1) {
        // Обновляем UI
        ui_update();

        // Если Wi-Fi появился позже (например, после реконнекта) — запускаем SNTP
        if (!g_sntp_started && wifi_is_connected()) {
            wifi_start_sntp();
            g_sntp_started = true;
        }

        // 1. Если запрошен reconnect и поток уже остановлен — выполняем реконнект
        if (g_wifi_pending_reconnect &&
            !http_stream_is_running() &&
            !wifi_is_testing() &&
            !g_wifi_reconnect_in_progress) {

            ESP_LOGI(TAG, "Stream stopped, performing Wi-Fi reconnect");
            wifi_reconnect(g_wifi_ssid, g_wifi_pass);

            g_wifi_pending_reconnect = false;
            g_wifi_reconnect_in_progress = true;
            g_wifi_link_seen_down = false;
        }

        // 2. Если идёт reconnect — ждём сначала down, потом up, затем рестартим поток
        // ========================================================
        // Wi-Fi reconnect monitor + watchdog
        // ========================================================
        if (g_wifi_reconnect_in_progress) {
            bool link_up = wifi_is_connected();

            if (!link_up) {
                g_wifi_link_seen_down = true;
            } else if (g_wifi_link_seen_down) {
                ESP_LOGI(TAG, "Wi-Fi reconnect finished. Restarting stream...");

                g_wifi_reconnect_in_progress = false;
                g_wifi_link_seen_down        = false;
                g_wifi_pending_reconnect     = false;

                http_stream_restart();
                ui_set_current_mode(MODE_PLAYBACK);
            } else {
                // Watchdog: если reconnect запрошен, но Wi-Fi ни разу не падал —
                // через 15 секунд принудительно возобновляем поток.
                uint64_t now_ms = esp_timer_get_time() / 1000;

                if (now_ms - g_wifi_reconnect_started_ms > 15000) {
                    ESP_LOGW(TAG, "Reconnect watchdog fired. Link stayed up. Resuming stream.");

                    g_wifi_reconnect_in_progress = false;
                    g_wifi_link_seen_down        = false;
                    g_wifi_pending_reconnect     = false;

                    http_stream_restart();
                    ui_set_current_mode(MODE_PLAYBACK);
                }
            }
        }

        // 3. Отдельный случай: тест завершился успехом, Wi-Fi уже подключен
        if (g_restart_stream_after_wifi &&
            wifi_is_connected() &&
            !wifi_is_testing() &&
            !g_wifi_test_active &&
            !g_wifi_reconnect_in_progress &&
            !g_wifi_pending_reconnect &&
            !http_stream_is_running()) {

            ESP_LOGI(TAG, "Restarting stream after successful Wi-Fi test");
            http_stream_start(get_station_url_from_catalog(g_selected_station));
            g_restart_stream_after_wifi = false;
        }

        uint32_t starve, overflow;
        audio_get_stats(&starve, &overflow);

        ui_set_playback_info(
            get_station_name(g_selected_station),
            http_stream_get_title(),
            audio_get_buffer_level(),
            PREFILL_TARGET,
            starve,
            overflow
        );

        ui_set_network_info(
            http_stream_get_ring_fill(),
            RINGBUF_SIZE
        );

        ui_event_t event = ui_get_event();

        if (event == UI_EVENT_STATION_CHANGED) {
            g_selected_station = ui_get_selected_station();

            // Сохраняем и индекс (для обратной совместимости), и ID (для надежности)
            storage_save_station(g_selected_station);

            const catalog_station_t* st = catalog_get_station_by_global_index(g_selected_station);
            if (st != nullptr) {
                storage_save_last_station_id(st->id);
            }

            http_stream_start(get_station_url_from_catalog(g_selected_station));
            ESP_LOGI(TAG, "Station changed to %d: %s", g_selected_station, get_station_name(g_selected_station));
        }
        else if (event == UI_EVENT_WIFI_SCAN_REQUESTED) {
            ESP_LOGI(TAG, "Pausing stream and starting Wi-Fi scan...");

            http_stream_request_stop();

            int found = wifi_scan_networks();

            ESP_LOGI(TAG, "Wi-Fi scan found %d networks", found);

            ui_set_wifi_scan_ready(found > 0);

            // Если сетей не нашли — сразу возобновляем поток,
            // чтобы радио не молчало просто так.
            if (found <= 0) {
                ESP_LOGI(TAG, "No Wi-Fi networks found. Resuming stream.");
                http_stream_restart();
            }
        }
        else if (event == UI_EVENT_WIFI_CONNECT_REQUESTED) {
            ESP_LOGI(TAG, "Wi-Fi Connect requested. Saving credentials...");

            char new_ssid[33] = {0};
            char new_pass[65] = {0};

            ui_get_wifi_credentials(new_ssid, sizeof(new_ssid), new_pass, sizeof(new_pass));

            bool same_ssid = (strcmp(new_ssid, g_wifi_ssid) == 0);
            bool same_pass = (strcmp(new_pass, g_wifi_pass) == 0);
            bool link_up   = wifi_is_connected();

            ESP_LOGI(TAG, "Wi-Fi Connect check: same_ssid=%d same_pass=%d link_up=%d",
                     (int)same_ssid, (int)same_pass, (int)link_up);

            // 1. Всегда сохраняем креды
            storage_save_wifi(new_ssid, new_pass);

            strncpy(g_wifi_ssid, new_ssid, sizeof(g_wifi_ssid) - 1);
            g_wifi_ssid[sizeof(g_wifi_ssid) - 1] = '\0';

            strncpy(g_wifi_pass, new_pass, sizeof(g_wifi_pass) - 1);
            g_wifi_pass[sizeof(g_wifi_pass) - 1] = '\0';

            ui_set_wifi_credentials(g_wifi_ssid, g_wifi_pass);

            // 2. Если креды те же и Wi-Fi уже подключён — сразу возобновляем поток.
            if (same_ssid && same_pass && link_up) {
                ESP_LOGI(TAG, "Wi-Fi unchanged and link up. Resuming playback immediately.");

                http_stream_restart();

                ui_set_current_mode(MODE_PLAYBACK);

                // ВАЖНО: выходим из обработчика, чтобы не провалиться в reconnect.
                goto after_wifi_connect;
            }

            // 3. Креды изменились или link down — полноценный reconnect.
            ESP_LOGI(TAG, "Wi-Fi changed or link down. Reconnecting...");

            http_stream_request_stop();

            wifi_reconnect(g_wifi_ssid, g_wifi_pass);

            g_wifi_pending_reconnect    = true;
            g_wifi_reconnect_in_progress = true;
            g_wifi_link_seen_down        = false;
            g_wifi_reconnect_started_ms  = esp_timer_get_time() / 1000;

            ui_set_current_mode(MODE_SETTINGS);

        after_wifi_connect:
            ;
        }
        else if (event == UI_EVENT_VOLUME_CHANGED) {
            audio_set_volume(ui_get_volume());
        }
        else if (event == UI_EVENT_MUTE_TOGGLED) {
            audio_toggle_mute();
        }
        else if (event == UI_EVENT_WIFI_REVERT) {
            if (g_wifi_test_active) {
                ESP_LOGW(TAG, "Wi-Fi test active, ignoring Revert");
            } else {
                ESP_LOGI(TAG, "Wi-Fi settings reverted to last saved");

                http_stream_request_stop();
                g_wifi_pending_reconnect = true;
                g_restart_stream_after_wifi = false;
                g_wifi_reconnect_in_progress = false;
            }
        }
        else if (event == UI_EVENT_WIFI_SAVE) {
            char new_ssid[33], new_pass[65];
            ui_get_wifi_credentials(new_ssid, sizeof(new_ssid), new_pass, sizeof(new_pass));

            storage_save_wifi(new_ssid, new_pass);
            ui_set_wifi_credentials(new_ssid, new_pass);

            strcpy(g_wifi_ssid, new_ssid);
            strcpy(g_wifi_pass, new_pass);

            ESP_LOGI(TAG, "Wi-Fi settings saved to NVS (no reconnect)");
        }
        else if (event == UI_EVENT_WIFI_TEST) {
            bool start_test = false;

            portENTER_CRITICAL(&g_wifi_test_mux);
            if (!g_wifi_test_active) {
                g_wifi_test_active = true;
                start_test = true;
            }
            portEXIT_CRITICAL(&g_wifi_test_mux);

            if (!start_test) {
                ESP_LOGW(TAG, "Wi-Fi test already active, ignoring repeated Test event");
                ui_set_wifi_test_result(WIFI_TEST_IN_PROGRESS);
            } else {
                ESP_LOGI(TAG, "Starting asynchronous Wi-Fi test");

                g_restart_stream_after_wifi = false;
                g_wifi_pending_reconnect = false;
                g_wifi_reconnect_in_progress = false;

                wifi_test_args_t *args = (wifi_test_args_t *)malloc(sizeof(wifi_test_args_t));
                if (args != NULL) {
                    ui_get_wifi_credentials(args->ssid, sizeof(args->ssid), args->pass, sizeof(args->pass));

                    ui_set_wifi_test_result(WIFI_TEST_IN_PROGRESS);

                    BaseType_t created = xTaskCreatePinnedToCore(
                        wifi_test_task,
                        "wifi_test",
                        8192,
                        args,
                        6,
                        NULL,
                        1
                    );

                    if (created != pdPASS) {
                        ESP_LOGE(TAG, "Failed to create Wi-Fi test task");
                        ui_set_wifi_test_result(WIFI_TEST_FAILED_OTHER);

                        portENTER_CRITICAL(&g_wifi_test_mux);
                        g_wifi_test_active = false;
                        portEXIT_CRITICAL(&g_wifi_test_mux);

                        free(args);
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to allocate Wi-Fi test args");
                    ui_set_wifi_test_result(WIFI_TEST_FAILED_OTHER);

                    portENTER_CRITICAL(&g_wifi_test_mux);
                    g_wifi_test_active = false;
                    portEXIT_CRITICAL(&g_wifi_test_mux);
                }
            }
        }
        else if (event == UI_EVENT_WIFI_CONNECT) {
            if (g_wifi_test_active) {
                ESP_LOGW(TAG, "Wi-Fi test active, ignoring Connect");
            } else {
                char new_ssid[33], new_pass[65];
                ui_get_wifi_credentials(new_ssid, sizeof(new_ssid), new_pass, sizeof(new_pass));

                strcpy(g_wifi_ssid, new_ssid);
                strcpy(g_wifi_pass, new_pass);

                storage_save_wifi(g_wifi_ssid, g_wifi_pass);
                ui_set_wifi_credentials(g_wifi_ssid, g_wifi_pass);

                ESP_LOGI(TAG, "Wi-Fi settings saved, preparing reconnect to %s", g_wifi_ssid);

                http_stream_request_stop();
                g_wifi_pending_reconnect = true;
                g_restart_stream_after_wifi = false;
                g_wifi_reconnect_in_progress = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
     }
}
