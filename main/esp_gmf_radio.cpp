#include "config.h"
#include "storage_manager.h"
#include "wifi_manager.h"
#include "http_stream.h"
#include "audio_player.h"
#include "ui_manager.h"
#include "sd_manager.h"
#include "catalog_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <M5Unified.hpp>
#include <cstring>

#include "audio_output.h"

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

static int g_previous_station = 0;
static uint64_t g_station_change_time_ms = 0;
static bool g_rollback_handled = false;

// ============================================================
// Station switch state machine (А2: fade + drain)
// ============================================================
typedef enum {
    SWITCH_IDLE = 0,
    SWITCH_FADING_OUT,
    SWITCH_DRAINING,
    SWITCH_STARTING,
    SWITCH_WAITING_DATA,
    SWITCH_FADING_IN
} station_switch_phase_t;

static volatile station_switch_phase_t g_switch_phase = SWITCH_IDLE;

const uint32_t FADE_OUT_MS = 250;   // было 120
const uint32_t FADE_IN_MS  = 600;   // было 120
const uint32_t WAIT_DATA_TIMEOUT_MS = 6000; // было 5000, даём время на rebuffer
static constexpr size_t   MIN_RING_FILL_FOR_FADE_IN = 8192;  // 8 KB минимум

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

// ============================================================
// Boot progress screen (до ui_init)
// ============================================================
static bool s_boot_panel_settled = false;

static void boot_draw(int percent, const char* msg)
{
    auto& d = M5.Display;

    // Первый вызов: дать панели выйти из reset и стабилизироваться.
    // Без этого первое fillScreen+drawString может отрисоваться дважды.
    if (!s_boot_panel_settled) {
        d.fillScreen(TFT_BLACK);
        vTaskDelay(pdMS_TO_TICKS(20));
        d.fillScreen(TFT_BLACK);
        s_boot_panel_settled = true;
    } else {
        d.fillScreen(TFT_BLACK);
    }

    // ---- Заголовок (центр по горизонтали, верх по вертикали) ----
    d.setFont(&fonts::Font4);
    d.setTextColor(0x07E0);
    d.setTextDatum(MC_DATUM);            // явно перед каждым draw
    d.drawString("GMF RADIO", 160, 55);

    // ---- Прогресс-бар ----
    int bar_x = 30, bar_y = 110, bar_w = 260, bar_h = 20;
    d.setTextDatum(TL_DATUM);            // переключили datum для примитивов
    d.drawRoundRect(bar_x, bar_y, bar_w, bar_h, 5, TFT_DARKGREY);

    int fill_w = (bar_w * percent) / 100;
    if (fill_w > 0) {
        uint16_t col = (percent < 30) ? TFT_RED
                   : (percent < 70) ? 0xFFE0
                   : 0x07E0;
        d.fillRoundRect(bar_x, bar_y, fill_w, bar_h, 5, col);
    }

    // ---- Процент (центр бара) ----
    char pct_buf[16];
    snprintf(pct_buf, sizeof(pct_buf), "%d%%", percent);
    d.setFont(&fonts::Font2);
    d.setTextColor(TFT_WHITE);
    d.setTextDatum(MC_DATUM);            // явно
    d.drawString(pct_buf, 160, bar_y + bar_h / 2);

    // ---- Сообщение статуса (тот же шрифт, что и версия) ----
    d.setFont(&fonts::Font0);
    d.setTextColor(TFT_LIGHTGRAY);
    d.setTextDatum(MC_DATUM);            // явно
    d.drawString(msg ? msg : "", 160, 155);

    // ---- Подпись чипа ----
    d.setTextColor(TFT_DARKGREY);
    d.setTextDatum(MC_DATUM);            // явно
    d.drawString("ESP32-S3  v1.0", 160, 210);

    d.setTextDatum(TL_DATUM);            // возврат в дефолт на выход
}

/**
 * @brief Точка входа приложения
 */
