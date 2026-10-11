/*
 * Интернет-радио на M5Stack CoreS3 SE + Audio Module (ES8388)
 * Архитектура: две задачи (сеть + декодер) с кольцевым буфером
 * + ВИЗУАЛИЗАЦИЯ БУФЕРА НА ЭКРАНЕ M5STACK
 */

#include "mp3dec.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_client.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2s_std.h"

#include "esp_crt_bundle.h"

#include <M5Unified.hpp>

static const char *TAG = "AUDIO";

/* ================= ПИНЫ ================= */
#define I2S_BCK_PIN    GPIO_NUM_0
#define I2S_WS_PIN     GPIO_NUM_6
#define I2S_DOUT_PIN   GPIO_NUM_13
#define I2S_DIN_PIN    GPIO_NUM_14
#define I2S_MCLK_PIN   GPIO_NUM_7

#define ES8388_ADDR    0x10
#define STM32_ADDR     0x33

/* ================= НАСТРОЙКИ СЕТИ ================= */
#define WIFI_SSID      "Elektron-38463"
#define WIFI_PASSWORD  "138ff5395"
#define STREAM_URL     "http://nashe1.hostingradio.ru/nashe-256"
//#define STREAM_URL     "http://ep128.hostingradio.ru:8030/ep128"
//#define STREAM_URL     "https://maximum.hostingradio.ru/maximum128.mp3"
//#define STREAM_URL     "https://radio7.hostingradio.ru:8040/radio7256.mp3"
//#define STREAM_URL     "https://ice1.somafm.com/groovesalad-128-mp3"

/* ================= НАСТРОЙКИ БУФЕРОВ ================= */
#define RINGBUF_SIZE   (256 * 1024)
#define DEC_BUF_SIZE   (128 * 1024)  // 128 КБ внутренний буфер декодера
#define PREFILL_TARGET (115 * 1024)  // 115 КБ (90% от буфера)
#define REBUF_THRESHOLD (16 * 1024)
#define FRAMES_PER_PASS 10

/* ================= ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ================= */
static i2s_chan_handle_t tx_chan = NULL;
static RingbufHandle_t s_ringbuf = NULL;
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static volatile uint32_t g_rb_in = 0;
static volatile uint32_t g_rb_out = 0;
static volatile uint32_t g_rb_overflow = 0;
static volatile uint32_t g_rb_starve = 0;
static volatile uint32_t g_dec_buf = 0;
static volatile bool g_playback_started = false;

static volatile bool g_prefill_active = false;
static volatile uint32_t g_prefill_current = 0;
static volatile uint32_t g_prefill_target = 0;

// === Метаданные ICY ===
static char g_station_name[64] = "Internet Radio";
static char g_stream_title[128] = "Connecting...";
static volatile bool g_metadata_updated = false;

// === Временные переменные для заголовков ответа ===
static int g_metaint = 0;

/* ================= ПРОТОТИПЫ ================= */
static esp_err_t i2s_setup(uint32_t sample_rate);

