#include "ui_manager.h"
#include "config.h"
#include "catalog_manager.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <M5Unified.hpp>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <new>

static const char* TAG = "UI";

// ============================================================
// Константы клавиатуры
// ============================================================
static const int KB_START_X = 10;
static const int KB_START_Y = 98;
static const int KB_KEY_W = 30;
static const int KB_KEY_H = 26;
static const int KB_GAP = 2;
static const int KB_SPECIAL_W = 100;

// ============================================================
// Константы кнопок Wi-Fi
// ============================================================
static const int WIFI_BTN_W = 145;
static const int WIFI_BTN_H = 35;
static const int WIFI_BTN_GAP = 10;
static const int WIFI_BTN_X = 12;
static const int WIFI_BTN_Y = 150;

// ============================================================
// Энкодер: анти-дребезг через накопитель
// ============================================================
static const int ENCODER_THRESHOLD = 3;
static int32_t s_encoder_accumulator = 0;

// ============================================================
// Сон экрана
// ============================================================
static const int      DISPLAY_BRIGHTNESS      = 200;
static const uint32_t DISPLAY_IDLE_TIMEOUT_MS = 60000;

// ============================================================
// Состояние UI
// ============================================================
static int s_selected_category = 0;
static int s_ui_station_idx = 0;
static int s_current_volume = 75;
static bool s_is_muted = false;

static int s_fav_station_idx = 0;
static bool enc_long_press_handled = false;

// Wi-Fi настройки
static char s_wifi_ssid[33] = DEFAULT_WIFI_SSID;
static char s_wifi_pass[65] = DEFAULT_WIFI_PASS;
static char s_wifi_ssid_original[33] = DEFAULT_WIFI_SSID;
static char s_wifi_pass_original[65] = DEFAULT_WIFI_PASS;
static bool s_keyboard_active = false;
static bool s_editing_ssid = true;
static bool s_shift_active = false;
static char s_input_buffer[65] = {0};
static bool s_cursor_blink = false;
static uint32_t s_cursor_blink_counter = 0;
static bool s_has_unsaved_changes = false;
static wifi_test_result_t s_wifi_test_result = WIFI_TEST_NOT_STARTED;

static int s_wifi_focus = 0; // 0=SSID поле, 1=PASS поле, 2=Кнопка Scan, 3=Кнопка Connect
static bool s_wifi_show_scan_results = false;
static int s_wifi_scan_selected_idx = -1; // -1 = ничего не выбрано, >=0 = индекс в списке

// Информация для экрана воспроизведения
static char s_station_name[64] = "Unknown";
static char s_track_title[128] = "Connecting...";
static uint32_t s_buffer_level = 0;
static uint32_t s_buffer_target = 115 * 1024;
static uint32_t s_starve_count = 0;
static uint32_t s_overflow_count = 0;

// ============================================================
// Сетевая телеметрия
// ============================================================
static uint32_t s_net_fill = 0;
static uint32_t s_net_capacity = RINGBUF_SIZE;

static uint32_t s_net_history[64] = {0};
static uint8_t  s_net_history_pos = 0;
static uint8_t  s_net_history_count = 0;

// События
static volatile ui_event_t s_pending_event = UI_EVENT_NONE;
static int s_new_station_idx = 0;
static int s_settings_idx = 0;
static int s_new_volume = 0;

static ui_mode_t s_current_mode = MODE_PLAYBACK;

// Клавиатура
static const char* kb_rows[4] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL_",
    "ZXCVBNM-."
};

typedef enum {
    KB_TARGET_NONE = 0,
    KB_TARGET_SSID,
    KB_TARGET_PASS
} kb_target_t;

static kb_target_t s_keyboard_target = KB_TARGET_NONE;

// ============================================================
// Спрайт для двойной буферизации
// ============================================================
static m5gfx::LGFX_Sprite* s_ui_sprite = nullptr;
static bool s_use_sprite = false;

// Если нужно временно отключить спрайт — поставь true
static const bool SPRITE_DEBUG_FORCE_DIRECT = false;

// ============================================================
// Чтение HMI
// ============================================================
static int32_t read_hmi_register_32(uint8_t reg) {
    uint8_t buf[4] = {0};
    if (!M5.In_I2C.readRegister(HMI_I2C_ADDR, reg, buf, 4, 100000)) return 0;
    return (int32_t)(buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24));
}

static bool read_hmi_register_8(uint8_t reg) {
    uint8_t val = 0;
    if (!M5.In_I2C.readRegister(HMI_I2C_ADDR, reg, &val, 1, 100000)) return false;
    return val == 0;
}

// ============================================================
// Отрисовка клавиатуры (template)
// ============================================================
template <typename T>
static void draw_keyboard_impl(T& lcd) {
    lcd.setFont(&fonts::Font2);

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 10; col++) {
            char c = kb_rows[row][col];
            if (!s_shift_active && c >= 'A' && c <= 'Z') c = c + 32;

            int x = KB_START_X + col * (KB_KEY_W + KB_GAP);
            int y = KB_START_Y + row * (KB_KEY_H + KB_GAP);

            lcd.fillRoundRect(x, y, KB_KEY_W, KB_KEY_H, 4, TFT_DARKGREY);
            lcd.setTextColor(TFT_WHITE);
            lcd.setTextDatum(MC_DATUM);

            char key_str[2] = {c, '\0'};
            lcd.drawString(key_str, x + KB_KEY_W / 2, y + KB_KEY_H / 2 + 1);
        }
    }

    int y5 = KB_START_Y + 4 * (KB_KEY_H + KB_GAP);

    // Shift
    lcd.fillRoundRect(KB_START_X, y5, KB_SPECIAL_W, KB_KEY_H, 4,
                      s_shift_active ? 0x07E0 : TFT_DARKGREY);
    lcd.setTextColor(s_shift_active ? TFT_BLACK : TFT_WHITE);
    lcd.setTextDatum(MC_DATUM);
    lcd.drawString("ABC", KB_START_X + KB_SPECIAL_W / 2, y5 + KB_KEY_H / 2 + 1);

    // Space
    lcd.fillRoundRect(KB_START_X + KB_SPECIAL_W + KB_GAP, y5,
                      KB_SPECIAL_W, KB_KEY_H, 4, TFT_DARKGREY);
    lcd.setTextColor(TFT_WHITE);
    lcd.drawString("___",
                   KB_START_X + KB_SPECIAL_W + KB_GAP + KB_SPECIAL_W / 2,
                   y5 + KB_KEY_H / 2 + 1);

    // Backspace
    lcd.fillRoundRect(KB_START_X + 2 * (KB_SPECIAL_W + KB_GAP), y5,
                      KB_SPECIAL_W, KB_KEY_H, 4, TFT_DARKGREY);
    lcd.setTextColor(TFT_WHITE);
    lcd.drawString("<-",
                   KB_START_X + 2 * (KB_SPECIAL_W + KB_GAP) + KB_SPECIAL_W / 2,
                   y5 + KB_KEY_H / 2 + 1);

    lcd.setTextDatum(TL_DATUM);
}

