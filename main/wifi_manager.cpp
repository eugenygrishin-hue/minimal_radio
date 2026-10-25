#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <cstring>

#include <stdlib.h>
#include <time.h>
#include "esp_sntp.h"

#include "cJSON.h"
#include "sd_manager.h"
#include <sys/stat.h>

static known_network_t s_known_networks[MAX_KNOWN_NETWORKS];
static int s_known_networks_count = 0;
static char s_last_attempted_ssid[33] = {0};
static char s_last_attempted_pass[65] = {0};

static const char* TAG = "WIFI";

static EventGroupHandle_t s_wifi_event_group = NULL;
static const int WIFI_CONNECTED_BIT = BIT0;
static volatile bool s_is_connected = false;

static int s_connection_attempts = 0;
static const int MAX_CONNECTION_ATTEMPTS = 3;

static volatile bool s_testing_mode = false;
static volatile bool s_test_passed = false;
static volatile bool s_test_ignore_disconnect = false;
static volatile wifi_test_result_t s_last_error = WIFI_TEST_NOT_STARTED;

static volatile bool s_manual_reconnect = false;
static volatile uint8_t s_last_disconnect_reason = 0;
static volatile bool s_test_running = false;
static bool s_sntp_started = false;

static wifi_scan_record_t s_scan_results[20];
static int s_scan_count = 0;

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi started, connecting...");
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*)data;
        s_is_connected = false;
        s_last_disconnect_reason = disconnected->reason;

        if (disconnected->reason == WIFI_REASON_AUTH_FAIL || disconnected->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
            s_last_error = WIFI_TEST_FAILED_AUTH;
        } else if (disconnected->reason == WIFI_REASON_NO_AP_FOUND) {
            s_last_error = WIFI_TEST_FAILED_NOT_FOUND;
        } else {
            s_last_error = WIFI_TEST_FAILED_OTHER;
        }

        if (s_testing_mode) {
            if (s_test_ignore_disconnect && disconnected->reason == WIFI_REASON_ASSOC_LEAVE) {
                s_test_ignore_disconnect = false;
                return;
            }
            ESP_LOGW(TAG, "Wi-Fi disconnected during test (reason: %d)", disconnected->reason);
            s_test_passed = false;
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            return;
        }

        if (s_manual_reconnect) {
            ESP_LOGI(TAG, "Manual reconnect in progress, skipping auto reconnect (reason: %d)", disconnected->reason);
            return;
        }

        s_connection_attempts++;
        if (s_connection_attempts <= MAX_CONNECTION_ATTEMPTS) {
            ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting... (attempt %d/%d, reason: %d)", s_connection_attempts, MAX_CONNECTION_ATTEMPTS, disconnected->reason);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Wi-Fi connection failed after %d attempts", MAX_CONNECTION_ATTEMPTS);
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_is_connected = true;
        s_connection_attempts = 0;
        s_last_error = WIFI_TEST_NOT_STARTED;
        s_last_disconnect_reason = 0;

        // АВТО-СОХРАНЕНИЕ: если подключились успешно, добавляем в знакомые сети
        if (s_last_attempted_ssid[0] != '\0') {
            wifi_known_networks_add(s_last_attempted_ssid, s_last_attempted_pass);
        }

        if (s_testing_mode) {
            s_test_passed = true;
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_connect(const char* ssid, const char* pass) {
    ESP_LOGI(TAG, "Connecting to Wi-Fi: SSID='%s'", ssid);
    strncpy(s_last_attempted_ssid, ssid, sizeof(s_last_attempted_ssid) - 1);
    strncpy(s_last_attempted_pass, pass, sizeof(s_last_attempted_pass) - 1);
    s_connection_attempts = 0;

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wc = {};
    strncpy((char*)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char*)wc.sta.password, pass, sizeof(wc.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "Waiting for connection...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (s_is_connected) {
        ESP_LOGI(TAG, "Wi-Fi connected successfully");
    } else {
        ESP_LOGE(TAG, "Wi-Fi connection failed - user can fix settings via UI");
    }
}

void wifi_reconnect(const char* ssid, const char* pass) {
    if (s_testing_mode || s_test_running) {
        ESP_LOGW(TAG, "Wi-Fi test active, ignoring reconnect request");
        return;
    }

    ESP_LOGI(TAG, "Reconnecting to Wi-Fi: SSID='%s'", ssid);
    s_connection_attempts = 0;
    s_manual_reconnect = true;

    if (s_wifi_event_group != NULL) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }

    wifi_config_t wc = {};
    strncpy((char*)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char*)wc.sta.password, pass, sizeof(wc.sta.password) - 1);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
    }

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_wifi_connect();
    vTaskDelay(pdMS_TO_TICKS(300));
    s_manual_reconnect = false;
}

