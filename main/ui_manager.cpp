#include "ui_manager.h"
#include "config.h"
#include "catalog_manager.h"
#include "wifi_manager.h"

#include "audio_player.h"
#include "audio_output.h"
#include "catalog_updater.h"

#include "font_cyr_8x16.h"

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

static const int WIFI_BTN_ROW2_Y = 148; // <-- НОВОЕ: 112 + 26 + 10

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


static char s_icy_name[96] = "";
static int  s_bitrate_kbps = 0;
static char s_codec[16] = "";

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

static int s_switch_phase = 0;

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

static int s_catalog_submode = 0;   // 0=меню, 1=выбор источника
static int s_upd_source_idx  = 0;   // индекс выбранного источника в s_sources
static uint32_t s_upd_refuse_ms = 0;
static uint32_t s_cat_saved_ms = 0;   // тост «Сохранено» после удаления категорий

// E5: избранное из PLAY
static int      s_playing_station_gidx = -1;
static uint32_t s_fav_toast_ms   = 0;
static char     s_fav_toast_text[32] = "";

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
// Режим выделения станций для группового удаления
// ============================================================
static bool s_selection_mode = false;
static uint32_t s_marked_ids[64];
static size_t s_marked_count = 0;

// ============================================================
// Режим выделения категорий для группового удаления
// ============================================================
static bool s_cat_selection_mode = false;
static size_t s_marked_cat_indices[64];
static size_t s_marked_cat_count = 0;

static bool is_cat_marked(size_t idx) {
    for (size_t i = 0; i < s_marked_cat_count; i++) {
        if (s_marked_cat_indices[i] == idx) return true;
    }
    return false;
}

static void toggle_cat_mark(size_t idx) {
    for (size_t i = 0; i < s_marked_cat_count; i++) {
        if (s_marked_cat_indices[i] == idx) {
            for (size_t j = i; j < s_marked_cat_count - 1; j++) {
                s_marked_cat_indices[j] = s_marked_cat_indices[j + 1];
            }
            s_marked_cat_count--;
            return;
        }
    }
    if (s_marked_cat_count < 64) {
        s_marked_cat_indices[s_marked_cat_count++] = idx;
    }
}

static void clear_cat_marks() {
    s_marked_cat_count = 0;
}

static bool is_station_marked(uint32_t id) {
    for (size_t i = 0; i < s_marked_count; i++) {
        if (s_marked_ids[i] == id) return true;
    }
    return false;
}

static void toggle_mark_station(uint32_t id) {
    // Снимаем отметку, если есть
    for (size_t i = 0; i < s_marked_count; i++) {
        if (s_marked_ids[i] == id) {
            for (size_t j = i; j < s_marked_count - 1; j++) {
                s_marked_ids[j] = s_marked_ids[j + 1];
            }
            s_marked_count--;
            return;
        }
    }
    // Добавляем отметку
    if (s_marked_count < 64) {
        s_marked_ids[s_marked_count++] = id;
    }
}

static void clear_marks() {
    s_marked_count = 0;
}

// ============================================================
// Кастомный 8×16 кириллический шрифт (Terminus ter-u16n)
// ============================================================

static uint16_t s_glyph_rgb[CYR_FONT_W * CYR_FONT_H]; // 128 px = 256 bytes

static uint32_t utf8_decode(const char** pp)
{
    const unsigned char* p = (const unsigned char*)(*pp);
    uint32_t cp;
    int len;

    if (p[0] < 0x80)                { cp = p[0];           len = 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F;   len = 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F;   len = 3; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07;   len = 4; }
    else { (*pp)++; return 0xFFFD; }

    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) { (*pp)++; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }

    *pp += len;
    return cp;
}

/**
 * Рисует строку кастомным шрифтом 8×16 через pushImage().
 * Фон пикселей глифа = TFT_BLACK.
 * Возвращает итоговый x (для цепочки сегментов телеметрии).
 */
template <typename T>
static int draw_cyr_str(T& lcd, int x, int y, const char* utf8, uint16_t color)
{
    if (!utf8) return x;

    const char* p = utf8;

    while (*p) {
        uint32_t cp = utf8_decode(&p);
        int gi = cyr_font_lookup(cp);

        if (gi < 0) {
            gi = cyr_font_lookup('?');
        }

        if (gi < 0) {
            x += CYR_FONT_W;
            continue;
        }

        const uint8_t* gdata = cyr_font_data[gi];

        for (int row = 0; row < CYR_FONT_H; row++) {
            uint8_t bits = gdata[row * CYR_FONT_RB];
            for (int col = 0; col < CYR_FONT_W; col++) {
                s_glyph_rgb[row * CYR_FONT_W + col] =
                    (bits & (0x80 >> col)) ? color : TFT_BLACK;
            }
        }

        lcd.pushImage(x, y, CYR_FONT_W, CYR_FONT_H, s_glyph_rgb);
        x += CYR_FONT_W;
    }

    return x;
}

/**
 * То же, что draw_cyr_str, но с ПРОЗРАЧНЫМ фоном:
 * выключенные пиксели глифа НЕ пишутся, поэтому текст можно
 * накладывать поверх цветных полос (progress / flow / val).
 * Возвращает итоговый x.
 */
template <typename T>
static int draw_cyr_str_trans(T& lcd, int x, int y, const char* utf8, uint16_t color)
{
    if (!utf8) return x;

    const char* p = utf8;

    while (*p) {
        uint32_t cp = utf8_decode(&p);
        int gi = cyr_font_lookup(cp);

        if (gi < 0) {
            gi = cyr_font_lookup('?');
        }

        if (gi < 0) {
            x += CYR_FONT_W;
            continue;
        }

        const uint8_t* gdata = cyr_font_data[gi];

        for (int row = 0; row < CYR_FONT_H; row++) {
            uint8_t bits = gdata[row * CYR_FONT_RB];
            for (int col = 0; col < CYR_FONT_W; col++) {
                if (bits & (0x80 >> col)) {          // рисуем ТОЛЬКО «включённые» пиксели
                    lcd.drawPixel(x + col, y + row, color);
                }
            }
        }

        x += CYR_FONT_W;
    }

    return x;
}

/**
 * Цветной сегмент телеметрии (замена draw_tele_seg для кастомного шрифта).
 */
template <typename T>
static int draw_cyr_tele_seg(T& lcd, const char* text, uint16_t color, int x, int y)
{
    return draw_cyr_str(lcd, x, y, text, color);
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
// PLAY layout constants (320×240)
// ============================================================
static constexpr int PLAY_MX       = 10;
static constexpr int PLAY_W        = 300;
static constexpr int PLAY_MAX_CH = PLAY_W / CYR_FONT_W; // 300 / 8 = 37

static constexpr int PLAY_Y_CAT    = 48;
static constexpr int PLAY_Y_STN    = 68;
static constexpr int PLAY_Y_TRK    = 90;
static constexpr int PLAY_Y_FLOW   = 150;
static constexpr int PLAY_Y_TELE   = 196;
static constexpr int PLAY_Y_HINT   = 216;

static constexpr uint32_t MARQUEE_MS_PER_CHAR = 150;
static constexpr uint32_t MARQUEE_PAUSE_MS    = 700;
static constexpr uint32_t FLOW_ANIM_MS        = 120;

// ============================================================
// Marquee state (per-line)
// ============================================================
typedef struct {
    char     text[128];
    uint32_t start_ms;
    bool     active;
} marquee_state_t;

static marquee_state_t s_mq_station = {"", 0, false};
static marquee_state_t s_mq_track   = {"", 0, false};

// ============================================================
// UTF-8 helpers
// ============================================================
static int utf8_char_count(const char* s)
{
    if (!s) return 0;
    int count = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        if ((*p & 0xC0) != 0x80) count++;
        p++;
    }
    return count;
}