extern "C" void app_main(void)
{
    // ============================================================
    // А2: Station switch state machine
    // ============================================================
    typedef enum {
        SWITCH_IDLE = 0,
        SWITCH_FADING_OUT,
        SWITCH_DRAINING,
        SWITCH_STARTING,
        SWITCH_WAITING_DATA,
        SWITCH_FADING_IN
    } station_switch_phase_t;

    station_switch_phase_t g_switch_phase = SWITCH_IDLE;
    int g_switch_target_station = -1;
    uint32_t g_switch_start_ms = 0;

    const uint32_t FADE_OUT_MS = 120;
    const uint32_t FADE_IN_MS = 120;
    const uint32_t WAIT_DATA_TIMEOUT_MS = 5000;
    //const size_t MIN_RING_FILL_FOR_FADE_IN = 8192; // 8 KB

    auto switch_force_idle = [&]() {
        if (g_switch_phase != SWITCH_IDLE) {
            ESP_LOGW(TAG, "Forcing station switch to IDLE");
            audio_output_set_fade_factor(256);
            g_switch_phase = SWITCH_IDLE;
            g_switch_target_station = -1;
        }
    };

    ESP_LOGI(TAG, "Starting GMF Radio...");

    // 1. Инициализация хранилища
    boot_draw(0, "Storage...");
    storage_init();

    // 2. Инициализация M5Stack
    auto cfg = M5.config();
    M5.begin(cfg);
    ESP_LOGI(TAG, "M5Stack initialized");

    // === BOOT SCREEN: сразу после M5.begin, до всего остального ===
    boot_draw(5, "SD card...");

    // 2.1 Инициализация SD
    bool sd_ok = sd_manager_init();
    if (sd_ok) {
        sd_manager_self_test();
    } else {
        ESP_LOGW("RADIO", "SD card unavailable.");
    }

    // 2.2 Инициализация Каталога  ← ГЛАВНЫЙ ТОРМОЗ (~10 сек)
    boot_draw(15, "Catalog (SD)...");
    catalog_init();

    boot_draw(40, "Catalog summary...");
    catalog_log_summary();
    catalog_self_test();

    // 2.2.5 Загрузка избранного
    boot_draw(50, "Favorites...");
    if (catalog_load_favorites()) {
        ESP_LOGI(TAG, "Favorites loaded: %u stations",
                 (unsigned)catalog_get_favorites_count());
    } else {
        ESP_LOGI(TAG, "No favorites file found");
    }

    // 2.3 Инициализация UI  ← boot-экран перетирается
    boot_draw(60, "UI...");
    ui_init();

    // 3. Wi-Fi
    boot_draw(70, "Wi-Fi...");
    if (!storage_load_wifi(g_wifi_ssid, sizeof(g_wifi_ssid),
                           g_wifi_pass, sizeof(g_wifi_pass))) {
        strcpy(g_wifi_ssid, DEFAULT_WIFI_SSID);
        strcpy(g_wifi_pass, DEFAULT_WIFI_PASS);
    }
    ui_set_wifi_credentials(g_wifi_ssid, g_wifi_pass);

    // 4. Последняя станция
    boot_draw(75, "Station...");

    // 4. Загрузка последней станции (сначала по ID, потом fallback на индекс)
    uint32_t last_st_id = storage_load_last_station_id();
    ESP_LOGI(TAG, "Station ID loaded: 0x%08lx", (unsigned long)last_st_id);

    size_t found_cat = 0, found_st = 0;
    bool found_by_id = false;

    if (last_st_id > 0) {
        found_by_id = catalog_find_station_by_id(last_st_id, &found_cat, &found_st);
    }

    if (found_by_id) {
        g_selected_station = (int)catalog_global_index(found_cat, found_st);
        ESP_LOGI(TAG, "Restored last station by ID: %d", g_selected_station);
    } else {
        int old_idx = storage_load_station();
        int total = (int)catalog_total_stations();
        if (total <= 0) total = g_total_stations;

        if (old_idx >= 0 && old_idx < total) {
            g_selected_station = old_idx;
            ESP_LOGI(TAG, "Restored last station by old index: %d", g_selected_station);
        } else {
            g_selected_station = 0;
            ESP_LOGI(TAG, "Invalid last station, defaulting to 0");
        }
    }

    // 5. Подключение Wi-Fi
    boot_draw(80, "Connecting Wi-Fi...");
    wifi_known_networks_load();
    if (!wifi_try_auto_connect()) {
        wifi_connect(g_wifi_ssid, g_wifi_pass);
    }

    if (wifi_is_connected() && !g_sntp_started) {
        wifi_start_sntp();
        g_sntp_started = true;
    }

    // 6. Аудио
    boot_draw(88, "Audio...");
    RingbufHandle_t ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (ringbuf == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return;
    }
    audio_init(ringbuf);
    audio_start();
    ui_set_volume(audio_output_get_volume());
    audio_output_set_fade_factor(256);

    // 7. HTTP stream
    boot_draw(95, "Stream...");
    http_stream_init(ringbuf);
    const catalog_station_t* st_init =
        catalog_get_station_by_global_index(g_selected_station);
    http_stream_start(get_station_url_from_catalog(g_selected_station),
                      st_init ? st_init->id : 0);

    boot_draw(100, "Ready!");
    vTaskDelay(pdMS_TO_TICKS(300));  // короткая пауза, чтобы "Ready!" мигнул

    ESP_LOGI(TAG, "All modules initialized, entering main loop");

    // 9. Главный цикл приложения
    while (1) {
        uint32_t now_ms_sw = (uint32_t)(esp_timer_get_time() / 1000);

        // ============================================================
        // А2: исполнение машины переключения станции
        // ============================================================
        if (g_switch_phase != SWITCH_IDLE) {
            switch (g_switch_phase) {

            case SWITCH_FADING_OUT:
                if (audio_output_fade_out_done()) {
                    g_switch_phase = SWITCH_DRAINING;
                    ESP_LOGI(TAG, "Fade out done, draining ring buffer");
                } else if (now_ms_sw - g_switch_start_ms > FADE_OUT_MS + 200) {
                    ESP_LOGW(TAG, "Fade out timeout, forcing silent");
                    audio_output_set_fade_factor(0);
                    g_switch_phase = SWITCH_DRAINING;
                }
                break;

            case SWITCH_DRAINING:
                http_stream_stop();
                http_stream_drain_ringbuf();
                audio_player_force_reset();   // ← НОВОЕ: чистим inbuf декодера

                g_switch_phase = SWITCH_STARTING;
                g_switch_start_ms = now_ms_sw;
                break;

            case SWITCH_STARTING: {
                const catalog_station_t* st =
                    catalog_get_station_by_global_index(g_switch_target_station);

                if (st != nullptr) {
                    storage_save_last_station_id(st->id);
                    http_stream_start(get_station_url_from_catalog(g_switch_target_station), st->id);
                    ESP_LOGI(TAG, "New stream started: %s", st->name);
                } else {
                    ESP_LOGW(TAG, "Station %d not found, aborting switch", g_switch_target_station);
                    switch_force_idle();
                    break;
                }

                g_switch_phase = SWITCH_WAITING_DATA;
                g_switch_start_ms = now_ms_sw;
                break;
            }

            case SWITCH_WAITING_DATA: {
                // Fade in запускаем только когда decode_task вышел из rebuffering
                // и реально начал выводить PCM. Иначе factor застынет/таймаутится.
                if (audio_player_is_outputting()) {
                    audio_output_fade_in(FADE_IN_MS);
                    g_switch_phase = SWITCH_FADING_IN;
                    g_switch_start_ms = now_ms_sw;
                    ESP_LOGI(TAG, "Decoder outputting, fading in");
                }
                else if (now_ms_sw - g_switch_start_ms > WAIT_DATA_TIMEOUT_MS) {
                    // Таймаут: форсируем fade in, пусть играет что есть
                    audio_output_fade_in(FADE_IN_MS);
                    g_switch_phase = SWITCH_FADING_IN;
                    g_switch_start_ms = now_ms_sw;
                    ESP_LOGW(TAG, "Wait output timeout, fading in anyway");
                }
                break;
            }

            case SWITCH_FADING_IN:
                if (!audio_output_is_fading()) {
                    g_switch_phase = SWITCH_IDLE;
                    ESP_LOGI(TAG, "Station switch complete: %d", g_switch_target_station);
                } else if (now_ms_sw - g_switch_start_ms > FADE_IN_MS + 300) {
                    ESP_LOGW(TAG, "Fade in timeout, forcing full volume");
                    audio_output_set_fade_factor(256);
                    g_switch_phase = SWITCH_IDLE;
                }
                break;

            default:
                switch_force_idle();
                break;
            }
        }

        // Передаём фазу в UI каждый цикл
        ui_set_switch_phase((int)g_switch_phase);

        // Обновляем UI
        ui_update();

        // ============================================================
        // Auto-rollback: только когда нет активного переключения
        // ============================================================
        if (!catalog_is_merging() && g_switch_phase == SWITCH_IDLE) {
            const catalog_station_t* current_st =
                catalog_get_station_by_global_index(g_selected_station);

            if (current_st != nullptr &&
                current_st->status == CAT_STATUS_DEAD &&
                !g_rollback_handled) {

                uint64_t now_ms = esp_timer_get_time() / 1000;

                if (now_ms - g_station_change_time_ms < 45000) {
                    const catalog_station_t* prev_st =
                        catalog_get_station_by_global_index(g_previous_station);

                    if (g_previous_station >= 0 &&
                        prev_st != nullptr &&
                        prev_st->status == CAT_STATUS_ALIVE) {

                        ESP_LOGW(TAG,
                                 "Station died quickly, rolling back to healthy previous: %d",
                                 g_previous_station);

                        g_selected_station = g_previous_station;
                        g_station_change_time_ms = now_ms;
                        storage_save_station(g_selected_station);
                        if (prev_st != nullptr) {
                            storage_save_last_station_id(prev_st->id);   // ← НОВОЕ
                        }
                        http_stream_start(get_station_url_from_catalog(g_selected_station), prev_st->id);
                    } else {
                        ESP_LOGW(TAG,
                                 "Station died, but previous station is unhealthy/invalid. Stopping playback.");
                        http_stream_stop();
                    }

                    g_rollback_handled = true;
                }
            }
        }

        // Если Wi-Fi появился позже — запускаем SNTP
        if (!g_sntp_started && wifi_is_connected()) {
            wifi_start_sntp();
            g_sntp_started = true;
        }

        // ============================================================
        // Wi-Fi reconnect logic
        // ============================================================

        // 1. Если запрошен reconnect и поток уже остановлен
        if (g_wifi_pending_reconnect &&
            !http_stream_is_running() &&
            !wifi_is_testing() &&
            !g_wifi_reconnect_in_progress &&
            g_switch_phase == SWITCH_IDLE) {

            ESP_LOGI(TAG, "Stream stopped, performing Wi-Fi reconnect");
            wifi_reconnect(g_wifi_ssid, g_wifi_pass);

            g_wifi_pending_reconnect = false;
            g_wifi_reconnect_in_progress = true;
            g_wifi_link_seen_down = false;
            g_wifi_reconnect_started_ms = esp_timer_get_time() / 1000;
        }

        // 2. Монитор реконнекта
        if (g_wifi_reconnect_in_progress) {
            bool link_up = wifi_is_connected();

            if (!link_up) {
                g_wifi_link_seen_down = true;
            } else if (g_wifi_link_seen_down) {
                ESP_LOGI(TAG, "Wi-Fi reconnect finished. Restarting stream...");

                switch_force_idle();

                g_wifi_reconnect_in_progress = false;
                g_wifi_link_seen_down = false;
                g_wifi_pending_reconnect = false;

                http_stream_restart();
                ui_set_current_mode(MODE_PLAYBACK);
            } else {
                uint64_t now_ms = esp_timer_get_time() / 1000;

                if (now_ms - g_wifi_reconnect_started_ms > 15000) {
                    ESP_LOGW(TAG, "Reconnect watchdog fired. Link stayed up. Resuming stream.");

                    switch_force_idle();

                    g_wifi_reconnect_in_progress = false;
                    g_wifi_link_seen_down = false;
                    g_wifi_pending_reconnect = false;

                    http_stream_restart();
                    ui_set_current_mode(MODE_PLAYBACK);
                }
            }
        }

        // 3. Тест Wi-Fi завершился успешно, рестартим поток
        if (g_restart_stream_after_wifi &&
            wifi_is_connected() &&
            !wifi_is_testing() &&
            !g_wifi_test_active &&
            !g_wifi_reconnect_in_progress &&
            !g_wifi_pending_reconnect &&
            !http_stream_is_running()) {

            ESP_LOGI(TAG, "Restarting stream after successful Wi-Fi test");

            switch_force_idle();

            const catalog_station_t* st_restart =
                catalog_get_station_by_global_index(g_selected_station);

            http_stream_start(
                get_station_url_from_catalog(g_selected_station),
                st_restart ? st_restart->id : 0);

            g_restart_stream_after_wifi = false;
        }

        // ============================================================
        // Передача телеметрии в UI
        // ============================================================
        uint32_t starve, overflow;
        audio_get_stats(&starve, &overflow);

        const char* base_name = get_station_name(g_selected_station);
        char display_name[128];

        const catalog_station_t* current_st_info =
            catalog_get_station_by_global_index(g_selected_station);

        if (current_st_info != nullptr) {
            if (current_st_info->status == CAT_STATUS_SUSPECT) {
                snprintf(display_name, sizeof(display_name), "%s [?]", base_name);
            } else if (current_st_info->status == CAT_STATUS_DEAD) {
                snprintf(display_name, sizeof(display_name), "%s [X]", base_name);
            } else {
                snprintf(display_name, sizeof(display_name), "%s", base_name);
            }
        } else {
            snprintf(display_name, sizeof(display_name), "%s", base_name);
        }

        ui_set_playback_info(
            display_name,
            http_stream_get_title(),
            audio_get_buffer_level(),
            PREFILL_TARGET,
            starve,
            overflow
        );

        ui_set_stream_meta(
            http_stream_get_icy_name(),
            http_stream_get_bitrate(),
            http_stream_get_content_type()
        );

        ui_set_network_info(
            http_stream_get_ring_fill(),
            RINGBUF_SIZE
        );

        // ============================================================
        // Обработка событий UI
        // ============================================================
        ui_event_t event = ui_get_event();

        if (event == UI_EVENT_STATION_CHANGED) {
            int new_idx = ui_get_selected_station();

            const catalog_station_t* target_st =
                catalog_get_station_by_global_index(new_idx);

            bool ignore = false;

            if (target_st == nullptr) {
                ESP_LOGW(TAG, "Station changed to invalid index: %d", new_idx);
                ignore = true;
            }

            // Та же станция и нет активного переключения — игнорируем
            if (new_idx == g_selected_station && g_switch_phase == SWITCH_IDLE) {
                ignore = true;
            }

            // Уже переключаемся на эту же станцию — не перезапускаем
            if (g_switch_phase != SWITCH_IDLE && new_idx == g_switch_target_station) {
                ignore = true;
            }

            if (!ignore) {
                bool was_active = (g_switch_phase != SWITCH_IDLE);

                if (was_active) {
                    ESP_LOGW(TAG,
                             "Switch already in progress (phase=%d), restarting to station %d",
                             (int)g_switch_phase,
                             new_idx);

                    audio_output_set_fade_factor(256);
                    http_stream_stop();
                    http_stream_drain_ringbuf();
                }

                // Previous station запоминаем только если сейчас idle,
                // то есть реально играющая станция — это g_selected_station.
                if (!was_active) {
                    const catalog_station_t* cur_st =
                        catalog_get_station_by_global_index(g_selected_station);

                    if (cur_st != nullptr && cur_st->status == CAT_STATUS_ALIVE) {
                        g_previous_station = g_selected_station;
                    } else {
                        g_previous_station = -1;
                    }
                }

                g_selected_station = new_idx;
                g_station_change_time_ms = esp_timer_get_time() / 1000;
                g_rollback_handled = false;

                storage_save_station(g_selected_station);

                g_switch_target_station = g_selected_station;

                uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

                if (was_active) {
                    // Старый stream уже остановлен и drained.
                    // Сразу переходим к запуску нового.
                    g_switch_phase = SWITCH_STARTING;
                    g_switch_start_ms = now_ms;
                } else {
                    //Normal switch: fade out old audio first.
                    g_switch_phase = SWITCH_FADING_OUT;
                    g_switch_start_ms = now_ms;
                    audio_output_fade_out(FADE_OUT_MS);
                }

                ESP_LOGI(TAG,
                         "Station switch initiated: -> %d (phase=%d)",
                         g_selected_station,
                         (int)g_switch_phase);
            }
        }
        else if (event == UI_EVENT_WIFI_SCAN_REQUESTED) {
            ESP_LOGI(TAG, "Pausing stream and starting Wi-Fi scan...");

            switch_force_idle();
            http_stream_request_stop();

            int found = wifi_scan_networks();

            ESP_LOGI(TAG, "Wi-Fi scan found %d networks", found);

            ui_set_wifi_scan_ready(found > 0);

            if (found <= 0) {
                ESP_LOGI(TAG, "No Wi-Fi networks found. Resuming stream.");
                http_stream_restart();
            }
        }
        else if (event == UI_EVENT_WIFI_CONNECT_REQUESTED) {
            ESP_LOGI(TAG, "Wi-Fi Connect requested. Saving credentials...");

            switch_force_idle();

            char new_ssid[33] = {0};
            char new_pass[65] = {0};

            ui_get_wifi_credentials(new_ssid, sizeof(new_ssid), new_pass, sizeof(new_pass));

            bool same_ssid = (strcmp(new_ssid, g_wifi_ssid) == 0);
            bool same_pass = (strcmp(new_pass, g_wifi_pass) == 0);
            bool link_up = wifi_is_connected();

            ESP_LOGI(TAG, "Wi-Fi Connect check: same_ssid=%d same_pass=%d link_up=%d",
                     (int)same_ssid,
                     (int)same_pass,
                     (int)link_up);

            storage_save_wifi(new_ssid, new_pass);

            strncpy(g_wifi_ssid, new_ssid, sizeof(g_wifi_ssid) - 1);
            g_wifi_ssid[sizeof(g_wifi_ssid) - 1] = '\0';

            strncpy(g_wifi_pass, new_pass, sizeof(g_wifi_pass) - 1);
            g_wifi_pass[sizeof(g_wifi_pass) - 1] = '\0';

            ui_set_wifi_credentials(g_wifi_ssid, g_wifi_pass);

            if (same_ssid && same_pass && link_up) {
                ESP_LOGI(TAG, "Wi-Fi unchanged and link up. Resuming playback immediately.");

                http_stream_restart();
                ui_set_current_mode(MODE_PLAYBACK);

                goto after_wifi_connect;
            }

            ESP_LOGI(TAG, "Wi-Fi changed or link down. Reconnecting...");

            http_stream_request_stop();

            wifi_reconnect(g_wifi_ssid, g_wifi_pass);

            g_wifi_pending_reconnect = true;
            g_wifi_reconnect_in_progress = true;
            g_wifi_link_seen_down = false;
            g_wifi_reconnect_started_ms = esp_timer_get_time() / 1000;

            ui_set_current_mode(MODE_SETTINGS);

        after_wifi_connect:
            ;
        }
        else if (event == UI_EVENT_VOLUME_CHANGED) {
            audio_output_set_volume((uint8_t)ui_get_volume());
        }
        else if (event == UI_EVENT_MUTE_TOGGLED) {
            audio_toggle_mute();
        }
        else if (event == UI_EVENT_WIFI_REVERT) {
            if (g_wifi_test_active) {
                ESP_LOGW(TAG, "Wi-Fi test active, ignoring Revert");
            } else {
                ESP_LOGI(TAG, "Wi-Fi settings reverted to last saved");

                switch_force_idle();
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

                switch_force_idle();

                g_restart_stream_after_wifi = false;
                g_wifi_pending_reconnect = false;
                g_wifi_reconnect_in_progress = false;

                wifi_test_args_t* args = (wifi_test_args_t*)malloc(sizeof(wifi_test_args_t));
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

                switch_force_idle();

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