// ============================================================
// Отрисовка кнопок Wi-Fi (template)
// ============================================================
template <typename T>
static void draw_wifi_action_buttons_impl(T& lcd) {
    // Статус WiFi соединения
    lcd.setFont(&fonts::efontCN_14);
    lcd.setTextDatum(TL_DATUM);

    if (wifi_is_connected()) {
        lcd.setTextColor(0x07E0);
        lcd.drawString("WiFi: подключен", 10, 102);
    } else {
        lcd.setTextColor(TFT_RED);
        lcd.drawString("WiFi: не подключен", 10, 102);
    }

    // Результат теста
    if (s_wifi_test_result == WIFI_TEST_IN_PROGRESS) {
        lcd.setTextColor(TFT_YELLOW);
        lcd.drawString("Тест: выполняется...", 10, 122);
    } else if (s_wifi_test_result == WIFI_TEST_SUCCESS) {
        lcd.setTextColor(0x07E0);
        lcd.drawString("Тест: успешно", 10, 122);
    } else if (s_wifi_test_result == WIFI_TEST_FAILED_AUTH) {
        lcd.setTextColor(TFT_RED);
        lcd.drawString("Тест: неверный пароль", 10, 122);
    } else if (s_wifi_test_result == WIFI_TEST_FAILED_NOT_FOUND) {
        lcd.setTextColor(TFT_RED);
        lcd.drawString("Тест: сеть не найдена", 10, 122);
    } else if (s_wifi_test_result == WIFI_TEST_FAILED_TIMEOUT) {
        lcd.setTextColor(TFT_RED);
        lcd.drawString("Тест: таймаут", 10, 122);
    } else if (s_wifi_test_result == WIFI_TEST_FAILED_OTHER) {
        lcd.setTextColor(TFT_RED);
        lcd.drawString("Тест: ошибка подключения", 10, 122);
    }

    // Кнопки действий
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(MC_DATUM);

    // Revert
    lcd.fillRoundRect(WIFI_BTN_X, WIFI_BTN_Y, WIFI_BTN_W, WIFI_BTN_H, 5, TFT_RED);
    lcd.setTextColor(TFT_WHITE);
    lcd.drawString("Revert",
                   WIFI_BTN_X + WIFI_BTN_W / 2,
                   WIFI_BTN_Y + WIFI_BTN_H / 2);

    // Save
    lcd.fillRoundRect(WIFI_BTN_X + WIFI_BTN_W + WIFI_BTN_GAP,
                      WIFI_BTN_Y,
                      WIFI_BTN_W,
                      WIFI_BTN_H,
                      5,
                      TFT_BLUE);
    lcd.setTextColor(TFT_WHITE);
    lcd.drawString("Save",
                   WIFI_BTN_X + WIFI_BTN_W + WIFI_BTN_GAP + WIFI_BTN_W / 2,
                   WIFI_BTN_Y + WIFI_BTN_H / 2);

    // Test
    lcd.fillRoundRect(WIFI_BTN_X,
                      WIFI_BTN_Y + WIFI_BTN_H + WIFI_BTN_GAP,
                      WIFI_BTN_W,
                      WIFI_BTN_H,
                      5,
                      TFT_ORANGE);
    lcd.setTextColor(TFT_BLACK);
    lcd.drawString("Test",
                   WIFI_BTN_X + WIFI_BTN_W / 2,
                   WIFI_BTN_Y + WIFI_BTN_H + WIFI_BTN_GAP + WIFI_BTN_H / 2);

    // Connect
    lcd.fillRoundRect(WIFI_BTN_X + WIFI_BTN_W + WIFI_BTN_GAP,
                      WIFI_BTN_Y + WIFI_BTN_H + WIFI_BTN_GAP,
                      WIFI_BTN_W,
                      WIFI_BTN_H,
                      5,
                      0x07E0);
    lcd.setTextColor(TFT_BLACK);
    lcd.drawString("Connect",
                   WIFI_BTN_X + WIFI_BTN_W + WIFI_BTN_GAP + WIFI_BTN_W / 2,
                   WIFI_BTN_Y + WIFI_BTN_H + WIFI_BTN_GAP + WIFI_BTN_H / 2);

    lcd.setTextDatum(TL_DATUM);
}

// ============================================================
// Статус-бар: WiFi / время / батарея
// ============================================================
template <typename T>
static void draw_status_bar_impl(T& lcd) {
    lcd.setFont(&fonts::Font0);

    // ---------- WiFi RSSI слева ----------
    lcd.setTextDatum(TL_DATUM);

    if (wifi_is_connected()) {
        int rssi = wifi_get_rssi();
        char buf[24];
        snprintf(buf, sizeof(buf), "WiFi %ddBm", rssi);

        if (rssi >= -50) {
            lcd.setTextColor(0x07E0);       // отличный сигнал
        } else if (rssi >= -65) {
            lcd.setTextColor(TFT_YELLOW);   // средний
        } else {
            lcd.setTextColor(TFT_RED);      // слабый
        }

        lcd.drawString(buf, 2, 4);
    } else {
        lcd.setTextColor(TFT_RED);
        lcd.drawString("WiFi --", 2, 4);
    }

    // ---------- Время по центру ----------
    // ВАЖНО: при MC_DATUM y — это центр текста, поэтому ставим y=8
    lcd.setTextDatum(MC_DATUM);

    time_t now = time(nullptr);

    // Время считается синхронизированным, если timestamp разумный.
    // 1700000000 ~= ноябрь 2023.
    if (now > 1700000000) {
        struct tm tm_info;
        localtime_r(&now, &tm_info);

        char buf[24];
        strftime(buf, sizeof(buf), "%H:%M %d.%m", &tm_info);

        lcd.setTextColor(TFT_WHITE);
        lcd.drawString(buf, 160, 8);
    } else {
        lcd.setTextColor(TFT_LIGHTGRAY);
        lcd.drawString("--:-- --.--", 160, 8);
    }

    // ---------- Батарея / USB справа ----------
    bool charging = M5.Power.isCharging();
    int bat = (int)M5.Power.getBatteryLevel();
    int mv = (int)M5.Power.getBatteryVoltage();

    // Диагностика в лог каждые ~5 секунд
    static uint32_t power_dbg_counter = 0;
    if (++power_dbg_counter >= 100) {
        power_dbg_counter = 0;
        ESP_LOGI(TAG, "PWR: charging=%d level=%d voltage=%dmV",
                 (int)charging, bat, mv);
    }

    lcd.setTextDatum(TR_DATUM);

    if (charging) {
        lcd.setTextColor(TFT_YELLOW);
        lcd.drawString("USB", 318, 4);
    }
    else if (mv >= 2800 && mv <= 4600) {
        // Li-Po/Li-Ion примерная карта:
        // 3.00V ~ 0%
        // 4.20V ~ 100%
        int pct = ((mv - 3000) * 100) / 1200;

        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;

        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", pct);

        if (pct > 50) {
            lcd.setTextColor(0x07E0);
        } else if (pct > 20) {
            lcd.setTextColor(TFT_YELLOW);
        } else {
            lcd.setTextColor(TFT_RED);
        }

        lcd.drawString(buf, 318, 4);
    }
    else if (bat >= 0 && bat < 100) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", bat);

        if (bat > 50) {
            lcd.setTextColor(0x07E0);
        } else if (bat > 20) {
            lcd.setTextColor(TFT_YELLOW);
        } else {
            lcd.setTextColor(TFT_RED);
        }

        lcd.drawString(buf, 318, 4);
    }
    else {
        lcd.setTextColor(TFT_LIGHTGRAY);
        lcd.drawString("--%", 318, 4);
    }

    lcd.setTextDatum(TL_DATUM);
}

