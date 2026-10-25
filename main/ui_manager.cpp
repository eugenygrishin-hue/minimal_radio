#include "ui_manager.h"
#include "config.h"
#include "catalog_manager.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_netif.h"

#include <M5Unified.hpp>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <new>


static const char* TAG = "UI";

// Координаты для экрана 320x240

static const int WIFI_BTN_X   = 5;
static const int WIFI_BTN_Y   = 112;
static const int WIFI_BTN_W   = 150;
static const int WIFI_BTN_H   = 26;
static const int WIFI_BTN_GAP = 10;

static const int WIFI_FIELD_Y[2] = {58, 84};
static const int WIFI_FIELD_H = 24;

static const int WIFI_KB_Y = 110;
static const int WIFI_KB_H = 24;
static const int WIFI_KB_X = 5;
static const int WIFI_KB_W = 30;
static const int WIFI_KB_STEP = 31;
static const int WIFI_KB_ROWS = 5;

static const char* WIFI_KB_LETTER_ROWS[4] = {
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm,.",
    "1234567890"
};

static const int WIFI_KB_ACTION_X[4] = {5, 71, 171, 237};
static const int WIFI_KB_ACTION_W[4] = {62, 96, 62, 62};
static const char* WIFI_KB_ACTION_LABEL[4] = {"Shift", "Space", "Del", "OK"};


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
static bool s_keyboard_active = false;
static bool s_cursor_blink = false;
static uint32_t s_cursor_blink_counter = 0;
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
// Wi-Fi manual input / on-screen keyboard helpers
// ============================================================

static bool s_keyboard_shift = false;

static int wifi_scan_view_start(void) {
    int start = s_wifi_scan_selected_idx;

    if (start > 2) {
        start = s_wifi_scan_selected_idx - 2;
    }

    if (start < 0) {
        start = 0;
    }

    return start;
}

static void wifi_kb_append_char(char c) {
    char* buf = (s_keyboard_target == KB_TARGET_PASS) ? s_wifi_pass : s_wifi_ssid;

    size_t max_len = (s_keyboard_target == KB_TARGET_PASS)
                         ? (sizeof(s_wifi_pass) - 1)
                         : (sizeof(s_wifi_ssid) - 1);

    size_t len = strlen(buf);

    if (len < max_len) {
        buf[len] = c;
        buf[len + 1] = '\0';
    }
}

static void wifi_kb_backspace(void) {
    char* buf = (s_keyboard_target == KB_TARGET_PASS) ? s_wifi_pass : s_wifi_ssid;
    size_t len = strlen(buf);

    if (len > 0) {
        buf[len - 1] = '\0';
    }
}

static void wifi_kb_close(void) {
    s_keyboard_active = false;
    s_keyboard_target = KB_TARGET_NONE;
    s_keyboard_shift = false;
}

static void wifi_kb_open(kb_target_t target) {
    s_keyboard_active = true;
    s_keyboard_target = target;
    s_keyboard_shift = false;

    // Если был открыт список скана — закрываем его
    s_wifi_show_scan_results = false;
    s_wifi_scan_selected_idx = -1;

    if (target == KB_TARGET_SSID) {
        s_wifi_focus = 0;
    } else {
        s_wifi_focus = 1;
    }

    ESP_LOGI(TAG, "Keyboard opened for target=%d", (int)target);
}

static void wifi_back_action(void) {
    if (s_keyboard_active) {
        wifi_kb_close();
        ESP_LOGI(TAG, "Keyboard closed by back action");
        return;
    }

    if (s_wifi_show_scan_results) {
        s_wifi_show_scan_results = false;
        s_wifi_scan_selected_idx = -1;
        s_wifi_focus = 0;
        s_keyboard_shift = false;
        ESP_LOGI(TAG, "Scan results closed by back action");
        return;
    }

    s_wifi_focus = 0;
    s_keyboard_shift = false;
    s_current_mode = MODE_SETTINGS;
    ESP_LOGI(TAG, "Exited Wi-Fi mode to SETTINGS");
}