bool wifi_is_connected() { return s_is_connected; }
wifi_test_result_t wifi_get_last_error() { return s_last_error; }

bool wifi_wait_connected(uint32_t timeout_ms) {
    if (s_is_connected) return true;
    if (s_wifi_event_group == NULL) return false;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return s_is_connected && (bits & WIFI_CONNECTED_BIT);
}

bool wifi_test_connection(const char* ssid, const char* pass) {
    if (s_test_running) {
        ESP_LOGW(TAG, "Wi-Fi test already running, refusing concurrent test");
        return false;
    }
    s_test_running = true;
    ESP_LOGI(TAG, "Testing Wi-Fi connection: SSID='%s'", ssid);
    s_testing_mode = true;
    bool result = false;

    for (int attempt = 1; attempt <= 2; attempt++) {
        s_test_passed = false;
        s_test_ignore_disconnect = true;
        s_last_error = WIFI_TEST_NOT_STARTED;
        s_last_disconnect_reason = 0;

        if (s_wifi_event_group != NULL) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }

        wifi_config_t wc = {};
        strncpy((char*)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
        strncpy((char*)wc.sta.password, pass, sizeof(wc.sta.password) - 1);

        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (err != ESP_OK) ESP_LOGW(TAG, "Test set_config failed: %s", esp_err_to_name(err));

        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_wifi_connect();

        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));

        if (s_test_passed) {
            ESP_LOGI(TAG, "Wi-Fi test: SUCCESS (attempt %d)", attempt);
            result = true;
            break;
        }

        ESP_LOGW(TAG, "Wi-Fi test attempt %d failed: error=%d, reason=%d", attempt, s_last_error, s_last_disconnect_reason);
        if (attempt == 1 && s_last_error != WIFI_TEST_FAILED_NOT_FOUND) {
            ESP_LOGW(TAG, "Retrying Wi-Fi test once...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        break;
    }

    s_testing_mode = false;
    s_test_ignore_disconnect = false;
    if (!result && s_last_error == WIFI_TEST_NOT_STARTED) s_last_error = WIFI_TEST_FAILED_TIMEOUT;
    if (!result) ESP_LOGW(TAG, "Wi-Fi test: FAILED (error=%d)", s_last_error);
    s_test_running = false;
    return result;
}

bool wifi_is_testing() { return s_testing_mode; }

int wifi_get_rssi(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

void wifi_start_sntp(void) {
    if (s_sntp_started) return;
    setenv("TZ", "MSK-3", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started, timezone=MSK-3");
}

int wifi_scan_networks(void) {
    ESP_LOGI(TAG, "Starting Wi-Fi scan...");
    wifi_scan_config_t scan_config = { .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan start failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t ap_count = 20;
    wifi_ap_record_t *ap_info = (wifi_ap_record_t *)heap_caps_malloc(20 * sizeof(wifi_ap_record_t), MALLOC_CAP_INTERNAL);
    if (!ap_info) {
        ESP_LOGE(TAG, "Failed to allocate memory for Wi-Fi scan results");
        return -1;
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan get records failed: %s", esp_err_to_name(err));
        free(ap_info);
        return -1;
    }

    s_scan_count = ap_count;
    for (int i = 0; i < ap_count; i++) {
        memcpy(s_scan_results[i].ssid, ap_info[i].ssid, 33);
        s_scan_results[i].rssi = ap_info[i].rssi;
    }
    free(ap_info);

    for (int i = 0; i < s_scan_count - 1; i++) {
        for (int j = 0; j < s_scan_count - i - 1; j++) {
            if (s_scan_results[j].rssi < s_scan_results[j + 1].rssi) {
                wifi_scan_record_t temp = s_scan_results[j];
                s_scan_results[j] = s_scan_results[j + 1];
                s_scan_results[j + 1] = temp;
            }
        }
    }
    ESP_LOGI(TAG, "Wi-Fi scan completed, found %d networks", s_scan_count);
    return s_scan_count;
}

int wifi_scan_get_count(void) { return s_scan_count; }
const wifi_scan_record_t* wifi_scan_get_record(int index) {
    if (index < 0 || index >= s_scan_count) return NULL;
    return &s_scan_results[index];
}

// ============================================================
// Known Networks Implementation
// ============================================================
bool wifi_known_networks_load(void) {
    if (!sd_manager_is_mounted()) return false;
    size_t len = 0;
    char* text = sd_manager_read_text("/sd/radio/known_networks.json", &len);
    if (!text) return false;

    cJSON* root = cJSON_Parse(text);
    free(text);
    if (!root) return false;

    s_known_networks_count = 0;
    cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "networks");
    if (cJSON_IsArray(arr)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, arr) {
            if (s_known_networks_count < MAX_KNOWN_NETWORKS) {
                cJSON* ssid = cJSON_GetObjectItemCaseSensitive(item, "ssid");
                cJSON* pass = cJSON_GetObjectItemCaseSensitive(item, "pass");
                cJSON* auto_conn = cJSON_GetObjectItemCaseSensitive(item, "auto");

                if (cJSON_IsString(ssid) && cJSON_IsString(pass)) {
                    strncpy(s_known_networks[s_known_networks_count].ssid, ssid->valuestring, 32);
                    strncpy(s_known_networks[s_known_networks_count].pass, pass->valuestring, 64);
                    s_known_networks[s_known_networks_count].auto_connect = cJSON_IsTrue(auto_conn);
                    s_known_networks_count++;
                }
            }
        }
    }
    cJSON_Delete(root);
    return true;
}

bool wifi_known_networks_save(void) {
    if (!sd_manager_is_mounted()) return false;
    mkdir("/sd/radio", 0777);

    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < s_known_networks_count; i++) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", s_known_networks[i].ssid);
        cJSON_AddStringToObject(item, "pass", s_known_networks[i].pass);
        cJSON_AddBoolToObject(item, "auto", s_known_networks[i].auto_connect);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "networks", arr);

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    bool ok = sd_manager_write_text("/sd/radio/known_networks.json", out);
    cJSON_free(out);
    return ok;
}

