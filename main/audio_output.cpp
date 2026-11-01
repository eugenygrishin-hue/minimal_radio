#include "audio_output.h"
#include "config.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <esp_timer.h>

#include <M5Unified.hpp>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <cstring>
#include <cstdio>
#include <atomic>
#include <cstdint>

static const char* TAG = "AUDIO_OUT";

/* ----------------------------------------------------------
 * Адреса
 * ---------------------------------------------------------- */

#ifndef ES8388_ADDR
#define ES8388_ADDR 0x10
#endif

#ifndef STM32_ADDR
#define STM32_ADDR 0x33
#endif

/* ----------------------------------------------------------
 * Наушники / ES8388:
 * I2S_NUM_0
 * MCLK GPIO 7
 * BCLK GPIO 0
 * WS   GPIO 6
 * DOUT GPIO 13
 * ---------------------------------------------------------- */

#ifndef HP_I2S_PORT
#define HP_I2S_PORT I2S_NUM_0
#endif

#ifndef HP_MCLK_PIN
#define HP_MCLK_PIN GPIO_NUM_7
#endif

#ifndef HP_BCLK_PIN
#define HP_BCLK_PIN GPIO_NUM_0
#endif

#ifndef HP_WS_PIN
#define HP_WS_PIN GPIO_NUM_6
#endif

#ifndef HP_DOUT_PIN
#define HP_DOUT_PIN GPIO_NUM_13
#endif

/* ----------------------------------------------------------
 * M5.Speaker PCM pool
 *
 * playRaw() в M5Unified — zero-copy.
 * Буфер, переданный в playRaw(), нельзя переиспользовать,
 * пока не пришёл setBufferReleaseCallback().
 *
 * Поэтому держим пул буферов и передаём в playRaw() только
 * свободные буферы.
 * ---------------------------------------------------------- */

static constexpr size_t SPK_POOL_COUNT = 4;
static constexpr size_t SPK_BUFFER_FRAMES = 2048;
static constexpr size_t SPK_BUFFER_SAMPLES = SPK_BUFFER_FRAMES * 2; // stereo interleaved
static constexpr uint32_t SPK_DEFAULT_RATE = 44100;

/* ----------------------------------------------------------
 * Состояние
 * ---------------------------------------------------------- */

static SemaphoreHandle_t s_mux = nullptr;

static volatile bool s_is_playing = false;

/* Фактически активный выход */
static volatile bool s_current_headphone = false;

/* Запрошенный выход от монитора наушников */
static volatile bool s_request_headphone = false;

/* Есть незавершённое переключение */
static volatile bool s_pending_switch = false;

static uint8_t  s_volume_percent = 75;
static uint32_t s_sample_rate = SPK_DEFAULT_RATE;

/* I2S для наушников / ES8388 */
static i2s_chan_handle_t s_hp_chan = nullptr;
static bool s_hp_chan_created = false;
static bool s_hp_enabled = false;

/* Debounce детекта наушников */
static uint32_t s_last_poll_ms = 0;
static bool s_last_raw_hp = false;
static uint8_t s_hp_stable_count = 0;
static bool s_debounced_hp = false;

/* ----------------------------------------------------------
 * Speaker backend state
 * ---------------------------------------------------------- */

static bool s_spk_ready = false;
static bool s_spk_pool_alloc_done = false;
static uint32_t s_spk_rate = SPK_DEFAULT_RATE;

static int16_t* s_spk_pool[SPK_POOL_COUNT] = {};

/*
 * free_mask:
 *   бит 1 -> буфер свободен;
 *   бит 0 -> буфер занят producer/task или уже передан в playRaw().
 */
static std::atomic<uint32_t> s_spk_free_mask{ (1u << SPK_POOL_COUNT) - 1u };

/*
 * Текущий буфер, в который producer пишет PCM.
 * Он не считается свободным, пока не будет заполнен и передан в playRaw().
 */