// ============================================================
// Мини-график сетевого буфера
// ============================================================
template <typename T>
static void draw_net_sparkline_impl(T& lcd, int x, int y, int w, int h) {
    if (s_net_history_count == 0 || s_net_capacity == 0) {
        return;
    }

    lcd.drawRect(x, y, w, h, TFT_DARKGREY);

    const int total_slots = 64;
    int step = w / total_slots;

    if (step < 1) {
        step = 1;
    }

    int samples = s_net_history_count;
    int start = (s_net_history_pos + total_slots - samples) % total_slots;

    for (int i = 0; i < samples; i++) {
        int idx = (start + i) % total_slots;
        uint32_t val = s_net_history[idx];

        int bar_h = (int)(((uint64_t)val * (uint64_t)(h - 2)) / (uint64_t)s_net_capacity);

        if (bar_h < 1 && val > 0) {
            bar_h = 1;
        }

        int bx = x + 1 + i * step;
        int bw = step - 1;

        if (bw < 1) {
            bw = 1;
        }

        if (bx + bw > x + w - 1) {
            break;
        }

        uint16_t color;

        if (val == 0) {
            color = TFT_RED;
        } else if (((uint64_t)val * 100ULL) / (uint64_t)s_net_capacity < 10ULL) {
            color = TFT_YELLOW;
        } else {
            color = 0x07E0;
        }

        lcd.fillRect(bx, y + h - 1 - bar_h, bw, bar_h, color);
    }
}