static int utf8_char_byte_len(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

static int utf8_copy_nth_char(const char* src, int n, char* dst, int dst_sz)
{
    if (!src || !dst || dst_sz <= 0) return 0;
    int idx = 0;
    const unsigned char* p = (const unsigned char*)src;
    while (*p) {
        int blen = utf8_char_byte_len(*p);
        if (idx == n) {
            if (blen >= dst_sz) blen = dst_sz - 1;
            memcpy(dst, p, (size_t)blen);
            dst[blen] = '\0';
            return blen;
        }
        p += blen;
        idx++;
    }
    return 0;
}

// ============================================================
// Marquee renderer
// ============================================================
template <typename T>
static void draw_marquee(T& lcd, const char* text, int x, int y,
                         int max_chars, marquee_state_t& state,
                         uint32_t now_ms, uint16_t color)
{
    if (!text) text = "";

    if (strcmp(text, state.text) != 0) {
        strlcpy(state.text, text, sizeof(state.text));
        state.start_ms = now_ms;
    }

    int total = utf8_char_count(text);
    lcd.setTextColor(color);

    if (total <= max_chars) {
        char buf[128];
        strlcpy(buf, text, sizeof(buf));

        draw_cyr_str(lcd, x, y, buf, color);

        state.active = false;
        return;
    }

    state.active = true;
    const int gap = 4;
    int cycle = total + gap;

    uint32_t elapsed = now_ms - state.start_ms;

    if (elapsed < MARQUEE_PAUSE_MS) {
        char buf[128];
        int out = 0;
        for (int i = 0; i < max_chars && i < total; i++) {
            out += utf8_copy_nth_char(text, i, buf + out, (int)sizeof(buf) - out);
        }
        buf[out] = '\0';

        draw_cyr_str(lcd, x, y, buf, color);

        return;
    }

    int offset = (int)(((elapsed - MARQUEE_PAUSE_MS) / MARQUEE_MS_PER_CHAR) % (uint32_t)cycle);

    char buf[160];
    int out = 0;
    for (int i = 0; i < max_chars; i++) {
        int idx = (offset + i) % cycle;
        if (idx < total) {
            out += utf8_copy_nth_char(text, idx, buf + out, (int)sizeof(buf) - out);
        } else {
            buf[out++] = ' ';
        }
    }
    buf[out] = '\0';

    draw_cyr_str(lcd, x, y, buf, color);
}

// ============================================================
// Flow-bar renderer
// ============================================================
template <typename T>
static void draw_flow_bar(T& lcd, int x, int y, int w, int h,
                          int net_pct, int bitrate_kbps,
                          bool starve_recent, bool overflow_recent,
                          uint32_t now_ms)
{
    // Фон полосы
    lcd.fillRoundRect(x, y, w, h, 3, TFT_BLACK);
    lcd.drawRoundRect(x, y, w, h, 3, TFT_DARKGREY);

    // Заполнение по уровню buffer/net
    int fill_w = (w * net_pct) / 100;
    if (fill_w < 0) fill_w = 0;
    if (fill_w > w) fill_w = w;

    uint16_t fill_color;
    if (net_pct == 0) {
        fill_color = TFT_RED;
    } else if (net_pct < 10) {
        fill_color = 0xFFE0; // жёлтый
    } else {
        fill_color = 0x07E0; // зелёный
    }

    if (fill_w > 0) {
        lcd.fillRoundRect(x, y, fill_w, h, 3, fill_color);
    }

    // Бегущий маркер активности
    if (net_pct > 0) {
        int marker_w = 14;
        int travel = w - marker_w;
        if (travel < 1) travel = 1;

        // Скорость маркера зависит от заявленного битрейта станции.
        // Если битрейт неизвестен — едет медленно.
        int px_per_sec = bitrate_kbps > 0 ? (bitrate_kbps * 8) : 10;

        if (px_per_sec > 300) px_per_sec = 300;
        if (px_per_sec < 20)  px_per_sec = 20;

        int marker_pos = (int)(
            ((uint64_t)now_ms * (uint64_t)px_per_sec / 1000ULL)
            % (uint32_t)travel
        );

        int mx = x + marker_pos;

        uint16_t marker_color = TFT_WHITE;

        if (starve_recent) {
            marker_color = TFT_RED;
        } else if (overflow_recent) {
            marker_color = 0xFFE0;
        }

        lcd.fillRoundRect(mx - 1, y + 2, marker_w + 2, h - 4, 3, TFT_BLACK);  // обводка
        lcd.fillRoundRect(mx,     y + 3, marker_w,     h - 6, 2, marker_color); // сам маркер
        //lcd.fillRoundRect(mx, y + 3, marker_w, h - 6, 2, marker_color);
    }

    // Мигающая рамка при starve
    if (starve_recent && ((now_ms / 300) % 2 == 0)) {
        lcd.drawRoundRect(x, y, w, h, 3, TFT_RED);
    }
}

// ============================================================
// Цветной сегмент телеметрии
// ============================================================
template <typename T>
static int draw_tele_seg(T& lcd, const char* text, uint16_t color, int x, int y)
{
    lcd.setTextColor(color);
    lcd.drawString(text, x, y);
    return utf8_char_count(text) * 8;
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
    // Wi-Fi Settings & Known Networks
    // ========================================================
    if (s_current_mode == MODE_WIFI) {
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);
        if (s_wifi_show_scan_results) lcd.drawString("Найденные сети", 10, 42);
        else if (s_keyboard_active) lcd.drawString("Ввод Wi-Fi", 10, 42);
        else lcd.drawString("Настройка Wi-Fi", 10, 42);

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
                    if (rec == nullptr) continue;
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
        } else {
            const char* labels[2] = {"SSID:", "Пароль:"};
            const char* values[2] = {s_wifi_ssid, s_wifi_pass};
            for (int i = 0; i < 2; i++) {
                bool focused = s_keyboard_active ? (s_keyboard_target == (i == 0 ? KB_TARGET_SSID : KB_TARGET_PASS)) : (s_wifi_focus == i);
                int fy = WIFI_FIELD_Y[i] - 2;
                if (focused) {
                    lcd.setTextColor(TFT_BLACK);
                    lcd.fillRoundRect(5, fy, 310, WIFI_FIELD_H, 4, 0x07E0);
                } else {
                    lcd.setTextColor(TFT_WHITE);
                    lcd.fillRoundRect(5, fy, 310, WIFI_FIELD_H, 4, TFT_BLACK);
                    lcd.drawRoundRect(5, fy, 310, WIFI_FIELD_H, 4, TFT_LIGHTGRAY);
                }
                char disp[72]; disp[0] = '\0';
                if (i == 1 && !focused) {
                    int n = (int)strlen(values[i]);
                    if (n > 30) n = 30;
                    for (int k = 0; k < n; k++) disp[k] = '*';
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

                bool cursor_here = false;
                if (s_keyboard_active) {
                    if (i == 0 && s_keyboard_target == KB_TARGET_SSID) cursor_here = true;
                    if (i == 1 && s_keyboard_target == KB_TARGET_PASS) cursor_here = true;
                }
                if (cursor_here && s_cursor_blink) {
                    int text_w = lcd.textWidth(line);
                    int cursor_x = 10 + text_w + 2;
                    if (cursor_x > 306) cursor_x = 306;
                    int cursor_y = WIFI_FIELD_Y[i] + 3;
                    int cursor_h = WIFI_FIELD_H - 8;
                    uint16_t cursor_color = focused ? TFT_BLACK : TFT_WHITE;
                    lcd.fillRect(cursor_x, cursor_y, 2, cursor_h, cursor_color);
                }
            }

            if (!s_keyboard_active) {
                const char* btn_labels[4] = {"Scan", "Connect", "Networks", "Back"};
                const int btn_x[4] = {5, 165, 5, 165};
                const int btn_y[4] = {WIFI_BTN_Y, WIFI_BTN_Y, WIFI_BTN_ROW2_Y, WIFI_BTN_ROW2_Y};

                for (int i = 0; i < 4; i++) {
                    int focus_idx = i + 2; // 2=Scan, 3=Connect, 4=Networks, 5=Back
                    bool focused = (s_wifi_focus == focus_idx);
                    int bx = btn_x[i];
                    int by = btn_y[i];

                    if (focused) {
                        lcd.setTextColor(TFT_BLACK);
                        lcd.fillRoundRect(bx, by, WIFI_BTN_W, WIFI_BTN_H, 4, 0x07E0);
                    } else {
                        lcd.setTextColor(0x07E0);
                        lcd.fillRoundRect(bx, by, WIFI_BTN_W, WIFI_BTN_H, 4, TFT_BLACK);
                        lcd.drawRoundRect(bx, by, WIFI_BTN_W, WIFI_BTN_H, 4, 0x07E0);
                    }
                    lcd.setFont(&fonts::efontCN_14);
                    lcd.setTextDatum(MC_DATUM);
                    lcd.drawString(btn_labels[i], bx + WIFI_BTN_W / 2, by + WIFI_BTN_H / 2 + 1);
                }
                lcd.setTextDatum(TL_DATUM);
            } else {
                draw_wifi_keyboard_impl(lcd);
            }
        }
    }
    // ========================================================
    // Known Networks Management
    // ========================================================
    else if (s_current_mode == MODE_KNOWN_NETWORKS) {
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);
        lcd.drawString("Управление сетями", 10, 48);

        int count = wifi_known_networks_get_count();
        if (count == 0) {
            lcd.setTextColor(TFT_LIGHTGRAY);
            lcd.drawString("Нет сохраненных сетей", 15, 80);
            lcd.drawString("Подключитесь к Wi-Fi, чтобы", 15, 100);
            lcd.drawString("добавить его в список", 15, 120);
        } else {
            int visible = 5;
            int start = s_settings_idx;
            if (start > 2) start = s_settings_idx - 2;
            if (start < 0) start = 0;

            int y = 68;
            for (int i = start; i < count && i < start + visible; i++) {
                const known_network_t* net = wifi_known_networks_get(i);
                if (!net) continue;

                if (i == s_settings_idx) {
                    lcd.setTextColor(TFT_BLACK);
                    lcd.fillRoundRect(5, y - 2, 310, 20, 4, 0x07E0);
                } else {
                    lcd.setTextColor(TFT_WHITE);
                }

                char line[64];
                snprintf(line, sizeof(line), "%s %s", net->auto_connect ? "[A]" : "[ ]", net->ssid);
                lcd.drawString(line, 15, y);
                y += 32;
            }

            // ИСПРАВЛЕНИЕ: используем efontCN_14 вместо Font0 для поддержки кириллицы
            lcd.setTextColor(TFT_LIGHTGRAY);
            lcd.setFont(&fonts::efontCN_14);
            lcd.drawString("A: Удалить сеть | B: Назад", 10, 216);
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
            // Нижняя подсказка: долгим нажатием удаляем (в т.ч. «пенёчки»)
            lcd.setFont(&fonts::efontCN_14);
            lcd.setTextColor(TFT_DARKGREY);
            lcd.drawString("Enc-long: удалить (вкл. [удалено])", 15, 216);
        } else {
            for (int i = start_idx; i < fav_count && i < start_idx + visible_items; i++) {
                int global_idx = catalog_get_favorite_global_index(i);
                const catalog_station_t* st = catalog_get_station_by_global_index(global_idx);
                if (st == nullptr) {
                    // Станция удалена из каталога — рисуем заглушку, чтобы y не «прыгал»
                    if (i == s_fav_station_idx) {
                        lcd.setTextColor(TFT_BLACK);
                        lcd.fillRoundRect(5, y - 5, 310, 24, 4, TFT_DARKGREY);
                    } else {
                        lcd.setTextColor(TFT_DARKGREY);
                    }
                    lcd.setFont(&fonts::efontCN_14);
                    lcd.drawString("[удалено]", 15, y);
                    y += 28;
                    continue;
                } // Станция могла быть удалена из каталога
                // --- Обработка статуса для избранного ---
                char fav_display_name[128];
                uint16_t base_color = TFT_WHITE;
                const char* suffix = "";

                if (st->status == CAT_STATUS_SUSPECT) {
                    suffix = " [?]";
                    base_color = 0xFFE0; // Желтый
                } else if (st->status == CAT_STATUS_DEAD) {
                    suffix = " [X]";
                    base_color = 0x8430; // Темно-красный/серый
                }

                snprintf(fav_display_name, sizeof(fav_display_name), "%s%s", st->name, suffix);
                // ----------------------------------------
                if (i == s_fav_station_idx) {
                    lcd.setTextColor(TFT_BLACK);
                    lcd.fillRoundRect(5, y - 5, 310, 24, 4, 0x07E0);
                } else {
                    lcd.setTextColor(base_color);
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
                lcd.fillRoundRect(5, y - 2, 310, 20, 4, 0x07E0);
            } else {
                lcd.setTextColor(TFT_WHITE);
            }

            // Для пункта "2. Catalog" показываем прогресс валидации
            if (i == 1 && catalog_is_validating()) {
                int done = catalog_get_validated_count();
                int total = catalog_get_staging_count();
                int pct = (total > 0) ? (done * 100 / total) : 0;
                char line[80];
                snprintf(line, sizeof(line), "Catalog - %d/%d (%d%%)", done, total, pct);
                lcd.drawString(line, 15, y);
            } else {
                lcd.drawString(items[i], 15, y);
            }

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
    // Catalog Management (Финальная версия с прогресс-баром)
    // ========================================================

	else if (s_current_mode == MODE_CATALOG) {
		lcd.setFont(&fonts::efontCN_14);
		lcd.setTextColor(0x07E0);
		lcd.drawString("Управление каталогом", 10, 48);

		// 1. Если идет слияние (Merge), показываем прогресс
		if (catalog_is_merging()) {

			catalog_merge_phase_t phase = catalog_get_merge_phase();
			int pct = catalog_get_merge_progress();

			static uint32_t last_log_ms = 0;
			uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

			if (now_ms - last_log_ms > 1000) {
				last_log_ms = now_ms;
				ESP_LOGI("UI_DRAW", "Merge draw active, pct=%d", pct);
			}

			const char *msg = "Слияние каталога...";

			if (phase == CATALOG_MERGE_PHASE_SAVE) {
				msg = "Сохранение каталога на SD...";
			} else if (phase == CATALOG_MERGE_PHASE_DONE) {
				msg = "Завершение...";
			}

			lcd.setFont(&fonts::efontCN_14);
			lcd.setTextColor(TFT_WHITE);
			lcd.drawString(msg, 15, 80);

			lcd.drawRoundRect(15, 110, 290, 24, 6, TFT_DARKGREY);

			int bar_w = (290 * pct) / 100;
			if (bar_w > 0) {
				lcd.fillRoundRect(15, 110, bar_w, 24, 6, 0x07E0);
			}

			char buf[48];
			snprintf(buf, sizeof(buf), "%d%%", pct);

			lcd.setTextDatum(MC_DATUM);
			lcd.setTextColor(TFT_WHITE);
			lcd.drawString(buf, 160, 122);
			lcd.setTextDatum(TL_DATUM);

			lcd.setTextColor(TFT_LIGHTGRAY);
			lcd.drawString("Радио продолжает играть...", 15, 150);

			return;
		}
		catalog_updater_phase_t uph = catalog_updater_get_phase();

		// ---- Экраны апдейтера (приоритет над меню) ----
		if (uph == UPD_DOWNLOADING) {
			lcd.setTextColor(TFT_WHITE);
			lcd.drawString("Загрузка каталога...", 15, 80);

			const upd_source_t *srcs = NULL;
			int nsrc = 0;
			srcs = catalog_updater_get_sources(&nsrc);
			if (s_upd_source_idx < nsrc) {
				lcd.setTextColor(TFT_LIGHTGRAY);
				lcd.drawString(srcs[s_upd_source_idx].url, 15, 100);
			}

			size_t dl = catalog_updater_get_bytes_downloaded();
			size_t tot = catalog_updater_get_bytes_total();
			int pct = catalog_updater_get_progress();

			lcd.drawRoundRect(15, 130, 290, 24, 6, TFT_DARKGREY);
			int bw = (290 * pct) / 100;
			if (bw > 0)
				lcd.fillRoundRect(15, 130, bw, 24, 6, 0x07E0);

			char buf[48];
			snprintf(buf, sizeof(buf), "%u / %u KB (%d%%)",
					(unsigned) (dl / 1024), (unsigned) (tot / 1024), pct);
			lcd.setTextDatum(MC_DATUM);

			// Тень
			lcd.setTextColor(TFT_BLACK);
			lcd.drawString(buf, 161, 143);

			// Текст
			lcd.setTextColor(TFT_WHITE);
			lcd.drawString(buf, 160, 142);
			lcd.setTextDatum(TL_DATUM);
			return;
		} else if (uph == UPD_CONVERTING) {
			lcd.setTextColor(TFT_WHITE);
			lcd.drawString("Конвертация SII → M3U...", 15, 80);
			char buf[48];
			snprintf(buf, sizeof(buf), "Станций: %d",
					catalog_updater_get_stations_parsed());
			lcd.setTextColor(TFT_LIGHTGRAY);
			lcd.drawString(buf, 15, 110);
			return;
		} else if (uph == UPD_IMPORTING) {
			lcd.setTextColor(TFT_WHITE);
			lcd.drawString("Импорт в staging...", 15, 80);
			return;
		} else if (uph == UPD_ASK_VALIDATE) {
			lcd.setTextColor(TFT_WHITE);
			char buf[64];
			snprintf(buf, sizeof(buf), "Загружено: %d станций",
					catalog_updater_get_stations_parsed());
			lcd.drawString(buf, 15, 80);

			lcd.setTextColor(TFT_LIGHTGRAY);
			lcd.drawString("Проверить перед применением?", 15, 110);

			lcd.setTextColor(0x07E0);
			lcd.drawString("A: Применить сразу", 15, 150);
			lcd.drawString("B: Проверить", 15, 175);
			lcd.setTextColor(TFT_DARKGREY);
			char hint[64];
			snprintf(hint, sizeof(hint), "(Проверка ~%d станций, долго)",
					catalog_updater_get_stations_parsed());
			lcd.drawString(hint, 15, 198);

			return;
		} else if (uph == UPD_VALIDATING) {
			// Переиспользуем существующий validation-экран
			int done = catalog_get_validated_count();
			int total = catalog_get_staging_count();
			int pct = (total > 0) ? (done * 100 / total) : 0;
			lcd.setTextColor(TFT_WHITE);
			lcd.drawString("Проверка живых станций...", 15, 80);
			lcd.drawRoundRect(15, 110, 290, 24, 6, TFT_DARKGREY);
			int bw = (290 * pct) / 100;
			if (bw > 0)
				lcd.fillRoundRect(15, 110, bw, 24, 6, 0x07E0);
			char buf[48];
			snprintf(buf, sizeof(buf), "%d / %d (%d%%)", done, total, pct);
			lcd.setTextDatum(MC_DATUM);

			// Тень
			lcd.setTextColor(TFT_BLACK);
			lcd.drawString(buf, 161, 123);

			// Текст
			lcd.setTextColor(TFT_WHITE);
			lcd.drawString(buf, 160, 122);
			lcd.setTextDatum(TL_DATUM);
			return;
		} else if (uph == UPD_MERGING) {
			catalog_merge_phase_t phase = catalog_get_merge_phase();
			int pct = catalog_get_merge_progress();

			const char *msg = "Слияние каталога...";
			if (phase == CATALOG_MERGE_PHASE_SAVE) {
				msg = "Сохранение каталога на SD...";
			} else if (phase == CATALOG_MERGE_PHASE_DONE) {
				msg = "Завершение...";
			}

			lcd.setTextColor(TFT_WHITE);
			lcd.drawString(msg, 15, 80);

			lcd.drawRoundRect(15, 110, 290, 24, 6, TFT_DARKGREY);

			int bar_w = (290 * pct) / 100;
			if (bar_w > 0) {
				lcd.fillRoundRect(15, 110, bar_w, 24, 6, 0x07E0);
			}

			char buf[48];
			snprintf(buf, sizeof(buf), "%d%%", pct);

			lcd.setTextDatum(MC_DATUM);
			lcd.setTextColor(TFT_WHITE);

			// Тень
			lcd.setTextColor(TFT_BLACK);
			lcd.drawString(buf, 161, 123);

			// Текст
			lcd.drawString(buf, 160, 122);
			lcd.setTextDatum(TL_DATUM);

			lcd.setTextColor(TFT_LIGHTGRAY);
			lcd.drawString("Радио продолжает играть...", 15, 150);

			return;
		} else if (uph == UPD_DONE) {
			updater_result_t res;
			catalog_updater_get_result(&res);

			bool nothing_changed = (res.added == 0 && res.updated == 0
					&& res.removed == 0);

			if (nothing_changed) {
				lcd.setTextColor(TFT_LIGHTGRAY);
				lcd.drawString("Каталог актуален", 15, 80);

				lcd.setTextColor(TFT_WHITE);

				char buf[80];
				snprintf(buf, sizeof(buf), "Проверено: %d станций из источника",
						res.staging_count);
				lcd.drawString(buf, 15, 105);

				snprintf(buf, sizeof(buf),
						"Все уже есть в каталоге (%d станций)",
						res.total_after);
				lcd.drawString(buf, 15, 125);
			} else {
				lcd.setTextColor(0x07E0);
				lcd.drawString("Каталог обновлён!", 15, 80);

				lcd.setTextColor(TFT_WHITE);

				char buf[80];
				snprintf(buf, sizeof(buf), "Было: %d   Стало: %d",
						res.total_before, res.total_after);
				lcd.drawString(buf, 15, 110);

				snprintf(buf, sizeof(buf),
						"Добавлено: %d  Обновлено: %d  Удалено: %d", res.added,
						res.updated, res.removed);
				lcd.drawString(buf, 15, 135);
			}

			lcd.setTextColor(TFT_LIGHTGRAY);
			lcd.drawString("A: ОК", 15, 180);
			return;
		} else if (uph == UPD_ERROR) {
			lcd.setTextColor(TFT_RED);
			lcd.drawString("Ошибка обновления", 15, 80);
			lcd.setTextColor(TFT_LIGHTGRAY);
			lcd.drawString(catalog_updater_get_error(), 15, 110);
			lcd.drawString("A: ОК", 15, 180);
			return;
		}

		// ---- Подменю выбора источника (s_catalog_submode == 1) ----
		if (s_catalog_submode == 1) {
			const upd_source_t *srcs = NULL;
			int nsrc = 0;
			srcs = catalog_updater_get_sources(&nsrc);

			lcd.setTextColor(TFT_WHITE);
			lcd.drawString("Выберите источник:", 15, 70);

			int y = 100;
			for (int i = 0; i < nsrc; i++) {
				if (i == s_upd_source_idx) {
					lcd.setTextColor(TFT_BLACK);
					lcd.fillRoundRect(5, y - 4, 310, 24, 4, 0x07E0);
				} else {
					lcd.setTextColor(TFT_WHITE);
				}
				char line[64];
				snprintf(line, sizeof(line), "%s (%s)", srcs[i].label,
						srcs[i].cc);
				lcd.drawString(line, 15, y);
				y += 28;
			}

            uint32_t now_upd = (uint32_t)(esp_timer_get_time() / 1000);

            if (s_upd_refuse_ms != 0 && (now_upd - s_upd_refuse_ms) < 1500) {
                lcd.setTextColor(TFT_RED);
                lcd.drawString("Нет сети или идёт операция", 15, 216);
            } else {
                lcd.setTextColor(TFT_DARKGREY);
                lcd.drawString("Enc: выбрать источник", 15, 196);
                lcd.setTextColor(TFT_LIGHTGRAY);
                lcd.drawString("A: Назад   B: Загрузить", 15, 216);
            }
            return;
		}
	// 2. Если идет глубокая валидация
	if (catalog_is_deep_validating()) {
		int done = catalog_get_deep_validated_count();
		int total = catalog_get_suspect_count() + done;
		if (total == 0)
			total = 1;
		int pct = (done * 100) / total;
		lcd.setTextColor(TFT_WHITE);
		lcd.drawString("Глубокая проверка SUSPECT...", 15, 80);
		lcd.drawRoundRect(15, 110, 290, 24, 6, TFT_DARKGREY);
		int bar_w = (290 * pct) / 100;
		if (bar_w > 0) {
			lcd.fillRoundRect(15, 110, bar_w, 24, 6, 0x07E0);
		}
		char buf[48];
		snprintf(buf, sizeof(buf), "%d / %d (%d%%)", done, total, pct);
		lcd.setTextDatum(MC_DATUM);
		lcd.setTextColor(TFT_WHITE);
		lcd.drawString(buf, 160, 122);
		lcd.setTextDatum(TL_DATUM);
		lcd.setTextColor(TFT_LIGHTGRAY);
		lcd.drawString("Можно слушать радио...", 15, 150);
		return;
	}

	// 3. Если идет обычная валидация
	if (catalog_is_validating()) {
		int done = catalog_get_validated_count();
		int total = catalog_get_staging_count();
		int pct = (total > 0) ? (done * 100 / total) : 0;
		lcd.setTextColor(TFT_WHITE);
		lcd.drawString("Проверка живых станций...", 15, 80);
		lcd.drawRoundRect(15, 110, 290, 24, 6, TFT_DARKGREY);
		int bar_w = (290 * pct) / 100;
		if (bar_w > 0) {
			lcd.fillRoundRect(15, 110, bar_w, 24, 6, 0x07E0);
		}
		char buf[48];
		snprintf(buf, sizeof(buf), "%d / %d (%d%%)", done, total, pct);
		//lcd.setTextDatum(MC_DATUM);
		//lcd.setTextColor(TFT_WHITE);
		//lcd.drawString(buf, 160, 122);
		//lcd.setTextDatum(TL_DATUM);

		lcd.setTextDatum(MC_DATUM);

		// Тень
		lcd.setTextColor(TFT_BLACK);
		lcd.drawString(buf, 161, 123);

		// Текст
		lcd.setTextColor(TFT_WHITE);
		lcd.drawString(buf, 160, 122);

		lcd.setTextDatum(TL_DATUM);

		lcd.setTextColor(TFT_LIGHTGRAY);
		lcd.drawString("Можно слушать радио...", 15, 150);
		return;
	}

		// 4. Обычное меню (строго синхронизировано с обработчиком ниже!)
		lcd.setFont(&fonts::efontCN_14);
		lcd.setTextColor(TFT_WHITE);

		int staging_count = catalog_get_staging_count();
		int alive_count = catalog_get_alive_count();
		int suspect_count = catalog_get_suspect_count();
		int dead_count = catalog_get_dead_count();

		char line[96];
		snprintf(line, sizeof(line), "В буфере: %d (A:%d S:%d D:%d)",
				staging_count, alive_count, suspect_count, dead_count);
		lcd.drawString(line, 15, 68); // Подняли выше

		int y = 84; // Подняли начало меню
		const char *items[] = { "1. Импортировать M3U с SD",          // index 0
				"2. Проверить живые (Fast)",          // index 1
				"3. Глубоко проверить SUSPECT",       // index 2
				"4. Применить изменения (Merge)",     // index 3
				"5. Зачистить мертвые станции",       // index 4
				"6. Обновить из сети",                // index 5
				"7. Назад" };
		int count = 7;
		for (int i = 0; i < count; i++) {
			if (i == s_settings_idx) {
				lcd.setTextColor(TFT_BLACK);
				lcd.fillRoundRect(5, y - 3, 310, 22, 4, 0x07E0); // было y-4 / h=24
			} else {
				lcd.setTextColor(TFT_WHITE);
			}
			lcd.drawString(items[i], 15, y);
			y += 22;                       // было 24
		}
	}

    // ========================================================
    // Playback
    // ========================================================
    if (s_current_mode == MODE_PLAYBACK) {

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        //lcd.setFont(&fonts::efontCN_14);

        // ---- 1. Категория (слева) + Badge bitrate/codec (справа) ----
        {
            const catalog_category_t* cat = catalog_get_category(s_selected_category);
            const char* cat_name = cat ? cat->name : "-";

            char badge[40] = "";
            if (s_bitrate_kbps > 0 && s_codec[0]) {
                snprintf(badge, sizeof(badge), "%dk %s", s_bitrate_kbps, s_codec);
            } else if (s_bitrate_kbps > 0) {
                snprintf(badge, sizeof(badge), "%dk", s_bitrate_kbps);
            } else if (s_codec[0]) {
                strlcpy(badge, s_codec, sizeof(badge));
            }

            int badge_chars = utf8_char_count(badge);
            int cat_max = PLAY_MAX_CH - badge_chars - 2;
            if (cat_max < 6) cat_max = 6;

            //lcd.setTextColor(TFT_LIGHTGRAY);
            char cat_buf[64];
            int cat_len = utf8_char_count(cat_name);
            if (cat_len <= cat_max) {
                strlcpy(cat_buf, cat_name, sizeof(cat_buf));
            } else {
                int out = 0;
                for (int i = 0; i < cat_max - 3 && i < cat_len; i++) {
                    out += utf8_copy_nth_char(cat_name, i, cat_buf + out, (int)sizeof(cat_buf) - out);
                }
                cat_buf[out++] = '.';
                cat_buf[out++] = '.';
                cat_buf[out++] = '.';
                cat_buf[out] = '\0';
            }
            //fix_cyr_temp(cat_buf);
            //lcd.drawString(cat_buf, PLAY_MX, PLAY_Y_CAT);
            draw_cyr_str(lcd, PLAY_MX, PLAY_Y_CAT, cat_buf, TFT_LIGHTGRAY);

            //if (badge[0]) {
            //    lcd.setTextColor(0x07E0);
            //    lcd.setTextDatum(TR_DATUM);
            //    lcd.drawString(badge, 320 - PLAY_MX, PLAY_Y_CAT);
            //    lcd.setTextDatum(TL_DATUM);
            //}

            if (badge[0]) {
                int bw = (int)strlen(badge) * CYR_FONT_W;
                draw_cyr_str(lcd, 320 - PLAY_MX - bw, PLAY_Y_CAT, badge, 0x07E0);
            }
        }

        // ---- 2. Название станции ----
        if (s_switch_phase != 0) {
            const char* switch_msg;

            if (s_switch_phase == 1) {
                switch_msg = "Затухание...";          // FADING_OUT (120 мс)
            } else if (s_switch_phase >= 2 && s_switch_phase <= 4) {
                switch_msg = "Подключение...";        // DRAINING + STARTING + WAITING_DATA
            } else {
                switch_msg = "Воспроизведение...";    // FADING_IN (120 мс)
            }

            draw_cyr_str(lcd, PLAY_MX, PLAY_Y_STN, switch_msg, TFT_YELLOW);
        } else {
            const char* base = s_station_name;
            if (s_icy_name[0] != '\0') {
                base = s_icy_name;
            }

            char stn_line[128];
            uint16_t stn_color = TFT_WHITE;

            if (strstr(s_station_name, " [?]") != nullptr) {
                snprintf(stn_line, sizeof(stn_line), "%s [?]", base);
                stn_color = 0xFFE0;
            } else if (strstr(s_station_name, " [X]") != nullptr) {
                snprintf(stn_line, sizeof(stn_line), "%s [X]", base);
                stn_color = 0x8430;
            } else {
                strlcpy(stn_line, base, sizeof(stn_line));
            }

            draw_marquee(lcd, stn_line, PLAY_MX, PLAY_Y_STN,
                         PLAY_MAX_CH, s_mq_station, now_ms, stn_color);
        }



        // ---- 3. StreamTitle (бегущая строка) ----

        {
            if (s_switch_phase != 0) {
                draw_cyr_str(lcd, PLAY_MX, PLAY_Y_TRK, "...", TFT_GREEN);
            } else {
                {
                    const char* trk = s_track_title;
                    if (!trk || !trk[0]) trk = "...";

                    draw_marquee(lcd, trk, PLAY_MX, PLAY_Y_TRK,
                                 PLAY_MAX_CH, s_mq_track, now_ms, TFT_GREEN);
                }
            }
        }


        // ================================================================
        // 4. Flow-bar
        // ================================================================
        {
            int net_pct = 0;
            if (s_buffer_target > 0) {
                net_pct = (int)((s_buffer_level * 100) / s_buffer_target);
                if (net_pct > 100) net_pct = 100;
            }

            bool starve_recent   = (s_starve_count > 0);
            bool overflow_recent = (s_overflow_count > 0);

            if (catalog_is_validating()) {
                int done  = catalog_get_validated_count();
                int total = catalog_get_staging_count();
                int pct   = (total > 0) ? (done * 100 / total) : 0;

                lcd.fillRoundRect(PLAY_MX, PLAY_Y_FLOW, PLAY_W, 16, 3, TFT_BLACK);
                lcd.drawRoundRect(PLAY_MX, PLAY_Y_FLOW, PLAY_W, 16, 3, TFT_DARKGREY);

                int vw = (PLAY_W * pct) / 100;
                if (vw > 0) {
                    lcd.fillRoundRect(PLAY_MX, PLAY_Y_FLOW, vw, 16, 3, 0x07E0);
                }

                char vbuf[48];
                snprintf(vbuf, sizeof(vbuf), "Val %d/%d (%d%%)", done, total, pct);

                //lcd.setTextColor(TFT_WHITE);
                //lcd.setTextDatum(MC_DATUM);
                //lcd.drawString(vbuf, PLAY_MX + PLAY_W / 2, PLAY_Y_FLOW + 8);
                //lcd.setTextDatum(TL_DATUM);

                //{
                //    int vw = (int)strlen(vbuf) * CYR_FONT_W;
                //    draw_cyr_str_trans(lcd, PLAY_MX + (PLAY_W - vw) / 2, PLAY_Y_FLOW, vbuf, TFT_WHITE);
                //}
                {
                    int vw = (int)strlen(vbuf) * CYR_FONT_W;
                    int tx = PLAY_MX + (PLAY_W - vw) / 2;
                    int ty = PLAY_Y_FLOW;
                    draw_cyr_str_trans(lcd, tx + 1, ty + 1, vbuf, TFT_BLACK);   // тень
                    draw_cyr_str_trans(lcd, tx,     ty,     vbuf, TFT_WHITE);   // текст
                }

            } else {
                draw_flow_bar(lcd,
                              PLAY_MX,
                              PLAY_Y_FLOW,
                              PLAY_W,
                              16,
                              net_pct,
                              s_bitrate_kbps,
                              starve_recent,
                              overflow_recent,
                              now_ms);
            }
        }

        // ---- 5. Телеметрия: Buf / Net / OVR / STR (цветная, одна строка) ----
        {
            int buf_pct = 0;
            if (s_buffer_target > 0) {
                buf_pct = (int)((s_buffer_level * 100) / s_buffer_target);
                if (buf_pct > 100) buf_pct = 100;
            }
            int buf_kb  = (int)(s_buffer_level / 1024);
            int net_pct = buf_pct;

            uint16_t buf_color;
            if (buf_pct >= 60)      buf_color = 0x07E0;
            else if (buf_pct >= 30) buf_color = 0xFFE0;
            else                    buf_color = TFT_RED;

            uint16_t net_color;
            if (net_pct == 0)       net_color = TFT_RED;
            else if (net_pct < 10)  net_color = 0xFFE0;
            else                    net_color = 0x07E0;

            uint16_t ovr_color = (s_overflow_count > 0) ? TFT_RED : TFT_DARKGREY;
            uint16_t str_color = (s_starve_count  > 0) ? TFT_RED : TFT_DARKGREY;

            char ovr_str[8], str_str[8];
            if (s_overflow_count > 99) strlcpy(ovr_str, "99+", sizeof(ovr_str));
            else snprintf(ovr_str, sizeof(ovr_str), "%lu", (unsigned long)s_overflow_count);
            if (s_starve_count > 99) strlcpy(str_str, "99+", sizeof(str_str));
            else snprintf(str_str, sizeof(str_str), "%lu", (unsigned long)s_starve_count);

            char buf_pct_str[16], buf_kb_str[16], net_pct_str[16];
            snprintf(buf_pct_str, sizeof(buf_pct_str), "%d%%", buf_pct);
            snprintf(buf_kb_str,  sizeof(buf_kb_str),  "%dK",  buf_kb);
            snprintf(net_pct_str, sizeof(net_pct_str), "%d%%", net_pct);

            int tx = PLAY_MX;
            tx += draw_cyr_tele_seg(lcd, "Buf ",  TFT_LIGHTGRAY, tx, PLAY_Y_TELE);
            tx += draw_cyr_tele_seg(lcd, buf_pct_str, buf_color,  tx, PLAY_Y_TELE);
            tx += draw_cyr_tele_seg(lcd, " ",     TFT_LIGHTGRAY, tx, PLAY_Y_TELE);
            tx += draw_cyr_tele_seg(lcd, buf_kb_str,  TFT_LIGHTGRAY, tx, PLAY_Y_TELE);
            tx += draw_cyr_tele_seg(lcd, " | ",   TFT_DARKGREY,  tx, PLAY_Y_TELE);
            tx += draw_cyr_tele_seg(lcd, "Net ", TFT_LIGHTGRAY, tx, PLAY_Y_TELE);
            tx += draw_cyr_tele_seg(lcd, net_pct_str, net_color, tx, PLAY_Y_TELE);
            tx += draw_cyr_tele_seg(lcd, " | ",  TFT_DARKGREY,  tx, PLAY_Y_TELE);
            tx += draw_cyr_tele_seg(lcd, "O:",   ovr_color,     tx, PLAY_Y_TELE);
            tx += draw_cyr_tele_seg(lcd, ovr_str, ovr_color,    tx, PLAY_Y_TELE);
            tx += draw_cyr_tele_seg(lcd, " S:",  str_color,     tx, PLAY_Y_TELE);
            tx += draw_cyr_tele_seg(lcd, str_str, str_color,    tx, PLAY_Y_TELE);
        }

        // ---- 6. Подсказки ----
        //lcd.setTextColor(TFT_DARKGREY);
        //lcd.drawString("A:Play  B:Mode  Enc:Vol", PLAY_MX, PLAY_Y_HINT);

        //draw_cyr_str(lcd, PLAY_MX, PLAY_Y_HINT, "A:Play  B:Mode  Enc:Vol", TFT_DARKGREY);

        // E5: тост избранного (2 с) поверх подсказки
        {
            uint32_t now_fav = (uint32_t)(esp_timer_get_time() / 1000);
            if (s_fav_toast_ms != 0 && (now_fav - s_fav_toast_ms) < 2000) {
                draw_cyr_str(lcd, PLAY_MX, PLAY_Y_HINT,
                             s_fav_toast_text, 0x07E0);
            } else {
                draw_cyr_str(lcd, PLAY_MX, PLAY_Y_HINT,
                             "A:Play B:Mode Enc:Vol Long:Fav",
                             TFT_DARKGREY);
            }
        }

        return;
    }

    // ========================================================
    // Categories (со скроллингом и режимом удаления)
    // ========================================================
    else if (s_current_mode == MODE_CATEGORIES) {
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);

        // Заголовок: в режиме выделения показываем счётчик
        if (s_cat_selection_mode) {
            char title[80];
            snprintf(title, sizeof(title), "Категории [ВЫДЕЛЕНИЕ: %d]", (int)s_marked_cat_count);
            lcd.drawString(title, 10, 48);
        } else {
            lcd.drawString("Выберите категорию:", 10, 48);
        }

        int visible_items = 5;
        int cat_count = (int)catalog_category_count(); // <-- ПРАВИЛЬНАЯ ПЕРЕМЕННАЯ
        int total_items = cat_count + 1; // +1 для служебного пункта

        int start_idx = s_selected_category;
        if (start_idx > 2) start_idx = s_selected_category - 2;
        if (start_idx < 0) start_idx = 0;

        int y = 68;

        // <-- ПРАВИЛЬНЫЙ ЦИКЛ ДЛЯ КАТЕГОРИЙ -->
        for (int i = start_idx; i < total_items && i < start_idx + visible_items; i++) {
            if (i < cat_count) {
                // ---- Обычная категория ----
                const catalog_category_t* c = catalog_get_category(i);
                if (c == nullptr) continue;

                bool is_marked = is_cat_marked((size_t)i);

                if (i == s_selected_category) {
                    lcd.setTextColor(TFT_BLACK);
                    lcd.fillRoundRect(5, y - 5, 310, 26, 4, 0x07E0);
                } else {
                    lcd.setTextColor(is_marked ? TFT_RED : TFT_WHITE);
                }

                char display_name[CATALOG_MAX_CATEGORY_LEN + 16];
                if (s_cat_selection_mode) {
                    snprintf(display_name, sizeof(display_name), is_marked ? "[x] %s" : "[ ] %s", c->name);
                } else {
                    snprintf(display_name, sizeof(display_name), "%s", c->name);
                }
                lcd.drawString(display_name, 15, y);
            } else {
                // ---- Служебный пункт ----
                if (i == s_selected_category) {
                    lcd.setTextColor(TFT_BLACK);
                    lcd.fillRoundRect(5, y - 5, 310, 26, 4, TFT_YELLOW);
                } else {
                    lcd.setTextColor(TFT_YELLOW);
                }

                char label[64];
                if (s_cat_selection_mode) {
                    if (s_marked_cat_count > 0) {
                        snprintf(label, sizeof(label), "Удалить отмеченные (%d)", (int)s_marked_cat_count);
                    } else {
                        snprintf(label, sizeof(label), "Выход из режима выделения");
                    }
                } else {
                    snprintf(label, sizeof(label), ">> Режим выделения");
                }
                lcd.drawString(label, 15, y);
            }
            y += 32;
        }
        // Тост «Сохранено» после удаления
        uint32_t now_cat = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_cat_saved_ms != 0 && (now_cat - s_cat_saved_ms) < 2000) {
            lcd.setFont(&fonts::efontCN_14);
            lcd.setTextColor(0x07E0);
            lcd.drawString("Сохранено на SD", 15, 216);
        }
    }

    // ========================================================
    // Stations (со скроллингом, избранным и режимом выделения)
    // ========================================================
    else if (s_current_mode == MODE_STATIONS) {
        const catalog_category_t* cur_cat = catalog_get_category(s_selected_category);
        lcd.setFont(&fonts::efontCN_14);
        lcd.setTextColor(0x07E0);

        if (s_selection_mode) {
            char title[80];
            snprintf(title, sizeof(title), "%s [ВЫДЕЛЕНИЕ: %d]",
                     cur_cat ? cur_cat->name : "---", (int)s_marked_count);
            lcd.drawString(title, 10, 48);
        } else {
            lcd.drawString(cur_cat ? cur_cat->name : "---", 10, 48);
        }

        int visible_items = 6;
        int station_count = cur_cat ? (int)cur_cat->count : 0;

        // 1. Находим "якорь" для отрисовки.
        // Идем назад от выбранного элемента, чтобы он гарантированно попал в окно из visible_items элементов.
        int start_draw_idx = s_ui_station_idx;
        int valid_count = 0;

        while (start_draw_idx > 0) {
            int check_idx = start_draw_idx - 1;
            bool is_valid = false;

            if (check_idx < station_count) {
                const catalog_station_t* st = catalog_get_station(s_selected_category, check_idx);
                if (st != nullptr && st->status != CAT_STATUS_DEAD) {
                    is_valid = true;
                }
            } else if (check_idx == station_count) {
                is_valid = true; // Служебная строка ВСЕГДА считается валидным элементом для отрисовки
            }

            if (is_valid) {
                valid_count++;
                if (valid_count >= visible_items) {
                    break;
                }
            }
            start_draw_idx = check_idx;
        }

        int y = 68;
        int visible_drawn = 0;
        int current_raw_idx = start_draw_idx;

        // 2. Рисуем элементы, пока не заполним экран или не кончится список
        while (visible_drawn < visible_items && current_raw_idx <= station_count) {

            // Служебный пункт
            if (current_raw_idx == station_count) {
                bool is_service_selected = (s_ui_station_idx == station_count);
                if (is_service_selected) {
                    lcd.setTextColor(TFT_BLACK);
                    lcd.fillRoundRect(5, y - 5, 310, 24, 4, 0x07E0);
                } else {
                    lcd.setTextColor(TFT_YELLOW);
                }
                lcd.setFont(&fonts::efontCN_14);
                char label[64];
                if (s_selection_mode) {
                    if (s_marked_count > 0) snprintf(label, sizeof(label), "Удалить отмеченные (%d)", (int)s_marked_count);
                    else snprintf(label, sizeof(label), "Выход из режима выделения");
                } else {
                    snprintf(label, sizeof(label), ">> Режим выделения");
                }
                lcd.drawString(label, 15, y);
                y += 28;
                visible_drawn++;
                current_raw_idx++;
                continue;
            }

            // Обычная станция
            const catalog_station_t* st = catalog_get_station(s_selected_category, current_raw_idx);
            current_raw_idx++; // Сразу двигаем индекс для следующей итерации

            if (st == nullptr || st->status == CAT_STATUS_DEAD) {
                continue; // Пропускаем мертвые, не увеличивая visible_drawn и y
            }

            bool is_fav = catalog_is_favorite(st->id);
            bool is_marked = is_station_marked(st->id);

            const char* status_suffix = "";
            uint16_t base_color = TFT_WHITE;

            if (st->status == CAT_STATUS_SUSPECT) {
                status_suffix = " [?]";
                base_color = 0xFFE0;
            } else if (is_marked) {
                base_color = 0xF800;
            }

            char final_name[128];
            if (s_selection_mode) {
                snprintf(final_name, sizeof(final_name), "%s %s%s", is_marked ? "[x]" : "[ ]", st->name, status_suffix);
            } else {
                snprintf(final_name, sizeof(final_name), "%s %s%s", is_fav ? "★" : " ", st->name, status_suffix);
            }

            // Подсветка: current_raw_idx уже инкрементирован, поэтому вычитаем 1
            if ((current_raw_idx - 1) == s_ui_station_idx) {
                lcd.setTextColor(TFT_BLACK);
                lcd.fillRoundRect(5, y - 5, 310, 24, 4, 0x07E0);
            } else {
                lcd.setTextColor(base_color);
            }

            lcd.setFont(&fonts::efontCN_16);
            lcd.drawString(final_name, 15, y);

            y += 28;
            visible_drawn++;
        }
        uint32_t now_sta = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_cat_saved_ms != 0 && (now_sta - s_cat_saved_ms) < 2000) {
            lcd.setFont(&fonts::efontCN_14);
            lcd.setTextColor(0x07E0);
            lcd.drawString("Сохранено на SD", 15, 216);
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
    // Headphone detection (опрос каждые 200 мс внутри функции)
    // ========================================================
    audio_output_poll();

    /*
     * При смене активного выхода громкость раздельная,
     * поэтому отображаемое значение тоже должно измениться.
     */
    {
        static bool s_last_hp = false;
        static bool s_hp_init = false;

        bool hp = audio_output_is_headphone();

        if (!s_hp_init) {
            s_hp_init = true;
            s_last_hp = hp;
            ui_set_volume(audio_output_get_volume());
        } else if (hp != s_last_hp) {
            s_last_hp = hp;
            ui_set_volume(audio_output_get_volume());
        }
    }

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

    if (catalog_is_merging()) {
        last_btn_a = btn_a;
        last_btn_b = btn_b;
        enc_is_pressed = btn_enc;

        // Принудительно перерисовываем UI во время merge,
        // но не слишком часто, чтобы не грузить SPI-дисплей.
        static uint32_t s_last_merge_draw_ms = 0;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        if (s_last_merge_draw_ms == 0 ||
            (now_ms - s_last_merge_draw_ms) >= 80) {

            s_last_merge_draw_ms = now_ms;
            draw_ui();
        }

        return;
    }

    // ============================================================
    // Принудительная перерисовка во время catalog_updater
    // ============================================================
    catalog_updater_phase_t uph = catalog_updater_get_phase();

    if (uph != UPD_IDLE &&
        uph != UPD_DONE &&
        uph != UPD_ERROR &&
        uph != UPD_ASK_VALIDATE) {

        static uint32_t s_last_upd_draw_ms = 0;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        if (s_last_upd_draw_ms == 0 || (now_ms - s_last_upd_draw_ms) >= 120) {
            s_last_upd_draw_ms = now_ms;
            draw_ui();
        }

        last_btn_a = btn_a;
        last_btn_b = btn_b;
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

    // PLAY-режим: принудительная перерисовка для marquee + flow-bar
    if (s_current_mode == MODE_PLAYBACK && s_display_awake) {
        static uint32_t s_last_play_anim_ms = 0;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        if (now_ms - s_last_play_anim_ms >= FLOW_ANIM_MS) {
            s_last_play_anim_ms = now_ms;
            ui_needs_redraw = true;
        }
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
        	    int total_items = cat_count + 1; // Учитываем служебный пункт

        	    s_selected_category += effective;
        	    if (s_selected_category < 0) s_selected_category = 0;
        	    if (total_items > 0 && s_selected_category >= total_items) {
        	        s_selected_category = total_items - 1;
        	    }
        	    ui_needs_redraw = true;
        	}
        	else if (s_current_mode == MODE_SETTINGS) {
        	    s_settings_idx += effective;
        	    if (s_settings_idx < 0) s_settings_idx = 0;
        	    if (s_settings_idx >= 4) s_settings_idx = 3;
        	    ui_needs_redraw = true;
        	}
            else if (s_current_mode == MODE_STATIONS) {
                const catalog_category_t* ec = catalog_get_category(s_selected_category);
                int station_count = ec ? (int)ec->count : 0;

                int direction = (effective > 0) ? 1 : -1;
                int steps = (effective > 0) ? effective : -effective;

                int target_idx = s_ui_station_idx;

                for (int step = 0; step < steps; step++) {
                    target_idx += direction;

                    // Границы списка
                    if (target_idx > station_count) {
                        target_idx = station_count;
                        break;
                    }
                    if (target_idx < 0) {
                        target_idx = 0;
                        break;
                    }

                    // Если станция мертва, мы НЕ засчитываем этот шаг (step--),
                    // но продолжаем цикл, чтобы проверить следующий индекс в том же направлении.
                    const catalog_station_t* next_st = catalog_get_station(s_selected_category, target_idx);
                    if (next_st != nullptr && next_st->status == CAT_STATUS_DEAD) {
                        step--;
                        continue;
                    }

                    // Нашли живую станцию (или служебный пункт) - фиксируем переход
                    s_ui_station_idx = target_idx;
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
            else if (s_current_mode == MODE_CATALOG) {
                if (s_catalog_submode == 1) {
                    int nsrc = 0;
                    catalog_updater_get_sources(&nsrc);
                    s_upd_source_idx += effective;
                    if (s_upd_source_idx < 0) s_upd_source_idx = 0;
                    if (nsrc > 0 && s_upd_source_idx >= nsrc) s_upd_source_idx = nsrc - 1;
                    ui_needs_redraw = true;
                } else {
                    s_settings_idx += effective;
                    if (s_settings_idx < 0) s_settings_idx = 0;
                    if (s_settings_idx >= 7) s_settings_idx = 6;   // 7 пунктов: 0..6
                    ui_needs_redraw = true;
                }
            }
            else if (s_current_mode == MODE_SETTINGS) {
                s_settings_idx += effective;
                if (s_settings_idx < 0) s_settings_idx = 0;
                if (s_settings_idx >= 4) s_settings_idx = 3;       // 4 пункта: 0..3
                ui_needs_redraw = true;
            }
            else if (s_current_mode == MODE_WIFI && !s_keyboard_active) {
                if (s_wifi_show_scan_results) {
                    int count = wifi_scan_get_count();
                    if (count > 0) {
                        if (s_wifi_scan_selected_idx < 0) s_wifi_scan_selected_idx = 0;
                        s_wifi_scan_selected_idx += effective;
                        if (s_wifi_scan_selected_idx < 0) s_wifi_scan_selected_idx = 0;
                        if (s_wifi_scan_selected_idx >= count) s_wifi_scan_selected_idx = count - 1;
                    } else {
                        s_wifi_scan_selected_idx = -1;
                    }
                } else {
                    // 0=SSID, 1=PASS, 2=Scan, 3=Connect, 4=Networks, 5=Back
                    s_wifi_focus += effective;
                    if (s_wifi_focus < 0) s_wifi_focus = 0;
                    if (s_wifi_focus > 5) s_wifi_focus = 5;
                }
                ui_needs_redraw = true;
            }
            else if (s_current_mode == MODE_VOLUME || s_current_mode == MODE_PLAYBACK) {
                /*
                 * Читаем текущую громкость из аудио-подсистемы,
                 * а не из локальной переменной UI.
                 * Это единственный источник правды.
                 */
                int cur_vol = audio_output_get_volume();
                int new_vol = cur_vol + effective * 5;

                if (new_vol < 0) new_vol = 0;
                if (new_vol > 100) new_vol = 100;

                if (new_vol != cur_vol) {
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
        if (s_current_mode == MODE_STATIONS && s_selection_mode) {
            s_selection_mode = false;
            clear_marks();
            ESP_LOGI(TAG, "Station selection mode cancelled by button A");
        }
        else if (s_current_mode == MODE_CATEGORIES && s_cat_selection_mode) {
            s_cat_selection_mode = false;
            clear_cat_marks();
            ESP_LOGI(TAG, "Category selection mode cancelled by button A");
        }
        else if (s_current_mode == MODE_WIFI) {
            wifi_back_action();
        }
        else if (s_current_mode == MODE_KNOWN_NETWORKS) {
            s_current_mode = MODE_WIFI;
            s_wifi_focus = 4;
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
        else if (s_current_mode == MODE_ABOUT) {
            s_current_mode = MODE_SETTINGS;
        }
        else if (s_current_mode == MODE_CATALOG) {
            catalog_updater_phase_t uph = catalog_updater_get_phase();

            if (uph == UPD_ASK_VALIDATE) {
                catalog_updater_confirm_validate(false);   // A = применить сразу (без валидации)
            }
            else if (uph == UPD_DONE || uph == UPD_ERROR) {
                catalog_updater_reset();                   // фаза -> IDLE, экран меню вернётся
                s_catalog_submode = 0;                     // гарантируем: не в подменю
                s_settings_idx    = 5;                     // подсветим «6. Обновить из сети»
                s_upd_refuse_ms   = 0;                     // сброс тоста отказа
                ESP_LOGI(TAG, "Updater dismissed, back to catalog menu");
            }
            else if (s_catalog_submode == 1) {
                s_catalog_submode = 0;                     // A = назад из подменю в меню
            }
            else {
                s_current_mode = MODE_SETTINGS;            // обычный выход из каталога
            }
        }

        s_encoder_accumulator = 0;
        ui_needs_redraw = true;
    }
    last_btn_a = btn_a;

    // ========================================================
    // 7. Кнопка B: вперёд
    //    В Wi-Fi тоже делаем безопасный выход, чтобы не было ловушки.
    // ========================================================
    if (btn_b_edge) { // <--- ИСПРАВЛЕНО: здесь должно быть btn_b_edge, а не btn_a_edge!
        if (s_current_mode == MODE_WIFI) {
            wifi_back_action();
        }
        else if (s_current_mode == MODE_KNOWN_NETWORKS) {
            s_current_mode = MODE_WIFI;
            s_wifi_focus = 4; // Возвращаем фокус на кнопку Networks
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
        else if (s_current_mode == MODE_ABOUT) {
            s_current_mode = MODE_SETTINGS;
        }
        else if (s_current_mode == MODE_CATALOG) {
            catalog_updater_phase_t uph = catalog_updater_get_phase();

            if (uph == UPD_ASK_VALIDATE) {
                catalog_updater_confirm_validate(true);    // B = проверить
            }
            else if (s_catalog_submode == 1) {
                // B уже обработан в btn_enc (старт). Здесь ничего.
            }
            else {
                s_current_mode = MODE_PLAYBACK;            // обычный переход
            }
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

                    if (global_idx < 0) {
                        // === «Пенёчек»: станция удалена из каталога. Удаляем закладку по индексу. ===
                        if (catalog_remove_favorite_at_index(s_fav_station_idx)) {
                            catalog_save_favorites();
                            ESP_LOGI(TAG, "Removed orphaned favorite at index %d", s_fav_station_idx);

                            int fav_count = (int)catalog_get_favorites_count();
                            if (s_fav_station_idx >= fav_count && fav_count > 0) {
                                s_fav_station_idx = fav_count - 1;
                            } else if (fav_count == 0) {
                                s_fav_station_idx = 0;
                            }
                        }
                    } else {
                        // === Живая станция: обычный toggle (удаление из избранного). ===
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
                }
                else if (s_current_mode == MODE_WIFI) {
                    wifi_back_action();
                }
                else if (s_current_mode == MODE_PLAYBACK) {
                    // E5: toggle favorite текущей играющей станции
                    if (s_playing_station_gidx >= 0) {
                        const catalog_station_t* st =
                            catalog_get_station_by_global_index(
                                (size_t)s_playing_station_gidx);

                        if (st != nullptr) {
                            bool added = catalog_toggle_favorite(st->id);
                            catalog_save_favorites();

                            ESP_LOGI(TAG, "Favorite %s: %s (from PLAY)",
                                     added ? "ADDED" : "REMOVED", st->name);

                            s_fav_toast_ms =
                                (uint32_t)(esp_timer_get_time() / 1000);
                            strlcpy(s_fav_toast_text,
                                    added ? "* In Favorites"
                                          : "- From Favorites",
                                    sizeof(s_fav_toast_text));
                        }
                    }
                }
                else {
                    // MUTE в остальных режимах (STA, FAV, CAT, SET, ABOUT, WIFI)
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
                	    const catalog_category_t* cat = catalog_get_category(s_selected_category);
                	    int station_count = cat ? (int)cat->count : 0;

                	    if (s_ui_station_idx < station_count) {
                	        // Обычная станция
                	        if (s_selection_mode) {
                	            // В режиме выделения: отметить/снять отметку
                	            const catalog_station_t* st = catalog_get_station(s_selected_category, s_ui_station_idx);
                	            if (st) {
                	                toggle_mark_station(st->id);
                	                ESP_LOGI(TAG, "Station %s %s (marked: %d)",
                	                         st->name,
                	                         is_station_marked(st->id) ? "MARKED" : "UNMARKED",
                	                         (int)s_marked_count);
                	            }
                	        } else {
                	            // Обычный режим: воспроизведение
                	            s_new_station_idx = (int)catalog_global_index(s_selected_category, s_ui_station_idx);
                	            s_pending_event = UI_EVENT_STATION_CHANGED;
                	            s_current_mode = MODE_PLAYBACK;
                	        }
                	    } else {
                	        // Служебный пункт
                	        if (s_selection_mode) {
                	            if (s_marked_count > 0) {
                	                ESP_LOGI(TAG, "Deleting %d marked stations...", (int)s_marked_count);
                	                catalog_remove_stations(s_marked_ids, s_marked_count);
                	                clear_marks();
                	                s_cat_saved_ms = (uint32_t)(esp_timer_get_time() / 1000);   //
                	                // Корректируем индекс, если вышли за границы
                	                const catalog_category_t* new_cat = catalog_get_category(s_selected_category);
                	                int new_count = new_cat ? (int)new_cat->count : 0;
                	                if (s_ui_station_idx >= new_count) {
                	                    s_ui_station_idx = new_count > 0 ? new_count - 1 : 0;
                	                }
                	            }
                	            s_selection_mode = false;
                	        } else {
                	            // Входим в режим выделения
                	            s_selection_mode = true;
                	            clear_marks();
                	            ESP_LOGI(TAG, "Selection mode enabled");
                	        }
                	    }
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
                        int cat_count = (int)catalog_category_count();

                        if (s_selected_category < cat_count) {
                            // ---- Нажали на обычную категорию ----
                            if (s_cat_selection_mode) {
                                // В режиме выделения: отметить/снять отметку
                                toggle_cat_mark((size_t)s_selected_category);
                                ESP_LOGI(TAG, "Category %s %s (marked: %d)",
                                         catalog_get_category(s_selected_category)->name,
                                         is_cat_marked((size_t)s_selected_category) ? "MARKED" : "UNMARKED",
                                         (int)s_marked_cat_count);
                            } else {
                                // Обычный режим: переход в список станций
                                s_current_mode = MODE_STATIONS;
                                s_ui_station_idx = 0;
                            }
                        } else {
                            // ---- Нажали на служебный пункт ----
                            if (s_cat_selection_mode) {
                                if (s_marked_cat_count > 0) {
                                    ESP_LOGI(TAG, "Deleting %d marked categories...", (int)s_marked_cat_count);
                                    catalog_remove_categories(s_marked_cat_indices, s_marked_cat_count);
                                    clear_cat_marks();
                                    s_cat_saved_ms = (uint32_t)(esp_timer_get_time() / 1000);   //
                                    // Корректируем индекс, если вышли за границы
                                    int new_count = (int)catalog_category_count();
                                    if (s_selected_category >= new_count) {
                                        s_selected_category = new_count > 0 ? new_count - 1 : 0;
                                    }
                                }
                                s_cat_selection_mode = false;
                            } else {
                                // Входим в режим выделения
                                s_cat_selection_mode = true;
                                clear_cat_marks();
                                ESP_LOGI(TAG, "Category selection mode enabled");
                            }
                        }
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
                            s_settings_idx = 0;
                            // Автоматическая загрузка рабочего каталога при входе
                            if (!catalog_is_validating() && !catalog_is_deep_validating() && !catalog_is_merging()) {
                                if (catalog_get_staging_count() == 0) {
                                    ESP_LOGI(TAG, "Auto-loading working catalog to staging...");
                                    catalog_load_working_to_staging();
                                }
                            }
                        }
                        else if (s_settings_idx == 2) {
                            s_current_mode = MODE_ABOUT;
                        }
                        else if (s_settings_idx == 3) {
                            s_current_mode = MODE_PLAYBACK;
                        }
                    }
                    else if (s_current_mode == MODE_CATALOG) {
                        // === ПОДСМЕНЮ: нажатие = запуск updater ===
                        if (s_catalog_submode == 1) {
                            const upd_source_t* srcs = NULL; int nsrc = 0;
                            srcs = catalog_updater_get_sources(&nsrc);

                            if (nsrc > 0 && s_upd_source_idx < nsrc) {
                                if (catalog_updater_start(srcs[s_upd_source_idx].cc)) {
                                    s_catalog_submode = 0;
                                    ESP_LOGI(TAG, "Updater launched for '%s'", srcs[s_upd_source_idx].cc);
                                } else {
                                    s_upd_refuse_ms = (uint32_t)(esp_timer_get_time() / 1000);
                                    ESP_LOGW(TAG, "Updater start refused (busy/no network)");
                                }
                            }
                        }
                        if (s_settings_idx == 0) {
                            ESP_LOGI(TAG, "Catalog: Importing M3U from SD...");
                            catalog_staging_load_from_m3u_dir(CATALOG_M3U_DIR);
                        }
                        else if (s_settings_idx == 1) {
                            ESP_LOGI(TAG, "Catalog: Starting background FAST validation...");
                            catalog_start_validation();
                        }
                        else if (s_settings_idx == 2) {
                            ESP_LOGI(TAG, "Catalog: Starting DEEP validation of SUSPECT stations...");
                            catalog_start_deep_validation();
                        }
                        else if (s_settings_idx == 3) {
                            ESP_LOGI(TAG, "Catalog: Merging staging to working...");
                            catalog_start_merge();
                        }
                        else if (s_settings_idx == 4) {
                            ESP_LOGI(TAG, "Catalog: Purging dead stations...");
                            catalog_purge_dead_stations();
                        }
                        else if (s_settings_idx == 5) {
                            // Пункт "6. Обновить из сети" → подменю
                            s_catalog_submode = 1;
                            s_upd_source_idx = 0;
                            ESP_LOGI(TAG, "Catalog update submode entered");
                        }
                        else if (s_settings_idx == 6) {
                            // "7. Назад"
                            s_current_mode = MODE_SETTINGS;
                            s_settings_idx = 1;
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
                            if (s_wifi_focus == 0) wifi_kb_open(KB_TARGET_SSID);
                            else if (s_wifi_focus == 1) wifi_kb_open(KB_TARGET_PASS);
                            else if (s_wifi_focus == 2) {
                                ESP_LOGI(TAG, "Wi-Fi Scan requested via encoder");
                                ui_trigger_wifi_scan();
                            }
                            else if (s_wifi_focus == 3) {
                                ESP_LOGI(TAG, "Wi-Fi Connect requested via encoder");
                                ui_trigger_wifi_connect();
                            }
                            else if (s_wifi_focus == 4) {
                                s_current_mode = MODE_KNOWN_NETWORKS;
                                s_settings_idx = 0;
                            }
                            else if (s_wifi_focus == 5) {
                                wifi_back_action();
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
	// При смене станции сбрасываем icy-метаданные
	// (будут заполнены через ui_set_stream_meta при получении headers)
	if (station_name && strcmp(station_name, s_station_name) != 0) {
	    s_icy_name[0] = '\0';
	    s_bitrate_kbps = 0;
	    s_codec[0] = '\0';
	}
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

void ui_set_volume(int volume_percent)
{
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;

    s_current_volume = volume_percent;
    s_new_volume = volume_percent;
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

void ui_set_stream_meta(const char* icy_name,
                        int         bitrate_kbps,
                        const char* content_type)
{
    // --- icy-name ---
    if (icy_name && icy_name[0]) {
        strncpy(s_icy_name, icy_name, sizeof(s_icy_name) - 1);
        s_icy_name[sizeof(s_icy_name) - 1] = '\0';
    } else {
        s_icy_name[0] = '\0';
    }

    // --- bitrate ---
    s_bitrate_kbps = (bitrate_kbps > 0 && bitrate_kbps < 10000) ? bitrate_kbps : 0;

    // --- codec из Content-Type ---
    s_codec[0] = '\0';
    if (content_type && content_type[0]) {
        if (strstr(content_type, "mpeg") || strstr(content_type, "mp3")) {
            strcpy(s_codec, "MP3");
        } else if (strstr(content_type, "aac")) {
            strcpy(s_codec, "AAC");
        } else if (strstr(content_type, "ogg") ||
                   strstr(content_type, "vorbis") ||
                   strstr(content_type, "opus")) {
            strcpy(s_codec, "OGG");
        } else if (strstr(content_type, "flac")) {
            strcpy(s_codec, "FLAC");
        } else if (strstr(content_type, "wav")) {
            strcpy(s_codec, "WAV");
        } else {
            // Fallback: берём subtype из type/subtype
            const char* slash = strchr(content_type, '/');
            if (slash && slash[1]) {
                const char* end = strchr(slash + 1, ';');
                size_t n = end ? (size_t)(end - slash - 1) : strlen(slash + 1);
                if (n >= sizeof(s_codec)) n = sizeof(s_codec) - 1;
                memcpy(s_codec, slash + 1, n);
                s_codec[n] = '\0';
                for (size_t i = 0; s_codec[i]; i++)
                    s_codec[i] = (char)toupper((unsigned char)s_codec[i]);
            }
        }
    }
}

void ui_set_switch_phase(int phase)
{
    s_switch_phase = phase;
}

void ui_set_playing_station(int global_index)
{
    s_playing_station_gidx = global_index;
}