static void wifi_select_scan_result(int idx) {
    int count = wifi_scan_get_count();

    if (count == 0) {
        s_wifi_show_scan_results = false;
        s_wifi_scan_selected_idx = -1;
        wifi_kb_open(KB_TARGET_SSID);
        return;
    }

    if (idx < 0 || idx >= count) {
        idx = 0;
    }

    const wifi_scan_record_t* rec = wifi_scan_get_record(idx);

    if (rec != nullptr) {
        strncpy(s_wifi_ssid, rec->ssid, sizeof(s_wifi_ssid) - 1);
        s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = '\0';

        ESP_LOGI(TAG, "Wi-Fi SSID selected from scan: %s", s_wifi_ssid);
    }

    s_wifi_show_scan_results = false;
    s_wifi_scan_selected_idx = -1;

    // Сразу открываем ввод пароля
    wifi_kb_open(KB_TARGET_PASS);
}

static void handle_wifi_touch(int32_t x, int32_t y) {
    ESP_LOGI(TAG, "handle_wifi_touch x=%d y=%d scan=%d kb=%d focus=%d",
             (int)x, (int)y,
             (int)s_wifi_show_scan_results,
             (int)s_keyboard_active,
             s_wifi_focus);

    // ------------------------------------------------------------
    // 1. Если показан список найденных сетей — работаем только с ним
    // ------------------------------------------------------------
    if (s_wifi_show_scan_results) {
        int count = wifi_scan_get_count();

        if (count == 0) {
            // Нет сетей — тап по нижней области открывает ручной ввод SSID
            if (y >= 76) {
                s_wifi_show_scan_results = false;
                s_wifi_scan_selected_idx = -1;
                wifi_kb_open(KB_TARGET_SSID);
            }
            return;
        }

        const int list_y = 76;
        const int item_h = 26;
        const int visible = 5;

        int start = wifi_scan_view_start();

        if (y >= list_y && y < list_y + visible * item_h) {
            int idx = start + (y - list_y) / item_h;

            if (idx >= 0 && idx < count) {
                wifi_select_scan_result(idx);
            }
        }

        return;
    }

    // ------------------------------------------------------------
    // 2. Тап по полям SSID / Password
    //    Работает и когда клавиатура открыта, и когда закрыта.
    // ------------------------------------------------------------
    for (int i = 0; i < 2; i++) {
        int fy = WIFI_FIELD_Y[i] - 2;

        if (x >= 5 && x < 315 && y >= fy && y < fy + WIFI_FIELD_H + 4) {
            wifi_kb_open(i == 0 ? KB_TARGET_SSID : KB_TARGET_PASS);
            return;
        }
    }

    // ------------------------------------------------------------
    // 3. Если клавиатура открыта — обрабатываем тапы по клавиатуре
    // ------------------------------------------------------------
    if (s_keyboard_active) {
        if (y >= WIFI_KB_Y && y < WIFI_KB_Y + WIFI_KB_ROWS * WIFI_KB_H) {
            int row = (y - WIFI_KB_Y) / WIFI_KB_H;

            // Буквенные/цифровые ряды
            if (row >= 0 && row < 4) {
                if (x >= WIFI_KB_X && x < WIFI_KB_X + 10 * WIFI_KB_STEP) {
                    int col = (x - WIFI_KB_X) / WIFI_KB_STEP;

                    const char* row_str = WIFI_KB_LETTER_ROWS[row];
                    int len = (int)strlen(row_str);

                    if (col >= 0 && col < len) {
                        char c = row_str[col];

                        if (s_keyboard_shift && c >= 'a' && c <= 'z') {
                            c = (char)(c - 'a' + 'A');
                        }

                        wifi_kb_append_char(c);

                        // После ввода символа Shift автоматически выключается
                        s_keyboard_shift = false;

                        ESP_LOGI(TAG, "Keyboard char: '%c' target=%d",
                                 c, (int)s_keyboard_target);
                    }
                }

                return;
            }

            // Нижний ряд действий: Shift / Space / Del / OK
            if (row == 4) {
                int ay = WIFI_KB_Y + 4 * WIFI_KB_H;

                if (y >= ay && y < ay + WIFI_KB_H - 2) {
                    for (int i = 0; i < 4; i++) {
                        int ax = WIFI_KB_ACTION_X[i];
                        int aw = WIFI_KB_ACTION_W[i];

                        if (x >= ax && x < ax + aw) {
                            switch (i) {
                                case 0: // Shift
                                    s_keyboard_shift = !s_keyboard_shift;
                                    ESP_LOGI(TAG, "Keyboard shift=%d", (int)s_keyboard_shift);
                                    break;

                                case 1: // Space
                                    wifi_kb_append_char(' ');
                                    ESP_LOGI(TAG, "Keyboard space");
                                    break;

                                case 2: // Del
                                    wifi_kb_backspace();
                                    ESP_LOGI(TAG, "Keyboard backspace");
                                    break;

                                case 3: // OK
                                    wifi_kb_close();
                                    ESP_LOGI(TAG, "Keyboard OK/close");
                                    break;
                            }

                            return;
                        }
                    }
                }
            }
        }

        return;
    }

    // ------------------------------------------------------------
    // 4. Если клавиатура закрыта — тапы по кнопкам Scan / Connect
    // ------------------------------------------------------------
    int by = WIFI_BTN_Y - 2;

    if (y >= by && y < by + WIFI_BTN_H + 4) {
        if (x >= 5 && x < 155) {
            ESP_LOGI(TAG, "Touch Scan pressed");
            ui_trigger_wifi_scan();
            return;
        }

        if (x >= 165 && x < 315) {
            ESP_LOGI(TAG, "Touch Connect pressed");
            ui_trigger_wifi_connect();
            return;
        }
    }
}
template <typename T>
static void draw_wifi_keyboard_impl(T& lcd) {
    lcd.setFont(&fonts::Font0);

    // Буквенные и цифровые ряды
    for (int row = 0; row < 4; row++) {
        const char* row_str = WIFI_KB_LETTER_ROWS[row];
        int len = (int)strlen(row_str);
        int y = WIFI_KB_Y + row * WIFI_KB_H;

        for (int col = 0; col < 10; col++) {
            if (col >= len) {
                continue;
            }

            char ch = row_str[col];

            if (s_keyboard_shift && ch >= 'a' && ch <= 'z') {
                ch = (char)(ch - 'a' + 'A');
            }

            int x = WIFI_KB_X + col * WIFI_KB_STEP;

            lcd.fillRoundRect(x, y, WIFI_KB_W, WIFI_KB_H - 2, 3, TFT_BLACK);
            lcd.drawRoundRect(x, y, WIFI_KB_W, WIFI_KB_H - 2, 3, 0x07E0);

            char buf[2] = {ch, '\0'};

            lcd.setTextColor(TFT_WHITE);
            lcd.setTextDatum(MC_DATUM);
            lcd.drawString(buf, x + WIFI_KB_W / 2, y + (WIFI_KB_H - 2) / 2);
        }
    }

    // Нижний ряд: Shift / Space / Del / OK
    int ay = WIFI_KB_Y + 4 * WIFI_KB_H;

    for (int i = 0; i < 4; i++) {
        bool active = (i == 0 && s_keyboard_shift);

        uint16_t bg = active ? 0x07E0 : TFT_BLACK;
        uint16_t fg = active ? TFT_BLACK : 0x07E0;

        int x = WIFI_KB_ACTION_X[i];
        int w = WIFI_KB_ACTION_W[i];

        lcd.fillRoundRect(x, ay, w, WIFI_KB_H - 2, 3, bg);
        lcd.drawRoundRect(x, ay, w, WIFI_KB_H - 2, 3, 0x07E0);

        lcd.setTextColor(fg);
        lcd.setTextDatum(MC_DATUM);
        lcd.drawString(WIFI_KB_ACTION_LABEL[i], x + w / 2, ay + (WIFI_KB_H - 2) / 2);
    }

    lcd.setTextDatum(TL_DATUM);
}

