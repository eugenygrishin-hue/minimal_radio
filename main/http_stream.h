#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/ringbuf.h"
#include <stdint.h>
#include <stdbool.h>

void http_stream_init(RingbufHandle_t ringbuf);

void http_stream_start(const char* url, uint32_t station_id);
void http_stream_stop();
void http_stream_request_stop();       // <-- NEW: неблокирующая просьба остановиться
bool http_stream_is_running();         // <-- NEW

const char* http_stream_get_title();
int http_stream_get_metaint();

uint32_t http_stream_get_ring_fill(void);
void http_stream_restart(void);

const char* http_stream_get_icy_name(void);
int         http_stream_get_bitrate(void);
const char* http_stream_get_content_type(void);

/**
 * @brief Полностью очищает ring buffer (выбрасывает все данные).
 * Вызывать ПОСЛЕ http_stream_stop() и ДО http_stream_start().
 */
void http_stream_drain_ringbuf(void);

/** true, если поток устойчиво умер (лимит неудач исчерпан) и задача вышла. */
bool http_stream_is_failed(void);

/** Сбрасывает флаг неудачи (main loop вызывает после обработки fallback). */
void http_stream_clear_failed(void);

/** true, если поток живой, но формат не поддерживается декодером (AAC+ и т.п.). */
bool http_stream_is_unsupported(void);

/** Причина в короткой строке, напр. "AAC+". Пуста, если unsupported == false. */
const char* http_stream_get_unsupported_reason(void);

/** Сброс флага после обработки main loop. */
void http_stream_clear_unsupported(void);

#ifdef __cplusplus
}
#endif
