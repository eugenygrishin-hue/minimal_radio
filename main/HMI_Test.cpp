#include <M5Unified.hpp>
#include "M5ModuleHMI_ESPIDF.hpp"

static const char *TAG = "HMI_TEST";
M5ModuleHMI_ESPIDF hmi;

extern "C" void app_main(void) {
    auto cfg = M5.config();
    M5.begin(cfg);

    if (!hmi.begin(I2C_NUM_0, 12, 11, 100000)) {
        ESP_LOGE(TAG, "Failed to initialize HMI module!");
        M5.Lcd.fillScreen(TFT_BLACK);
        M5.Lcd.setTextColor(TFT_RED);
        M5.Lcd.setCursor(10, 10);
        M5.Lcd.println("HMI Init FAILED!");
        return;
    }

    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("HMI M129 OK");

    // === ПРИНУДИТЕЛЬНОЕ ВЫКЛЮЧЕНИЕ СВЕТОДИОДОВ ПРИ СТАРТЕ ===
    ESP_LOGI(TAG, "Turning OFF both LEDs at startup...");
    hmi.setLEDStatus(1, 0);
    hmi.setLEDStatus(2, 0);
    vTaskDelay(pdMS_TO_TICKS(200)); // Даем модулю время обработать команду

    int32_t prev_enc = 0;
    bool prev_btn_s = false;
    bool prev_btn_a = false;
    bool prev_btn_b = false;

    while (1) {
        int32_t enc_val = hmi.getEncoderValue();
        bool btn_s = hmi.getButtonS();
        bool btn_a = hmi.getButton1();
        bool btn_b = hmi.getButton2();

        if (enc_val != prev_enc) {
            ESP_LOGI(TAG, "ENCODER: %ld", enc_val);
            prev_enc = enc_val;
        }
        if (btn_s != prev_btn_s) {
            ESP_LOGI(TAG, "BTN S (encoder): %s", btn_s ? "PRESSED" : "RELEASED");
            prev_btn_s = btn_s;
        }
        if (btn_a != prev_btn_a) {
            ESP_LOGI(TAG, "BTN A: %s", btn_a ? "PRESSED" : "RELEASED");
            prev_btn_a = btn_a;
        }
        if (btn_b != prev_btn_b) {
            ESP_LOGI(TAG, "BTN B: %s", btn_b ? "PRESSED" : "RELEASED");
            prev_btn_b = btn_b;
        }

        // Управление светодиодами
        uint8_t led_a_state = btn_a ? 1 : 0;
        uint8_t led_b_state = btn_b ? 1 : 0;

        hmi.setLEDStatus(1, led_a_state);
        hmi.setLEDStatus(2, led_b_state);

        // Обновляем экран
        M5.Lcd.fillRect(0, 40, 320, 100, TFT_BLACK);
        M5.Lcd.setCursor(10, 40);
        M5.Lcd.setTextColor(TFT_GREEN);
        M5.Lcd.printf("Encoder: %ld", enc_val);

        M5.Lcd.setCursor(10, 70);
        M5.Lcd.setTextColor(btn_s ? TFT_RED : TFT_WHITE);
        M5.Lcd.printf("Btn S: %s", btn_s ? "PRESSED " : "RELEASED");

        M5.Lcd.setCursor(10, 100);
        M5.Lcd.setTextColor(btn_a ? TFT_RED : TFT_WHITE);
        M5.Lcd.printf("Btn A: %s", btn_a ? "PRESSED " : "RELEASED");

        M5.Lcd.setCursor(10, 130);
        M5.Lcd.setTextColor(btn_b ? TFT_RED : TFT_WHITE);
        M5.Lcd.printf("Btn B: %s", btn_b ? "PRESSED " : "RELEASED");

        vTaskDelay(pdMS_TO_TICKS(100)); // Увеличили задержку до 100 мс для стабильности I2C
    }
}
