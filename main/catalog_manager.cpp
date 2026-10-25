#include "catalog_manager.h"
#include "m3u_parser.h"        // <-- ДОБАВИТЬ: чтобы видеть m3u_parse_file
#include "esp_timer.h"         // <-- ДОБАВИТЬ: чтобы видеть esp_timer_get_time
#include "esp_http_client.h"   // <-- ДОБАВИТЬ: для HTTP валидации
#include "sd_manager.h"
#include "config.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <sys/stat.h>          // Для mkdir
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>

static const char* TAG = "CATALOG";

static void ensure_sd_radio_dir(void);

catalog_t g_catalog = {};

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static void ensure_sd_radio_dir(void) {
    mkdir("/sd/radio", 0777);
}

static void copy_trunc(char* dst, size_t dst_size, const char* src)
{
    if (dst == nullptr || dst_size == 0) {
        return;
    }

    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }

    size_t len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
}

static char* trim_in_place(char* s)
{
    if (s == nullptr) {
        return s;
    }

    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }

    return s;
}

static bool starts_with(const char* s, const char* prefix)
{
    if (s == nullptr || prefix == nullptr) {
        return false;
    }

    size_t lp = strlen(prefix);
    return strncmp(s, prefix, lp) == 0;
}

static bool ends_with_ci(const char* s, const char* suffix)
{
    if (s == nullptr || suffix == nullptr) {
        return false;
    }

    size_t ls = strlen(s);
    size_t lf = strlen(suffix);

    if (ls < lf) {
        return false;
    }

    for (size_t i = 0; i < lf; i++) {
        unsigned char c1 = (unsigned char)s[ls - lf + i];
        unsigned char c2 = (unsigned char)suffix[i];

        if (tolower(c1) != tolower(c2)) {
            return false;
        }
    }

    return true;
}

static __attribute__((unused)) bool join_path(char* out, size_t out_size, const char* dir, const char* name)
{
    if (out == nullptr || out_size == 0 || dir == nullptr || name == nullptr) {
        return false;
    }

    size_t dl = strlen(dir);
    size_t nl = strlen(name);

    bool need_sep = !(dl > 0 && dir[dl - 1] == '/');

    size_t total = dl + (need_sep ? 1 : 0) + nl + 1;

    if (total > out_size) {
        return false;
    }

    memcpy(out, dir, dl);

    size_t pos = dl;

    if (need_sep) {
        out[pos++] = '/';
    }

    memcpy(out + pos, name, nl + 1);

    return true;
}

static uint32_t fnv1a32(const char* s)
{
    uint32_t h = 2166136261u;

    while (s != nullptr && *s != '\0') {
        h ^= (uint8_t)(*s++);
        h *= 16777619u;
    }

    return h;
}

static void normalize_id_input(
    const char* category,
    const char* name,
    char* out,
    size_t out_size
)
{
    if (out == nullptr || out_size == 0) {
        return;
    }

    size_t o = 0;
    bool prev_space = false;

    const char* cat = (category != nullptr) ? category : "";
    const char* nm  = (name != nullptr) ? name : "";

    for (int part = 0; part < 2; part++) {
        const char* src = (part == 0) ? cat : nm;

        if (part == 1) {
            if (o + 1 < out_size) {
                out[o++] = '|';
            }
            prev_space = false;
        } else {
            prev_space = false;
        }

        for (const unsigned char* p = (const unsigned char*)src; *p != '\0'; p++) {
            unsigned char c = *p;

            if (isspace(c)) {
                if (!prev_space && o > 0 && out[o - 1] != ' ' && out[o - 1] != '|') {
                    if (o + 1 < out_size) {
                        out[o++] = ' ';
                    }
                }
                prev_space = true;
            } else {
                prev_space = false;

                if (c >= 'A' && c <= 'Z') {
                    c = (unsigned char)(c - 'A' + 'a');
                }

                if (o + 1 < out_size) {
                    out[o++] = (char)c;
                }
            }
        }
    }

    while (o > 0 && out[o - 1] == ' ') {
        o--;
    }

    if (o < out_size) {
        out[o] = '\0';
    } else {
        out[out_size - 1] = '\0';
    }
}

uint32_t catalog_make_station_id(const char* category, const char* name)
{
    char buf[CATALOG_MAX_CATEGORY_LEN + CATALOG_MAX_NAME_LEN + 4];

    const char* cat = (category != nullptr && category[0] != '\0') ? category : "general";
    const char* nm  = (name != nullptr && name[0] != '\0') ? name : "station";

    normalize_id_input(cat, nm, buf, sizeof(buf));

    uint32_t id = fnv1a32(buf);
    if (id == 0) {
        id = 1;
    }

    return id;
}

static uint16_t parse_bitrate_from_name(const char* name)
{
    if (name == nullptr) {
        return 0;
    }

    const char* open = strrchr(name, '(');
    if (open == nullptr) {
        return 0;
    }

    const char* close = strchr(open, ')');
    const char* end = (close != nullptr) ? close : (open + strlen(open));

    const char* p = open + 1;

    while (p < end && !isdigit((unsigned char)*p)) {
        p++;
    }

    if (p >= end) {
        return 0;
    }

    uint32_t val = 0;

    while (p < end && isdigit((unsigned char)*p)) {
        val = val * 10 + (uint32_t)(*p - '0');
        if (val > 65535u) {
            val = 65535u;
            break;
        }
        p++;
    }

    return (uint16_t)val;
}

static __attribute__((unused)) void category_from_path(const char* path, char* out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return;
    }

    if (path == nullptr) {
        copy_trunc(out, out_size, "General");
        return;
    }

    const char* base = strrchr(path, '/');
    base = (base != nullptr) ? (base + 1) : path;

    char tmp[256];
    char work[256];

    copy_trunc(tmp, sizeof(tmp), base);
    copy_trunc(work, sizeof(work), tmp);

    if (ends_with_ci(work, ".m3u")) {
        size_t len = strlen(work);
        if (len >= 4) {
            work[len - 4] = '\0';
        }
    }

    char* par = strchr(work, '(');
    if (par != nullptr) {
        char* close = strchr(par, ')');
        if (close != nullptr) {
            *par = '\0';
        }
    }

    trim_in_place(work);

    if (work[0] == '\0') {
        copy_trunc(work, sizeof(work), tmp);
    }

    copy_trunc(out, out_size, work);
}