// ============================================================
// Отрисовка UI (template)
// ============================================================
template <typename T>
static void draw_ui_impl(T& lcd) {
    lcd.fillScreen(TFT_BLACK);

    // Верхний статус-бар
    draw_status_bar_impl(lcd);


    const char* modes[] = {"PLAY", "FAV", "CAT", "STA", "SET"};
    int menu_y = 18;
    int menu_h = 22;
    for (int i = 0; i < 5; i++) { // Теперь всегда 5 вкладок
        if (i == (int)s_current_mode) {
            lcd.fillRect(i * 64, menu_y, 64, menu_h, 0x07E0);
            lcd.setTextColor(TFT_BLACK);
        } else {
            lcd.fillRect(i * 64, menu_y, 64, menu_h, TFT_BLACK);
            lcd.setTextColor(TFT_LIGHTGRAY);
        }
        lcd.setFont(&fonts::Font0);
        lcd.setTextDatum(MC_DATUM);
        lcd.drawString(modes[i], i * 64 + 32, menu_y + menu_h / 2 + 1);
    }

    lcd.setTextDatum(TL_DATUM);

    if (s_is_muted) {
        lcd.setFont(&fonts::Font0);
        lcd.setTextColor(TFT_RED);
        lcd.setTextDatum(TR_DATUM);
        lcd.drawString("MUTE", 318, menu_y + 4);
        lcd.setTextDatum(TL_DATUM);
    }

    // ========================================================
    // Wi-Fi режим
    // ========================================================
    // ========================================================
    // Wi-Fi Settings
    // ========================================================
    if (s_current_mode == MODE_WIFI) {
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);
        lcd.drawString("Настройка Wi-Fi", 10, 48);

        if (s_wifi_show_scan_results) {
            // --- РЕЖИМ ОТОБРАЖЕНИЯ РЕЗУЛЬТАТОВ СКАНИРОВАНИЯ ---
            lcd.setTextColor(TFT_WHITE);
            lcd.drawString("Выберите сеть:", 10, 68);
            int count = wifi_scan_get_count();
            int start_idx = s_wifi_scan_selected_idx;
            if (start_idx > 3) start_idx = s_wifi_scan_selected_idx - 3;

            int y = 88;
            for (int i = start_idx; i < count && i < start_idx + 4; i++) {
                const wifi_scan_record_t* rec = wifi_scan_get_record(i);
                if (!rec) continue;

                if (i == s_wifi_scan_selected_idx) {
                    lcd.setTextColor(TFT_BLACK);
                    lcd.fillRoundRect(5, y - 5, 310, 24, 4, 0x07E0);
                } else {
                    lcd.setTextColor(TFT_WHITE);
                }

                char line_buf[64];
                // Формируем строку: "SSID (RSSI)"
                snprintf(line_buf, sizeof(line_buf), "%s (%d)", rec->ssid, rec->rssi);
                lcd.drawString(line_buf, 15, y);
                y += 28;
            }
            lcd.setTextColor(TFT_LIGHTGRAY);
            lcd.drawString("Нажми для выбора или A для отмены", 10, 210);
        }
        else {
            // --- ОБЫЧНЫЙ РЕЖИМ ВВОДА ---
            int y = 68;
            const char* labels[] = {"SSID:", "Password:"};
            const char* values[] = {s_wifi_ssid, s_wifi_pass};

            for (int i = 0; i < 2; i++) {
                bool is_focused = (s_wifi_focus == i);
                lcd.setFont(&fonts::efontCN_14);
                if (is_focused) {
                    lcd.setTextColor(TFT_BLACK);
                    lcd.fillRoundRect(5, y - 5, 310, 26, 4, 0x07E0);
                } else {
                    lcd.setTextColor(TFT_WHITE);
                }

                char line_buf[64];
                if (i == 1) { // Маскируем пароль
                    snprintf(line_buf, sizeof(line_buf), "%s %s", labels[i], "********");
                } else {
                    snprintf(line_buf, sizeof(line_buf), "%s %s", labels[i], values[i]);
                }
                lcd.drawString(line_buf, 15, y);
                y += 32;
            }

            // Кнопки
            // Кнопки Scan и Connect
            y = 140;
            const char* btn_labels[] = {"Scan", "Connect"}; // Сделали надписи короче, чтобы точно влезли
            for (int i = 0; i < 2; i++) {
                int focus_idx = i + 2; // 2 и 3
                bool is_focused = (s_wifi_focus == focus_idx);
                lcd.setFont(&fonts::efontCN_14);

                int btn_x = (i == 0) ? 5 : 165;

                if (is_focused) {
                    lcd.setTextColor(TFT_BLACK);
                    lcd.fillRoundRect(btn_x, y - 5, 150, 26, 4, 0x07E0); // Зеленая заливка при фокусе
                } else {
                    lcd.setTextColor(0x07E0); // Зеленый текст
                    lcd.fillRoundRect(btn_x, y - 5, 150, 26, 4, TFT_BLACK); // ⚠️ ЯВНАЯ ОЧИСТКА ФОНА ЧЕРНЫМ
                    lcd.drawRoundRect(btn_x, y - 5, 150, 26, 4, 0x07E0);    // Зеленая рамка
                }

                // Центрируем текст внутри кнопки
                lcd.setTextDatum(MC_DATUM);
                lcd.drawString(btn_labels[i], btn_x + 75, y + 8);
                lcd.setTextDatum(TL_DATUM); // Возвращаем выравнивание по умолчанию
            }

            if (s_keyboard_active) {
                // Здесь должен быть твой существующий код отрисовки клавиатуры
                // draw_keyboard_impl(lcd);
            }
        }
    }

    // ========================================================
    // Favorites (Реальный список)
    // ========================================================
    if (s_current_mode == MODE_FAVORITES) {
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);
        lcd.drawString("Избранное", 10, 48);

        int visible_items = 6;
        int fav_count = (int)catalog_get_favorites_count();
        int start_idx = s_fav_station_idx;
        if (start_idx > 3) {
            start_idx = s_fav_station_idx - 3;
        }

        int y = 68;
        if (fav_count == 0) {
            lcd.setTextColor(TFT_LIGHTGRAY);
            lcd.setFont(&fonts::efontCN_14);
            lcd.drawString("Список пуст", 15, y);
            lcd.drawString("Долгое нажатие в STA", 15, y + 24);
            lcd.drawString("добавит станцию", 15, y + 48);
        } else {
            for (int i = start_idx; i < fav_count && i < start_idx + visible_items; i++) {
                int global_idx = catalog_get_favorite_global_index(i);
                const catalog_station_t* st = catalog_get_station_by_global_index(global_idx);
                if (st == nullptr) continue; // Станция могла быть удалена из каталога

                if (i == s_fav_station_idx) {
                    lcd.setTextColor(TFT_BLACK);
                    lcd.fillRoundRect(5, y - 5, 310, 24, 4, 0x07E0);
                } else {
                    lcd.setTextColor(TFT_WHITE);
                }
                lcd.setFont(&fonts::efontCN_16);
                lcd.drawString(st->name, 15, y);
                y += 28;
            }
        }
    }
    // ========================================================
    // Settings Menu
    // ========================================================
    else if (s_current_mode == MODE_SETTINGS) {
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);
        lcd.drawString("Настройки", 10, 48);

        const char* items[] = {"1. Wi-Fi Setup", "2. Catalog", "3. About", "4. Back"};
        int count = 4;
        int y = 68;

        for (int i = 0; i < count; i++) {
            if (i == s_settings_idx) {
                lcd.setTextColor(TFT_BLACK);
                lcd.fillRoundRect(5, y - 5, 310, 26, 4, 0x07E0);
            } else {
                lcd.setTextColor(TFT_WHITE);
            }
            lcd.drawString(items[i], 15, y);
            y += 32;
        }
    }
    // ========================================================
    // About (Заглушка, полноценный экран сделаем в Step 4.2)
    // ========================================================
    else if (s_current_mode == MODE_ABOUT) {
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);
        lcd.drawString("О программе", 10, 48);
        lcd.setTextColor(TFT_WHITE);
        lcd.drawString("M5Core S3 Radio", 15, 80);
        lcd.drawString("FW: v1.0.0 (Sprint 3)", 15, 110);
        lcd.drawString("Stations: 450", 15, 140);
        lcd.drawString("Нажми для выхода", 15, 200);
    }
    // ========================================================
    // Catalog (Заглушка, обновление из GitHub будет в Step 4.1)
    // ========================================================
    else if (s_current_mode == MODE_CATALOG) {
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);
        lcd.drawString("Каталог", 10, 48);
        lcd.setTextColor(TFT_WHITE);
        lcd.drawString("Обновление из GitHub", 15, 80);
        lcd.drawString("будет в Step 4.1", 15, 110);
        lcd.drawString("Нажми для выхода", 15, 200);
    }

    // ========================================================
    // Playback
    // ========================================================
    if (s_current_mode == MODE_PLAYBACK) {
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);
        const catalog_category_t* play_cat = catalog_get_category(s_selected_category);
        lcd.drawString(play_cat ? play_cat->name : "---", 10, 48);

        lcd.setFont(&fonts::efontCN_16);
        lcd.setTextColor(TFT_WHITE);
        lcd.drawString(s_station_name, 10, 68);

        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(TFT_LIGHTGRAY);
        lcd.drawString(s_track_title, 10, 98);

        lcd.setFont(&fonts::Font0);
        lcd.setTextColor(0x07E0);
        lcd.drawString("Buffer:", 10, 128);

        int bar_w = 280;
        int bar_h = 16;
        lcd.drawRoundRect(10, 146, bar_w, bar_h, 4, TFT_WHITE);

        int fill_pct = 0;
        if (s_buffer_target > 0) {
            fill_pct = (int)((s_buffer_level * 100) / s_buffer_target);
        }

        if (fill_pct > 100) fill_pct = 100;

        int fill_w = (fill_pct * bar_w) / 100;
        if (fill_w > 0) {
            lcd.fillRoundRect(10, 146, fill_w, bar_h, 4, 0x07E0);
        }

        char buf[48];

        lcd.setTextColor(TFT_WHITE);
        snprintf(buf, sizeof(buf), "Buf: %d%% (%lu KB)",
                 fill_pct,
                 (unsigned long)(s_buffer_level / 1024));
        lcd.drawString(buf, 10, 168);

        int net_pct = 0;
        if (s_net_capacity > 0) {
            net_pct = (int)((s_net_fill * 100) / s_net_capacity);
        }

        if (net_pct > 100) net_pct = 100;

        if (net_pct == 0) {
            lcd.setTextColor(TFT_RED);
        } else if (net_pct < 10) {
            lcd.setTextColor(TFT_YELLOW);
        } else {
            lcd.setTextColor(0x07E0);
        }

        snprintf(buf, sizeof(buf), "Net: %d%% (%lu KB)",
                 net_pct,
                 (unsigned long)(s_net_fill / 1024));
        lcd.drawString(buf, 10, 186);

        // Мини-график сетевого буфера
        draw_net_sparkline_impl(lcd, 10, 204, 300, 18);

        lcd.setTextColor(TFT_RED);
        snprintf(buf, sizeof(buf), "Starve: %lu | Overflow: %lu",
                 (unsigned long)s_starve_count,
                 (unsigned long)s_overflow_count);
        lcd.drawString(buf, 10, 226);
    }

    // ========================================================
    // Categories (со скроллингом)
    // ========================================================
    else if (s_current_mode == MODE_CATEGORIES) {
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);
        lcd.drawString("Выберите категорию:", 10, 48);

        int visible_items = 5; // На экране помещается 5 строк (68, 100, 132, 164, 196)
        int cat_count = (int)catalog_category_count();

        // Вычисляем начальный индекс для отрисовки, чтобы выбранный элемент был виден
        int start_idx = s_selected_category;
        if (start_idx > 2) {
            start_idx = s_selected_category - 2; // Держим выбранную строку примерно 3-й сверху
        }

        int y = 68;
        for (int i = start_idx; i < cat_count && i < start_idx + visible_items; i++) {
            const catalog_category_t* c = catalog_get_category(i);
            if (c == nullptr) continue;

            if (i == s_selected_category) {
                lcd.setTextColor(TFT_BLACK);
                lcd.fillRoundRect(5, y - 5, 310, 26, 4, 0x07E0);
            } else {
                lcd.setTextColor(TFT_WHITE);
            }
            lcd.drawString(c->name, 15, y);
            y += 32;
        }
    }

    // ========================================================
    // Stations (со скроллингом и индикацией избранного)
    // ========================================================
    else if (s_current_mode == MODE_STATIONS) {
        const catalog_category_t* cur_cat = catalog_get_category(s_selected_category);
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);
        lcd.drawString(cur_cat ? cur_cat->name : "---", 10, 48);

        int visible_items = 6;
        int count = cur_cat ? (int)cur_cat->count : 0;
        int start_idx = s_ui_station_idx;
        if (start_idx > 3) {
            start_idx = s_ui_station_idx - 3;
        }

        int y = 68;
        for (int i = start_idx; i < count && i < start_idx + visible_items; i++) {
            const catalog_station_t* st = catalog_get_station(s_selected_category, i);
            if (st == nullptr) continue;

            // Проверяем, есть ли станция в избранном
            bool is_fav = catalog_is_favorite(st->id);

            if (i == s_ui_station_idx) {
                lcd.setTextColor(TFT_BLACK);
                lcd.fillRoundRect(5, y - 5, 310, 24, 4, 0x07E0);
            } else {
                lcd.setTextColor(TFT_WHITE);
            }

            lcd.setFont(&fonts::efontCN_16);

            // Формируем имя со звёздочкой для выравнивания
            char display_name[CATALOG_MAX_NAME_LEN + 10];
            if (is_fav) {
                // Жёлтая звёздочка, если не выделена, или чёрная, если выделена
                snprintf(display_name, sizeof(display_name), "★ %s", st->name);
            } else {
                // Пробелы для сохранения выравнивания текста
                snprintf(display_name, sizeof(display_name), "  %s", st->name);
            }

            lcd.drawString(display_name, 15, y);
            y += 28;
        }
    }

    // ========================================================
    // Volume
    // ========================================================
    else if (s_current_mode == MODE_VOLUME) {
        lcd.setFont(&fonts::Font4);
        lcd.setTextColor(TFT_WHITE);
        lcd.setTextDatum(MC_DATUM);

        char vol_str[16];
        snprintf(vol_str, sizeof(vol_str), "%d%%", s_current_volume);
        lcd.drawString(vol_str, 160, 88);

        lcd.setTextDatum(TL_DATUM);

        int bar_w = 280;
        int bar_h = 20;

        lcd.drawRoundRect(20, 138, bar_w, bar_h, 5, TFT_WHITE);

        int fill_w = (s_current_volume * bar_w) / 100;
        if (fill_w > 0) {
            lcd.fillRoundRect(20, 138, fill_w, bar_h, 5, 0x07E0);
        }
    }
}