static int s_spk_cur_idx = -1;
static size_t s_spk_cur_pos = 0;

static int64_t s_spk_no_free_since = 0;

/* ----------------------------------------------------------
 * Вспомогательные функции
 * ---------------------------------------------------------- */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static inline uint8_t speaker_volume_255(void)
{
    return (uint8_t)((s_volume_percent * 255u) / 100u);
}

static inline uint8_t es8388_volume_reg(void)
{
    /*
     * Старая рабочая формула из minimal_radio:
     * 100% -> 0x00
     * 0%   -> 0xC0
     */
    return (uint8_t)(0xC0u - ((uint32_t)s_volume_percent * 0xC0u) / 100u);
}

static bool i2c_write8(uint8_t addr, uint8_t reg, uint8_t value, uint32_t freq)
{
    return M5.In_I2C.writeRegister8(addr, reg, value, freq);
}

static uint8_t i2c_read8(uint8_t addr, uint8_t reg, uint32_t freq)
{
    return M5.In_I2C.readRegister8(addr, reg, freq);
}

/* ----------------------------------------------------------
 * RGB LED на Audio Module M144 / STM32 0x33
 * ---------------------------------------------------------- */

static void led_apply(bool headphone)
{
    uint8_t rgb[3];

    if (headphone) {
        /* Зелёный = наушники */
        rgb[0] = 0;
        rgb[1] = 255;
        rgb[2] = 0;
    } else {
        /* Синий = встроенный динамик */
        rgb[0] = 0;
        rgb[1] = 0;
        rgb[2] = 255;
    }

    /* Яркость */
    i2c_write8(STM32_ADDR, 0x30, 50, 400000);

    /* Цвет */
    M5.In_I2C.writeRegister(STM32_ADDR, 0x40, rgb, 3, 400000);
}

/* ----------------------------------------------------------
 * Детект наушников: STM32 0x33, регистр 0x20
 * 1 = вставлены, 0 = извлечены
 * ---------------------------------------------------------- */

static bool read_hp_raw(void)
{
    uint8_t status = i2c_read8(STM32_ADDR, 0x20, 400000);
    return (status == 1);
}

/* ----------------------------------------------------------
 * ES8388 / наушники
 * ---------------------------------------------------------- */

static void es8388_write_reg(uint8_t reg, uint8_t value)
{
    i2c_write8(ES8388_ADDR, reg, value, 100000);
}

static void es8388_set_mute(bool mute)
{
    uint8_t v = mute ? 0xC0 : 0x1E;
    es8388_write_reg(0x1A, v);
    es8388_write_reg(0x1B, v);
}

static void es8388_apply_volume(void)
{
    uint8_t v = es8388_volume_reg();
    es8388_write_reg(0x1A, v);
    es8388_write_reg(0x1B, v);
}

static void es8388_init_common(void)
{
    es8388_set_mute(true);
    vTaskDelay(pdMS_TO_TICKS(50));

    static const uint8_t regs[] = {
        0x08, 0x00,
        0x02, 0xFF,
        0x2B, 0x80,

        0x00, 0x05,
        0x01, 0x40,

        0x03, 0x00,
        0x0A, 0x00,
        0x0B, 0x00,

        0x09, 0x88,
        0x0C, 0x2C,
        0x0D, 0x02,

        0x0F, 0x28,
        0x10, 0x00,
        0x11, 0x00,

        0x12, 0xEA,
        0x13, 0xC0,
        0x14, 0x12,
        0x15, 0x06,
        0x16, 0xC3,

        0x04, 0x3F,

        0x17, 0x18,
        0x18, 0x02,
        0x19, 0x00,

        0x1A, 0x1E,
        0x1B, 0x1E,

        0x26, 0x00,
        0x27, 0x90,

        0x28, 0x38,
        0x29, 0x38,

        0x2A, 0x90,
        0x2B, 0x80,

        0x2E, 0x1E,
        0x2F, 0x1E,

        0x30, 0x00,
        0x31, 0x00,

        0x02, 0x00
    };

    for (size_t i = 0; i < sizeof(regs); i += 2) {
        es8388_write_reg(regs[i], regs[i + 1]);
    }

    /*
     * Route для наушников.
     */
    es8388_write_reg(0x28, 0x0C);
    es8388_write_reg(0x29, 0x00);

    es8388_set_mute(true);
}