// ============================================================
// Экран "О программе": палитра и хелперы
// ============================================================
static constexpr uint16_t ABOUT_ACCENT = 0x07E0;   // фирменный зелёный
static constexpr uint16_t ABOUT_DIM    = 0x03E0;   // тёмно-зелёный, разделители
static constexpr uint16_t ABOUT_LABEL  = TFT_LIGHTGRAY;
static constexpr uint16_t ABOUT_VALUE  = TFT_WHITE;

template <typename T>
static void about_section(T& lcd, int y, const char* title) {
    lcd.setFont(&fonts::efontCN_14);
    lcd.setTextDatum(TL_DATUM);
    // маленький зелёный маркер перед заголовком секции
    lcd.fillRect(10, y + 2, 4, 8, ABOUT_ACCENT);
    lcd.setTextColor(ABOUT_ACCENT);
    lcd.drawString(title, 18, y);
    // Линия-разделитель теперь ближе к тексту (y + 15 вместо y + 16)
    lcd.drawLine(10, y + 15, 310, y + 15, ABOUT_DIM);
}

template <typename T>
static void about_row(T& lcd, int y, const char* label, const char* value,
                      uint16_t color = ABOUT_VALUE) {
    lcd.setFont(&fonts::efontCN_14);
    // подпись слева, тускло
    lcd.setTextDatum(TL_DATUM);
    lcd.setTextColor(ABOUT_LABEL);
    lcd.drawString(label, 18, y);
    // значение справа, акцентным цветом
    lcd.setTextColor(color);
    lcd.setTextDatum(TR_DATUM);
    lcd.drawString(value, 306, y);
    lcd.setTextDatum(TL_DATUM);
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

    int active_tab = (int)s_current_mode;

    // Wi-Fi, About и Catalog — это суб-меню настроек,
    // поэтому визуально подсвечиваем вкладку SET.
    if (active_tab == (int)MODE_WIFI ||
        active_tab == (int)MODE_ABOUT ||
        active_tab == (int)MODE_CATALOG) {
        active_tab = (int)MODE_SETTINGS;
    }

    for (int i = 0; i < 5; i++) {
        if (i == active_tab) {
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
    // Wi-Fi Settings
    // ========================================================
    if (s_current_mode == MODE_WIFI) {
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);

        if (s_wifi_show_scan_results) {
            lcd.drawString("Найденные сети", 10, 42);
        } else if (s_keyboard_active) {
            lcd.drawString("Ввод Wi-Fi", 10, 42);
        } else {
            lcd.drawString("Настройка Wi-Fi", 10, 42);
        }

        // ----------------------------------------------------
        // Режим списка найденных сетей
        // ----------------------------------------------------
        if (s_wifi_show_scan_results) {
            lcd.setFont(&fonts::efontCN_14);
            lcd.setTextColor(TFT_LIGHTGRAY);
            lcd.drawString("Тап/энкодер - выбрать, A - назад", 10, 58);

            int count = wifi_scan_get_count();

            if (count == 0) {
                lcd.setTextColor(TFT_YELLOW);
                lcd.drawString("Сети не найдены", 15, 90);
                lcd.drawString("Тапни или нажми A и введи SSID вручную", 15, 112);
            } else {
                const int list_y = 76;
                const int item_h = 26;
                const int visible = 5;

                int start = wifi_scan_view_start();
                int y = list_y;

                for (int i = start; i < count && i < start + visible; i++) {
                    const wifi_scan_record_t* rec = wifi_scan_get_record(i);

                    if (rec == nullptr) {
                        continue;
                    }

                    bool selected = (i == s_wifi_scan_selected_idx);

                    if (selected) {
                        lcd.setTextColor(TFT_BLACK);
                        lcd.fillRoundRect(5, y, 310, item_h - 2, 4, 0x07E0);
                    } else {
                        lcd.setTextColor(TFT_WHITE);
                        lcd.fillRoundRect(5, y, 310, item_h - 2, 4, TFT_BLACK);
                        lcd.drawRoundRect(5, y, 310, item_h - 2, 4, 0x07E0);
                    }

                    char line[64];
                    snprintf(line, sizeof(line), "%s (%d)", rec->ssid, (int)rec->rssi);

                    lcd.setFont(&fonts::efontCN_14);
                    lcd.drawString(line, 12, y + 4);

                    y += item_h;
                }
            }
        }

        // ----------------------------------------------------
        // Обычный режим: поля ввода + кнопки / клавиатура
        // ----------------------------------------------------
        else {
            const char* labels[2] = {"SSID:", "Пароль:"};
            const char* values[2] = {s_wifi_ssid, s_wifi_pass};

            for (int i = 0; i < 2; i++) {
                bool focused;

                if (s_keyboard_active) {
                    focused = (s_keyboard_target == (i == 0 ? KB_TARGET_SSID : KB_TARGET_PASS));
                } else {
                    focused = (s_wifi_focus == i);
                }

                int fy = WIFI_FIELD_Y[i] - 2;

                if (focused) {
                    lcd.setTextColor(TFT_BLACK);
                    lcd.fillRoundRect(5, fy, 310, WIFI_FIELD_H, 4, 0x07E0);
                } else {
                    lcd.setTextColor(TFT_WHITE);
                    lcd.fillRoundRect(5, fy, 310, WIFI_FIELD_H, 4, TFT_BLACK);
                    lcd.drawRoundRect(5, fy, 310, WIFI_FIELD_H, 4, TFT_LIGHTGRAY);
                }

                // Увеличиваем буферы, чтобы открытый пароль не обрезался слишком рано.
                char disp[72];
                disp[0] = '\0';

                // Для SSID всегда показываем как есть.
                // Для пароля:
                // - если поле пароля в фокусе -> показываем открытым;
                // - если не в фокусе -> маскируем звёздочками.
                if (i == 1 && !focused) {
                    int n = (int)strlen(values[i]);

                    if (n > 30) {
                        n = 30;
                    }

                    for (int k = 0; k < n; k++) {
                        disp[k] = '*';
                    }

                    disp[n] = '\0';
                } else {
                    strncpy(disp, values[i], sizeof(disp) - 1);
                    disp[sizeof(disp) - 1] = '\0';
                }

                char line[96];
                snprintf(line, sizeof(line), "%s %s", labels[i], disp);

                lcd.setFont(&fonts::efontCN_14);
                lcd.setTextColor(focused ? TFT_BLACK : TFT_WHITE);
                lcd.drawString(line, 10, WIFI_FIELD_Y[i] + 2);

                // --------------------------------------------------------
                // Мигающий курсор: показываем только когда клавиатура активна
                // и активна именно для этого поля.
                // --------------------------------------------------------
                bool cursor_here = false;

                if (s_keyboard_active) {
                    if (i == 0 && s_keyboard_target == KB_TARGET_SSID) {
                        cursor_here = true;
                    }

                    if (i == 1 && s_keyboard_target == KB_TARGET_PASS) {
                        cursor_here = true;
                    }
                }

                if (cursor_here && s_cursor_blink) {
                    // Ширина текущей строки текста.
                    // Если вдруг getUTF8width не скомпилируется, замени на:
                    // int text_w = lcd.getTextWidth(line);
                	int text_w = lcd.textWidth(line);

                    int cursor_x = 10 + text_w + 2;

                    // Не выпускаем курсор за правый край поля.
                    if (cursor_x > 306) {
                        cursor_x = 306;
                    }

                    int cursor_y = WIFI_FIELD_Y[i] + 3;
                    int cursor_h = WIFI_FIELD_H - 8;

                    // На зелёном фоне курсор чёрный, на чёрном — белый.
                    uint16_t cursor_color = focused ? TFT_BLACK : TFT_WHITE;

                    lcd.fillRect(cursor_x, cursor_y, 2, cursor_h, cursor_color);
                }
            }

            // Кнопки Scan / Connect рисуем только если клавиатура закрыта
            if (!s_keyboard_active) {
                const char* btn_labels[2] = {"Scan", "Connect"};
                const int btn_x[2] = {5, 165};

                for (int i = 0; i < 2; i++) {
                    int focus_idx = i + 2; // 2=Scan, 3=Connect
                    bool focused = (s_wifi_focus == focus_idx);

                    int bx = btn_x[i];
                    int by = WIFI_BTN_Y - 2;

                    if (focused) {
                        lcd.setTextColor(TFT_BLACK);
                        lcd.fillRoundRect(bx, by, 150, WIFI_BTN_H, 4, 0x07E0);
                    } else {
                        lcd.setTextColor(0x07E0);
                        lcd.fillRoundRect(bx, by, 150, WIFI_BTN_H, 4, TFT_BLACK);
                        lcd.drawRoundRect(bx, by, 150, WIFI_BTN_H, 4, 0x07E0);
                    }

                    lcd.setFont(&fonts::efontCN_14);
                    lcd.setTextDatum(MC_DATUM);
                    lcd.drawString(btn_labels[i], bx + 75, WIFI_BTN_Y + 10);
                    lcd.setTextDatum(TL_DATUM);
                }
            } else {
                draw_wifi_keyboard_impl(lcd);
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
    // About Screen (Без заголовка, компактная вёрстка)
    // ========================================================
    else if (s_current_mode == MODE_ABOUT) {
        char line[64];

        // ---- СИСТЕМА ----
        // Начинаем сразу под верхними вкладками (y=40)
        about_section(lcd, 42, "СИСТЕМА");

        const esp_app_desc_t* app = esp_app_get_description();
        about_row(lcd, 59, "FW", app->version, ABOUT_ACCENT);

        snprintf(line, sizeof(line), "%s %s", app->date, app->time);
        about_row(lcd, 74, "Build", line);

        uint64_t up_s = (uint64_t)(esp_timer_get_time() / 1000000);
        uint32_t uh = (uint32_t)(up_s / 3600);
        uint32_t um = (uint32_t)((up_s / 60) % 60);
        uint32_t us = (uint32_t)(up_s % 60);
        if (uh > 0) {
            snprintf(line, sizeof(line), "%uh %um %us", (unsigned int)uh, (unsigned int)um, (unsigned int)us);
        } else {
            snprintf(line, sizeof(line), "%um %us", (unsigned int)um, (unsigned int)us);
        }
        about_row(lcd, 89, "Аптайм", line);

        int  mv  = (int)M5.Power.getBatteryVoltage();
        int  pct = (int)M5.Power.getBatteryLevel();
        bool chg = M5.Power.isCharging();
        if (chg) {
            snprintf(line, sizeof(line), "%dmV %d%% [USB]", mv, pct);
        } else {
            snprintf(line, sizeof(line), "%dmV %d%%", mv, pct);
        }
        uint16_t bat_col = (pct > 50) ? ABOUT_ACCENT : (pct > 20 ? TFT_YELLOW : TFT_RED);
        about_row(lcd, 104, "Батарея", line, bat_col);

        // ---- СЕТЬ ----
        // Отступ 16 пикселей от последней строки
        about_section(lcd, 122, "СЕТЬ");

        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        about_row(lcd, 139, "MAC", line);

        if (wifi_is_connected()) {
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip_info;
            if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                snprintf(line, sizeof(line), IPSTR, IP2STR(&ip_info.ip));
            } else {
                snprintf(line, sizeof(line), "нет IP");
            }
            about_row(lcd, 154, "IP", line, ABOUT_ACCENT);

            int rssi = wifi_get_rssi();
            snprintf(line, sizeof(line), "%d dBm", rssi);
            uint16_t rssi_col = (rssi >= -60) ? ABOUT_ACCENT
                                : (rssi >= -75) ? TFT_YELLOW : TFT_RED;
            about_row(lcd, 169, "RSSI", line, rssi_col);
        } else {
            about_row(lcd, 154, "IP",   "не подключено", TFT_RED);
            about_row(lcd, 169, "RSSI", "--", TFT_LIGHTGRAY);
        }

        // ---- КАТАЛОГ ----
        // Отступ 14 пикселей
        about_section(lcd, 187, "КАТАЛОГ");

        const char* src = catalog_get_source_str();
        int total_st = (int)catalog_total_stations();
        int fav_count = (int)catalog_get_favorites_count();

        // Объединяем в одну строку, чтобы точно влезло
        snprintf(line, sizeof(line), "%s (%d) | Избр: %d", src, total_st, fav_count);
        about_row(lcd, 204, "Каталог", line, ABOUT_ACCENT);

        // Итоговая нижняя точка: 204 + 14 (высота шрифта) = 218 пикселей.
        // До края экрана (240) остаётся 22 пикселя запаса!
    }
    // ========================================================
    // Catalog Management (Staging)
    // ========================================================
    else if (s_current_mode == MODE_CATALOG) {
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);
        lcd.drawString("Управление каталогом", 10, 48);

        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(TFT_WHITE);

        int staging_count = catalog_staging_get_count();
        char line[64];
        snprintf(line, sizeof(line), "В буфере: %d станций", staging_count);
        lcd.drawString(line, 15, 76);

        int y = 100;
        const char* items[] = {
            "1. Загрузить РАБОЧИЙ каталог", // <-- НОВОЕ
            "2. Импортировать M3U с SD",
            "3. Проверить живые (Validate)",
            "4. Применить изменения (Merge)",
            "5. Очистить буфер",
            "6. Назад"
        };
        int count = 6; // <-- Было 5, стало 6

        for (int i = 0; i < count; i++) {
            const char* label = items[i];
            char vlabel[64];
            if (i == 1 && catalog_is_validating()) {
                snprintf(vlabel, sizeof(vlabel), "2. Идёт проверка %d/%d",
                         catalog_get_validated_count(), catalog_get_staging_count());
                label = vlabel;
            }

            if (i == s_settings_idx) {
                lcd.setTextColor(TFT_BLACK);
                lcd.fillRoundRect(5, y - 5, 310, 26, 4, 0x07E0);
            } else {
                lcd.setTextColor(TFT_WHITE);
            }
            lcd.drawString(label, 15, y);
            y += 32;
        }
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

        // ========================================================
        // Если идет валидация — показываем прогресс-бар вместо Net/sparkline
        // ========================================================
        if (catalog_is_validating()) {
            int done = catalog_get_validated_count();
            int total = catalog_get_staging_count();
            int pct = (total > 0) ? (done * 100 / total) : 0;

            lcd.setFont(&fonts::Font0);
            lcd.setTextColor(0x07E0);
            lcd.drawString("Validate:", 10, 186);

            // Рамка прогресс-бара (та же геометрия, что и у Buffer)
            lcd.drawRoundRect(10, 198, 280, 16, 4, TFT_DARKGREY);
            int vbar_w = (280 * pct) / 100;
            if (vbar_w > 0) {
                lcd.fillRoundRect(10, 198, vbar_w, 16, 4, 0x07E0);
            }

            snprintf(buf, sizeof(buf), "%d / %d (%d%%)", done, total, pct);
            lcd.setTextColor(TFT_WHITE);
            lcd.drawString(buf, 10, 218);

        } else {
            // ========================================================
            // Обычная сетевая телеметрия
            // ========================================================
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
static void draw_ui() {
    if (s_use_sprite && s_ui_sprite != nullptr) {
        draw_ui_impl(*s_ui_sprite);
        s_ui_sprite->pushSprite(0, 0);
    } else {
        draw_ui_impl(M5.Lcd);
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

    if (touch_pressed) {
        ESP_LOGI(TAG, "Touch raw: x=%d y=%d mode=%d",
                 (int)touch.x, (int)touch.y, (int)s_current_mode);
    }

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
        ESP_LOGI(TAG, "WiFi touch: x=%d y=%d kb=%d scan=%d focus=%d",
                 (int)touch.x,
                 (int)touch.y,
                 (int)s_keyboard_active,
                 (int)s_wifi_show_scan_results,
                 s_wifi_focus);

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
            else if (s_current_mode == MODE_SETTINGS || s_current_mode == MODE_CATALOG) {
                s_settings_idx += effective;
                if (s_settings_idx < 0) s_settings_idx = 0;
                if (s_current_mode == MODE_SETTINGS) {
                    if (s_settings_idx >= 4) s_settings_idx = 3;  // 4 пункта: 0..3
                }
                else if (s_current_mode == MODE_CATALOG) {
                    if (s_settings_idx >= 5) s_settings_idx = 4;  // 5 пунктов: 0..4
                }
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
            wifi_back_action();
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
            wifi_back_action();
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
                    wifi_back_action();
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
                            s_settings_idx = 0; // Сбрасываем индекс для подменю
                        }
                        else if (s_settings_idx == 2) {
                            s_current_mode = MODE_ABOUT;
                        }
                        else if (s_settings_idx == 3) {
                            s_current_mode = MODE_PLAYBACK;
                        }
                    }
                    else if (s_current_mode == MODE_CATALOG) {
                        if (s_settings_idx == 0) {
                            ESP_LOGI(TAG, "Catalog: Loading working catalog to staging...");
                            catalog_load_working_to_staging();
                        }
                        else if (s_settings_idx == 1) {
                            ESP_LOGI(TAG, "Catalog: Importing M3U from SD...");
                            catalog_staging_load_from_m3u_dir(CATALOG_M3U_DIR);
                        }
                        else if (s_settings_idx == 2) {
                            ESP_LOGI(TAG, "Catalog: Starting background validation...");
                            catalog_start_validation();
                        }
                        else if (s_settings_idx == 3) {
                            ESP_LOGI(TAG, "Catalog: Merging staging to working...");
                            catalog_staging_merge_to_working();
                        }
                        else if (s_settings_idx == 4) {
                            ESP_LOGI(TAG, "Catalog: Clearing staging...");
                            catalog_staging_clear();
                        }
                        else if (s_settings_idx == 5) {
                            s_current_mode = MODE_SETTINGS;
                            s_settings_idx = 1; // Возвращаемся на пункт "Catalog"
                        }
                    }
                    else if (s_current_mode == MODE_ABOUT) {
                        s_current_mode = MODE_SETTINGS;
                    }
                    else if (s_current_mode == MODE_WIFI) {
                        if (s_wifi_show_scan_results) {
                            wifi_select_scan_result(s_wifi_scan_selected_idx);
                        }
                        else if (s_keyboard_active) {
                            wifi_kb_close();
                        }
                        else {
                            if (s_wifi_focus == 0) {
                                wifi_kb_open(KB_TARGET_SSID);
                            }
                            else if (s_wifi_focus == 1) {
                                wifi_kb_open(KB_TARGET_PASS);
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
}
if (pass) {
strcpy(s_wifi_pass, pass);
}
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
