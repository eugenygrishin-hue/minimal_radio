#include "http_stream.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <cstring>
#include <strings.h>
#include <stdlib.h>

#include "config.h"

static const char *TAG = "HTTP_STREAM";

static RingbufHandle_t s_ringbuf = NULL;
static TaskHandle_t s_http_task_handle = NULL;

static volatile bool s_http_task_running = false;
static volatile bool s_force_stop = false;

static SemaphoreHandle_t s_http_mutex = NULL;

static char s_stream_title[128] = "Connecting...";
static int s_metaint = 0;

// =====================================================================
// 1. Улучшенный детектор русского UTF-8 (теперь видит ВСЕ русские буквы)
// =====================================================================
static bool is_valid_utf8_russian(const char* str) {
    if (!str) return false;
    const char* p = str;
    int russian_chars = 0;
    int total_high_bytes = 0;

    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0xC0) {
            total_high_bytes++;
            bool is_russian = false;

            // Заглавные А-Я: D0 90 - D0 AF
            if (c == 0xD0 && p[1] >= 0x90 && p[1] <= 0xAF) is_russian = true;
            // Строчные а-п: D0 B0 - D0 BF (этого не хватало в старой версии!)
            if (c == 0xD0 && p[1] >= 0xB0 && p[1] <= 0xBF) is_russian = true;
            // Строчные р-я: D1 80 - D1 8F
            if (c == 0xD1 && p[1] >= 0x80 && p[1] <= 0x8F) is_russian = true;
            // Ё: D0 81, ё: D1 91
            if (c == 0xD0 && p[1] == 0x81) is_russian = true;
            if (c == 0xD1 && p[1] == 0x91) is_russian = true;

            if (is_russian) {
                russian_chars++;
                p += 2; // Пропускаем валидный 2-байтовый символ
            } else {
                // Высокий байт, но не русская буква - это не валидный русский UTF-8
                return false;
            }
        } else {
            p++; // ASCII символы (английские буквы, цифры, пробелы)
        }
    }

    // Если есть высокие байты, но ни одной русской буквы - это не русский UTF-8
    if (total_high_bytes > 0 && russian_chars == 0) {
        return false;
    }

    return true; // Строка либо чистый ASCII, либо валидный русский UTF-8
}

// =====================================================================
// 2. Математически точный конвертер CP1251 -> UTF-8
// =====================================================================
static void cp1251_to_utf8(const char* src, char* dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j < dst_size - 2; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c >= 0xC0 && c <= 0xFF) {
            if (c == 0xA8) { // Ё
                if (j + 2 < dst_size) { dst[j++] = 0xD0; dst[j++] = 0x81; }
            }
            else if (c == 0xB8) { // ё
                if (j + 2 < dst_size) { dst[j++] = 0xD1; dst[j++] = 0x91; }
            }
            else if (c >= 0xC0 && c <= 0xEF) { // А-п (включая заглавные и часть строчных)
                if (j + 2 < dst_size) {
                    dst[j++] = 0xD0;
                    dst[j++] = c - 0xC0 + 0x90;
                }
            }
            else if (c >= 0xF0 && c <= 0xFF) { // р-я (вторая половина строчных)
                if (j + 2 < dst_size) {
                    dst[j++] = 0xD1;
                    dst[j++] = c - 0xF0 + 0x80;
                }
            }
        } else {
            dst[j++] = c; // ASCII символы копируем как есть
        }
    }
    dst[j] = '\0';
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER &&
        strcasecmp(evt->header_key, "icy-metaint") == 0) {
        s_metaint = atoi(evt->header_value);
    }
    return ESP_OK;
}

