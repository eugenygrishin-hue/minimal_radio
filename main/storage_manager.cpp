#include "storage_manager.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "STORAGE";

// Ключи для NVS
static const char* NVS_NAMESPACE_WIFI = "wifi_cfg";
static const char* NVS_KEY_SSID = "ssid";
static const char* NVS_KEY_PASS = "pass";

static const char* NVS_NAMESPACE_RADIO = "radio";
static const char* NVS_KEY_LAST_STATION = "last_st";

static const char* NVS_KEY_LAST_ST_ID = "last_st_id";

void storage_init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS initialization failed, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "Storage initialized");
}

void storage_save_wifi(const char* ssid, const char* pass) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE_WIFI, esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save SSID: %s", esp_err_to_name(err));
    }

    err = nvs_set_str(handle, NVS_KEY_PASS, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save password: %s", esp_err_to_name(err));
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Wi-Fi credentials saved: SSID='%s'", ssid);
}

bool storage_load_wifi(char* ssid, size_t ssid_len, char* pass, size_t pass_len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace '%s' not found", NVS_NAMESPACE_WIFI);
        return false;
    }

    size_t ssid_size = ssid_len;
    err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load SSID: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }

    size_t pass_size = pass_len;
    err = nvs_get_str(handle, NVS_KEY_PASS, pass, &pass_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load password: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Wi-Fi credentials loaded: SSID='%s'", ssid);
    return true;
}

void storage_save_station(int station_idx) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_RADIO, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE_RADIO, esp_err_to_name(err));
        return;
    }

    err = nvs_set_i32(handle, NVS_KEY_LAST_STATION, station_idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save station index: %s", esp_err_to_name(err));
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Station index saved: %d", station_idx);
}

int storage_load_station() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_RADIO, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace '%s' not found, returning default", NVS_NAMESPACE_RADIO);
        return 0;
    }

    int32_t station_idx = 0;
    err = nvs_get_i32(handle, NVS_KEY_LAST_STATION, &station_idx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load station index: %s", esp_err_to_name(err));
        nvs_close(handle);
        return 0;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Station index loaded: %d", station_idx);
    return (int)station_idx;
}

void storage_save_last_station_id(uint32_t station_id) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_RADIO, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE_RADIO, esp_err_to_name(err));
        return;
    }
    err = nvs_set_u32(handle, NVS_KEY_LAST_ST_ID, station_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save station ID: %s", esp_err_to_name(err));
    }
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "Station ID saved: 0x%08lx", (unsigned long)station_id);
}

uint32_t storage_load_last_station_id(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_RADIO, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace '%s' not found for ID, returning 0", NVS_NAMESPACE_RADIO);
        return 0;
    }
    uint32_t station_id = 0;
    err = nvs_get_u32(handle, NVS_KEY_LAST_ST_ID, &station_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load station ID: %s", esp_err_to_name(err));
        nvs_close(handle);
        return 0;
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "Station ID loaded: 0x%08lx", (unsigned long)station_id);
    return station_id;
}