/* ================= ЗАДАЧА-МОНИТОР БУФЕРОВ ================= */
static void buffer_monitor_task(void *pvParameters) {
    uint32_t last_in = 0, last_out = 0;

    M5.Display.setRotation(1);
    M5.Display.setTextSize(2);
    M5.Display.fillScreen(TFT_BLACK);

    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(10, 10);
    M5.Display.print("INTERNET RADIO");
    M5.Display.drawFastHLine(10, 35, 300, TFT_DARKGREY);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        uint32_t cur_in = g_rb_in;
        uint32_t cur_out = g_rb_out;

        uint32_t in_rate = (cur_in - last_in) * 2;
        uint32_t out_rate = (cur_out - last_out) * 2;
        last_in = cur_in;
        last_out = cur_out;

        int32_t rb_level = (int32_t)(cur_in - cur_out);
        if (rb_level < 0) rb_level = 0;
        if (rb_level > RINGBUF_SIZE) rb_level = RINGBUF_SIZE;

        M5.Display.fillRect(10, 40, 300, 20, TFT_BLACK);
        if (g_prefill_active) {
            M5.Display.setTextColor(TFT_YELLOW);
            M5.Display.setCursor(10, 40);
            M5.Display.print("PREFILLING BUFFER...");
        } else if (g_playback_started) {
            M5.Display.setTextColor(TFT_GREEN);
            M5.Display.setCursor(10, 40);
            M5.Display.print("PLAYING");
        } else {
            M5.Display.setTextColor(TFT_RED);
            M5.Display.setCursor(10, 40);
            M5.Display.print("BUFFERING / RECONNECTING");
        }

        int percent = 0;
        uint32_t current_val = 0;
        uint32_t target_val = 1;

        if (g_prefill_active) {
            current_val = g_prefill_current;
            target_val = g_prefill_target;
        } else {
            current_val = rb_level;
            target_val = RINGBUF_SIZE;
        }

        if (target_val > 0) {
            percent = (current_val * 100) / target_val;
            if (percent > 100) percent = 100;
        }

        M5.Display.fillRect(20, 80, 280, 30, TFT_DARKGREY);

        uint32_t bar_color = TFT_GREEN;
        if (percent < 10) bar_color = TFT_RED;
        else if (percent < 25) bar_color = TFT_YELLOW;

        int fill_width = (percent * 280) / 100;
        if (fill_width > 0) {
            M5.Display.fillRect(20, 80, fill_width, 30, bar_color);
        }

        M5.Display.drawRect(20, 80, 280, 30, TFT_WHITE);

        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.setCursor(135, 88);
        M5.Display.printf("%d%%", percent);

        M5.Display.fillRect(10, 135, 300, 90, TFT_BLACK);

        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_CYAN);

        M5.Display.setCursor(20, 140);
        M5.Display.printf("RING BUF : %5lu KB / %3d KB", (unsigned long)(rb_level / 1024), RINGBUF_SIZE / 1024);

        M5.Display.setCursor(20, 160);
        M5.Display.printf("IN RATE  : %4lu KB/s", (unsigned long)(in_rate / 1024));

        M5.Display.setCursor(170, 160);
        M5.Display.printf("OUT RATE : %4lu KB/s", (unsigned long)(out_rate / 1024));

        M5.Display.setCursor(20, 180);
        M5.Display.printf("DEC BUF  : %5lu KB", (unsigned long)(g_dec_buf / 1024));

        M5.Display.setCursor(170, 180);
        // Выравнивание по левому краю с фиксированной шириной (%-3lu), чтобы не было хвостов
        M5.Display.printf("ERRS     : OVF:%-3lu STR:%-3lu", (unsigned long)g_rb_overflow, (unsigned long)g_rb_starve);

        // === Название станции и текущий трек (с поддержкой UTF-8) ===
        M5.Display.fillRect(10, 200, 300, 40, TFT_BLACK);

        // Сохраняем текущий шрифт
        const lgfx::IFont* prev_font = M5.Display.getFont();

        // Устанавливаем шрифт с поддержкой кириллицы
        M5.Display.setFont(&fonts::efontCN_16);
        M5.Display.setTextSize(1);

        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.setCursor(20, 205);
        M5.Display.printf("%.30s", g_station_name);

        M5.Display.setTextColor(TFT_GREEN);
        M5.Display.setCursor(20, 225);
        M5.Display.printf("%.35s", g_stream_title);

        // Восстанавливаем шрифт и размер
        M5.Display.setFont(prev_font);
        M5.Display.setTextSize(2);
    }
}

/* ================= HTTP EVENT HANDLER (для перехвата заголовков ответа) ================= */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            // evt->header_key и evt->header_value содержат заголовок ответа
            if (strcasecmp(evt->header_key, "icy-name") == 0) {
                strncpy(g_station_name, evt->header_value, sizeof(g_station_name) - 1);
                ESP_LOGI(TAG, "📻 Станция: %s", g_station_name);
            }
            else if (strcasecmp(evt->header_key, "icy-genre") == 0) {
                ESP_LOGI(TAG, "🎵 Жанр: %s", evt->header_value);
            }
            else if (strcasecmp(evt->header_key, "icy-br") == 0) {
                ESP_LOGI(TAG, "📊 Битрейт: %s кбит/с", evt->header_value);
            }
            else if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
                g_metaint = atoi(evt->header_value);
                ESP_LOGI(TAG, "📋 ICY metaint: %d байт", g_metaint);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

