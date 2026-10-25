#include "audio_player.h"
#include "config.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mp3dec.h"
#include <M5Unified.hpp>
#include <cstring>

static const char* TAG = "AUDIO";

static i2s_chan_handle_t tx_chan = NULL;
static RingbufHandle_t s_ringbuf = NULL;

static uint32_t g_rb_in = 0, g_rb_out = 0, g_rb_overflow = 0, g_rb_starve = 0, g_dec_buf = 0;
static volatile bool g_playback_started = false, g_prefill_active = false;
static int g_current_volume = 75;
static bool g_is_muted = false;

// Новые переменные для отслеживания состояния I2S
static uint32_t g_current_sample_rate = 0;
static bool g_i2s_channel_created = false;
static bool g_i2s_enabled = false;

/**
 * @brief Создаёт и инициализирует I2S канал (вызывается только один раз)
 */
static esp_err_t i2s_ensure_channel(void) {
    if (g_i2s_channel_created) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 24;
    chan_cfg.dma_frame_num = 1023;

    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {};
    // Временная частота 44100, будет перезаписана при первом фрейме
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100);
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

    i2s_std_gpio_config_t gpio_cfg = {};
    gpio_cfg.mclk = I2S_MCLK_PIN;
    gpio_cfg.bclk = I2S_BCK_PIN;
    gpio_cfg.ws = I2S_WS_PIN;
    gpio_cfg.dout = I2S_DOUT_PIN;
    gpio_cfg.din = I2S_DIN_PIN;
    std_cfg.gpio_cfg = gpio_cfg;

    err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S std mode: %s", esp_err_to_name(err));
        return err;
    }

    g_i2s_channel_created = true;
    ESP_LOGI(TAG, "I2S channel created successfully");
    return ESP_OK;
}

/**
* @brief Динамически обновляет частоту дискретизации I2S на лету
*/
static esp_err_t i2s_update_sample_rate(uint32_t sample_rate) {
    if (g_current_sample_rate == sample_rate) {
        return ESP_OK; // Частота не изменилась, ничего не делаем
    }
    if (!g_i2s_channel_created) {
        return ESP_ERR_INVALID_STATE;
    }

    // 1. Сначала выключаем канал перед перенастройкой
    i2s_channel_disable(tx_chan);

    // 2. Перенастраиваем тактирование
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    esp_err_t err = i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, ">>> I2S sample rate updated to %lu Hz", (unsigned long)sample_rate);
        g_current_sample_rate = sample_rate;

        // 3. КРИТИЧЕСКИ ВАЖНО: Включаем канал обратно после успешной перенастройки!
        i2s_channel_enable(tx_chan);
        g_i2s_enabled = true; // Гарантируем, что флаг установлен
    } else {
        ESP_LOGE(TAG, "Failed to reconfig I2S clock to %lu Hz: %s", (unsigned long)sample_rate, esp_err_to_name(err));
        // На всякий случай включаем канал обратно, чтобы устройство не зависло в тишине при ошибке
        i2s_channel_enable(tx_chan);
    }

    return err;
}

static void es8388_init() {
    // Пробуждение и базовая инициализация ES8388
    M5.In_I2C.writeRegister8(ES8388_ADDR, 0x1A, 0xC0, 100000);
    M5.In_I2C.writeRegister8(ES8388_ADDR, 0x1B, 0xC0, 100000);
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t regs[] = {0x08,0x00, 0x02,0xFF, 0x2B,0x80, 0x00,0x05, 0x01,0x40, 0x03,0x00, 0x0A,0x00, 0x0B,0x00, 0x09,0x88, 0x0C,0x2C, 0x0D,0x02, 0x0F,0x28, 0x10,0x00, 0x11,0x00, 0x12,0xEA, 0x13,0xC0, 0x14,0x12, 0x15,0x06, 0x16,0xC3, 0x04,0x3F, 0x17,0x18, 0x18,0x02, 0x19,0x00, 0x1A,0x1E, 0x1B,0x1E, 0x26,0x00, 0x27,0x90, 0x28,0x38, 0x29,0x38, 0x2A,0x90, 0x2B,0x80, 0x2E,0x1E, 0x2F,0x1E, 0x30,0x00, 0x31,0x00, 0x02,0x00};
    for (int i = 0; i < sizeof(regs); i += 2) {
        M5.In_I2C.writeRegister8(ES8388_ADDR, regs[i], regs[i+1], 100000);
    }

    // Конфигурация STM32 для аудиомодуля M144 (CoreS3 SE)
    M5.In_I2C.writeRegister8(STM32_ADDR, 0x10, 0x00, 100000);

    audio_set_volume(g_current_volume);
}

