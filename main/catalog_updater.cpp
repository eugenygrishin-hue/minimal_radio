#include "catalog_updater.h"
#include "catalog_manager.h"
#include "wifi_manager.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static const char* TAG = "CAT_UPD";

// ============================================================
// Источники (задел под E17: расширение списка)
// ============================================================
static const upd_source_t s_sources[] = {
    { "ru", "Россия",
      "https://raw.githubusercontent.com/AlonDrilich/radio-playlists/main/ets2/ru/live_streams.sii" },
    { "rs", "Сербия",
      "https://raw.githubusercontent.com/AlonDrilich/radio-playlists/main/ets2/rs/live_streams.sii" },
};
static const int s_sources_count = (int)(sizeof(s_sources) / sizeof(s_sources[0]));

// ============================================================
// Лимиты и пути
// ============================================================
#define UPD_MAX_SII_BYTES   (256 * 1024)   // защита DRAM
#define UPD_MAX_M3U_BYTES   (512 * 1024)
#define UPD_TMP_M3U_PATH    "/sd/radio/_update.m3u"

// ============================================================
// Состояние
// ============================================================
static volatile catalog_updater_phase_t s_phase    = UPD_IDLE;
static volatile int      s_progress        = 0;
static volatile size_t   s_bytes_dl        = 0;
static volatile size_t   s_bytes_total     = 0;
static volatile int      s_stations_parsed = 0;
static char              s_error[128]      = "";
static updater_result_t  s_result          = {};

static volatile bool s_confirm_pending  = false;
static volatile bool s_confirm_validate = false;

static char         s_active_cc[8] = "";
static TaskHandle_t s_task         = NULL;

// ============================================================
// hex-decoder: \xHH → байт, остальные символы как есть
// Применяется ТОЛЬКО к полям name и tag (URL не hex-encoded)
// ============================================================
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_hex_escape(const char* src, char* dst, size_t dst_sz)
{
    size_t j = 0;
    const char* p = src;
    while (*p && j + 1 < dst_sz) {
        if (p[0] == '\\' && p[1] == 'x' &&
            isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])) {
            int hi = hexval(p[2]);
            int lo = hexval(p[3]);
            dst[j++] = (char)((hi << 4) | lo);
            p += 4;
        } else {
            dst[j++] = *p++;
        }
    }
    dst[j] = '\0';
    return (int)j;
}

// ============================================================
// Парсинг одной строки stream_data[N]: "url|name|tag|country|bitrate|flag"
// Возвращает true если строка валидна и содержит http(s) URL
// ============================================================
static bool sii_parse_line(const char* line,
                           char* url,  size_t url_sz,
                           char* name, size_t name_sz,
                           char* tag,  size_t tag_sz,
                           int*  bitrate)
{
    *bitrate = 0;
    url[0] = name[0] = tag[0] = '\0';

    // Начало кавычки
    const char* q1 = strchr(line, '"');
    if (!q1) return false;
    q1++;

    // Конец кавычки (первая сырая " после начала — hex-encoded name не содержит сырых ")
    const char* q2 = strchr(q1, '"');
    if (!q2 || q2 <= q1) return false;

    size_t len = (size_t)(q2 - q1);
    if (len == 0 || len >= 1024) return false;

    char buf[1024];
    memcpy(buf, q1, len);
    buf[len] = '\0';

    // Разбиваем по '|'
    char* fields[6] = {0};
    int nf = 0;
    char* tok = buf;
    while (tok && nf < 6) {
        fields[nf++] = tok;
        char* bar = strchr(tok, '|');
        if (bar) { *bar = '\0'; tok = bar + 1; }
        else      { tok = NULL; }
    }
    if (nf < 5) return false;   // нужно url|name|tag|country|bitrate

    // URL — как есть (не hex)
    strlcpy(url, fields[0], url_sz);
    if (strncmp(url, "http://",  7) != 0 &&
        strncmp(url, "https://", 8) != 0) {
        return false;
    }

    // name, tag — hex-decode
    decode_hex_escape(fields[1], name, name_sz);
    decode_hex_escape(fields[2], tag,  tag_sz);

    // bitrate
    *bitrate = atoi(fields[4]);

    return name[0] != '\0';
}

