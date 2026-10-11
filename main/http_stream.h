#pragma once

#include "freertos/ringbuf.h"
#include <stdint.h>
#include <stdbool.h>

void http_stream_init(RingbufHandle_t ringbuf);
void http_stream_start(const char* url);
void http_stream_stop();
void http_stream_request_stop();       // <-- NEW: неблокирующая просьба остановиться
bool http_stream_is_running();         // <-- NEW

const char* http_stream_get_title();
int http_stream_get_metaint();

uint32_t http_stream_get_ring_fill(void);
