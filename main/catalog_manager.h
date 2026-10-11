#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CATALOG_JSON_PATH "/sd/radio/catalog.json"
#define CATALOG_M3U_DIR   "/sd/radio/m3u"

#define CATALOG_MAX_NAME_LEN      96
#define CATALOG_MAX_URL_LEN       256
#define CATALOG_MAX_CATEGORY_LEN  48

typedef struct {
    uint32_t id;
    char name[CATALOG_MAX_NAME_LEN + 1];
    char url[CATALOG_MAX_URL_LEN + 1];
    uint16_t bitrate_kbps;
} catalog_station_t;

typedef struct {
    char name[CATALOG_MAX_CATEGORY_LEN + 1];

    catalog_station_t* stations;
    size_t count;
    size_t capacity;
} catalog_category_t;

typedef struct {
    catalog_category_t* categories;
    size_t category_count;
    size_t category_capacity;

    size_t total_stations;

    bool loaded;
    bool from_sd;
    bool from_embedded;

    int version;
    char updated_at[32];

    uint32_t favorite_ids[64];
    size_t favorite_count;
} catalog_t;

extern catalog_t g_catalog;

bool catalog_init(void);
void catalog_log_summary(void);
bool catalog_self_test(void);

uint32_t catalog_make_station_id(const char* category, const char* name);

size_t catalog_category_count(void);
const catalog_category_t* catalog_get_category(size_t category_index);

size_t catalog_total_stations(void);

const catalog_station_t* catalog_get_station_by_global_index(size_t global_index);
size_t catalog_global_index(size_t category_index, size_t station_index);

const catalog_station_t* catalog_get_station(size_t category_index, size_t station_index);


bool catalog_find_station_by_id(
    uint32_t id,
    size_t* out_category_index,
    size_t* out_station_index
);

bool catalog_add_station(
    const char* category,
    const char* name,
    const char* url,
    uint16_t bitrate_kbps
);

bool catalog_load_json_file(const char* path);
bool catalog_save_json_file(const char* path);

bool catalog_parse_m3u_text(const char* m3u_text, const char* category_name);
bool catalog_import_m3u_file(const char* path, const char* category_name);
bool catalog_import_m3u_dir(const char* dir_path);

bool catalog_rebuild_from_m3u_dir(const char* dir_path);

void catalog_clear(void);

bool catalog_load_favorites(void);
void catalog_save_favorites(void);
bool catalog_is_favorite(uint32_t station_id);
bool catalog_toggle_favorite(uint32_t station_id);
size_t catalog_get_favorites_count(void);
int catalog_get_favorite_global_index(size_t fav_index);