// ============================================================
// Wrapper-функции отрисовки
// ============================================================
__attribute__((unused)) static void draw_keyboard() {
    if (s_use_sprite && s_ui_sprite != nullptr) {
        draw_keyboard_impl(*s_ui_sprite);
    } else {
        draw_keyboard_impl(M5.Lcd);
    }
}

__attribute__((unused)) static void draw_wifi_action_buttons() {
    if (s_use_sprite && s_ui_sprite != nullptr) {
        draw_wifi_action_buttons_impl(*s_ui_sprite);
    } else {
        draw_wifi_action_buttons_impl(M5.Lcd);
    }
}

static void draw_ui() {
    if (s_use_sprite && s_ui_sprite != nullptr) {
        draw_ui_impl(*s_ui_sprite);
        s_ui_sprite->pushSprite(0, 0);
    } else {
        draw_ui_impl(M5.Lcd);
    }
}

// ============================================================
// Обработка тача для Wi-Fi
// ============================================================
static void handle_wifi_touch(int x, int y) {
    // ==========================
    // Клавиатура активна
    // ==========================
    if (s_keyboard_active) {
        // Тап по статус-бару или меню — прячем клавиатуру
        if (y < 42) {
            s_keyboard_active = false;
            return;
        }

        // Тап по полю SSID — переключаем фокус
        if (y >= 42 && y <= 66) {
            s_editing_ssid = true;
            strcpy(s_input_buffer, s_wifi_ssid);
            s_shift_active = false;
            return;
        }

        // Тап по полю PASS — переключаем фокус
        if (y >= 72 && y <= 96) {
            s_editing_ssid = false;
            strcpy(s_input_buffer, s_wifi_pass);
            s_shift_active = false;
            return;
        }

        // Тап в промежутке между полями и клавиатурой — прячем клавиатуру
        if (y < KB_START_Y) {
            s_keyboard_active = false;
            return;
        }

        // Проверяем обычные клавиши
        for (int row = 0; row < 4; row++) {
            for (int col = 0; col < 10; col++) {
                int kx = KB_START_X + col * (KB_KEY_W + KB_GAP);
                int ky = KB_START_Y + row * (KB_KEY_H + KB_GAP);

                if (x >= kx && x <= kx + KB_KEY_W && y >= ky && y <= ky + KB_KEY_H) {
                    char c = kb_rows[row][col];
                    if (!s_shift_active && c >= 'A' && c <= 'Z') c = c + 32;

                    int len = strlen(s_input_buffer);
                    if (len < 64) {
                        s_input_buffer[len] = c;
                        s_input_buffer[len + 1] = '\0';

                        if (s_editing_ssid) {
                            strcpy(s_wifi_ssid, s_input_buffer);
                        } else {
                            strcpy(s_wifi_pass, s_input_buffer);
                        }

                        s_has_unsaved_changes = true;
                    }

                    s_shift_active = false;
                    return;
                }
            }
        }

        // Нижняя служебная строка
        int y5 = KB_START_Y + 4 * (KB_KEY_H + KB_GAP);

        // Shift
        if (x >= KB_START_X && x <= KB_START_X + KB_SPECIAL_W &&
            y >= y5 && y <= y5 + KB_KEY_H) {
            s_shift_active = !s_shift_active;
            return;
        }

        // Space
        if (x >= KB_START_X + KB_SPECIAL_W + KB_GAP &&
            x <= KB_START_X + 2 * KB_SPECIAL_W + KB_GAP &&
            y >= y5 && y <= y5 + KB_KEY_H) {
            int len = strlen(s_input_buffer);
            if (len < 64) {
                s_input_buffer[len] = ' ';
                s_input_buffer[len + 1] = '\0';

                if (s_editing_ssid) {
                    strcpy(s_wifi_ssid, s_input_buffer);
                } else {
                    strcpy(s_wifi_pass, s_input_buffer);
                }

                s_has_unsaved_changes = true;
            }
            return;
        }

        // Backspace
        if (x >= KB_START_X + 2 * (KB_SPECIAL_W + KB_GAP) &&
            x <= KB_START_X + 3 * KB_SPECIAL_W + 2 * KB_GAP &&
            y >= y5 && y <= y5 + KB_KEY_H) {
            int len = strlen(s_input_buffer);
            if (len > 0) {
                s_input_buffer[len - 1] = '\0';

                if (s_editing_ssid) {
                    strcpy(s_wifi_ssid, s_input_buffer);
                } else {
                    strcpy(s_wifi_pass, s_input_buffer);
                }

                s_has_unsaved_changes = true;
            }
            return;
        }

        // Любой другой тап — прячем клавиатуру
        s_keyboard_active = false;
        return;
    }

    // ==========================
    // Клавиатура не активна
    // ==========================

    // Тап по полю SSID
    if (y >= 42 && y <= 66) {
        s_editing_ssid = true;
        s_keyboard_active = true;
        strcpy(s_input_buffer, s_wifi_ssid);
        s_shift_active = false;
        return;
    }

    // Тап по полю PASS
    if (y >= 72 && y <= 96) {
        s_editing_ssid = false;
        s_keyboard_active = true;
        strcpy(s_input_buffer, s_wifi_pass);
        s_shift_active = false;
        return;
    }

    // Если тест Wi-Fi уже выполняется — игнорируем кнопки
    if (s_wifi_test_result == WIFI_TEST_IN_PROGRESS) {
        ESP_LOGW(TAG, "Wi-Fi test in progress, ignoring action buttons");
        return;
    }

    // Revert
    if (x >= WIFI_BTN_X && x <= WIFI_BTN_X + WIFI_BTN_W &&
        y >= WIFI_BTN_Y && y <= WIFI_BTN_Y + WIFI_BTN_H) {
        strcpy(s_wifi_ssid, s_wifi_ssid_original);
        strcpy(s_wifi_pass, s_wifi_pass_original);
        s_has_unsaved_changes = false;
        s_pending_event = UI_EVENT_WIFI_REVERT;
        return;
    }

    // Save
    if (x >= WIFI_BTN_X + WIFI_BTN_W + WIFI_BTN_GAP &&
        x <= WIFI_BTN_X + 2 * WIFI_BTN_W + WIFI_BTN_GAP &&
        y >= WIFI_BTN_Y && y <= WIFI_BTN_Y + WIFI_BTN_H) {
        s_pending_event = UI_EVENT_WIFI_SAVE;
        return;
    }

    // Test
    if (x >= WIFI_BTN_X && x <= WIFI_BTN_X + WIFI_BTN_W &&
        y >= WIFI_BTN_Y + WIFI_BTN_H + WIFI_BTN_GAP &&
        y <= WIFI_BTN_Y + 2 * WIFI_BTN_H + WIFI_BTN_GAP) {
        s_pending_event = UI_EVENT_WIFI_TEST;
        s_wifi_test_result = WIFI_TEST_IN_PROGRESS;
        return;
    }

    // Connect
    if (x >= WIFI_BTN_X + WIFI_BTN_W + WIFI_BTN_GAP &&
        x <= WIFI_BTN_X + 2 * WIFI_BTN_W + WIFI_BTN_GAP &&
        y >= WIFI_BTN_Y + WIFI_BTN_H + WIFI_BTN_GAP &&
        y <= WIFI_BTN_Y + 2 * WIFI_BTN_H + WIFI_BTN_GAP) {
        s_pending_event = UI_EVENT_WIFI_CONNECT;
        return;
    }
}