/* ================= ЗАДАЧА 1: ЧТЕНИЕ ИЗ СЕТИ (С ПРАВИЛЬНЫМ ПАРСЕРОМ ICY) ================= */
static void http_read_task(void *pvParameters) {
    ESP_LOGI(TAG, "Задача чтения из сети запущена");

    esp_http_client_config_t config = {};
    config.url = STREAM_URL;
    config.buffer_size = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 10000;
    config.event_handler = http_event_handler;  // <-- ДОБАВЛЕНО: обработчик заголовков

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Icy-MetaData", "1");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка открытия HTTP: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    esp_http_client_fetch_headers(client);

    // === УДАЛЕНО: старый код чтения заголовков через get_header ===
    // Теперь заголовки читаются через event_handler

    uint8_t *net_buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!net_buf) {
        ESP_LOGE(TAG, "Нет памяти для сетевого буфера");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Начинаем чтение потока...");
    int total_read = 0;

    // === State machine для парсинга ICY ===
    enum { STATE_AUDIO, STATE_META_LEN, STATE_META_DATA } state = STATE_AUDIO;
    int bytes_until_meta = g_metaint;
    int meta_bytes_remaining = 0;
    uint8_t meta_len_byte = 0;
    char meta_buf[256] = {};
    int meta_buf_pos = 0;

    while (1) {
        int read_len = esp_http_client_read(client, (char *)net_buf, 4096);
        if (read_len <= 0) {
            ESP_LOGW(TAG, "HTTP поток завершен. Всего прочитано: %d байт", total_read);
            break;
        }

        if (g_metaint == 0) {
            // === Нет метаданных — просто отправляем всё в буфер ===
            if (xRingbufferSend(s_ringbuf, net_buf, read_len, pdMS_TO_TICKS(500)) != pdTRUE) {
                g_rb_overflow = g_rb_overflow + 1;
            } else {
                g_rb_in += read_len;
            }
        } else {
            // === Парсинг ICY через state machine ===
            int offset = 0;

            while (offset < read_len) {
                if (state == STATE_AUDIO) {
                    int audio_chunk = (read_len - offset < bytes_until_meta) ?
                                      (read_len - offset) : bytes_until_meta;

                    if (xRingbufferSend(s_ringbuf, net_buf + offset, audio_chunk, pdMS_TO_TICKS(500)) != pdTRUE) {
                        g_rb_overflow = g_rb_overflow + 1;
                    } else {
                        g_rb_in += audio_chunk;
                    }

                    offset += audio_chunk;
                    bytes_until_meta -= audio_chunk;

                    if (bytes_until_meta == 0) {
                        state = STATE_META_LEN;
                        meta_len_byte = 0;
                        meta_buf_pos = 0;
                    }
                }
                else if (state == STATE_META_LEN) {
                    meta_len_byte = net_buf[offset];
                    meta_bytes_remaining = meta_len_byte * 16;
                    offset++;

                    if (meta_bytes_remaining == 0) {
                        state = STATE_AUDIO;
                        bytes_until_meta = g_metaint;
                    } else {
                        state = STATE_META_DATA;
                    }
                }
                else if (state == STATE_META_DATA) {
                    int available = read_len - offset;
                    int to_copy = (available < meta_bytes_remaining) ? available : meta_bytes_remaining;

                    if (meta_buf_pos + to_copy < (int)sizeof(meta_buf) - 1) {
                        memcpy(meta_buf + meta_buf_pos, net_buf + offset, to_copy);
                        meta_buf_pos += to_copy;
                    }

                    offset += to_copy;
                    meta_bytes_remaining -= to_copy;

                    if (meta_bytes_remaining == 0) {
                        meta_buf[meta_buf_pos] = '\0';
                        char *title_start = strstr(meta_buf, "StreamTitle='");
                        if (title_start) {
                            title_start += 13;
                            char *title_end = strchr(title_start, '\'');
                            if (title_end) {
                                int title_len = title_end - title_start;
                                if (title_len > (int)sizeof(g_stream_title) - 1) {
                                    title_len = sizeof(g_stream_title) - 1;
                                }
                                memcpy(g_stream_title, title_start, title_len);
                                g_stream_title[title_len] = '\0';
                                g_metadata_updated = true;
                                ESP_LOGI(TAG, "🎶 Сейчас играет: %s", g_stream_title);
                            }
                        }
                        state = STATE_AUDIO;
                        bytes_until_meta = g_metaint;
                    }
                }
            }
        }

        total_read += read_len;

        if (total_read % (100 * 1024) < 4096) {
            ESP_LOGI(TAG, "Сеть: прочитано %d КБ", total_read / 1024);
        }
    }

    if (net_buf) heap_caps_free(net_buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}

/* ================= ЗАДАЧА 2: ДЕКОДИРОВАНИЕ И ВЫВОД ЗВУКА ================= */
static void decode_play_task(void *pvParameters) {
    ESP_LOGI(TAG, "Задача декодирования запущена");

    HMP3Decoder mp3_decoder = MP3InitDecoder();
    if (!mp3_decoder) {
        ESP_LOGE(TAG, "Не удалось создать MP3 декодер");
        vTaskDelete(NULL);
        return;
    }

    uint8_t *inbuf = (uint8_t *)heap_caps_malloc(DEC_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    short *outbuf = (short *)heap_caps_malloc(2304 * sizeof(short), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!inbuf || !outbuf) {
        ESP_LOGE(TAG, "Нет памяти для буферов декодера");
        vTaskDelete(NULL);
        return;
    }

    int bytes_in_buffer = 0;
    bool i2s_started = false;
    int frames_decoded = 0;
    int frames_skipped = 0;

    int last_reported = 0;
    int prefill_target = PREFILL_TARGET;

    auto do_prefill = [&]() {
        g_prefill_active = true;
        g_prefill_target = prefill_target;
        last_reported = 0;

        while (bytes_in_buffer < prefill_target) {
            size_t item_size = 0;
            void *data = xRingbufferReceive(s_ringbuf, &item_size, pdMS_TO_TICKS(2000));
            if (data != NULL) {
                if (bytes_in_buffer + item_size <= DEC_BUF_SIZE) {
                    memcpy(inbuf + bytes_in_buffer, data, item_size);
                    bytes_in_buffer += item_size;
                    g_rb_out += item_size;
                }
                vRingbufferReturnItem(s_ringbuf, data);
            }

            g_prefill_current = bytes_in_buffer;

            if (bytes_in_buffer - last_reported >= 16 * 1024) {
                int percent = (bytes_in_buffer * 100) / prefill_target;
                ESP_LOGI(TAG, "Предзаполнение: %3d%% (%d/%d)", percent, bytes_in_buffer, prefill_target);
                last_reported = bytes_in_buffer;
            }
        }
        g_prefill_active = false;
    };

    ESP_LOGI(TAG, "Предзаполнение буфера (128 КБ)...");
    do_prefill();
    ESP_LOGI(TAG, "Буфер предзаполнен: %d байт. Воспроизведение!", bytes_in_buffer);
    g_playback_started = true;

    while (1) {
        if (bytes_in_buffer < REBUF_THRESHOLD) {
            ESP_LOGW(TAG, "⚠ Буфер опустошён (%d байт). Переподключение...", bytes_in_buffer);
            g_playback_started = false;

            while (1) {
                size_t item_size = 0;
                void *data = xRingbufferReceive(s_ringbuf, &item_size, 0);
                if (data == NULL) break;
                vRingbufferReturnItem(s_ringbuf, data);
            }

            g_rb_in = 0;
            g_rb_out = 0;
            bytes_in_buffer = 0;

            ESP_LOGI(TAG, "Повторное предзаполнение буфера (128 КБ)...");
            do_prefill();

            ESP_LOGI(TAG, "Буфер восстановлен: %d байт. Продолжаем воспроизведение!", bytes_in_buffer);
            g_playback_started = true;
            continue;
        }

        while (bytes_in_buffer < PREFILL_TARGET) {
            size_t item_size = 0;
            TickType_t timeout = (bytes_in_buffer < 4 * 1024) ? pdMS_TO_TICKS(500) : pdMS_TO_TICKS(50);

            void *data = xRingbufferReceive(s_ringbuf, &item_size, timeout);
            if (data != NULL) {
                if (bytes_in_buffer + item_size <= DEC_BUF_SIZE) {
                    memcpy(inbuf + bytes_in_buffer, data, item_size);
                    bytes_in_buffer += item_size;
                    g_rb_out += item_size;
                }
                vRingbufferReturnItem(s_ringbuf, data);
            } else {
                if (bytes_in_buffer < 4 * 1024) {
                    g_rb_starve = g_rb_starve + 1;
                }
                break;
            }
        }

        // === ЗАЩИТА ОТ ЩЕЛЧКОВ: если буфер почти пуст, отправляем тишину ===
        if (bytes_in_buffer < 2048 && i2s_started) {
            static short silence_buf[2048];
            memset(silence_buf, 0, sizeof(silence_buf));
            size_t bytes_written = 0;
            i2s_channel_write(tx_chan, silence_buf, sizeof(silence_buf),
                              &bytes_written, pdMS_TO_TICKS(10));
            g_rb_starve = g_rb_starve + 1;
            continue; // Возвращаемся к пополнению буфера
        }

        g_dec_buf = bytes_in_buffer;

        uint8_t *read_ptr = inbuf;
        int bytes_left = bytes_in_buffer;
        int frames_this_pass = 0;

        while (bytes_left > 0 && frames_this_pass < FRAMES_PER_PASS) {
            int offset = MP3FindSyncWord(read_ptr, bytes_left);
            if (offset < 0) {
                bytes_left = 0;
                break;
            }
            if (offset > 0) {
                read_ptr += offset;
                bytes_left -= offset;
            }

            uint8_t *saved_ptr = read_ptr;
            int saved_left = bytes_left;

            int ret = MP3Decode(mp3_decoder, &read_ptr, &bytes_left, outbuf, 0);

            if (ret == ERR_MP3_INDATA_UNDERFLOW) {
                break;
            } else if (ret != ERR_MP3_NONE) {
                read_ptr = saved_ptr + 1;
                bytes_left = saved_left - 1;
                frames_skipped++;
                vTaskDelay(pdMS_TO_TICKS(26));
                continue;
            }

            MP3FrameInfo fi;
            MP3GetLastFrameInfo(mp3_decoder, &fi);

            if (fi.samprate == 0 || fi.nChans == 0) {
                read_ptr = saved_ptr + 1;
                bytes_left = saved_left - 1;
                frames_skipped++;
                vTaskDelay(pdMS_TO_TICKS(26));
                continue;
            }

            if (!i2s_started) {
                ESP_LOGI(TAG, "Настройка I2S на %d Гц, %d каналов", fi.samprate, fi.nChans);
                if (i2s_setup(fi.samprate) == ESP_OK) {
                    i2s_started = true;
                } else {
                    break;
                }
            }

            if (fi.nChans == 1) {
                static short stereo_buf[2304];
                for (int i = 0; i < fi.outputSamps; i++) {
                    stereo_buf[i * 2]     = outbuf[i];
                    stereo_buf[i * 2 + 1] = outbuf[i];
                }
                size_t bytes_written = 0;
                i2s_channel_write(tx_chan, stereo_buf, fi.outputSamps * 2 * sizeof(short),
                                  &bytes_written, portMAX_DELAY);
            } else {
                size_t bytes_written = 0;
                i2s_channel_write(tx_chan, outbuf, fi.outputSamps * sizeof(short),
                                  &bytes_written, portMAX_DELAY);
            }
            frames_decoded++;
            frames_this_pass++;
        }

        if (bytes_left > 0 && bytes_left < bytes_in_buffer) {
            memmove(inbuf, read_ptr, bytes_left);
        }
        bytes_in_buffer = bytes_left;
    }

    MP3FreeDecoder(mp3_decoder);
    if (inbuf) heap_caps_free(inbuf);
    if (outbuf) heap_caps_free(outbuf);
    vTaskDelete(NULL);
}

/* ================= ES8388 (Через встроенный I2C от M5Unified) ================= */
static void es8388_write(uint8_t reg, uint8_t val)
{
    // Добавлен 4-й аргумент: частота 100000 Гц (100 кГц)
    // Приведен адрес к uint8_t
    M5.In_I2C.writeRegister8((uint8_t)ES8388_ADDR, reg, val, 100000);
}

static uint8_t es8388_read(uint8_t reg)
{
    // Добавлен 3-й аргумент: частота 100000 Гц
    return M5.In_I2C.readRegister8((uint8_t)ES8388_ADDR, reg, 100000);
}

static void es8388_init(void)
{
    ESP_LOGI(TAG, "Инициализация ES8388...");
    es8388_write(0x08, 0x00);
    es8388_write(0x02, 0xFF);
    es8388_write(0x2B, 0x80);
    es8388_write(0x00, 0x05);
    es8388_write(0x01, 0x40);
    es8388_write(0x03, 0x00);
    es8388_write(0x0A, 0x00);
    es8388_write(0x0B, 0x00);
    es8388_write(0x09, 0x88);
    es8388_write(0x0C, 0x2C);
    es8388_write(0x0D, 0x02);
    es8388_write(0x0F, 0x28);
    es8388_write(0x10, 0x00);
    es8388_write(0x11, 0x00);
    es8388_write(0x12, 0xEA);
    es8388_write(0x13, 0xC0);
    es8388_write(0x14, 0x12);
    es8388_write(0x15, 0x06);
    es8388_write(0x16, 0xC3);
    es8388_write(0x04, 0x3F);
    es8388_write(0x17, 0x18);
    es8388_write(0x18, 0x02);
    es8388_write(0x19, 0x00);
    es8388_write(0x1A, 0x1E);
    es8388_write(0x1B, 0x1E);
    es8388_write(0x26, 0x00);
    es8388_write(0x27, 0x90);
    es8388_write(0x28, 0x38);
    es8388_write(0x29, 0x38);
    es8388_write(0x2A, 0x90);
    es8388_write(0x2B, 0x80);
    es8388_write(0x2E, 0x1E);
    es8388_write(0x2F, 0x1E);
    es8388_write(0x30, 0x00);
    es8388_write(0x31, 0x00);
    es8388_write(0x02, 0x00);

    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t reg0 = es8388_read(0x00);
    ESP_LOGI(TAG, "🔍 Проверка связи: ES8388 Reg 0x00 = 0x%02X", reg0);
    ESP_LOGI(TAG, "ES8388 успешно инициализирован");
}

static void set_trrs_mux(uint8_t mode)
{
    ESP_LOGI(TAG, "Установка MUX: %s", mode ? "CTIA (American)" : "OMTP (National)");
    // Добавлен 4-й аргумент: частота 100000 Гц
    M5.In_I2C.writeRegister8((uint8_t)STM32_ADDR, 0x10, mode, 100000);
}

/* ================= I2S ================= */
static esp_err_t i2s_setup(uint32_t sample_rate)
{
    ESP_LOGI(TAG, "Настройка I2S: %lu Гц", (unsigned long)sample_rate);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 24;
    chan_cfg.dma_frame_num = 1023;

    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    std_cfg.slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

    std_cfg.gpio_cfg.mclk = I2S_MCLK_PIN;
    std_cfg.gpio_cfg.bclk = I2S_BCK_PIN;
    std_cfg.gpio_cfg.ws   = I2S_WS_PIN;
    std_cfg.gpio_cfg.dout = I2S_DOUT_PIN;
    std_cfg.gpio_cfg.din  = I2S_DIN_PIN;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv   = false;

    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "✅ I2S запущен");
    return ESP_OK;
}

/* ================= WiFi ================= */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi: дисконнект, переподключаюсь...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi: получен IP " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // === ОТКЛЮЧАЕМ POWER SAVE для стабильного потока ===
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi: Power Save отключен");

    ESP_LOGI(TAG, "WiFi: подключение к \"%s\"...", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

/* ================= app_main ================= */
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== ИНТЕРНЕТ-РАДИО (M5 CoreS3 + Экран) ===");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Инициализация M5 (включает внутренний I2C на GPIO 11/12 автоматически)
    auto cfg = M5.config();
    M5.begin(cfg);

    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 100);
    M5.Display.print("Starting...");

    // УДАЛЕНО: i2c_master_init(); (больше не нужно, M5 сам всё настроил)

    set_trrs_mux(0x00);
    es8388_init();

    wifi_init_sta();

    s_ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (s_ringbuf == NULL) {
        ESP_LOGE(TAG, "Не удалось создать кольцевой буфер");
        return;
    }
    ESP_LOGI(TAG, "Кольцевой буфер создан: %d байт (NOSPLIT)", RINGBUF_SIZE);

    xTaskCreatePinnedToCore(buffer_monitor_task, "buf_monitor", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(http_read_task, "http_read", 4096, NULL, 15, NULL, 1);
    xTaskCreatePinnedToCore(decode_play_task, "decode_play", 8192, NULL, 10, NULL, 0);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
