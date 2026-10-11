#include "sd_manager.h"

//#include <M5Unified.h>

#include <esp_log.h>
#include <esp_err.h>

#include <esp_vfs.h>
#include <esp_vfs_fat.h>

#include <driver/spi_common.h>
#include <driver/sdspi_host.h>
#include <sdmmc_cmd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char* TAG = "SD";

static const char* MOUNT_POINT = "/sd";
static const char* RADIO_DIR = "/sd/radio";
static const char* SELFTEST_FILE = "/sd/radio/selftest.txt";

static bool s_mounted = false;
static bool s_own_spi_bus = false;
static sdmmc_card_t* s_card = nullptr;

// Проверенные пины SD для M5Stack CoreS3 SE из старого рабочего диалога.
#define SD_SPI_HOST   SPI2_HOST
#define SD_CS_PIN     GPIO_NUM_4
#define SD_MOSI_PIN   GPIO_NUM_37
#define SD_MISO_PIN   GPIO_NUM_35
#define SD_CLK_PIN    GPIO_NUM_36

// Для стабильности временно используем низкую скорость.
// 20 MHz работал один раз, но для SD 2GB + общий SPI с дисплеем лучше 1 MHz.
#define SD_SPI_MAX_FREQ_KHZ 1000

const char* sd_manager_mount_point(void)
{
    return MOUNT_POINT;
}

bool sd_manager_is_mounted(void)
{
    return s_mounted;
}

uint64_t sd_manager_free_bytes(void)
{
    if (!s_mounted) {
        return 0;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;

    esp_err_t err = esp_vfs_fat_info(MOUNT_POINT, &total_bytes, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_info failed: %s", esp_err_to_name(err));
        return 0;
    }

    return free_bytes;
}

bool sd_manager_ensure_dir(const char* path)
{
    if (!s_mounted || path == nullptr) {
        return false;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    if (mkdir(path, 0777) == 0) {
        ESP_LOGI(TAG, "Created directory: %s", path);
        return true;
    }

    ESP_LOGE(TAG, "Failed to create directory: %s", path);
    return false;
}

bool sd_manager_write_text(const char* path, const char* text)
{
    if (!s_mounted || path == nullptr || text == nullptr) {
        return false;
    }

    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Failed to open for write: %s", path);
        return false;
    }

    size_t len = strlen(text);
    size_t written = fwrite(text, 1, len, f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "Short write: %s (%u/%u)",
                 path,
                 (unsigned)written,
                 (unsigned)len);
        return false;
    }

    return true;
}

char* sd_manager_read_text(const char* path, size_t* out_len)
{
    if (out_len != nullptr) {
        *out_len = 0;
    }

    if (!s_mounted || path == nullptr) {
        return nullptr;
    }

    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Failed to open for read: %s", path);
        return nullptr;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return nullptr;
    }

    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return nullptr;
    }

    rewind(f);

    char* buf = (char*)malloc((size_t)size + 1);
    if (buf == nullptr) {
        ESP_LOGE(TAG, "OOM for file: %s", path);
        fclose(f);
        return nullptr;
    }

    size_t read_bytes = fread(buf, 1, (size_t)size, f);
    buf[read_bytes] = '\0';
    fclose(f);

    if (out_len != nullptr) {
        *out_len = read_bytes;
    }

    return buf;
}

