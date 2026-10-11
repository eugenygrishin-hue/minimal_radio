#pragma once
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

#define HMI_ADDR             0x41
#define ENCODER_REG          0x00
#define INCREMENT_REG        0x10
#define BUTTON_REG           0x20
#define LED_REG              0x30
#define RESET_COUNTER_REG    0x40
#define FIRMWARE_VERSION_REG 0xFE
#define I2C_ADDRESS_REG      0xFF

class M5ModuleHMI_ESPIDF {
public:
    bool begin(i2c_port_t port_num, int sda_pin, int scl_pin, uint32_t speed = 100000) {
        _port = port_num;
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port = port_num;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.sda_io_num = (gpio_num_t)sda_pin;
        bus_cfg.scl_io_num = (gpio_num_t)scl_pin;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = true;

        esp_err_t err = i2c_new_master_bus(&bus_cfg, &_bus_handle);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE("HMI", "i2c_new_master_bus failed: %s", esp_err_to_name(err));
            return false;
        }

        err = i2c_master_probe(_bus_handle, HMI_ADDR, 1000 / portTICK_PERIOD_MS);
        if (err != ESP_OK) {
            ESP_LOGE("HMI", "Device 0x%02X not found", HMI_ADDR);
            if (_bus_handle) i2c_del_master_bus(_bus_handle);
            _bus_handle = nullptr;
            return false;
        }

        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = HMI_ADDR;
        dev_cfg.scl_speed_hz = speed;

        err = i2c_master_bus_add_device(_bus_handle, &dev_cfg, &_dev_handle);
        if (err != ESP_OK) {
            ESP_LOGE("HMI", "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
            if (_bus_handle) i2c_del_master_bus(_bus_handle);
            _bus_handle = nullptr;
            return false;
        }

        ESP_LOGI("HMI", "HMI Module initialized on pins SDA:%d, SCL:%d", sda_pin, scl_pin);
        return true;
    }

    int32_t getEncoderValue() {
        uint8_t buf[4] = {0};
        if (!readRegister(ENCODER_REG, buf, 4)) return 0;
        return (int32_t)(buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24));
    }

    int32_t getIncrementValue() {
        uint8_t buf[4] = {0};
        if (!readRegister(INCREMENT_REG, buf, 4)) return 0;
        return (int32_t)(buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24));
    }

    bool getButtonS() {
        uint8_t val = 0;
        if (!readRegister(BUTTON_REG, &val, 1)) return false;
        return val == 0;
    }

    bool getButton1() {
        uint8_t val = 0;
        if (!readRegister(BUTTON_REG + 1, &val, 1)) return false;
        return val == 0;
    }

    bool getButton2() {
        uint8_t val = 0;
        if (!readRegister(BUTTON_REG + 2, &val, 1)) return false;
        return val == 0;
    }

    void resetCounter() {
        uint8_t dummy = 0;
        writeRegister(RESET_COUNTER_REG, &dummy, 1);
    }

    void setLEDStatus(uint8_t index, uint8_t status) {
        uint8_t buf[1] = {status};
        // Исправлено: регистры светодиодов начинаются с 0x30
        uint8_t reg = LED_REG + index - 1;  // 0x30 + 1 - 1 = 0x30, 0x30 + 2 - 1 = 0x31
        writeRegister(reg, buf, 1);
    }

private:
    i2c_master_bus_handle_t _bus_handle = nullptr;
    i2c_master_dev_handle_t _dev_handle = nullptr;
    i2c_port_t _port;

    bool readRegister(uint8_t reg, uint8_t* buffer, size_t length) {
        uint8_t write_buf[1] = {reg};
        esp_err_t err = i2c_master_transmit_receive(
            _dev_handle, write_buf, 1, buffer, length, 1000 / portTICK_PERIOD_MS
        );
        return (err == ESP_OK);
    }

    bool writeRegister(uint8_t reg, const uint8_t* buffer, size_t length) {
        uint8_t* write_buf = (uint8_t*)malloc(length + 1);
        if (!write_buf) return false;
        write_buf[0] = reg;
        if (buffer) memcpy(write_buf + 1, buffer, length);

        esp_err_t err = i2c_master_transmit(_dev_handle, write_buf, length + 1, 1000 / portTICK_PERIOD_MS);
        free(write_buf);
        return (err == ESP_OK);
    }
};