// ============================================================
// Публичные функции
// ============================================================
void ui_init() {
    s_use_sprite = false;

    if (s_ui_sprite != nullptr) {
        delete s_ui_sprite;
        s_ui_sprite = nullptr;
    }

    if (!SPRITE_DEBUG_FORCE_DIRECT) {
        s_ui_sprite = new (std::nothrow) m5gfx::LGFX_Sprite(&M5.Lcd);

        if (s_ui_sprite != nullptr) {
            s_ui_sprite->setPsram(true);

            if (s_ui_sprite->createSprite(320, 240)) {
                s_ui_sprite->setTextDatum(TL_DATUM);
                s_use_sprite = true;
                ESP_LOGI(TAG, "UI sprite created: 320x240, psram=true");
            } else {
                ESP_LOGW(TAG, "UI sprite createSprite failed");
                delete s_ui_sprite;
                s_ui_sprite = nullptr;
            }
        } else {
            ESP_LOGW(TAG, "UI sprite allocation failed");
        }
    } else {
        ESP_LOGW(TAG, "UI sprite debug forced direct drawing");
    }

    // Явно зажигаем подсветку
    M5.Display.setBrightness(DISPLAY_BRIGHTNESS);

    ESP_LOGI(TAG, "UI initialized, sprite_enabled=%d, brightness=%d",
             s_use_sprite, DISPLAY_BRIGHTNESS);
}