static void http_interruptible_delay(int ms) {
    for (int elapsed = 0; elapsed < ms && !s_force_stop; elapsed += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void http_clear_ringbuf(void) {
    if (s_ringbuf == NULL) return;

    size_t item_size;
    void *data;
    while ((data = xRingbufferReceive(s_ringbuf, &item_size, 0)) != NULL) {
        vRingbufferReturnItem(s_ringbuf, data);
    }
}

static void http_read_task(void *pvParameters) {
    const char *url = (const char *)pvParameters;

    s_http_task_running = true;
    ESP_LOGI(TAG, "HTTP task started, URL: %s", url);

    uint8_t *net_buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (net_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate net_buf");
        s_http_task_running = false;
        s_http_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_handle_t client = NULL;
    int retry_delay_ms = 1000;

    enum { STATE_AUDIO, STATE_META_LEN, STATE_META_DATA } state = STATE_AUDIO;
    int bytes_until_meta = 0;
    int meta_bytes_remaining = 0;
    int meta_buf_pos = 0;
    char meta_buf[256] = {};

    while (s_http_task_running && !s_force_stop) {
        if (client == NULL) {
            esp_http_client_config_t config = {};
            config.url = url;
            config.buffer_size = 4096;
            config.crt_bundle_attach = esp_crt_bundle_attach;
            config.timeout_ms = 1000;
            // ============================
            config.user_agent = "WinampMPEG/5.0"; // Или "VLC/3.0.0 LibVLC/3.0.0"
            // ============================
            config.event_handler = http_event_handler;

            client = esp_http_client_init(&config);
            if (client == NULL) {
                ESP_LOGW(TAG, "HTTP client init failed, retry in %d ms", retry_delay_ms);
                http_interruptible_delay(retry_delay_ms);
                if (s_force_stop) break;
                if (retry_delay_ms < 8000) retry_delay_ms *= 2;
                continue;
            }

            esp_http_client_set_header(client, "Icy-MetaData", "1");

            if (esp_http_client_open(client, 0) != ESP_OK) {
                ESP_LOGW(TAG, "HTTP open failed, retry in %d ms", retry_delay_ms);
                esp_http_client_cleanup(client);
                client = NULL;

                http_interruptible_delay(retry_delay_ms);
                if (s_force_stop) break;

                if (retry_delay_ms < 8000) retry_delay_ms *= 2;
                continue;
            }

            esp_http_client_fetch_headers(client);

            state = STATE_AUDIO;
            bytes_until_meta = s_metaint;
            meta_bytes_remaining = 0;
            meta_buf_pos = 0;
            memset(meta_buf, 0, sizeof(meta_buf));

            retry_delay_ms = 1000;
            ESP_LOGI(TAG, "HTTP connected, metaint=%d", s_metaint);
        }

        if (s_force_stop) break;

        int read_len = esp_http_client_read(client, (char *)net_buf, 4096);
        if (read_len <= 0) {
            ESP_LOGW(TAG, "HTTP read returned %d, reconnecting in %d ms", read_len, retry_delay_ms);

            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;

            http_interruptible_delay(retry_delay_ms);
            if (s_force_stop) break;

            if (retry_delay_ms < 8000) retry_delay_ms *= 2;
            continue;
        }

        retry_delay_ms = 1000;

        int offset = 0;

        if (s_metaint == 0) {
            xRingbufferSend(s_ringbuf, net_buf, read_len, pdMS_TO_TICKS(100));
        } else {
            while (offset < read_len) {
                if (state == STATE_AUDIO) {
                    int chunk = (read_len - offset < bytes_until_meta)
                                    ? (read_len - offset)
                                    : bytes_until_meta;

                    xRingbufferSend(s_ringbuf, net_buf + offset, chunk, pdMS_TO_TICKS(100));

                    offset += chunk;
                    bytes_until_meta -= chunk;

                    if (bytes_until_meta == 0) {
                        state = STATE_META_LEN;
                        meta_buf_pos = 0;
                    }
                } else if (state == STATE_META_LEN) {
                    meta_bytes_remaining = net_buf[offset++] * 16;
                    state = (meta_bytes_remaining == 0) ? STATE_AUDIO : STATE_META_DATA;

                    if (state == STATE_AUDIO) {
                        bytes_until_meta = s_metaint;
                    }
                } else {
                    int to_copy = (read_len - offset < meta_bytes_remaining)
                                      ? (read_len - offset)
                                      : meta_bytes_remaining;

                    if (meta_buf_pos + to_copy < 255) {
                        memcpy(meta_buf + meta_buf_pos, net_buf + offset, to_copy);
                        meta_buf_pos += to_copy;
                    }

                    offset += to_copy;
                    meta_bytes_remaining -= to_copy;

                    if (meta_bytes_remaining == 0) {
                        meta_buf[meta_buf_pos] = '\0';

                        char *t = strstr(meta_buf, "StreamTitle='");
                        if (t) {
                            t += 13;
                            char *e = strchr(t, '\'');
                            if (e) {
                                int l = e - t;
                                if (l > 127) l = 127;

                                char temp_title[128];
                                memcpy(temp_title, t, l);
                                temp_title[l] = '\0';

                                // УЛУЧШЕННОЕ ОПРЕДЕЛЕНИЕ: проверяем, является ли это валидным UTF-8 с РУССКИМИ буквами
                                if (!is_valid_utf8_russian(temp_title)) {
                                    // Это не русский UTF-8, конвертируем из CP1251
                                    cp1251_to_utf8(temp_title, s_stream_title, sizeof(s_stream_title));
                                } else {
                                    // Это валидный русский UTF-8, просто копируем
                                    strncpy(s_stream_title, temp_title, sizeof(s_stream_title) - 1);
                                    s_stream_title[sizeof(s_stream_title) - 1] = '\0';
                                }

                                ESP_LOGI(TAG, "StreamTitle: %s", s_stream_title);
                            }
                        }

                        state = STATE_AUDIO;
                        bytes_until_meta = s_metaint;
                    }
                }
            }
        }
    }

    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    free(net_buf);

    ESP_LOGI(TAG, "HTTP task exiting");
    s_http_task_running = false;
    s_http_task_handle = NULL;
    vTaskDelete(NULL);
}

void http_stream_init(RingbufHandle_t ringbuf) {
    s_ringbuf = ringbuf;

    if (s_http_mutex == NULL) {
        s_http_mutex = xSemaphoreCreateMutex();
    }

    ESP_LOGI(TAG, "HTTP stream initialized");
}

bool http_stream_is_running(void) {
    return s_http_task_running;
}

void http_stream_request_stop(void) {
    // Неблокирующая остановка.
    // Задача сама выйдет, когда закончит текущий read/delay.
    s_force_stop = true;
}

static void http_stream_stop_unlocked(void) {
    bool need_clear = false;

    if (s_http_task_handle != NULL) {
        s_force_stop = true;

        int wait = 0;
        while (s_http_task_running && wait < 1000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait++;
        }

        if (!s_http_task_running) {
            s_force_stop = false;
            need_clear = true;
        } else {
            // НИКОГДА не делаем vTaskDelete здесь.
            // Иначе lwIP может упасть.
            ESP_LOGE(TAG, "HTTP task did not exit in 10s; refusing force kill");
            need_clear = false;
        }
    } else {
        s_force_stop = false;
        need_clear = true;
    }

    if (need_clear) {
        http_clear_ringbuf();
    }
}

static void http_stream_start_unlocked(const char *url) {
    if (url == NULL) {
        ESP_LOGE(TAG, "Cannot start HTTP stream: URL is NULL");
        return;
    }

    http_stream_stop_unlocked();

    if (s_http_task_running) {
        ESP_LOGE(TAG, "Cannot start HTTP stream: old task is still running");
        return;
    }

    s_force_stop = false;
    s_metaint = 0;
    strcpy(s_stream_title, "Connecting...");

    http_clear_ringbuf();

    BaseType_t created = xTaskCreatePinnedToCore(
        http_read_task,
        "http_read",
        8192,
        (void *)url,
        15,
        &s_http_task_handle,
        1
    );

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create HTTP task");
        s_http_task_handle = NULL;
        s_http_task_running = false;
    }
}

void http_stream_stop(void) {
    if (s_http_mutex == NULL) return;

    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    http_stream_stop_unlocked();
    xSemaphoreGive(s_http_mutex);
}

void http_stream_start(const char *url) {
    if (s_http_mutex == NULL) return;

    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    http_stream_start_unlocked(url);
    xSemaphoreGive(s_http_mutex);
}

const char *http_stream_get_title(void) {
    return s_stream_title;
}

int http_stream_get_metaint(void) {
    return s_metaint;
}

uint32_t http_stream_get_ring_fill(void) {
    if (s_ringbuf == NULL) {
        return 0;
    }

    size_t free_bytes = xRingbufferGetCurFreeSize(s_ringbuf);

    if (free_bytes >= RINGBUF_SIZE) {
        return 0;
    }

    uint32_t used = (uint32_t)(RINGBUF_SIZE - free_bytes);

    if (used > RINGBUF_SIZE) {
        used = RINGBUF_SIZE;
    }

    return used;
}