static void decode_play_task(void *pvParameters) {
    HMP3Decoder mp3_decoder = MP3InitDecoder();
    uint8_t *inbuf = (uint8_t *)heap_caps_malloc(DEC_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    short *outbuf = (short *)heap_caps_malloc(2304 * sizeof(short), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!inbuf || !outbuf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers");
        vTaskDelete(NULL);
        return;
    }

    int bytes_in_buffer = 0;

    auto do_prefill = [&]() {
        g_prefill_active = true;
        while (bytes_in_buffer < PREFILL_TARGET) {
            size_t s;
            void *d = xRingbufferReceive(s_ringbuf, &s, pdMS_TO_TICKS(2000));
            if (d) {
                if (bytes_in_buffer + s <= DEC_BUF_SIZE) {
                    memcpy(inbuf + bytes_in_buffer, d, s);
                    bytes_in_buffer += s;
                    g_rb_out += s;
                }
                vRingbufferReturnItem(s_ringbuf, d);
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        g_prefill_active = false;
    };

    do_prefill();
    g_playback_started = true;

    while (1) {
        // 1. Обработка андеррана (опустошения буфера)
        if (bytes_in_buffer < REBUF_THRESHOLD) {
            ESP_LOGW(TAG, "Buffer underrun! Rebuffering...");
            g_playback_started = false;

            size_t s;
            void *d;
            while ((d = xRingbufferReceive(s_ringbuf, &s, 0)) != NULL) {
                vRingbufferReturnItem(s_ringbuf, d);
            }

            bytes_in_buffer = 0;
            memset(inbuf, 0, DEC_BUF_SIZE);
            g_rb_in = 0;
            g_rb_out = 0;

            do_prefill();
            g_playback_started = true;
            continue;
        }

        // 2. Дозаполнение буфера до целевого уровня
        while (bytes_in_buffer < PREFILL_TARGET) {
            size_t s;
            void *d = xRingbufferReceive(s_ringbuf, &s, (bytes_in_buffer < 4096) ? pdMS_TO_TICKS(500) : pdMS_TO_TICKS(20));
            if (d) {
                if (bytes_in_buffer + s <= DEC_BUF_SIZE) {
                    memcpy(inbuf + bytes_in_buffer, d, s);
                    bytes_in_buffer += s;
                    g_rb_out += s;
                }
                vRingbufferReturnItem(s_ringbuf, d);
            } else {
                break;
            }
        }

        // 3. Защита от тишины при низком буфере
        if (bytes_in_buffer < 8192 && g_i2s_enabled) {
            short sil[1024] = {0};
            size_t w;
            i2s_channel_write(tx_chan, sil, sizeof(sil), &w, pdMS_TO_TICKS(20));
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        g_dec_buf = bytes_in_buffer;
        uint8_t *rp = inbuf;
        int bl = bytes_in_buffer, fp = 0;

        // 4. Декодирование фреймов
        while (bl > 0 && fp < FRAMES_PER_PASS) {
            int off = MP3FindSyncWord(rp, bl);
            if (off < 0) {
                rp++;
                bl--;
                continue;
            }
            if (off > 0) {
                rp += off;
                bl -= off;
            }

            uint8_t *sp = rp;
            int sl = bl;
            int ret = MP3Decode(mp3_decoder, &rp, &bl, outbuf, 0);

            if (ret == ERR_MP3_INDATA_UNDERFLOW) break;
            if (ret != ERR_MP3_NONE) {
                rp = sp + 1;
                bl = sl - 1;
                continue;
            }

            MP3FrameInfo fi;
            MP3GetLastFrameInfo(mp3_decoder, &fi);
            if (fi.samprate == 0 || fi.nChans == 0) {
                rp = sp + 1;
                bl = sl - 1;
                continue;
            }

            // 5. Инициализация I2S при первом успешном фрейме
            if (!g_i2s_channel_created) {
                if (i2s_ensure_channel() != ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
            }

            // 6. КЛЮЧЕВОЕ ИСПРАВЛЕНИЕ: Динамическое обновление частоты дискретизации
            if (g_current_sample_rate != (uint32_t)fi.samprate) {
                i2s_update_sample_rate(fi.samprate);
            }

            // 7. Включение канала (один раз)
            if (!g_i2s_enabled) {
                i2s_channel_enable(tx_chan);
                g_i2s_enabled = true;
            }

            // 8. Запись в I2S (с конвертацией моно в стерео при необходимости)
            size_t w;
            if (fi.nChans == 1) {
                short st[2304];
                for(int i = 0; i < fi.outputSamps; i++) {
                    st[i*2] = outbuf[i];
                    st[i*2+1] = outbuf[i];
                }
                i2s_channel_write(tx_chan, st, fi.outputSamps * 4, &w, portMAX_DELAY);
            } else {
                i2s_channel_write(tx_chan, outbuf, fi.outputSamps * 2, &w, portMAX_DELAY);
            }
            fp++;
        }

        // 9. Сдвиг остатков буфера
        if (bl > 0 && bl < bytes_in_buffer) {
            memmove(inbuf, rp, bl);
        } else if (bl == 0) {
            bl = 0;
        }
        bytes_in_buffer = bl;
        vTaskDelay(1);
    }
}

void audio_init(RingbufHandle_t ringbuf) {
    s_ringbuf = ringbuf;
    es8388_init();
    ESP_LOGI(TAG, "Audio player initialized");
}

void audio_start() {
    xTaskCreatePinnedToCore(decode_play_task, "decode", 8192, NULL, 10, NULL, 0);
    ESP_LOGI(TAG, "Audio decoder task started");
}

void audio_set_volume(int vol) {
    g_current_volume = vol < 0 ? 0 : (vol > 100 ? 100 : vol);
    uint8_t dac_vol = (uint8_t)(0xC0 - (g_current_volume * 0xC0) / 100);
    if (!g_is_muted) {
        M5.In_I2C.writeRegister8(ES8388_ADDR, 0x1A, dac_vol, 100000);
        M5.In_I2C.writeRegister8(ES8388_ADDR, 0x1B, dac_vol, 100000);
    }
}

void audio_toggle_mute() {
    g_is_muted = !g_is_muted;
    if (g_is_muted) {
        M5.In_I2C.writeRegister8(ES8388_ADDR, 0x1A, 0xC0, 100000);
        M5.In_I2C.writeRegister8(ES8388_ADDR, 0x1B, 0xC0, 100000);
    } else {
        audio_set_volume(g_current_volume);
    }
}

uint32_t audio_get_buffer_level() {
    return g_dec_buf;
}

void audio_get_stats(uint32_t* starve, uint32_t* overflow) {
    if (starve) *starve = g_rb_starve;
    if (overflow) *overflow = g_rb_overflow;
}

int audio_get_volume() {
    return g_current_volume;
}

bool audio_is_muted() {
    return g_is_muted;
}
