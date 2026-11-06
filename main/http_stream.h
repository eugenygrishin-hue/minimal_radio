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

#ifdef __cplusplus
}
#endif