void ui_update() {
    static bool last_btn_a = false, last_btn_b = false, enc_is_pressed = false;
    static uint32_t enc_press_start_time = 0, redraw_counter = 0;
    static bool ui_needs_redraw = true;

    // Состояние сна экрана
    static uint64_t s_last_activity_us = esp_timer_get_time();
    static bool     s_display_awake    = true;

    M5.update();

    // ========================================================
    // 1. Собираем весь ввод в один снимок кадра
    // ========================================================
    auto touch          = M5.Touch.getDetail();
    bool touch_pressed  = touch.wasPressed();

    int32_t increment   = read_hmi_register_32(0x10);
    bool btn_a          = read_hmi_register_8(0x21);
    bool btn_b          = read_hmi_register_8(0x22);
    bool btn_enc        = read_hmi_register_8(0x20);

    bool btn_a_edge       = btn_a && !last_btn_a;
    bool btn_b_edge       = btn_b && !last_btn_b;
    bool enc_just_pressed = btn_enc && !enc_is_pressed;
    bool enc_just_released = !btn_enc && enc_is_pressed;

    bool activity = touch_pressed || btn_a_edge || btn_b_edge ||
                    enc_just_pressed || enc_just_released || (increment != 0);

    uint64_t now_us = esp_timer_get_time();

    // ========================================================
    // 2. Управление сном / пробуждением экрана
    // ========================================================
    if (activity) {
        s_last_activity_us = now_us;
        if (!s_display_awake) {
            M5.Display.setBrightness(DISPLAY_BRIGHTNESS);
            s_display_awake = true;
            ui_needs_redraw = true;
            ESP_LOGI(TAG, "Display wakeup");
        }
    } else if (s_display_awake &&
               (now_us - s_last_activity_us) > (uint64_t)DISPLAY_IDLE_TIMEOUT_MS * 1000ULL) {
        M5.Display.setBrightness(0);
        s_display_awake = false;
        ESP_LOGI(TAG, "Display sleep (idle %u ms)", (unsigned)DISPLAY_IDLE_TIMEOUT_MS);
    }

    // Пока экран спит — ничего не рисуем и не обрабатываем,
    // но синхронизируем флаги кнопок, чтобы после пробуждения
    // не генерировались ложные front-edges.
    if (!s_display_awake) {
        last_btn_a     = btn_a;
        last_btn_b     = btn_b;
        enc_is_pressed = btn_enc;
        return;
    }

    // ========================================================
    // 3. Мигание курсора
    // ========================================================
    s_cursor_blink_counter++;
    if (s_cursor_blink_counter >= 10) {
        s_cursor_blink = !s_cursor_blink;
        s_cursor_blink_counter = 0;
        if (s_keyboard_active) ui_needs_redraw = true;
    }

    redraw_counter++;
    if (redraw_counter >= 10) {
        ui_needs_redraw = true;
        redraw_counter = 0;
    }

    // ========================================================
    // 4. Обработка тача в режиме Wi-Fi
    // ========================================================
    if (s_current_mode == MODE_WIFI && touch_pressed) {
        handle_wifi_touch(touch.x, touch.y);
        ui_needs_redraw = true;
    }

    // ========================================================
    // 5. Энкодер: ВРАЩЕНИЕ
    // ========================================================
    if (increment != 0) {
        s_encoder_accumulator += increment;

        int32_t effective = 0;

        if (s_encoder_accumulator >= ENCODER_THRESHOLD) {
            effective = s_encoder_accumulator / ENCODER_THRESHOLD;
            s_encoder_accumulator %= ENCODER_THRESHOLD;
        } else if (s_encoder_accumulator <= -ENCODER_THRESHOLD) {
            effective = s_encoder_accumulator / ENCODER_THRESHOLD;
            s_encoder_accumulator %= ENCODER_THRESHOLD;
        }

        if (effective != 0) {
            if (s_current_mode == MODE_CATEGORIES) {
                int cat_count = (int)catalog_category_count();
                s_selected_category += effective;

                if (s_selected_category < 0) s_selected_category = 0;
                if (cat_count > 0 && s_selected_category >= cat_count) {
                    s_selected_category = cat_count - 1;
                }

                ui_needs_redraw = true;
            }
            else if (s_current_mode == MODE_STATIONS) {
                const catalog_category_t* ec = catalog_get_category(s_selected_category);
                int count = ec ? (int)ec->count : 0;

                s_ui_station_idx += effective;

                if (s_ui_station_idx < 0) s_ui_station_idx = 0;
                if (count > 0 && s_ui_station_idx >= count) {
                    s_ui_station_idx = count - 1;
                }

                ui_needs_redraw = true;
            }
            else if (s_current_mode == MODE_FAVORITES) {
                int fav_count = (int)catalog_get_favorites_count();

                s_fav_station_idx += effective;

                if (s_fav_station_idx < 0) s_fav_station_idx = 0;
                if (fav_count > 0 && s_fav_station_idx >= fav_count) {
                    s_fav_station_idx = fav_count - 1;
                }

                ui_needs_redraw = true;
            }
            else if (s_current_mode == MODE_SETTINGS) {
                s_settings_idx += effective;

                if (s_settings_idx < 0) s_settings_idx = 0;
                if (s_settings_idx >= 4) s_settings_idx = 3;

                ui_needs_redraw = true;
            }
            else if (s_current_mode == MODE_WIFI && !s_keyboard_active) {
                if (s_wifi_show_scan_results) {
                    int count = wifi_scan_get_count();

                    if (count > 0) {
                        if (s_wifi_scan_selected_idx < 0) {
                            s_wifi_scan_selected_idx = 0;
                        }

                        s_wifi_scan_selected_idx += effective;

                        if (s_wifi_scan_selected_idx < 0) {
                            s_wifi_scan_selected_idx = 0;
                        }

                        if (s_wifi_scan_selected_idx >= count) {
                            s_wifi_scan_selected_idx = count - 1;
                        }
                    } else {
                        s_wifi_scan_selected_idx = -1;
                    }
                } else {
                    // 0 = SSID, 1 = Password, 2 = Scan, 3 = Connect
                    s_wifi_focus += effective;

                    if (s_wifi_focus < 0) s_wifi_focus = 0;
                    if (s_wifi_focus > 3) s_wifi_focus = 3;
                }

                ui_needs_redraw = true;
            }
            else if (s_current_mode == MODE_VOLUME || s_current_mode == MODE_PLAYBACK) {
                int new_vol = s_current_volume + effective * 5;

                if (new_vol < 0) new_vol = 0;
                if (new_vol > 100) new_vol = 100;

                if (new_vol != s_current_volume) {
                    s_current_volume = new_vol;
                    s_new_volume = new_vol;
                    s_pending_event = UI_EVENT_VOLUME_CHANGED;
                    ui_needs_redraw = true;
                }
            }
        }
    }

    // ========================================================
    // 6. Кнопка A: назад / отмена
    // ========================================================
    if (btn_a_edge) {
        if (s_current_mode == MODE_WIFI) {
            if (s_keyboard_active) {
                s_keyboard_active = false;
                s_keyboard_target = KB_TARGET_NONE;
            }
            else if (s_wifi_show_scan_results) {
                s_wifi_show_scan_results = false;
                s_wifi_scan_selected_idx = -1;
                s_wifi_focus = 0;
            }
            else {
                s_current_mode = MODE_SETTINGS;
                s_wifi_focus = 0;
                s_keyboard_active = false;
                s_keyboard_target = KB_TARGET_NONE;
            }
        }
        else if (s_current_mode == MODE_PLAYBACK) {
            s_current_mode = MODE_SETTINGS;
        }
        else if (s_current_mode == MODE_SETTINGS) {
            s_current_mode = MODE_STATIONS;
        }
        else if (s_current_mode == MODE_STATIONS) {
            s_current_mode = MODE_CATEGORIES;
        }
        else if (s_current_mode == MODE_CATEGORIES) {
            s_current_mode = MODE_FAVORITES;
        }
        else if (s_current_mode == MODE_FAVORITES) {
            s_current_mode = MODE_PLAYBACK;
        }
        else if (s_current_mode == MODE_ABOUT || s_current_mode == MODE_CATALOG) {
            s_current_mode = MODE_SETTINGS;
        }

        s_encoder_accumulator = 0;
        ui_needs_redraw = true;
    }
    last_btn_a = btn_a;

    // ========================================================
    // 7. Кнопка B: вперёд
    //    В Wi-Fi тоже делаем безопасный выход, чтобы не было ловушки.
    // ========================================================
    if (btn_b_edge) {
        if (s_current_mode == MODE_WIFI) {
            if (s_keyboard_active) {
                s_keyboard_active = false;
                s_keyboard_target = KB_TARGET_NONE;
            }
            else if (s_wifi_show_scan_results) {
                s_wifi_show_scan_results = false;
                s_wifi_scan_selected_idx = -1;
                s_wifi_focus = 0;
            }
            else {
                s_current_mode = MODE_SETTINGS;
                s_wifi_focus = 0;
                s_keyboard_active = false;
                s_keyboard_target = KB_TARGET_NONE;
            }
        }
        else if (s_current_mode == MODE_PLAYBACK) {
            s_current_mode = MODE_FAVORITES;
        }
        else if (s_current_mode == MODE_FAVORITES) {
            s_current_mode = MODE_CATEGORIES;
        }
        else if (s_current_mode == MODE_CATEGORIES) {
            s_current_mode = MODE_STATIONS;
        }
        else if (s_current_mode == MODE_STATIONS) {
            s_current_mode = MODE_SETTINGS;
        }
        else if (s_current_mode == MODE_SETTINGS) {
            s_current_mode = MODE_PLAYBACK;
        }
        else if (s_current_mode == MODE_ABOUT || s_current_mode == MODE_CATALOG) {
            s_current_mode = MODE_SETTINGS;
        }

        s_encoder_accumulator = 0;
        ui_needs_redraw = true;
    }
    last_btn_b = btn_b;

    // ========================================================
    // 8. Кнопка энкодера: короткое и долгое нажатие
    // ========================================================
    if (btn_enc) {
        if (!enc_is_pressed) {
            enc_is_pressed = true;
            enc_press_start_time = esp_timer_get_time() / 1000;
            s_encoder_accumulator = 0;
            enc_long_press_handled = false;
        } else {
            uint32_t now = esp_timer_get_time() / 1000;

            if (now - enc_press_start_time >= 1000 && !enc_long_press_handled) {
                if (s_current_mode == MODE_STATIONS) {
                    int global_idx = catalog_global_index(s_selected_category, s_ui_station_idx);
                    const catalog_station_t* st = catalog_get_station_by_global_index(global_idx);

                    if (st != nullptr) {
                        bool added = catalog_toggle_favorite(st->id);
                        catalog_save_favorites();
                        ESP_LOGI(TAG, "Favorite %s: %s", added ? "ADDED" : "REMOVED", st->name);
                    }
                }
                else if (s_current_mode == MODE_FAVORITES) {
                    int global_idx = catalog_get_favorite_global_index(s_fav_station_idx);
                    const catalog_station_t* st = catalog_get_station_by_global_index(global_idx);

                    if (st != nullptr) {
                        bool added = catalog_toggle_favorite(st->id);
                        catalog_save_favorites();
                        ESP_LOGI(TAG, "Favorite %s: %s", added ? "ADDED" : "REMOVED", st->name);

                        if (!added) {
                            int fav_count = (int)catalog_get_favorites_count();

                            if (s_fav_station_idx >= fav_count && fav_count > 0) {
                                s_fav_station_idx = fav_count - 1;
                            } else if (fav_count == 0) {
                                s_fav_station_idx = 0;
                            }
                        }
                    }
                }
                else if (s_current_mode == MODE_WIFI) {
                    // Долгое нажатие в Wi-Fi = безопасный выход/отмена
                    if (s_keyboard_active) {
                        s_keyboard_active = false;
                        s_keyboard_target = KB_TARGET_NONE;
                    }
                    else if (s_wifi_show_scan_results) {
                        s_wifi_show_scan_results = false;
                        s_wifi_scan_selected_idx = -1;
                        s_wifi_focus = 0;
                    }
                    else {
                        s_current_mode = MODE_SETTINGS;
                        s_wifi_focus = 0;
                        s_keyboard_active = false;
                        s_keyboard_target = KB_TARGET_NONE;
                    }
                }
                else {
                    // В остальных режимах долгое нажатие = MUTE
                    s_is_muted = !s_is_muted;
                    s_pending_event = UI_EVENT_MUTE_TOGGLED;
                }

                enc_long_press_handled = true;
                ui_needs_redraw = true;
            }
        }
    } else {
        if (enc_is_pressed) {
            if (!enc_long_press_handled) {
                if (s_current_mode == MODE_STATIONS) {
                    s_new_station_idx = (int)catalog_global_index(s_selected_category, s_ui_station_idx);
                    s_pending_event = UI_EVENT_STATION_CHANGED;
                    s_current_mode = MODE_PLAYBACK;
                }
                else if (s_current_mode == MODE_FAVORITES) {
                    int global_idx = catalog_get_favorite_global_index(s_fav_station_idx);

                    if (global_idx >= 0) {
                        s_new_station_idx = global_idx;
                        s_pending_event = UI_EVENT_STATION_CHANGED;
                        s_current_mode = MODE_PLAYBACK;
                    }
                }
                else if (s_current_mode == MODE_CATEGORIES) {
                    s_current_mode = MODE_STATIONS;
                    s_ui_station_idx = 0;
                }
                else if (s_current_mode == MODE_SETTINGS) {
                    if (s_settings_idx == 0) {
                        s_current_mode = MODE_WIFI;
                        s_wifi_focus = 0;
                        s_keyboard_active = false;
                        s_keyboard_target = KB_TARGET_NONE;
                        s_wifi_show_scan_results = false;
                        s_wifi_scan_selected_idx = -1;
                    }
                    else if (s_settings_idx == 1) {
                        s_current_mode = MODE_CATALOG;
                    }
                    else if (s_settings_idx == 2) {
                        s_current_mode = MODE_ABOUT;
                    }
                    else if (s_settings_idx == 3) {
                        s_current_mode = MODE_PLAYBACK;
                    }
                }
                else if (s_current_mode == MODE_ABOUT || s_current_mode == MODE_CATALOG) {
                    s_current_mode = MODE_SETTINGS;
                }
                else if (s_current_mode == MODE_WIFI) {
                    if (s_wifi_show_scan_results) {
                        int count = wifi_scan_get_count();

                        if (count > 0) {
                            if (s_wifi_scan_selected_idx < 0) {
                                s_wifi_scan_selected_idx = 0;
                            }

                            if (s_wifi_scan_selected_idx >= 0 && s_wifi_scan_selected_idx < count) {
                                const wifi_scan_record_t* rec = wifi_scan_get_record(s_wifi_scan_selected_idx);

                                if (rec != nullptr) {
                                    strncpy(s_wifi_ssid, rec->ssid, sizeof(s_wifi_ssid) - 1);
                                    s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = '\0';

                                    s_wifi_show_scan_results = false;
                                    s_wifi_scan_selected_idx = -1;
                                    s_wifi_focus = 1;

                                    // Сразу открываем ввод пароля
                                    s_keyboard_active = true;
                                    s_keyboard_target = KB_TARGET_PASS;

                                    ESP_LOGI(TAG, "Wi-Fi SSID selected from scan: %s", s_wifi_ssid);
                                }
                            }
                        } else {
                            // Нет сетей — переходим в ручной ввод SSID
                            s_wifi_show_scan_results = false;
                            s_wifi_scan_selected_idx = -1;
                            s_wifi_focus = 0;
                            s_keyboard_active = true;
                            s_keyboard_target = KB_TARGET_SSID;
                        }
                    }
                    else if (s_keyboard_active) {
                        // Повторный клик закрывает клавиатуру
                        s_keyboard_active = false;
                        s_keyboard_target = KB_TARGET_NONE;
                    }
                    else {
                        if (s_wifi_focus == 0) {
                            s_keyboard_active = true;
                            s_keyboard_target = KB_TARGET_SSID;
                        }
                        else if (s_wifi_focus == 1) {
                            s_keyboard_active = true;
                            s_keyboard_target = KB_TARGET_PASS;
                        }
                        else if (s_wifi_focus == 2) {
                            ESP_LOGI(TAG, "Wi-Fi Scan requested via encoder");
                            ui_trigger_wifi_scan();
                        }
                        else if (s_wifi_focus == 3) {
                            ESP_LOGI(TAG, "Wi-Fi Connect requested via encoder");
                            ui_trigger_wifi_connect();
                        }
                    }
                }
            }

            enc_is_pressed = false;
            s_encoder_accumulator = 0;
            ui_needs_redraw = true;
        }
    }

    // ========================================================
    // 9. Перерисовка
    // ========================================================
    if (ui_needs_redraw) {
        draw_ui();
        ui_needs_redraw = false;
    }
}