static esp_err_t hp_i2s_init(uint32_t sample_rate)
{
    if (s_hp_chan_created) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(HP_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 1023;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_hp_chan, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HP i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    s_hp_chan_created = true;

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO
    );

    std_cfg.gpio_cfg = {};
    std_cfg.gpio_cfg.mclk = HP_MCLK_PIN;
    std_cfg.gpio_cfg.bclk = HP_BCLK_PIN;
    std_cfg.gpio_cfg.ws   = HP_WS_PIN;
    std_cfg.gpio_cfg.dout = HP_DOUT_PIN;
    std_cfg.gpio_cfg.din  = GPIO_NUM_NC;

    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv   = false;

    err = i2s_channel_init_std_mode(s_hp_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HP i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_hp_chan);
        s_hp_chan = nullptr;
        s_hp_chan_created = false;
        return err;
    }

    err = i2s_channel_enable(s_hp_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HP i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_hp_chan);
        s_hp_chan = nullptr;
        s_hp_chan_created = false;
        return err;
    }

    s_hp_enabled = true;
    s_sample_rate = sample_rate;

    ESP_LOGI(TAG, "HP I2S enabled: port=0 rate=%lu Hz stereo 16-bit",
             (unsigned long)sample_rate);

    return ESP_OK;
}

static void hp_i2s_deinit(void)
{
    if (s_hp_enabled && s_hp_chan != nullptr) {
        i2s_channel_disable(s_hp_chan);
        s_hp_enabled = false;
    }

    if (s_hp_chan_created && s_hp_chan != nullptr) {
        i2s_del_channel(s_hp_chan);
        s_hp_chan = nullptr;
        s_hp_chan_created = false;
    }

    ESP_LOGI(TAG, "HP I2S deinitialized, GPIO13 released");
}