bool wifi_known_networks_add(const char* ssid, const char* pass) {
    if (!ssid || !pass) return false;
    for (int i = 0; i < s_known_networks_count; i++) {
        if (strcmp(s_known_networks[i].ssid, ssid) == 0) {
            strncpy(s_known_networks[i].pass, pass, sizeof(s_known_networks[i].pass) - 1);
            s_known_networks[i].auto_connect = true;
            return wifi_known_networks_save();
        }
    }
    if (s_known_networks_count < MAX_KNOWN_NETWORKS) {
        strncpy(s_known_networks[s_known_networks_count].ssid, ssid, sizeof(s_known_networks[s_known_networks_count].ssid) - 1);
        strncpy(s_known_networks[s_known_networks_count].pass, pass, sizeof(s_known_networks[s_known_networks_count].pass) - 1);
        s_known_networks[s_known_networks_count].auto_connect = true;
        s_known_networks_count++;
        return wifi_known_networks_save();
    }
    return false;
}

bool wifi_known_networks_remove(int index) {
    if (index < 0 || index >= s_known_networks_count) return false;
    for (int i = index; i < s_known_networks_count - 1; i++) {
        s_known_networks[i] = s_known_networks[i + 1];
    }
    s_known_networks_count--;
    return wifi_known_networks_save();
}

int wifi_known_networks_get_count(void) { return s_known_networks_count; }
const known_network_t* wifi_known_networks_get(int index) {
    if (index < 0 || index >= s_known_networks_count) return NULL;
    return &s_known_networks[index];
}

bool wifi_try_auto_connect(void) {
    if (s_known_networks_count == 0) return false;
    ESP_LOGI(TAG, "Scanning for known networks...");
    int count = wifi_scan_networks();
    if (count <= 0) return false;

    int best_rssi = -1000;
    int best_net_idx = -1;

    for (int i = 0; i < count; i++) {
        const wifi_scan_record_t* rec = wifi_scan_get_record(i);
        if (!rec) continue;
        for (int j = 0; j < s_known_networks_count; j++) {
            if (s_known_networks[j].auto_connect && strcmp(rec->ssid, s_known_networks[j].ssid) == 0) {
                if (rec->rssi > best_rssi) {
                    best_rssi = rec->rssi;
                    best_net_idx = j;
                }
            }
        }
    }

    if (best_net_idx >= 0) {
        ESP_LOGI(TAG, "Auto-connecting to best known network: %s (RSSI: %d)", s_known_networks[best_net_idx].ssid, best_rssi);
        wifi_connect(s_known_networks[best_net_idx].ssid, s_known_networks[best_net_idx].pass);
        return true;
    }
    return false;
}