void ui_set_playback_info(const char* station_name,
                          const char* track_title,
                          uint32_t buffer_level,
                          uint32_t buffer_target,
                          uint32_t starve_count,
                          uint32_t overflow_count) {
    if (station_name) strncpy(s_station_name, station_name, sizeof(s_station_name) - 1);
    if (track_title) strncpy(s_track_title, track_title, sizeof(s_track_title) - 1);
    s_buffer_level = buffer_level;
    s_buffer_target = buffer_target;
    s_starve_count = starve_count;
    s_overflow_count = overflow_count;
}

ui_event_t ui_get_event() {
    ui_event_t event = s_pending_event;
    s_pending_event = UI_EVENT_NONE;
    return event;
}

int ui_get_selected_station() {
    return s_new_station_idx;
}

int ui_get_volume() {
    return s_new_volume;
}

void ui_get_wifi_credentials(char* ssid, size_t ssid_len, char* pass, size_t pass_len) {
    if (ssid) {
        strncpy(ssid, s_wifi_ssid, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
    }
    if (pass) {
        strncpy(pass, s_wifi_pass, pass_len - 1);
        pass[pass_len - 1] = '\0';
    }
}

void ui_set_wifi_credentials(const char* ssid, const char* pass) {
    if (ssid) {
        strcpy(s_wifi_ssid, ssid);
        strcpy(s_wifi_ssid_original, ssid);
    }
    if (pass) {
        strcpy(s_wifi_pass, pass);
        strcpy(s_wifi_pass_original, pass);
    }
    s_has_unsaved_changes = false;
    ESP_LOGI(TAG, "UI Wi-Fi credentials synced: SSID='%s'", s_wifi_ssid);
}

void ui_set_wifi_test_result(wifi_test_result_t result) {
    s_wifi_test_result = result;
}

wifi_test_result_t ui_get_wifi_test_result() {
    return s_wifi_test_result;
}

void ui_set_network_info(uint32_t net_fill_bytes, uint32_t ring_capacity_bytes) {
    s_net_fill = net_fill_bytes;
    s_net_capacity = ring_capacity_bytes ? ring_capacity_bytes : RINGBUF_SIZE;

    // Пишем в кольцевой буфер истории
    s_net_history[s_net_history_pos] = net_fill_bytes;
    s_net_history_pos = (uint8_t)((s_net_history_pos + 1) % 64);

    if (s_net_history_count < 64) {
        s_net_history_count++;
    }
}
void ui_trigger_wifi_scan(void) {
    s_pending_event = UI_EVENT_WIFI_SCAN_REQUESTED;
}
void ui_trigger_wifi_connect(void) {
    s_pending_event = UI_EVENT_WIFI_CONNECT_REQUESTED;
}

void ui_set_wifi_scan_ready(bool ready) {
    s_wifi_show_scan_results = ready;
    if (ready) {
        s_wifi_scan_selected_idx = 0; // Сбрасываем выделение на первую сеть
    }
    // Следующий вызов ui_update() автоматически отрисует изменения
}

void ui_set_current_mode(ui_mode_t mode) {
    s_current_mode = mode;
    s_encoder_accumulator = 0; // Сбрасываем накопитель энкодера, чтобы не было случайных сдвигов
}