static esp_err_t hp_i2s_set_sample_rate(uint32_t sample_rate)
{
    if (!s_hp_chan_created || s_hp_chan == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_sample_rate == sample_rate) {
        return ESP_OK;
    }

    if (s_hp_enabled) {
        i2s_channel_disable(s_hp_chan);
        s_hp_enabled = false;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    esp_err_t err = i2s_channel_reconfig_std_clock(s_hp_chan, &clk_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HP i2s_channel_reconfig_std_clock failed: %s", esp_err_to_name(err));
    }

    esp_err_t err2 = i2s_channel_enable(s_hp_chan);
    if (err2 != ESP_OK) {
        ESP_LOGE(TAG, "HP i2s_channel_enable after reconfig failed: %s", esp_err_to_name(err2));
        return err2;
    }

    s_hp_enabled = true;

    if (err == ESP_OK) {
        s_sample_rate = sample_rate;
        ESP_LOGI(TAG, "HP I2S sample rate -> %lu Hz", (unsigned long)sample_rate);
    }

    return err;
}

/* ----------------------------------------------------------
 * Speaker buffer release callback
 *
 * Вызывается из spk_task.
 * Нельзя блокировать, нельзя вызывать begin()/end().
 * Только помечаем буфер свободным.
 * ---------------------------------------------------------- */

static void spk_buffer_release_cb(void* args, const void* data, uint8_t channel)
{
    (void)args;
    (void)channel;

    for (size_t i = 0; i < SPK_POOL_COUNT; ++i) {
        if (s_spk_pool[i] == data) {
            s_spk_free_mask.fetch_or(1u << i, std::memory_order_release);
            return;
        }
    }
}

/* ----------------------------------------------------------
 * Speaker pool allocation / reset
 * ---------------------------------------------------------- */

static bool spk_pool_alloc(void)
{
    if (s_spk_pool_alloc_done) {
        return true;
    }

    for (size_t i = 0; i < SPK_POOL_COUNT; ++i) {
        s_spk_pool[i] = nullptr;
    }

    const size_t bytes = SPK_BUFFER_SAMPLES * sizeof(int16_t);

    for (size_t i = 0; i < SPK_POOL_COUNT; ++i) {
        int16_t* p = (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (p == nullptr) {
            p = (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        }

        if (p == nullptr) {
            p = (int16_t*)malloc(bytes);
        }

        if (p == nullptr) {
            ESP_LOGE(TAG, "SPK pool alloc failed at buffer %u", (unsigned int)i);

            for (size_t j = 0; j < i; ++j) {
                free(s_spk_pool[j]);
                s_spk_pool[j] = nullptr;
            }

            return false;
        }

        s_spk_pool[i] = p;
    }

    s_spk_pool_alloc_done = true;

    ESP_LOGI(TAG,
             "SPK pool alloc: count=%u frames=%u samples=%u bytes_each=%u total=%u",
             (unsigned int)SPK_POOL_COUNT,
             (unsigned int)SPK_BUFFER_FRAMES,
             (unsigned int)SPK_BUFFER_SAMPLES,
             (unsigned int)bytes,
             (unsigned int)(bytes * SPK_POOL_COUNT));

    return true;
}

static void spk_pool_reset(void)
{
    s_spk_free_mask.store((1u << SPK_POOL_COUNT) - 1u, std::memory_order_release);
    s_spk_cur_idx = -1;
    s_spk_cur_pos = 0;
    s_spk_no_free_since = 0;
}

/* ----------------------------------------------------------
 * Speaker stream begin / stop
 * ---------------------------------------------------------- */

static bool spk_stream_begin(uint32_t rate)
{
    if (s_spk_ready) {
        return true;
    }

    if (!spk_pool_alloc()) {
        return false;
    }

    if (rate == 0) {
        rate = SPK_DEFAULT_RATE;
    }

    auto cfg = M5.Speaker.config();

    /*
     * Если по какой-то причине board config не проставил пины,
     * используем известную конфигурацию CoreS3 SE.
     */
    if (cfg.pin_data_out < 0) {
        cfg.pin_bck       = GPIO_NUM_34;
        cfg.pin_ws        = GPIO_NUM_33;
        cfg.pin_data_out  = GPIO_NUM_13;
        cfg.pin_mck       = I2S_PIN_NO_CHANGE;
        cfg.i2s_port      = (i2s_port_t)I2S_NUM_1;
        cfg.magnification = 4;
        cfg.stereo        = false;
    }

    /*
     * Для CoreS3 SE встроенный динамик mono.
     * Входной PCM у нас stereo, но playRaw() получит stereo=true,
     * а внутренний микшер сам сложит L/R в mono, потому что cfg.stereo=false.
     */
    cfg.sample_rate     = rate;
    cfg.stereo          = false;

    /*
     * Увеличиваем DMA и приоритет speaker task.
     * По умолчанию dma_buf_len=256, dma_buf_count=8, priority=2 —
     * для Wi-Fi radio stream это слишком мало/низко.
     */
    cfg.dma_buf_len     = 1024;
    cfg.dma_buf_count   = 8;
    cfg.task_priority   = 8;
    cfg.task_pinned_core = 1;

    /*
     * Если это CoreS3 SE / CoreS3, магнфикация обычно 4.
     * Не завышаем до 16, чтобы не получить клиппинг.
     */
    if (cfg.pin_bck == GPIO_NUM_34 && cfg.pin_ws == GPIO_NUM_33 && cfg.pin_data_out == GPIO_NUM_13) {
        cfg.i2s_port      = (i2s_port_t)I2S_NUM_1;
        cfg.magnification = 4;
        cfg.pin_mck       = I2S_PIN_NO_CHANGE;
    }

    M5.Speaker.config(cfg);

    /*
     * Callback Must be set before first play*() or after end().
     * Мы как раз после end()/before begin().
     */
    M5.Speaker.setBufferReleaseCallback(nullptr, spk_buffer_release_cb);

    if (!M5.Speaker.begin()) {
        ESP_LOGE(TAG, "SPK M5.Speaker.begin() failed");
        return false;
    }

    M5.Speaker.setVolume(speaker_volume_255());
    M5.Speaker.setAllChannelVolume(255);

    s_spk_rate = rate;
    s_spk_ready = true;
    spk_pool_reset();

    ESP_LOGI(TAG,
             "SPK M5 callback backend enabled: rate=%lu dma=%ux%u prio=%u core=%u mag=%u pool=%ux%u frames",
             (unsigned long)rate,
             (unsigned int)cfg.dma_buf_len,
             (unsigned int)cfg.dma_buf_count,
             (unsigned int)cfg.task_priority,
             (unsigned int)cfg.task_pinned_core,
             (unsigned int)cfg.magnification,
             (unsigned int)SPK_POOL_COUNT,
             (unsigned int)SPK_BUFFER_FRAMES);

    return true;
}

static void spk_stream_stop(bool hard_end)
{
    if (s_spk_ready) {
        M5.Speaker.stop();

        /*
         * Ждём, пока speaker task освободит очередь.
         * Это не гарантирует 100% доставку всех callback,
         * но практически достаточно, чтобы не переиспользовать
         * буферы слишком рано.
         */
        int64_t t0 = esp_timer_get_time();
        while (M5.Speaker.isPlaying(0) && (esp_timer_get_time() - t0) < 500000) {
            vTaskDelay(1);
        }

        /*
         * Небольшой grace period для callback, которые могут
         * прийти сразу после isPlaying()==0.
         */
        vTaskDelay(pdMS_TO_TICKS(20));

        spk_pool_reset();
    }

    if (hard_end) {
        M5.Speaker.end();
        s_spk_ready = false;
    }
}

/* ----------------------------------------------------------
 * Speaker pool producer helpers
 * ---------------------------------------------------------- */

static int spk_acquire_buffer(void)
{
    while (true) {
        uint32_t mask = s_spk_free_mask.load(std::memory_order_acquire);

        if (mask != 0) {
            uint32_t bit = mask & (~mask + 1u);
            uint32_t idx = (uint32_t)__builtin_ctz(bit);
            uint32_t desired = mask & ~bit;

            if (s_spk_free_mask.compare_exchange_weak(
                    mask,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                s_spk_no_free_since = 0;
                return (int)idx;
            }
        }

        /*
         * Watchdog: если долго нет свободных буферов и speaker не играет,
         * вероятно callback потерялся из-за stop()/discard. Сбрасываем пул.
         */
        if (s_spk_no_free_since == 0) {
            s_spk_no_free_since = esp_timer_get_time();
        } else if ((esp_timer_get_time() - s_spk_no_free_since) > 1000000) {
            if (!M5.Speaker.isPlaying(0)) {
                ESP_LOGW(TAG, "SPK pool watchdog reset: no free buffers and speaker idle");
                spk_pool_reset();
            }
            s_spk_no_free_since = esp_timer_get_time();
        }

        vTaskDelay(1);
    }
}

static bool spk_queue_buffer(int idx)
{
    if (idx < 0 || idx >= (int)SPK_POOL_COUNT) {
        return false;
    }

    bool ok = false;

    /*
     * С explicit channel playRaw() normally waits for a free slot.
     * Но на всякий случай делаем несколько попыток.
     */
    for (int attempt = 0; attempt < 20 && !ok; ++attempt) {
        ok = M5.Speaker.playRaw(s_spk_pool[idx],
                                SPK_BUFFER_SAMPLES,
                                s_spk_rate,
                                true,   // входной PCM stereo
                                1,      // repeat once
                                0,      // явный канал 0
                                false); // не стопим текущий звук
        if (!ok) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    if (!ok) {
        s_spk_free_mask.fetch_or(1u << idx, std::memory_order_release);
        ESP_LOGW(TAG, "SPK playRaw failed for buffer %d", idx);
        return false;
    }

    return true;
}

static esp_err_t spk_write_pcm(const int16_t* samples,
                               size_t frames,
                               uint32_t sample_rate)
{
    uint32_t rate = sample_rate ? sample_rate : SPK_DEFAULT_RATE;

    if (!s_spk_ready) {
        if (!spk_stream_begin(rate)) {
            return ESP_FAIL;
        }
    }

    /*
     * Смена sample rate требует пересоздания speaker backend.
     */
    if (rate != s_spk_rate) {
        ESP_LOGI(TAG, "SPK sample rate change: %lu -> %lu",
                 (unsigned long)s_spk_rate,
                 (unsigned long)rate);

        spk_stream_stop(true);

        if (!spk_stream_begin(rate)) {
            return ESP_FAIL;
        }
    }

    const size_t total_samples = frames * 2; // stereo interleaved
    size_t offset = 0;

    while (offset < total_samples) {
        if (s_spk_cur_idx < 0) {
            int idx = spk_acquire_buffer();
            if (idx < 0) {
                return ESP_FAIL;
            }

            s_spk_cur_idx = idx;
            s_spk_cur_pos = 0;
        }

        size_t space = SPK_BUFFER_SAMPLES - s_spk_cur_pos;
        size_t remain = total_samples - offset;
        size_t n = (remain < space) ? remain : space;

        memcpy(s_spk_pool[s_spk_cur_idx] + s_spk_cur_pos,
               samples + offset,
               n * sizeof(int16_t));

        s_spk_cur_pos += n;
        offset += n;

        if (s_spk_cur_pos == SPK_BUFFER_SAMPLES) {
            int idx = s_spk_cur_idx;

            if (!spk_queue_buffer(idx)) {
                s_spk_cur_idx = -1;
                s_spk_cur_pos = 0;
                return ESP_FAIL;
            }

            s_spk_cur_idx = -1;
            s_spk_cur_pos = 0;
        }
    }

    /*
     * Диагностика каждые ~200 вызовов write_pcm().
     */
    static uint32_t spk_calls = 0;
    spk_calls++;

    if ((spk_calls % 200) == 0) {
        uint32_t free_mask = s_spk_free_mask.load(std::memory_order_acquire);

        ESP_LOGI(TAG,
                 "SPK pool: calls=%lu rate=%lu free_mask=0x%02X cur_idx=%d cur_pos=%lu/%lu",
                 (unsigned long)spk_calls,
                 (unsigned long)s_spk_rate,
                 (unsigned int)free_mask,
                 s_spk_cur_idx,
                 (unsigned long)s_spk_cur_pos,
                 (unsigned long)SPK_BUFFER_SAMPLES);
    }

    return ESP_OK;
}

/* ----------------------------------------------------------
 * Переключение выходов.
 * Вызывается ТОЛЬКО под мьютексом.
 * ---------------------------------------------------------- */

static void switch_locked(bool to_headphone)
{
    if (to_headphone == s_current_headphone) {
        s_pending_switch = false;
        return;
    }

    ESP_LOGI(TAG, "Switching audio output: %s",
             to_headphone ? "SPEAKER -> HEADPHONE" : "HEADPHONE -> SPEAKER");

    /* 1. Заглушаем текущий выход */
    if (s_current_headphone) {
        es8388_set_mute(true);
    } else {
        spk_stream_stop(true);
    }

    vTaskDelay(pdMS_TO_TICKS(80));

    /* 2. Переключаем железо */
    if (to_headphone) {
        /*
         * Освобождаем speaker backend полностью.
         * M5.Speaker использует GPIO13, он не должен висеть занятым.
         */
        spk_stream_stop(true);

        vTaskDelay(pdMS_TO_TICKS(50));

        es8388_init_common();

        esp_err_t err = hp_i2s_init(s_sample_rate ? s_sample_rate : SPK_DEFAULT_RATE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HP I2S init failed, fallback to speaker");

            hp_i2s_deinit();

            if (!spk_stream_begin(s_sample_rate ? s_sample_rate : SPK_DEFAULT_RATE)) {
                ESP_LOGE(TAG, "Speaker fallback begin failed");
            }

            s_current_headphone = false;
            s_request_headphone = false;
            s_pending_switch = false;
            led_apply(false);
            return;
        }

        es8388_set_mute(false);
        es8388_apply_volume();

        s_current_headphone = true;
        led_apply(true);
    } else {
        /* Освобождаем headphone I2S */
        es8388_set_mute(true);
        hp_i2s_deinit();

        vTaskDelay(pdMS_TO_TICKS(50));

        if (!spk_stream_begin(s_sample_rate ? s_sample_rate : SPK_DEFAULT_RATE)) {
            ESP_LOGE(TAG, "SPK stream begin failed");
        }

        s_current_headphone = false;
        led_apply(false);
    }

    s_pending_switch = false;

    ESP_LOGI(TAG, "Audio output now: %s",
             s_current_headphone ? "HEADPHONE" : "SPEAKER");
}

/* ----------------------------------------------------------
 * Public API
 * ---------------------------------------------------------- */

extern "C" void audio_output_init(void)
{
    if (s_mux == nullptr) {
        s_mux = xSemaphoreCreateMutex();
        if (s_mux == nullptr) {
            ESP_LOGE(TAG, "Failed to create audio_output mutex");
            return;
        }
    }

    s_is_playing = false;
    s_volume_percent = 75;
    s_sample_rate = SPK_DEFAULT_RATE;
    s_spk_rate = SPK_DEFAULT_RATE;

    if (!spk_pool_alloc()) {
        ESP_LOGE(TAG, "SPK pool allocation failed");
    }

    /* Первичный debounce: 3 чтения подряд */
    int true_count = 0;
    for (int i = 0; i < 3; ++i) {
        if (read_hp_raw()) {
            true_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    bool hp = (true_count >= 2);

    s_last_raw_hp = hp;
    s_hp_stable_count = 3;
    s_debounced_hp = hp;

    s_current_headphone = false;
    s_request_headphone = hp;
    s_pending_switch = hp;

    /*
     * Гарантированно освобождаем speaker, если M5.begin() уже что-то поднял.
     * Это важно, потому что GPIO13 общий с ES8388.
     */
    M5.Speaker.end();
    vTaskDelay(pdMS_TO_TICKS(50));

    /*
     * Если наушники не вставлены, сразу поднимаем speaker backend.
     * Если вставлены — speaker не нужен, GPIO13 отдаём ES8388.
     */
    if (!hp) {
        spk_stream_begin(SPK_DEFAULT_RATE);
    }

    es8388_init_common();
    es8388_set_mute(true);

    led_apply(false);

    ESP_LOGI(TAG, "audio_output_init: initial headphone=%d, speaker_backend=M5_CALLBACK_POOL",
             hp ? 1 : 0);

    /* Если наушники уже вставлены на старте — переключаемся сразу */
    if (s_pending_switch) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
        switch_locked(true);
        xSemaphoreGive(s_mux);
    }
}

extern "C" void audio_output_start(void)
{
    s_is_playing = true;

    /*
     * При старте нового потока сбрасываем пул, чтобы не тащить
     * хвосты предыдущей станции.
     */
    if (!s_current_headphone) {
        spk_stream_stop(false);
        spk_pool_reset();
    }

    if (s_pending_switch) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
        switch_locked(s_request_headphone);
        xSemaphoreGive(s_mux);
    }
}

extern "C" void audio_output_stop(void)
{
    s_is_playing = false;

    xSemaphoreTake(s_mux, portMAX_DELAY);

    if (s_current_headphone) {
        es8388_set_mute(true);
    } else {
        spk_stream_stop(false);
    }

    xSemaphoreGive(s_mux);
}

extern "C" esp_err_t audio_output_write_pcm(const int16_t* samples,
                                            size_t frames,
                                            uint32_t sample_rate,
                                            uint8_t channels)
{
    if (s_mux == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (samples == nullptr || frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Интерфейс адаптера рассчитан на stereo interleaved.
     * Моно должен расширять до стерео вызывающий код (audio_player).
     */
    if (channels != 2) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);

    if (s_pending_switch) {
        switch_locked(s_request_headphone);
    }

    esp_err_t err = ESP_OK;

    if (!s_is_playing) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s_current_headphone) {
        /* Наушники: собственный I2S + ES8388, stereo */
        if (!s_hp_chan_created) {
            err = hp_i2s_init(sample_rate);
        } else if (s_sample_rate != sample_rate) {
            err = hp_i2s_set_sample_rate(sample_rate);
        }

        if (err == ESP_OK && s_hp_enabled && s_hp_chan != nullptr) {
            size_t bytes_to_write = frames * 2 * sizeof(int16_t);
            size_t bytes_written = 0;

            err = i2s_channel_write(s_hp_chan,
                                    samples,
                                    bytes_to_write,
                                    &bytes_written,
                                    portMAX_DELAY);

            if (err == ESP_OK && bytes_written != bytes_to_write) {
                err = ESP_FAIL;
            }
        }
    } else {
        /* Встроенный динамик: M5.Speaker + buffer pool + release callback */
        err = spk_write_pcm(samples, frames, sample_rate);
    }

    xSemaphoreGive(s_mux);
    return err;
}

extern "C" void audio_output_poll(void)
{
    if (s_mux == nullptr) {
        return;
    }

    uint32_t now = now_ms();
    if ((now - s_last_poll_ms) < 200) {
        return;
    }
    s_last_poll_ms = now;

    bool raw = read_hp_raw();

    if (raw == s_last_raw_hp) {
        if (s_hp_stable_count < 3) {
            s_hp_stable_count++;
        }
    } else {
        s_last_raw_hp = raw;
        s_hp_stable_count = 1;
    }

    if (s_hp_stable_count >= 3 && raw != s_debounced_hp) {
        s_debounced_hp = raw;
        s_request_headphone = raw;
        s_pending_switch = true;

        ESP_LOGI(TAG, "Headphone state changed: %s", raw ? "INSERTED" : "REMOVED");

        /*
         * Если воспроизведение сейчас не активно, можно переключить сразу.
         * Если активно — переключение выполнит audio_output_write_pcm()
         * из контекста аудио-задачи.
         */
        if (!s_is_playing) {
            xSemaphoreTake(s_mux, portMAX_DELAY);
            switch_locked(s_request_headphone);
            xSemaphoreGive(s_mux);
        }
    }
}

extern "C" void audio_output_set_volume(uint8_t volume_percent)
{
    if (volume_percent > 100) {
        volume_percent = 100;
    }

    s_volume_percent = volume_percent;

    if (s_mux == nullptr) {
        return;
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);

    if (s_current_headphone) {
        es8388_apply_volume();
    } else {
        M5.Speaker.setVolume(speaker_volume_255());
    }

    xSemaphoreGive(s_mux);
}

extern "C" bool audio_output_is_headphone(void)
{
    return s_current_headphone;
}