bool sd_manager_init(void)
{
    if (s_mounted) {
        return true;
    }

    ESP_LOGI(TAG, "Initializing SD card via SPI (CoreS3 SE proven pins)...");
    ESP_LOGI(TAG, "SPI host: %d", (int)SD_SPI_HOST);
    ESP_LOGI(TAG, "Pins: CS=%d MOSI=%d MISO=%d CLK=%d",
             (int)SD_CS_PIN,
             (int)SD_MOSI_PIN,
             (int)SD_MISO_PIN,
             (int)SD_CLK_PIN);
    ESP_LOGI(TAG, "Max SD SPI frequency: %d kHz", SD_SPI_MAX_FREQ_KHZ);

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = SD_MOSI_PIN;
    bus_cfg.miso_io_num = SD_MISO_PIN;
    bus_cfg.sclk_io_num = SD_CLK_PIN;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4096;

    // M5GFX уже мог инициализировать SPI2 для дисплея.
    // Глушим шум именно на время попытки инициализации шины.
    esp_log_level_set("spi", ESP_LOG_NONE);
    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    esp_log_level_set("spi", ESP_LOG_WARN);

    if (ret == ESP_OK) {
        s_own_spi_bus = true;
        ESP_LOGI(TAG, "SPI bus initialized for SD");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        s_own_spi_bus = false;
        ESP_LOGW(TAG, "SPI bus already initialized. Reusing it for SD.");
        ESP_LOGW(TAG, "WARNING: this assumes M5GFX uses the same MOSI/MISO/CLK pins as SD.");
    } else {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        s_own_spi_bus = false;
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_SPI_MAX_FREQ_KHZ;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_PIN;
    slot_config.host_id = SD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;
    mount_config.disk_status_check_enable = false;

    // Для отладки можно временно поставить ESP_LOG_INFO.
    // Потом вернём WARN.
    esp_log_level_set("sdspi_transaction", ESP_LOG_WARN);

    ret = esp_vfs_fat_sdspi_mount(
        MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &s_card
    );

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "Failed to mount filesystem. "
                          "Ensure SD card is inserted and formatted as FAT32/FAT16.");
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "SD card timeout. Check physical insertion and card format.");
        } else if (ret == ESP_ERR_INVALID_RESPONSE) {
            ESP_LOGW(TAG, "SD card invalid response. Try lower SPI frequency or check pins.");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "SD card invalid state. Try full power-cycle of the board.");
        } else {
            ESP_LOGW(TAG, "Failed to initialize SD card: %s", esp_err_to_name(ret));
        }

        s_card = nullptr;
        s_mounted = false;

        // Освобождаем SPI bus только если его инициализировали мы.
        if (s_own_spi_bus) {
            esp_err_t free_ret = spi_bus_free(SD_SPI_HOST);
            if (free_ret != ESP_OK) {
                ESP_LOGW(TAG, "spi_bus_free failed: %s", esp_err_to_name(free_ret));
            }
            s_own_spi_bus = false;
        }

        return false;
    }

    s_mounted = true;

    ESP_LOGI(TAG, "SD mounted at %s", MOUNT_POINT);
    ESP_LOGI(TAG, "Free space: %llu MB",
             (unsigned long long)(sd_manager_free_bytes() / (1024ULL * 1024ULL)));

    if (s_card != nullptr) {
        sdmmc_card_print_info(stdout, s_card);
    }

    if (!sd_manager_ensure_dir(RADIO_DIR)) {
        ESP_LOGW(TAG, "Failed to ensure directory %s", RADIO_DIR);
    }

    return true;
}

bool sd_manager_self_test(void)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "Self-test skipped: SD not mounted");
        return false;
    }

    if (!sd_manager_ensure_dir(RADIO_DIR)) {
        ESP_LOGE(TAG, "Self-test failed: cannot ensure %s", RADIO_DIR);
        return false;
    }

    const char* payload = "OK\n";

    if (!sd_manager_write_text(SELFTEST_FILE, payload)) {
        ESP_LOGE(TAG, "Self-test failed: write error");
        return false;
    }

    size_t len = 0;
    char* readback = sd_manager_read_text(SELFTEST_FILE, &len);

    bool ok = (readback != nullptr) &&
              (len == strlen(payload)) &&
              (strcmp(readback, payload) == 0);

    if (readback != nullptr) {
        free(readback);
    }

    if (remove(SELFTEST_FILE) != 0) {
        ESP_LOGW(TAG, "Self-test warning: failed to remove %s", SELFTEST_FILE);
    }

    ESP_LOGI(TAG, "Self-test: %s", ok ? "OK" : "FAIL");
    return ok;
}