// ============================================================
// SII → M3U (в память)
// ============================================================
static int sii_to_m3u(const char* sii, size_t sii_len,
                      char* m3u, size_t m3u_sz,
                      int* stations_out)
{
    size_t wp = 0;
    int stations = 0;

    wp += (size_t)snprintf(m3u + wp, m3u_sz - wp, "#EXTM3U\n");

    const char* p   = sii;
    const char* end = sii + sii_len;

    while (p < end) {
    	const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
        if (!nl) nl = end;

        size_t line_len = (size_t)(nl - p);
        if (line_len > 0 && line_len < 2048) {
            char line[2048];
            memcpy(line, p, line_len);
            line[line_len] = '\0';

            if (strstr(line, "stream_data[")) {
                char url[512], name[256], tag[128];
                int  bitrate = 0;

                if (sii_parse_line(line, url, sizeof(url),
                                          name, sizeof(name),
                                          tag,  sizeof(tag),
                                          &bitrate)) {
                    // guard от переполнения m3u
                    if (wp + 512 < m3u_sz) {
                        wp += (size_t)snprintf(m3u + wp, m3u_sz - wp,
                                               "#EXTINF:-1,%s (%dkbps)\n",
                                               name, bitrate);
                        wp += (size_t)snprintf(m3u + wp, m3u_sz - wp,
                                               "%s\n", url);
                        stations++;
                    }
                }
            }
        }
        p = nl + 1;
    }

    if (stations_out) *stations_out = stations;
    return stations;
}

static uint16_t parse_br_from_name_local(const char* name)
{
    if (!name) return 0;
    const char* open = strrchr(name, '(');
    if (!open) return 0;
    const char* close = strchr(open, ')');
    const char* end = close ? close : (open + strlen(open));
    const char* p = open + 1;
    while (p < end && !isdigit((unsigned char)*p)) p++;
    if (p >= end) return 0;
    uint32_t val = 0;
    while (p < end && isdigit((unsigned char)*p)) {
        val = val * 10 + (uint32_t)(*p - '0');
        if (val > 65535u) { val = 65535u; break; }
        p++;
    }
    return (uint16_t)val;
}