// ---------------------------------------------------------------------
// Dynamic storage
// ---------------------------------------------------------------------

static bool ensure_category_capacity(size_t need)
{
    if (need <= g_catalog.category_capacity) {
        return true;
    }

    size_t new_cap = g_catalog.category_capacity ? g_catalog.category_capacity * 2 : 8;
    while (new_cap < need) {
        new_cap *= 2;
    }

    catalog_category_t* p = (catalog_category_t*)heap_caps_realloc(
        g_catalog.categories,
        new_cap * sizeof(catalog_category_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (p == nullptr) {
        ESP_LOGE(TAG, "OOM: cannot realloc categories to %u", (unsigned)new_cap);
        return false;
    }

    g_catalog.categories = p;
    g_catalog.category_capacity = new_cap;
    return true;
}

static bool ensure_station_capacity_in_category(catalog_category_t* cat, size_t need)
{
    if (cat == nullptr) {
        return false;
    }

    if (need <= cat->capacity) {
        return true;
    }

    size_t new_cap = cat->capacity ? cat->capacity * 2 : 4;
    while (new_cap < need) {
        new_cap *= 2;
    }

    catalog_station_t* p = (catalog_station_t*)heap_caps_realloc(
        cat->stations,
        new_cap * sizeof(catalog_station_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (p == nullptr) {
        ESP_LOGE(TAG, "OOM: cannot realloc stations for category '%s' to %u",
                 cat->name, (unsigned)new_cap);
        return false;
    }

    cat->stations = p;
    cat->capacity = new_cap;
    return true;
}

static catalog_category_t* find_or_create_category(const char* name)
{
    char clean[CATALOG_MAX_CATEGORY_LEN + 1];

    copy_trunc(clean, sizeof(clean), (name != nullptr && name[0] != '\0') ? name : "General");
    trim_in_place(clean);

    if (clean[0] == '\0') {
        copy_trunc(clean, sizeof(clean), "General");
    }

    for (size_t i = 0; i < g_catalog.category_count; i++) {
        if (strcmp(g_catalog.categories[i].name, clean) == 0) {
            return &g_catalog.categories[i];
        }
    }

    if (!ensure_category_capacity(g_catalog.category_count + 1)) {
        return nullptr;
    }

    catalog_category_t* cat = &g_catalog.categories[g_catalog.category_count];
    memset(cat, 0, sizeof(*cat));

    copy_trunc(cat->name, sizeof(cat->name), clean);

    g_catalog.category_count++;
    return cat;
}

static bool add_station_to_category(
    catalog_category_t* cat,
    const char* name,
    const char* url,
    uint16_t bitrate,
    uint32_t id
)
{
    if (cat == nullptr || name == nullptr || url == nullptr) {
        return false;
    }

    if (name[0] == '\0' || url[0] == '\0') {
        return false;
    }

    if (strlen(name) > CATALOG_MAX_NAME_LEN || strlen(url) > CATALOG_MAX_URL_LEN) {
        ESP_LOGW(TAG, "Station too long, skipped: %s / %s", name, url);
        return false;
    }

    for (size_t i = 0; i < cat->count; i++) {
        if (cat->stations[i].id == id) {
            return true; // уже есть, не дублируем
        }
    }

    if (!ensure_station_capacity_in_category(cat, cat->count + 1)) {
        return false;
    }

    catalog_station_t* st = &cat->stations[cat->count++];
    memset(st, 0, sizeof(*st));

    st->id = id;
    copy_trunc(st->name, sizeof(st->name), name);
    copy_trunc(st->url, sizeof(st->url), url);
    st->bitrate_kbps = bitrate;

    g_catalog.total_stations++;

    return true;
}

bool catalog_add_station(
    const char* category,
    const char* name,
    const char* url,
    uint16_t bitrate_kbps
)
{
    catalog_category_t* cat = find_or_create_category(category);
    if (cat == nullptr) {
        return false;
    }

    if (bitrate_kbps == 0) {
        bitrate_kbps = parse_bitrate_from_name(name);
    }

    uint32_t id = catalog_make_station_id(cat->name, name);
    return add_station_to_category(cat, name, url, bitrate_kbps, id);
}

void catalog_clear(void)
{
    for (size_t i = 0; i < g_catalog.category_count; i++) {
        if (g_catalog.categories[i].stations != nullptr) {
            heap_caps_free(g_catalog.categories[i].stations);
            g_catalog.categories[i].stations = nullptr;
        }
    }

    if (g_catalog.categories != nullptr) {
        heap_caps_free(g_catalog.categories);
        g_catalog.categories = nullptr;
    }

    memset(&g_catalog, 0, sizeof(g_catalog));
    g_catalog.version = 1;
}

// ---------------------------------------------------------------------
// Embedded fallback from config.cpp
// ---------------------------------------------------------------------

static bool catalog_load_embedded(void)
{
    catalog_clear();

    if (g_category_count <= 0) {
        ESP_LOGW(TAG, "Embedded catalog is empty");
        return false;
    }

    for (int c = 0; c < g_category_count; c++) {
        const radio_category_t* rc = &g_categories[c];

        catalog_category_t* cat = find_or_create_category(rc->category);
        if (cat == nullptr) {
            return false;
        }

        for (int s = 0; s < rc->count; s++) {
            const char* name = rc->stations[s].name;
            const char* url  = rc->stations[s].url;

            if (name == nullptr || url == nullptr) {
                continue;
            }

            uint16_t br = parse_bitrate_from_name(name);
            uint32_t id = catalog_make_station_id(cat->name, name);

            if (!add_station_to_category(cat, name, url, br, id)) {
                ESP_LOGW(TAG, "Failed to add embedded station: %s", name);
            }
        }
    }

    g_catalog.loaded = (g_catalog.total_stations > 0);
    g_catalog.from_embedded = g_catalog.loaded;
    g_catalog.from_sd = false;

    copy_trunc(g_catalog.updated_at, sizeof(g_catalog.updated_at), "embedded");

    return g_catalog.loaded;
}

// ---------------------------------------------------------------------
// M3U parser
// ---------------------------------------------------------------------

bool catalog_parse_m3u_text(const char* m3u_text, const char* category_name)
{
    if (m3u_text == nullptr) {
        return false;
    }

    catalog_category_t* cat = find_or_create_category(
        (category_name != nullptr && category_name[0] != '\0') ? category_name : "General"
    );

    if (cat == nullptr) {
        return false;
    }

    size_t len = strlen(m3u_text);
    char* buf = (char*)malloc(len + 1);
    if (buf == nullptr) {
        ESP_LOGE(TAG, "OOM for M3U buffer, len=%u", (unsigned)len);
        return false;
    }

    memcpy(buf, m3u_text, len + 1);

    char pending_name[CATALOG_MAX_NAME_LEN + 1];
    pending_name[0] = '\0';
    bool have_pending = false;

    char* saveptr = nullptr;

    for (
        char* line = strtok_r(buf, "\r\n", &saveptr);
        line != nullptr;
        line = strtok_r(nullptr, "\r\n", &saveptr)
    ) {
        char* t = trim_in_place(line);

        if (*t == '\0') {
            continue;
        }

        if (
            starts_with(t, "#CURTRACK") ||
            starts_with(t, "#EXTM3U") ||
            starts_with(t, "#EXTURL:")
        ) {
            continue;
        }

        if (starts_with(t, "#EXTINF:")) {
            char* p = t + strlen("#EXTINF:");
            p = trim_in_place(p);

            // Поддержка классического формата:
            // #EXTINF:-1,Name
            // #EXTINF:0,Name
            if (*p == '-' || isdigit((unsigned char)*p)) {
                char* comma = strchr(p, ',');
                if (comma != nullptr) {
                    p = comma + 1;
                    p = trim_in_place(p);
                }
            }

            copy_trunc(pending_name, sizeof(pending_name), p);
            have_pending = true;
            continue;
        }

        if (*t == '#') {
            continue;
        }

        // Обычная строка — URL.
        const char* url = t;

        if (have_pending) {
            uint16_t br = parse_bitrate_from_name(pending_name);
            uint32_t id = catalog_make_station_id(cat->name, pending_name);

            add_station_to_category(cat, pending_name, url, br, id);

            have_pending = false;
            pending_name[0] = '\0';
        } else {
            // Если вдруг URL без EXTINF — добавим как есть, имя = URL.
            uint32_t id = catalog_make_station_id(cat->name, url);
            add_station_to_category(cat, url, url, 0, id);
        }
    }

    free(buf);
    return true;
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

bool catalog_import_m3u_file(const char* path, const char* category_name)
{
    if (!sd_manager_is_mounted()) {
        ESP_LOGW(TAG, "SD not mounted, cannot import M3U: %s", path ? path : "(null)");
        return false;
    }

    if (path == nullptr) {
        return false;
    }

    char cat_buf[CATALOG_MAX_CATEGORY_LEN + 1];
    const char* cat = category_name;

    if (cat == nullptr || cat[0] == '\0') {
        category_from_path(path, cat_buf, sizeof(cat_buf));
        cat = cat_buf;
    }

    size_t len = 0;
    char* text = sd_manager_read_text(path, &len);
    if (text == nullptr) {
        ESP_LOGW(TAG, "Failed to read M3U file: %s", path);
        return false;
    }

    bool ok = catalog_parse_m3u_text(text, cat);
    free(text);

    if (ok) {
        ESP_LOGI(TAG, "Imported M3U: %s -> category '%s'", path, cat);
    }

    return ok;
}

bool catalog_import_m3u_dir(const char* dir_path)
{
    if (!sd_manager_is_mounted()) {
        ESP_LOGW(TAG, "SD not mounted, cannot import M3U dir: %s", dir_path ? dir_path : "(null)");
        return false;
    }

    if (dir_path == nullptr) {
        return false;
    }

    DIR* d = opendir(dir_path);
    if (d == nullptr) {
        ESP_LOGW(TAG, "Cannot open M3U dir: %s", dir_path);
        return false;
    }

    size_t imported = 0;
    struct dirent* ent;

    while ((ent = readdir(d)) != nullptr) {
    	if (ent->d_name[0] == '.') {
    	    continue;
    	}

        if (!ends_with_ci(ent->d_name, ".m3u")) {
            continue;
        }

        char path[512];

        if (!join_path(path, sizeof(path), dir_path, ent->d_name)) {
            ESP_LOGW(TAG, "M3U path too long, skipped");
            continue;
        }

        if (catalog_import_m3u_file(path, nullptr)) {
            imported++;
        }
    }

    closedir(d);

    ESP_LOGI(TAG, "Imported %u M3U files from %s", (unsigned)imported, dir_path);

    return imported > 0;
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

bool catalog_rebuild_from_m3u_dir(const char* dir_path)
{
    catalog_clear();

    g_catalog.version = 1;
    copy_trunc(g_catalog.updated_at, sizeof(g_catalog.updated_at), "m3u-dir");

    bool imported_any = catalog_import_m3u_dir(dir_path);

    g_catalog.loaded = imported_any && (g_catalog.total_stations > 0);
    g_catalog.from_sd = g_catalog.loaded;
    g_catalog.from_embedded = false;

    if (g_catalog.loaded && sd_manager_is_mounted()) {
        catalog_save_json_file(CATALOG_JSON_PATH);
    }

    return g_catalog.loaded;
}

// ---------------------------------------------------------------------
// JSON persistence
// ---------------------------------------------------------------------

static uint32_t json_get_uint(const cJSON* obj, const char* key, uint32_t def)
{
    if (obj == nullptr || key == nullptr) {
        return def;
    }

    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        double d = item->valuedouble;
        if (d < 0.0) {
            d = 0.0;
        }
        if (d > 4294967295.0) {
            d = 4294967295.0;
        }
        return (uint32_t)d;
    }

    return def;
}

bool catalog_load_json_file(const char* path)
{
    if (!sd_manager_is_mounted()) {
        return false;
    }

    size_t len = 0;
    char* text = sd_manager_read_text(path, &len);
    if (text == nullptr) {
        return false;
    }

    cJSON* root = cJSON_Parse(text);
    free(text);

    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON: %s", path);
        return false;
    }

    catalog_clear();

    g_catalog.version = (int)json_get_uint(root, "version", 1);

    cJSON* updated = cJSON_GetObjectItemCaseSensitive(root, "updated_at");
    if (cJSON_IsString(updated) && updated->valuestring != nullptr) {
        copy_trunc(g_catalog.updated_at, sizeof(g_catalog.updated_at), updated->valuestring);
    }

    cJSON* cats = cJSON_GetObjectItemCaseSensitive(root, "categories");
    if (!cJSON_IsArray(cats)) {
        ESP_LOGE(TAG, "Invalid catalog JSON: missing categories array");
        cJSON_Delete(root);
        catalog_clear();
        return false;
    }

    cJSON* cat_item = nullptr;
    cJSON_ArrayForEach(cat_item, cats) {
        cJSON* name_item = cJSON_GetObjectItemCaseSensitive(cat_item, "name");
        if (!cJSON_IsString(name_item) || name_item->valuestring == nullptr) {
            continue;
        }

        catalog_category_t* cat = find_or_create_category(name_item->valuestring);
        if (cat == nullptr) {
            continue;
        }

        cJSON* stations = cJSON_GetObjectItemCaseSensitive(cat_item, "stations");
        if (!cJSON_IsArray(stations)) {
            continue;
        }

        cJSON* st_item = nullptr;
        cJSON_ArrayForEach(st_item, stations) {
            cJSON* nm = cJSON_GetObjectItemCaseSensitive(st_item, "name");
            cJSON* url = cJSON_GetObjectItemCaseSensitive(st_item, "url");

            if (!cJSON_IsString(nm) || nm->valuestring == nullptr) {
                continue;
            }
            if (!cJSON_IsString(url) || url->valuestring == nullptr) {
                continue;
            }

            uint16_t br = (uint16_t)json_get_uint(st_item, "bitrate", 0);
            uint32_t id = json_get_uint(st_item, "id", 0);

            if (id == 0) {
                id = catalog_make_station_id(cat->name, nm->valuestring);
            }

            add_station_to_category(cat, nm->valuestring, url->valuestring, br, id);
        }
    }

    cJSON_Delete(root);

    g_catalog.loaded = (g_catalog.total_stations > 0);
    g_catalog.from_sd = g_catalog.loaded;
    g_catalog.from_embedded = false;

    if (!g_catalog.loaded) {
        catalog_clear();
    }

    return g_catalog.loaded;
}

bool catalog_save_json_file(const char* path)
{
    if (!sd_manager_is_mounted()) {
        return false;
    }

    if (!g_catalog.loaded) {
        return false;
    }

    ensure_sd_radio_dir();

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }

    cJSON_AddNumberToObject(root, "version", g_catalog.version > 0 ? g_catalog.version : 1);
    cJSON_AddStringToObject(
        root,
        "updated_at",
        g_catalog.updated_at[0] != '\0' ? g_catalog.updated_at : "unknown"
    );

    cJSON* cats = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "categories", cats);

    for (size_t ci = 0; ci < g_catalog.category_count; ci++) {
        const catalog_category_t* cat = &g_catalog.categories[ci];

        cJSON* cobj = cJSON_CreateObject();
        if (cobj == nullptr) {
            continue;
        }

        cJSON_AddStringToObject(cobj, "name", cat->name);

        cJSON* stations = cJSON_CreateArray();
        cJSON_AddItemToObject(cobj, "stations", stations);

        for (size_t si = 0; si < cat->count; si++) {
            const catalog_station_t* st = &cat->stations[si];

            cJSON* sobj = cJSON_CreateObject();
            if (sobj == nullptr) {
                continue;
            }

            cJSON_AddNumberToObject(sobj, "id", (double)st->id);
            cJSON_AddStringToObject(sobj, "name", st->name);
            cJSON_AddStringToObject(sobj, "url", st->url);
            cJSON_AddNumberToObject(sobj, "bitrate", st->bitrate_kbps);

            cJSON_AddItemToArray(stations, sobj);
        }

        cJSON_AddItemToArray(cats, cobj);
    }

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (out == nullptr) {
        ESP_LOGE(TAG, "Failed to serialize catalog JSON");
        return false;
    }

    bool ok = sd_manager_write_text(path, out);
    cJSON_free(out);

    if (ok) {
        ESP_LOGI(TAG, "Catalog saved: %s", path);
    } else {
        ESP_LOGW(TAG, "Failed to save catalog: %s", path);
    }

    return ok;
}

// ---------------------------------------------------------------------
// Public getters
// ---------------------------------------------------------------------

size_t catalog_category_count(void)
{
    return g_catalog.category_count;
}

const catalog_category_t* catalog_get_category(size_t category_index)
{
    if (category_index >= g_catalog.category_count) {
        return nullptr;
    }
    return &g_catalog.categories[category_index];
}

size_t catalog_total_stations(void)
{
    return g_catalog.total_stations;
}

const catalog_station_t* catalog_get_station(size_t category_index, size_t station_index)
{
    const catalog_category_t* cat = catalog_get_category(category_index);
    if (cat == nullptr || station_index >= cat->count) {
        return nullptr;
    }
    return &cat->stations[station_index];
}

const catalog_station_t* catalog_get_station_by_global_index(size_t global_index)
{
    size_t offset = 0;
    for (size_t ci = 0; ci < g_catalog.category_count; ci++) {
        const catalog_category_t* cat = &g_catalog.categories[ci];
        if (global_index < offset + cat->count) {
            return &cat->stations[global_index - offset];
        }
        offset += cat->count;
    }
    return nullptr;
}

size_t catalog_global_index(size_t category_index, size_t station_index)
{
    size_t offset = 0;
    for (size_t ci = 0; ci < g_catalog.category_count; ci++) {
        if (ci == category_index) {
            return offset + station_index;
        }
        offset += g_catalog.categories[ci].count;
    }
    return 0;
}

bool catalog_find_station_by_id(
    uint32_t id,
    size_t* out_category_index,
    size_t* out_station_index
)
{
    for (size_t ci = 0; ci < g_catalog.category_count; ci++) {
        const catalog_category_t* cat = &g_catalog.categories[ci];

        for (size_t si = 0; si < cat->count; si++) {
            if (cat->stations[si].id == id) {
                if (out_category_index != nullptr) {
                    *out_category_index = ci;
                }
                if (out_station_index != nullptr) {
                    *out_station_index = si;
                }
                return true;
            }
        }
    }

    return false;
}

// ---------------------------------------------------------------------
// Init / logs / self-test
// ---------------------------------------------------------------------

bool catalog_init(void)
{
    catalog_clear();

    copy_trunc(g_catalog.updated_at, sizeof(g_catalog.updated_at), "init");

    if (sd_manager_is_mounted()) {
        if (catalog_load_json_file(CATALOG_JSON_PATH)) {
            ESP_LOGI(TAG, "Loaded catalog from SD: %s", CATALOG_JSON_PATH);
            return true;
        }

        ESP_LOGI(TAG, "SD catalog not found or invalid, using embedded fallback");
    }

    if (catalog_load_embedded()) {
        ESP_LOGI(TAG, "Loaded embedded fallback catalog");

        if (sd_manager_is_mounted()) {
            if (catalog_save_json_file(CATALOG_JSON_PATH)) {
                ESP_LOGI(TAG, "Initial catalog cached to SD");
            } else {
                ESP_LOGW(TAG, "Failed to cache initial catalog to SD");
            }
        }

        return true;
    }

    ESP_LOGW(TAG, "No catalog loaded");
    return false;
}

void catalog_log_summary(void)
{
    ESP_LOGI(
        TAG,
        "Catalog source: %s",
        g_catalog.from_sd ? "SD" : (g_catalog.from_embedded ? "embedded" : "none")
    );

    ESP_LOGI(
        TAG,
        "Version: %d, updated_at: %s",
        g_catalog.version,
        g_catalog.updated_at[0] != '\0' ? g_catalog.updated_at : "-"
    );

    ESP_LOGI(
        TAG,
        "Categories: %u, stations: %u",
        (unsigned)g_catalog.category_count,
        (unsigned)g_catalog.total_stations
    );

    if (g_catalog.category_count > 0 && g_catalog.categories[0].count > 0) {
        const catalog_station_t* st = &g_catalog.categories[0].stations[0];

        ESP_LOGI(
            TAG,
            "First station: id=0x%08lx name='%s' url='%s' bitrate=%u",
            (unsigned long)st->id,
            st->name,
            st->url,
            (unsigned)st->bitrate_kbps
        );
    }
}

bool catalog_self_test(void)
{
    bool ok = true;

    uint32_t id1 = catalog_make_station_id("ROCK", "Наше Радио (256kbps)");
    uint32_t id2 = catalog_make_station_id("ROCK", "Наше Радио (256kbps)");

    if (id1 == 0 || id1 != id2) {
        ESP_LOGE(TAG, "Self-test failed: station ID is not stable");
        ok = false;
    }

    if (parse_bitrate_from_name("Test (128kbps)") != 128) {
        ESP_LOGE(TAG, "Self-test failed: bitrate parse 128kbps");
        ok = false;
    }

    if (parse_bitrate_from_name("Test (128-320kbps)") != 128) {
        ESP_LOGE(TAG, "Self-test failed: bitrate parse 128-320kbps");
        ok = false;
    }

    if (parse_bitrate_from_name("No bitrate") != 0) {
        ESP_LOGE(TAG, "Self-test failed: bitrate parse no bitrate");
        ok = false;
    }

    if (g_catalog.loaded && g_catalog.category_count > 0 && g_catalog.categories[0].count > 0) {
        uint32_t fid = g_catalog.categories[0].stations[0].id;
        size_t ci = 0;
        size_t si = 0;

        if (!catalog_find_station_by_id(fid, &ci, &si)) {
            ESP_LOGE(TAG, "Self-test failed: cannot find first station by ID");
            ok = false;
        }
    }

    ESP_LOGI(TAG, "Self-test: %s", ok ? "OK" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------
// Favorites management
// ---------------------------------------------------------------------
bool catalog_load_favorites(void)
{
    if (!sd_manager_is_mounted()) return false;
    size_t len = 0;
    char* text = sd_manager_read_text("/sd/radio/favorites.json", &len);
    if (!text) return false;

    cJSON* root = cJSON_Parse(text);
    free(text);
    if (!root) return false;

    g_catalog.favorite_count = 0;
    cJSON* favs = cJSON_GetObjectItemCaseSensitive(root, "favorites");
    if (cJSON_IsArray(favs)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, favs) {
            if (g_catalog.favorite_count < 64 && cJSON_IsNumber(item)) {
                g_catalog.favorite_ids[g_catalog.favorite_count++] = (uint32_t)item->valuedouble;
            }
        }
    }
    cJSON_Delete(root);
    return true;
}

void catalog_save_favorites(void)
{
    if (!sd_manager_is_mounted()) return;
    ensure_sd_radio_dir();
    cJSON* root = cJSON_CreateObject();
    cJSON* favs = cJSON_CreateArray();
    for (size_t i = 0; i < g_catalog.favorite_count; i++) {
        cJSON_AddItemToArray(favs, cJSON_CreateNumber(g_catalog.favorite_ids[i]));
    }
    cJSON_AddItemToObject(root, "favorites", favs);

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out) {
        sd_manager_write_text("/sd/radio/favorites.json", out);
        cJSON_free(out);
    }
}

bool catalog_is_favorite(uint32_t station_id)
{
    for (size_t i = 0; i < g_catalog.favorite_count; i++) {
        if (g_catalog.favorite_ids[i] == station_id) return true;
    }
    return false;
}

bool catalog_toggle_favorite(uint32_t station_id)
{
    for (size_t i = 0; i < g_catalog.favorite_count; i++) {
        if (g_catalog.favorite_ids[i] == station_id) {
            // Удаляем
            for (size_t j = i; j < g_catalog.favorite_count - 1; j++) {
                g_catalog.favorite_ids[j] = g_catalog.favorite_ids[j + 1];
            }
            g_catalog.favorite_count--;
            return false; // Было в избранном, теперь удалено
        }
    }
    // Добавляем
    if (g_catalog.favorite_count < 64) {
        g_catalog.favorite_ids[g_catalog.favorite_count++] = station_id;
        return true; // Добавлено
    }
    return false; // Лимит исчерпан
}

size_t catalog_get_favorites_count(void)
{
    return g_catalog.favorite_count;
}

int catalog_get_favorite_global_index(size_t fav_index)
{
    if (fav_index >= g_catalog.favorite_count) return -1;
    uint32_t target_id = g_catalog.favorite_ids[fav_index];

    size_t offset = 0;
    for (size_t ci = 0; ci < g_catalog.category_count; ci++) {
        const catalog_category_t* cat = &g_catalog.categories[ci];
        for (size_t si = 0; si < cat->count; si++) {
            if (cat->stations[si].id == target_id) {
                return (int)(offset + si);
            }
        }
        offset += cat->count;
    }
    return -1; // Станция не найдена в текущем каталоге (возможно, удалена)
}

const char* catalog_get_source_str(void) {
    // Простая проверка: если файл на SD существует, значит источник SD
	ensure_sd_radio_dir();
    FILE* f = fopen("/sd/radio/catalog.json", "r");
    if (f) {
        fclose(f);
        return "SD";
    }
    return "Embedded";
}

// ============================================================
// Staging Catalog & Validation (в фоне, чтобы не морозить UI)
// ============================================================
static staging_station_t* s_staging = NULL;
static size_t s_staging_count = 0;
static size_t s_staging_capacity = 0;

// Переменные для сохранения уровня логирования (чтобы заглушить красный спам при валидации)
static esp_log_level_t s_old_tls_level;
static esp_log_level_t s_old_transport_level;
static esp_log_level_t s_old_http_level;

// Переменные для отслеживания прогресса валидации UI-ем
static int s_staging_validated_count = 0;
static int s_staging_total_count = 0;
static bool s_is_validating = false;
static TaskHandle_t s_validation_task_handle = NULL;

// Forward declaration
static void catalog_validation_task(void *pvParameters);

static void staging_ensure_capacity(size_t need) {
    if (need <= s_staging_capacity) return;
    size_t new_cap = s_staging_capacity ? s_staging_capacity * 2 : 256;
    while (new_cap < need) new_cap *= 2;

    staging_station_t* p = (staging_station_t*)heap_caps_realloc(
        s_staging,
        new_cap * sizeof(staging_station_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (p) {
        s_staging = p;
        s_staging_capacity = new_cap;
    } else {
        ESP_LOGE(TAG, "OOM: cannot realloc staging catalog to %d", (int)new_cap);
    }
}

void catalog_staging_clear(void) {
    if (s_staging) {
        heap_caps_free(s_staging);
        s_staging = NULL;
    }
    s_staging_count = 0;
    s_staging_capacity = 0;
    s_staging_validated_count = 0;
    s_staging_total_count = 0;
    s_is_validating = false;
    if (s_validation_task_handle != NULL) {
        vTaskDelete(s_validation_task_handle);
        s_validation_task_handle = NULL;
    }
    ESP_LOGI(TAG, "Staging catalog cleared");
}

int catalog_staging_get_count(void) {
    return (int)s_staging_count;
}

const staging_station_t* catalog_staging_get_station(int index) {
    if (index < 0 || (size_t)index >= s_staging_count) return NULL;
    return &s_staging[index];
}

bool catalog_is_validating(void) {
    return s_is_validating;
}

int catalog_get_validated_count(void) {
    return s_staging_validated_count;
}

int catalog_get_staging_count(void) {
    return (int)s_staging_count;
}

// Функция, которую вызывает UI для запуска валидации в фоне
void catalog_start_validation(void) {
    if (s_staging_count == 0 || s_is_validating) {
        ESP_LOGW(TAG, "Nothing to validate or already validating");
        return;
    }

    // 1. Сохраняем текущие уровни логирования и отключаем спам
    s_old_tls_level = esp_log_level_get("esp-tls");
    s_old_transport_level = esp_log_level_get("transport_base");
    s_old_http_level = esp_log_level_get("HTTP_CLIENT");

    esp_log_level_set("esp-tls", ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);

    s_staging_validated_count = 0;
    s_staging_total_count = (int)s_staging_count;
    s_is_validating = true;

    ESP_LOGI(TAG, "Starting background validation of %d staging stations...", s_staging_total_count);

    xTaskCreatePinnedToCore(
        catalog_validation_task,
        "cat_validate",
        4096,
        NULL,
        1, // Низкий приоритет, чтобы не мешать аудио и UI
        &s_validation_task_handle,
        1
    );
}

bool catalog_staging_load_from_m3u_dir(const char* dir_path) {
    if (!sd_manager_is_mounted()) {
        ESP_LOGW(TAG, "SD not mounted, cannot load staging");
        return false;
    }
    catalog_staging_clear(); // Очищаем предыдущий staging

    DIR* d = opendir(dir_path);
    if (!d) {
        ESP_LOGW(TAG, "Cannot open M3U dir: %s", dir_path);
        return false;
    }

    int total_parsed = 0;
    struct dirent* ent;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        size_t len = strlen(ent->d_name);
        if (len < 5 || strcasecmp(ent->d_name + len - 4, ".m3u") != 0) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);

        // Имя файла без расширения как default_group
        char group[CATALOG_MAX_CATEGORY_LEN + 1];
        strncpy(group, ent->d_name, sizeof(group) - 1);
        group[sizeof(group) - 1] = '\0';
        char* dot = strrchr(group, '.');
        if (dot) *dot = '\0';

        // Хак: используем g_catalog как временный буфер для парсинга M3U,
        // чтобы не дублировать код парсера, но сохраняем его состояние.
        catalog_category_t* saved_cats = g_catalog.categories;
        size_t saved_cat_count = g_catalog.category_count;
        size_t saved_cat_cap = g_catalog.category_capacity;
        size_t saved_total = g_catalog.total_stations;
        bool saved_loaded = g_catalog.loaded;

        g_catalog.categories = NULL;
        g_catalog.category_count = 0;
        g_catalog.category_capacity = 0;
        g_catalog.total_stations = 0;
        g_catalog.loaded = false;

        size_t file_len = 0;
        char* text = sd_manager_read_text(path, &file_len);
        bool ok = false;
        if (text) {
            ok = catalog_parse_m3u_text(text, group);
            free(text);
        }

        if (ok) {
            // Копируем всё, что распарсилось, в staging
            for (size_t ci = 0; ci < g_catalog.category_count; ci++) {
                catalog_category_t* cat = &g_catalog.categories[ci];
                for (size_t si = 0; si < cat->count; si++) {
                    catalog_station_t* st = &cat->stations[si];

                    staging_ensure_capacity(s_staging_count + 1);
                    if (s_staging_count < s_staging_capacity) {
                        staging_station_t* ss = &s_staging[s_staging_count++];
                        ss->id = st->id;
                        strncpy(ss->name, st->name, sizeof(ss->name) - 1);
                        strncpy(ss->url, st->url, sizeof(ss->url) - 1);
                        strncpy(ss->group, group, sizeof(ss->group) - 1);
                        ss->bitrate_kbps = st->bitrate_kbps;
                        ss->is_alive = false;
                        ss->last_checked = 0;
                        total_parsed++;
                    }
                }
            }
        }

        // Освобождаем память, которую напарсил M3U в g_catalog
        for (size_t i = 0; i < g_catalog.category_count; i++) {
            if (g_catalog.categories[i].stations) heap_caps_free(g_catalog.categories[i].stations);
        }
        if (g_catalog.categories) heap_caps_free(g_catalog.categories);

        // Восстанавливаем g_catalog
        g_catalog.categories = saved_cats;
        g_catalog.category_count = saved_cat_count;
        g_catalog.category_capacity = saved_cat_cap;
        g_catalog.total_stations = saved_total;
        g_catalog.loaded = saved_loaded;
    }

    closedir(d);
    ESP_LOGI(TAG, "Staging loaded: %d stations from M3U dir", total_parsed);
    return total_parsed > 0;
}

// ============================================================
// Staging Validation & Merge Implementation
// ============================================================
static bool catalog_validate_station_url(const char* url) {
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 1500;
    config.buffer_size = 512;
    config.user_agent = "WinampMPEG/5.0";

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    esp_http_client_set_header(client, "Icy-MetaData", "1");

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200 && status != 206) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    char* ct = nullptr;
    esp_http_client_get_header(client, "Content-Type", &ct);
    if (ct && strcasestr(ct, "text/html")) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    uint8_t buf[512];
    int read_len = esp_http_client_read(client, (char*)buf, sizeof(buf));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len < 32) return false;

    // Проверка магических чисел
    if (buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') return true;
    if (buf[0] == 'O' && buf[1] == 'g' && buf[2] == 'g' && buf[3] == 'S') return true;
    if (buf[0] == 'f' && buf[1] == 'L' && buf[2] == 'a' && buf[3] == 'C') return true;
    if (buf[0] == '#' && buf[1] == 'E' && buf[2] == 'X' && buf[3] == 'T') return true;

    for (int i = 0; i < read_len - 1 && i < 512; i++) {
        if (buf[i] == 0xFF && (buf[i+1] & 0xE0) == 0xE0) {
            return true;
        }
    }

    return false;
}

int catalog_staging_validate(void) {
    if (s_staging_count == 0) {
        ESP_LOGW(TAG, "Staging catalog is empty, nothing to validate");
        return 0;
    }

    s_is_validating = true;
    s_staging_total_count = (int)s_staging_count;
    s_staging_validated_count = 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);

    ESP_LOGI(TAG, "Starting validation of %d staging stations...", (int)s_staging_count);

    int alive_count = 0;
    for (size_t i = 0; i < s_staging_count; i++) {
        if (!s_staging[i].is_alive) {
            bool alive = catalog_validate_station_url(s_staging[i].url);
            s_staging[i].is_alive = alive;
            s_staging[i].last_checked = now;
            if (alive) alive_count++;
        }
        s_staging_validated_count++;

        // Даем 50 мс на отрисовку UI и работу аудио-декодера
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    s_is_validating = false;
    ESP_LOGI(TAG, "Validation complete: %d alive out of %d checked", alive_count, (int)s_staging_count);
    return (int)s_staging_count;
}

bool catalog_staging_merge_to_working(void) {
    if (s_staging_count == 0) {
        ESP_LOGW(TAG, "Staging catalog is empty, nothing to merge");
        return false;
    }

    int added_count = 0;
    int skipped_count = 0;
    int dead_count = 0;

    ESP_LOGI(TAG, "Merging staging catalog to working catalog...");

    for (size_t i = 0; i < s_staging_count; i++) {
        if (!s_staging[i].is_alive) {
            dead_count++;
            continue;
        }

        // Проверяем, есть ли уже такая станция по ID (дедупликация по URL)
        size_t cat_idx = 0, st_idx = 0;
        if (catalog_find_station_by_id(s_staging[i].id, &cat_idx, &st_idx)) {
            skipped_count++;
            continue; // Уже есть в основном каталоге
        }

        // Добавляем в основной каталог
        if (catalog_add_station(
            s_staging[i].group[0] != '\0' ? s_staging[i].group : "Imported",
            s_staging[i].name,
            s_staging[i].url,
            s_staging[i].bitrate_kbps
        )) {
            added_count++;
        }
    }

    ESP_LOGI(TAG, "Merge complete: Added %d, Skipped (duplicate) %d, Dead %d",
             added_count, skipped_count, dead_count);

    // Сохраняем обновленный основной каталог на SD
    ensure_sd_radio_dir();
    bool ok = catalog_save_json_file(CATALOG_JSON_PATH);

    // Очищаем staging после успешного слияния
    if (ok) {
        catalog_staging_clear();
        ESP_LOGI(TAG, "Staging cleared after successful merge");
    }

    return ok;
}

static void catalog_validation_task(void *pvParameters) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    int alive_count = 0;

    for (size_t i = 0; i < s_staging_count; i++) {
        if (!s_is_validating) break; // Если очистили каталог во время валидации

        if (!s_staging[i].is_alive) {
            bool alive = catalog_validate_station_url(s_staging[i].url);
            s_staging[i].is_alive = alive;
            s_staging[i].last_checked = now;
            if (alive) alive_count++;
        }
        s_staging_validated_count++;

        // Логируем прогресс каждые 100 станций
        if (s_staging_validated_count % 100 == 0) {
            ESP_LOGI(TAG, "Validation progress: %d/%d", s_staging_validated_count, s_staging_total_count);
        }

        // Даём 50 мс на отрисовку UI и работу аудио-декодера
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 2. Восстанавливаем уровни логирования обратно
    esp_log_level_set("esp-tls", s_old_tls_level);
    esp_log_level_set("transport_base", s_old_transport_level);
    esp_log_level_set("HTTP_CLIENT", s_old_http_level);

    ESP_LOGI(TAG, "Validation complete: %d alive out of %d checked", alive_count, s_staging_total_count);
    s_is_validating = false;
    s_validation_task_handle = NULL;
    vTaskDelete(NULL);
}


// ============================================================
// Загрузка текущего рабочего каталога в Staging (для очистки)
// ============================================================
bool catalog_load_working_to_staging(void) {
    if (!g_catalog.loaded || g_catalog.total_stations == 0) {
        ESP_LOGW(TAG, "Working catalog is empty, nothing to load");
        return false;
    }

    catalog_staging_clear(); // Очищаем старый буфер

    for (size_t ci = 0; ci < g_catalog.category_count; ci++) {
        const catalog_category_t* cat = &g_catalog.categories[ci];
        for (size_t si = 0; si < cat->count; si++) {
            const catalog_station_t* st = &cat->stations[si];

            staging_ensure_capacity(s_staging_count + 1);
            if (s_staging_count < s_staging_capacity) {
                staging_station_t* ss = &s_staging[s_staging_count++];
                ss->id = st->id;
                strncpy(ss->name, st->name, sizeof(ss->name) - 1);
                strncpy(ss->url, st->url, sizeof(ss->url) - 1);
                strncpy(ss->group, cat->name, sizeof(ss->group) - 1);
                ss->bitrate_kbps = st->bitrate_kbps;
                ss->is_alive = false; // Сбрасываем, чтобы валидатор проверил заново
                ss->last_checked = 0;
            }
        }
    }

    ESP_LOGI(TAG, "Working catalog loaded to staging: %d stations ready for validation", (int)s_staging_count);
    return s_staging_count > 0;
}

// ============================================================
// Групповое удаление станций
// ============================================================
bool catalog_remove_stations(const uint32_t* ids, size_t count) {
    if (ids == nullptr || count == 0) return false;

    size_t removed_count = 0;

    for (size_t ci = 0; ci < g_catalog.category_count; ci++) {
        catalog_category_t* cat = &g_catalog.categories[ci];
        size_t new_count = 0;

        for (size_t si = 0; si < cat->count; si++) {
            bool should_remove = false;
            for (size_t i = 0; i < count; i++) {
                if (cat->stations[si].id == ids[i]) {
                    should_remove = true;
                    break;
                }
            }

            if (!should_remove) {
                if (new_count != si) {
                    cat->stations[new_count] = cat->stations[si];
                }
                new_count++;
            } else {
                removed_count++;
            }
        }

        cat->count = new_count;
    }

    g_catalog.total_stations -= removed_count;

    // Удаляем эти ID из избранного
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < g_catalog.favorite_count; j++) {
            if (g_catalog.favorite_ids[j] == ids[i]) {
                for (size_t k = j; k < g_catalog.favorite_count - 1; k++) {
                    g_catalog.favorite_ids[k] = g_catalog.favorite_ids[k + 1];
                }
                g_catalog.favorite_count--;
                break;
            }
        }
    }

    // Сохраняем изменения на SD
    if (sd_manager_is_mounted()) {
        ensure_sd_radio_dir();
        catalog_save_json_file(CATALOG_JSON_PATH);
        catalog_save_favorites();
    }

    ESP_LOGI(TAG, "Removed %d stations from catalog", (int)removed_count);
    return removed_count > 0;
}

