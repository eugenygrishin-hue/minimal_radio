#include "config.h"
#include <cstddef>

// === СПИСОК СТАНЦИЙ ===
static const radio_station_t rock_stations[] = {
    {"Наше Радио 256", "http://nashe1.hostingradio.ru/nashe-256"},
    {"Rock FM", "http://nashe1.hostingradio.ru:80/rock-256"},
    {"Maximum", "https://maximum.hostingradio.ru/maximum128.mp3"},
    {"Наше Punks", "http://jfm1.hostingradio.ru:14536/nashepunks.mp3"},
    {"Classic Rock", "http://streams.radiobob.de/bob-classicrock/mp3-192/streams.radiobob.de/"}
};

static const radio_station_t jazz_stations[] = {
    {"Radio Jazz", "https://nashe1.hostingradio.ru:18000/jazz-128.mp3"},
    {"Jazz Legends", "http://jazz128legends.streamr.ru"},
    {"Smooth Jazz", "http://jazz128smooth.streamr.ru"},
    {"Jazz FM", "http://jazzfm91.streamb.live/SB00009"}
};

static const radio_station_t pop_stations[] = {
    {"Radio 7", "https://radio7.hostingradio.ru:8040/radio7256.mp3"},
    {"Retro FM", "http://retroserver.streamr.ru:8043/retro256.mp3"},
    {"Дорожное Радио", "http://dorognoe.hostingradio.ru:8000/radio"},
    {"Европа Плюс", "http://emgregion.hostingradio.ru:8064/moscow.europaplus.mp3"},
    {"Best FM", "http://nashe1.hostingradio.ru/best-256"},
    {"Шансон", "http://chanson.hostingradio.ru:8041/chanson256.mp3"}
};

const radio_category_t g_categories[] = {
    {"РОК", rock_stations, 5},
    {"ДЖАЗ", jazz_stations, 4},
    {"ПОП", pop_stations, 6}
};

const int g_category_count = 3;
const int g_total_stations = 15;

const char* get_station_url(int index) {
    int offset = 0;
    for (int cat = 0; cat < g_category_count; cat++) {
        if (index < offset + g_categories[cat].count) {
            return g_categories[cat].stations[index - offset].url;
        }
        offset += g_categories[cat].count;
    }
    return NULL;
}