// ============================================================
// HTTP download (esp_http_client, как в http_stream.cpp)
// ============================================================
static bool upd_download(const char* url, char** out_buf, size_t* out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    esp_http_client_config_t config = {};
    config.url               = url;
    config.buffer_size       = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms        = 15000;
    config.user_agent        = "GMFRadio/1.0";
    config.event_handler     = NULL;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        snprintf(s_error, sizeof(s_error), "http init failed");
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "http open failed (%d)", (int)err);
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);

    long clen = esp_http_client_get_content_length(client);
    if (clen <= 0) clen = UPD_MAX_SII_BYTES;   // chunked / unknown
    if (clen > (long)UPD_MAX_SII_BYTES) {
        snprintf(s_error, sizeof(s_error), "too large (%ld B)", clen);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    char* buf = (char*)malloc((size_t)clen + 1);
    if (!buf) {
        snprintf(s_error, sizeof(s_error), "oom (%ld B)", clen);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    size_t got = 0;
    s_bytes_total = (size_t)clen;

    while (got < (size_t)clen) {
        int r = esp_http_client_read(client, buf + got, (size_t)clen - got);
        if (r <= 0) break;
        got += (size_t)r;
        s_bytes_dl = got;
        s_progress = (int)(got * 100 / (size_t)clen);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (got == 0) {
        free(buf);
        snprintf(s_error, sizeof(s_error), "empty response");
        return false;
    }

    buf[got] = '\0';
    *out_buf = buf;
    *out_len = got;
    return true;
}

// ============================================================
// Категория из кода источника: "ru" → "Radio-Browser RU"
// ============================================================
static void make_category(const char* cc, char* out, size_t out_sz)
{
    snprintf(out, out_sz, "Radio-Browser %s", cc);
    for (char* p = out + strlen("Radio-Browser "); *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    }
}

// ============================================================
// Фоновая задача
// ============================================================
static void upd_task(void* arg)
{
    const char* cc = (const char*)arg;

    // Найти источник
    const upd_source_t* src = NULL;
    for (int i = 0; i < s_sources_count; i++) {
        if (strcmp(s_sources[i].cc, cc) == 0) { src = &s_sources[i]; break; }
    }
    if (!src) {
        snprintf(s_error, sizeof(s_error), "unknown source '%s'", cc);
        s_phase = UPD_ERROR;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (!wifi_is_connected()) {
        snprintf(s_error, sizeof(s_error), "no network");
        s_phase = UPD_ERROR;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // ---- DOWNLOADING ----
    s_phase = UPD_DOWNLOADING;
    s_progress = 0;
    s_bytes_dl = 0;
    s_bytes_total = 0;
    ESP_LOGI(TAG, "Downloading: %s", src->url);

    char* sii_buf = NULL;
    size_t sii_len = 0;
    if (!upd_download(src->url, &sii_buf, &sii_len)) {
        s_phase = UPD_ERROR;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Downloaded %u bytes", (unsigned)sii_len);

    // ---- CONVERTING ----
    s_phase = UPD_CONVERTING;
    s_progress = 0;

    char* m3u_buf = (char*)malloc(UPD_MAX_M3U_BYTES);
    if (!m3u_buf) {
        snprintf(s_error, sizeof(s_error), "oom m3u");
        free(sii_buf);
        s_phase = UPD_ERROR;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int stations = 0;
    sii_to_m3u(sii_buf, sii_len, m3u_buf, UPD_MAX_M3U_BYTES, &stations);
    free(sii_buf);
    s_stations_parsed = stations;
    s_progress = 100;

    if (stations == 0) {
        snprintf(s_error, sizeof(s_error), "no stations parsed");
        free(m3u_buf);
        s_phase = UPD_ERROR;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Converted %d stations to M3U", stations);


    // ---- IMPORTING ----
    s_phase = UPD_IMPORTING;
    s_progress = 0;

    char category[64];
    make_category(cc, category, sizeof(category));

    int staging_before = catalog_get_staging_count();
    int new_count = 0;

    // Парсим m3u_buf напрямую в staging.
    // НЕ пишем на SD, НЕ вызываем catalog_import_m3u_file (он импортирует в working).
    {
        const char* p = m3u_buf;
        char pending_name[256] = "";
        bool have_pending = false;

        while (*p) {
            const char* eol = strchr(p, '\n');
            size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

            char line[512];
            if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
            memcpy(line, p, line_len);
            line[line_len] = '\0';

            // Trim \r и \n с конца
            while (line_len > 0 && (line[line_len - 1] == '\r' || line[line_len - 1] == '\n')) {
                line[--line_len] = '\0';
            }

            // Двигаем указатель
            if (eol) {
                p = eol + 1;
            } else {
                p += line_len; // последняя строка без \n
            }

            if (line_len == 0) continue;

            // #EXTINF:-1,Name (128kbps)
            if (strncmp(line, "#EXTINF:", 8) == 0) {
                char* ns = line + 8;
                // Пропускаем число до запятой (-1, 0, 128 и т.д.)
                if (*ns == '-' || isdigit((unsigned char)*ns)) {
                    char* comma = strchr(ns, ',');
                    if (comma) ns = comma + 1;
                }
                while (*ns == ' ') ns++;
                size_t nl = strlen(ns);
                while (nl > 0 && ns[nl - 1] == ' ') ns[--nl] = '\0';

                strncpy(pending_name, ns, sizeof(pending_name) - 1);
                pending_name[sizeof(pending_name) - 1] = '\0';
                have_pending = true;
                continue;
            }

            // Другие комментарии (#EXTM3U, #CURTRACK и т.д.)
            if (line[0] == '#') continue;

            // URL-строка
            const char* url = line;
            while (*url == ' ') url++;
            if (*url == '\0') continue;

            if (have_pending && pending_name[0] != '\0') {
                uint32_t id = catalog_make_station_id(category, pending_name);
                uint16_t br = parse_br_from_name_local(pending_name);

                if (catalog_staging_add_station(id, pending_name, url, br, category)) {
                    new_count++;
                }
                have_pending = false;
                pending_name[0] = '\0';
            }
        }
    }

    free(m3u_buf);
    m3u_buf = NULL;

    // Удаляем временный файл, если он остался от предыдущих прогонов
    remove(UPD_TMP_M3U_PATH);

    s_progress = 100;

    int staging_after = catalog_get_staging_count();

    ESP_LOGI(TAG, "Import OK: staging %d -> %d (new: %d, category '%s')",
             staging_before, staging_after, new_count, category);

    // Источник истины для UI — количество РЕАЛЬНО НОВЫХ станций
    s_stations_parsed = new_count;

    // === НОВЫХ НЕТ → это НЕ ошибка, а «каталог актуален» ===
    if (new_count == 0) {
        s_result.added         = 0;
        s_result.updated       = 0;
        s_result.removed       = 0;
        s_result.total_before  = staging_before;
        s_result.total_after   = staging_after;
        s_result.staging_count = stations;   // ← СКОЛЬКО БЫЛО В M3U (100)
        s_phase = UPD_DONE;
        s_progress = 100;
        ESP_LOGI(TAG, "No new stations: checked %d from source, all %d already in catalog",
                 stations, staging_after);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // === Есть новые → спрашиваем про валидацию ===
    s_phase = UPD_ASK_VALIDATE;
    s_confirm_pending  = true;
    s_confirm_validate = false;
    s_progress = 0;

    while (s_confirm_pending) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    bool do_validate = s_confirm_validate;

    // ---- VALIDATING (опционально) ----
    if (do_validate) {
        s_phase = UPD_VALIDATING;
        s_progress = 0;
        ESP_LOGI(TAG, "Starting validation...");
        catalog_start_validation();

        while (catalog_is_validating()) {
            int done  = catalog_get_validated_count();
            int total = catalog_get_staging_count();
            s_progress = (total > 0) ? (done * 100 / total) : 0;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ESP_LOGI(TAG, "Validation done");
    }

    // ---- MERGING ----
    s_phase = UPD_MERGING;
    s_progress = 0;
    ESP_LOGI(TAG, "Starting merge...");

    int total_before = (int)catalog_total_stations();
    catalog_start_merge();

    while (catalog_is_merging()) {
        s_progress = catalog_get_merge_progress();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    int total_after = (int)catalog_total_stations();

    s_result.total_before  = total_before;
    s_result.total_after   = total_after;
    s_result.staging_count = stations;
    s_result.added   = (total_after > total_before) ? (total_after - total_before) : 0;
    s_result.updated = 0;
    s_result.removed = (total_before > total_after) ? (total_before - total_after) : 0;

    s_phase = UPD_DONE;
    s_progress = 100;
    ESP_LOGI(TAG, "Update complete: before=%d after=%d added=%d removed=%d",
             total_before, total_after, s_result.added, s_result.removed);

    s_task = NULL;
    vTaskDelete(NULL);
}

// ============================================================
// Публичный API
// ============================================================
const upd_source_t* catalog_updater_get_sources(int* count)
{
    if (count) *count = s_sources_count;
    return s_sources;
}

bool catalog_updater_is_busy(void)
{
    return (s_task != NULL);
}

bool catalog_updater_start(const char* cc)
{
    if (!cc) return false;

    if (s_task != NULL) {
        ESP_LOGW(TAG, "Updater already running");
        return false;
    }
    if (catalog_is_merging() || catalog_is_validating()) {
        ESP_LOGW(TAG, "Merge/validation in progress, cannot start updater");
        return false;
    }

    strlcpy(s_active_cc, cc, sizeof(s_active_cc));

    s_phase = UPD_IDLE;
    s_error[0] = '\0';
    memset(&s_result, 0, sizeof(s_result));
    s_stations_parsed = 0;
    s_progress = 0;
    s_bytes_dl = 0;
    s_bytes_total = 0;
    s_confirm_pending = false;
    s_confirm_validate = false;

    BaseType_t ok = xTaskCreatePinnedToCore(
        upd_task, "cat_upd", 8192, (void*)s_active_cc, 5, &s_task, 1);

    if (ok != pdPASS) {
        s_task = NULL;
        snprintf(s_error, sizeof(s_error), "task create failed");
        s_phase = UPD_ERROR;
        return false;
    }
    ESP_LOGI(TAG, "Updater started for '%s'", cc);
    return true;
}

catalog_updater_phase_t catalog_updater_get_phase(void)    { return s_phase; }
int      catalog_updater_get_progress(void)                { return s_progress; }
size_t   catalog_updater_get_bytes_downloaded(void)        { return s_bytes_dl; }
size_t   catalog_updater_get_bytes_total(void)             { return s_bytes_total; }
int      catalog_updater_get_stations_parsed(void)         { return s_stations_parsed; }
const char* catalog_updater_get_error(void)                { return s_error; }

bool catalog_updater_get_result(updater_result_t* out)
{
    if (s_phase != UPD_DONE) return false;
    if (out) *out = s_result;
    return true;
}

void catalog_updater_confirm_validate(bool do_validate)
{
    if (s_phase != UPD_ASK_VALIDATE) return;
    s_confirm_validate = do_validate;
    s_confirm_pending  = false;
    ESP_LOGI(TAG, "Validate confirmed: %s", do_validate ? "YES" : "NO");
}

void catalog_updater_reset(void)
{
    // Защита: сбрасываем ТОЛЬКО из терминальных состояний.
    // Если задача ещё крутится (download/convert/import/validate/merge) —
    // не трогаем, иначе убьём работающий updater на полпути.
    catalog_updater_phase_t ph = s_phase;

    if (ph != UPD_IDLE && ph != UPD_DONE && ph != UPD_ERROR) {
        ESP_LOGW(TAG, "reset() ignored: updater busy (phase=%d)", (int)ph);
        return;
    }

    s_phase            = UPD_IDLE;
    s_progress         = 0;
    s_stations_parsed  = 0;
    s_bytes_dl         = 0;
    s_bytes_total      = 0;

    s_error[0] = '\0';
    memset(&s_result, 0, sizeof(s_result));

    // Гасим возможный «зависший» запрос подтверждения
    s_confirm_pending  = false;
    s_confirm_validate = false;

    // Задача в терминальных фазах уже сделала vTaskDelete(NULL) — handle невалиден
    s_task = NULL;

    ESP_LOGI(TAG, "Updater reset to IDLE");
}