// ============================================================
// Удаление категории
// ============================================================
bool catalog_remove_category(size_t category_index) {
    if (category_index >= g_catalog.category_count) return false;

    catalog_category_t* cat = &g_catalog.categories[category_index];

    // 1. Удаляем станции внутри категории
    if (cat->stations) {
        heap_caps_free(cat->stations);
        cat->stations = nullptr;
    }
    g_catalog.total_stations -= cat->count;

    // 2. Сдвигаем оставшиеся категории, чтобы закрыть дыру
    for (size_t i = category_index; i < g_catalog.category_count - 1; i++) {
        g_catalog.categories[i] = g_catalog.categories[i + 1];
    }
    g_catalog.category_count--;

    // 3. Сохраняем обновленный каталог на SD
    if (sd_manager_is_mounted()) {
        ensure_sd_radio_dir();
        catalog_save_json_file(CATALOG_JSON_PATH);
    }

    ESP_LOGI(TAG, "Category removed at index %d", (int)category_index);
    return true;
}

// ============================================================
// Пакетное удаление категорий (с сортировкой индексов)
// ============================================================
size_t catalog_remove_categories(const size_t* indices, size_t count) {
    if (indices == nullptr || count == 0) return 0;

    // Копируем индексы и сортируем по убыванию, чтобы при удалении
    // индексы не "сдвигались" и мы не удалили не те категории.
    size_t sorted[64];
    size_t actual_count = (count > 64) ? 64 : count;
    for (size_t i = 0; i < actual_count; i++) sorted[i] = indices[i];

    // Простая пузырьковая сортировка по убыванию
    for (size_t i = 0; i < actual_count - 1; i++) {
        for (size_t j = 0; j < actual_count - i - 1; j++) {
            if (sorted[j] < sorted[j + 1]) {
                size_t temp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = temp;
            }
        }
    }

    size_t removed = 0;
    for (size_t i = 0; i < actual_count; i++) {
        size_t idx = sorted[i];
        if (idx >= g_catalog.category_count) continue; // Защита от невалидных индексов

        catalog_category_t* cat = &g_catalog.categories[idx];

        // Удаляем станции внутри категории
        if (cat->stations) {
            heap_caps_free(cat->stations);
            cat->stations = nullptr;
        }
        g_catalog.total_stations -= cat->count;

        // Сдвигаем оставшиеся категории
        for (size_t k = idx; k < g_catalog.category_count - 1; k++) {
            g_catalog.categories[k] = g_catalog.categories[k + 1];
        }
        g_catalog.category_count--;
        removed++;
    }

    // Сохраняем обновлённый каталог на SD
    if (removed > 0 && sd_manager_is_mounted()) {
        ensure_sd_radio_dir();
        catalog_save_json_file(CATALOG_JSON_PATH);
    }

    ESP_LOGI(TAG, "Removed %d categories", (int)removed);
    return removed;
}
