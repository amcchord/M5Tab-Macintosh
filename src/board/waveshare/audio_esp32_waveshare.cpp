/*
 *  audio_esp32_waveshare.cpp - Audio support for the Waveshare
 *                              ESP32-P4-WIFI6-Touch-LCD-10.1.
 *
 *  BasiliskII ESP32 Port
 *
 *  Uses the Waveshare BSP's esp_codec_dev-based ES8311 speaker path:
 *     bsp_audio_init()                - I2S channel setup
 *     bsp_audio_codec_speaker_init()  - ES8311 codec, NS4150B amp enable
 *     esp_codec_dev_open/write/close  - sample submission
 *
 *  This file mirrors the public interface of src/basilisk/audio_esp32.cpp
 *  (the Tab5 version) so the rest of the codebase is unaware of the board.
 *  The Apple Mixer pump, format conversion, and volume math are the same;
 *  only the final sink is different.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "audio_guest_memory.h"
#include "main.h"
#include "prefs.h"
#include "audio.h"
#include "audio_defs.h"

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bsp/esp32_p4_wifi6_touch_lcd_x.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#define DEBUG 0
#include "debug.h"

/* ----------------------------------------------------------------------- */
/* Configuration                                                           */
/* ----------------------------------------------------------------------- */

#define AUDIO_TASK_STACK_SIZE  4096
#define AUDIO_TASK_PRIORITY    2

/* Pin audio on the core opposite to the emulation loop. */
#if defined(CONFIG_ARDUINO_RUNNING_CORE)
#define EMULATION_TASK_CORE CONFIG_ARDUINO_RUNNING_CORE
#elif defined(ARDUINO_RUNNING_CORE)
#define EMULATION_TASK_CORE ARDUINO_RUNNING_CORE
#else
#define EMULATION_TASK_CORE 1
#endif

#if defined(portNUM_PROCESSORS) && (portNUM_PROCESSORS > 1)
#define AUDIO_TASK_CORE ((EMULATION_TASK_CORE == 0) ? 1 : 0)
#else
#define AUDIO_TASK_CORE EMULATION_TASK_CORE
#endif

#define AUDIO_SAMPLE_RATE      22050
#define AUDIO_BUFFER_FRAMES    1024
#define AUDIO_CHANNELS         2
#define AUDIO_SAMPLE_SIZE      16
#define AUDIO_BYTES_PER_FRAME  (AUDIO_CHANNELS * (AUDIO_SAMPLE_SIZE / 8))
#define AUDIO_BUFFER_SIZE      (AUDIO_BUFFER_FRAMES * AUDIO_BYTES_PER_FRAME)

#define MAC_MAX_VOLUME 0x0100

/* ----------------------------------------------------------------------- */
/* State                                                                   */
/* ----------------------------------------------------------------------- */

static int audio_sample_rate_index   = 0;
static int audio_sample_size_index   = 0;
static int audio_channel_count_index = 0;

static TaskHandle_t     audio_task_handle  = NULL;
static volatile bool    audio_task_running = false;
static SemaphoreHandle_t audio_irq_done_sem = NULL;

static int16_t *audio_mix_buf = NULL;
static esp_codec_dev_handle_t codec_dev = NULL;
static bool speaker_initialized = false;

static int  main_volume    = MAC_MAX_VOLUME / 2;
static int  speaker_volume = MAC_MAX_VOLUME / 2;
static bool main_mute      = false;
static bool speaker_mute   = false;

static const char *TAG = "audio_ws";

/* ----------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ----------------------------------------------------------------------- */

static void set_audio_status_format(void)
{
    AudioStatus.sample_rate = audio_sample_rates[audio_sample_rate_index];
    AudioStatus.sample_size = audio_sample_sizes[audio_sample_size_index];
    AudioStatus.channels    = audio_channel_counts[audio_channel_count_index];
}

/* Map Mac 8.8 fixed point volume to 0..100 for esp_codec_dev. */
static int get_effective_volume_percent(void)
{
    if (main_mute || speaker_mute) return 0;
    uint32_t combined = (main_volume * speaker_volume) / MAC_MAX_VOLUME;
    uint32_t pct = (combined * 100) / MAC_MAX_VOLUME;
    if (pct > 100) pct = 100;
    return (int)pct;
}

/*
 * Force-reset the ES8311 codec via I2C before the BSP's normal init path
 * runs. Mirrors the ES8388 reset on Tab5: the codec keeps its previous
 * state through ESP32 software resets and crashes (it has its own power
 * rail), and the BSP's es8311_codec_new() does a fairly compressed init
 * that can leave the chip stuck if it was actively producing samples
 * when the SoC reset.
 *
 * Sequence:
 *   1. Drive the NS4150B power-amp enable pin LOW so the speaker is
 *      isolated during the reset (no pops, no DC click).
 *   2. Issue an I2C software reset on the ES8311 (register 0x00 bit 7),
 *      hold for 50 ms, release. Done via i2c_master_bus + i2c_master_dev
 *      since the Arduino Wire instance is busy with display/touch and we
 *      want to share the BSP's existing master bus.
 *   3. Brief settle delay so the chip's internal POR finishes before the
 *      BSP's bsp_audio_codec_speaker_init() walks its init register list.
 *
 * Any I2C error is logged but non-fatal - the BSP init below will get
 * another shot at the chip.
 */
static constexpr uint8_t ES8311_I2C_ADDR        = 0x18;
static constexpr uint8_t ES8311_REG_RESET       = 0x00;
static constexpr uint8_t ES8311_RESET_BIT       = 0x80;

static void reset_es8311(void)
{
    Serial.println("[AUDIO] Force-resetting ES8311 codec...");

    /* Make sure the BSP's I2C master bus is up. bsp_i2c_init() is
     * idempotent (the BSP guards against double-init) so calling it
     * here before audio init is safe even though the panel/touch
     * already brought it up earlier in boot. */
    bsp_i2c_init();

    /* Mute the amp first. The ES8311 driver normally manages this pin
     * but since we're talking to the chip ahead of the driver, pull it
     * down ourselves so the speaker is silent during the reset. */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << BSP_POWER_AMP_IO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level((gpio_num_t)BSP_POWER_AMP_IO, 0);
    delay(5);

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        Serial.println("[AUDIO] WARN: bsp_i2c_get_handle returned NULL, skipping codec reset");
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ES8311_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK || dev == NULL) {
        Serial.printf("[AUDIO] WARN: i2c add device failed (0x%x), skipping codec reset\n", err);
        return;
    }

    /* Assert reset (reg 0x00 bit 7). */
    uint8_t reset_assert[2]   = { ES8311_REG_RESET, ES8311_RESET_BIT };
    uint8_t reset_release[2]  = { ES8311_REG_RESET, 0x00 };

    err = i2c_master_transmit(dev, reset_assert, sizeof(reset_assert), 50 /*ms*/);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] WARN: ES8311 reset assert failed (0x%x)\n", err);
    }
    delay(50);

    err = i2c_master_transmit(dev, reset_release, sizeof(reset_release), 50 /*ms*/);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] WARN: ES8311 reset release failed (0x%x)\n", err);
    }
    delay(20);

    /* Sanity read of register 0x00. After release, bit 7 should be 0. */
    uint8_t reg_addr  = ES8311_REG_RESET;
    uint8_t reg_value = 0xFF;
    err = i2c_master_transmit_receive(dev, &reg_addr, 1, &reg_value, 1, 50);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] WARN: ES8311 readback failed (0x%x)\n", err);
    } else {
        Serial.printf("[AUDIO] ES8311 reset OK, reg0x00=0x%02X\n", reg_value);
    }

    i2c_master_bus_rm_device(dev);
}

static bool init_speaker(void)
{
    Serial.println("[AUDIO] Initializing ES8311 codec via Waveshare BSP...");

    /* Always run the full ES8311 reset before BSP init - the codec
     * keeps its state across ESP32 resets and the BSP's init path
     * doesn't itself perform a power-down + chip reset, so a crash
     * mid-playback could otherwise leave the codec stuck enough to
     * produce silent audio until the next power cycle. The reset is
     * harmless on a fresh power-on. */
    reset_es8311();

    /* Re-drive the NS4150B power amplifier enable pin high. reset_es8311()
     * dropped it low during the I2C reset; bring it back up now so the
     * BSP's init writes immediately produce audible output. The ES8311
     * driver normally toggles this pin via the gpio_if registered by
     * audio_codec_new_gpio(), but because we construct the codec via the
     * BSP wrapper we want to be sure the amp is on even before the first
     * DMA buffer plays. */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << BSP_POWER_AMP_IO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level((gpio_num_t)BSP_POWER_AMP_IO, 1);
    Serial.printf("[AUDIO] Power amp enable GPIO%d driven high\n", (int)BSP_POWER_AMP_IO);

    /* Force bsp_audio_init with a STEREO Philips config before the BSP's
     * lazy-init path (called inside bsp_audio_codec_speaker_init) picks its
     * default MONO config. Feeding stereo int16 samples through a MONO
     * I2S channel is the most common cause of "silent audio" on this
     * board. */
    const i2s_std_config_t stereo_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk         = (gpio_num_t)BSP_I2S_MCLK,
            .bclk         = (gpio_num_t)BSP_I2S_SCLK,
            .ws           = (gpio_num_t)BSP_I2S_LCLK,
            .dout         = (gpio_num_t)BSP_I2S_DOUT,
            .din          = (gpio_num_t)BSP_I2S_DSIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    esp_err_t err = bsp_audio_init(&stereo_cfg);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] ERROR: bsp_audio_init(stereo) failed: %s\n", esp_err_to_name(err));
        return false;
    }

    codec_dev = bsp_audio_codec_speaker_init();
    if (codec_dev == NULL) {
        Serial.println("[AUDIO] ERROR: bsp_audio_codec_speaker_init returned NULL");
        return false;
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = AUDIO_SAMPLE_SIZE;
    fs.channel         = AUDIO_CHANNELS;
    fs.channel_mask    = 0;
    fs.sample_rate     = AUDIO_SAMPLE_RATE;
    fs.mclk_multiple   = 0;

    err = esp_codec_dev_open(codec_dev, &fs);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] ERROR: esp_codec_dev_open failed: %s\n", esp_err_to_name(err));
        codec_dev = NULL;
        return false;
    }

    /* Bump the initial volume: the Mac combined volume math gives only ~25%
     * out of the box which is very quiet through the tiny onboard speaker.
     * Mac OS Sound control panel can adjust via audio_set_*_volume(). */
    esp_codec_dev_set_out_vol(codec_dev, 80.0f);
    esp_codec_dev_set_out_mute(codec_dev, false);

    Serial.printf("[AUDIO] Codec initialized: %d Hz, %d ch, %d bits, vol=80%%\n",
                  AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_SAMPLE_SIZE);

    speaker_initialized = true;
    return true;
}

static void stop_speaker(void)
{
    if (!speaker_initialized) return;
    if (codec_dev) {
        esp_codec_dev_close(codec_dev);
        esp_codec_dev_delete(codec_dev);
        codec_dev = NULL;
    }
    speaker_initialized = false;
    Serial.println("[AUDIO] Speaker stopped");
}

/* ----------------------------------------------------------------------- */
/* Audio task                                                              */
/* ----------------------------------------------------------------------- */

static void audioTask(void *param)
{
    (void)param;
    Serial.printf("[AUDIO] Audio task started on Core %d\n", xPortGetCoreID());

    const TickType_t active_poll_interval = pdMS_TO_TICKS(2);
    const TickType_t idle_poll_interval   = pdMS_TO_TICKS(20);

    int last_num_sources = -1;

    while (audio_task_running) {
        if (AudioStatus.num_sources != last_num_sources) {
            Serial.printf("[AUDIO] num_sources %d -> %d\n",
                          last_num_sources, AudioStatus.num_sources);
            last_num_sources = AudioStatus.num_sources;
        }

        if (AudioStatus.num_sources > 0 &&
            audio_open &&
            audio_irq_done_sem != NULL &&
            speaker_initialized &&
            !main_mute &&
            !speaker_mute) {

            /* Drop any stale completion signal before issuing a request. */
            while (xSemaphoreTake(audio_irq_done_sem, 0) == pdTRUE) {}

            D(bug("[AUDIO] Triggering audio interrupt\n"));
            SetInterruptFlag(INTFLAG_AUDIO);
            TriggerInterrupt();

            if (xSemaphoreTake(audio_irq_done_sem, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (!AudioGuestRAMRange(audio_data, adatStreamInfo + 4)) continue;
                uint32_t apple_stream_info = ReadMacInt32(audio_data + adatStreamInfo);
                WriteMacInt32(audio_data + adatStreamInfo, 0);

                if (apple_stream_info && audio_mix_buf != NULL &&
                    AudioGuestRAMRange(apple_stream_info, scd_buffer + 4)) {
                    const uint32_t sample_count    = ReadMacInt32(apple_stream_info + scd_sampleCount);
                    const uint32_t src_channels    = ReadMacInt16(apple_stream_info + scd_numChannels);
                    const uint32_t src_sample_size = ReadMacInt16(apple_stream_info + scd_sampleSize);
                    const uint32_t src_buffer_mac  = ReadMacInt32(apple_stream_info + scd_buffer);

                    bool format_ok = true;
                    if (sample_count == 0 || sample_count > AUDIO_BUFFER_FRAMES) format_ok = false;
                    if ((src_channels != 1 && src_channels != 2) ||
                        (src_sample_size != 8 && src_sample_size != 16)) format_ok = false;

                    if (format_ok) {
                        const uint32_t source_bytes = sample_count * src_channels * (src_sample_size / 8);
                        const uint8_t *src = AudioGuestSamples(src_buffer_mac, source_bytes);
                        if (src != NULL) {
                            const int out_samples = (int)sample_count * AUDIO_CHANNELS;
                            if ((int)(out_samples * sizeof(int16_t)) <= AUDIO_BUFFER_SIZE) {
                                if (src_sample_size == 8) {
                                    if (src_channels == 1) {
                                        for (uint32_t i = 0; i < sample_count; ++i) {
                                            const int16_t s = (int16_t)((((int)src[i]) - 128) * 256);
                                            audio_mix_buf[i * 2 + 0] = s;
                                            audio_mix_buf[i * 2 + 1] = s;
                                        }
                                    } else {
                                        for (int i = 0; i < out_samples; ++i) {
                                            audio_mix_buf[i] = (int16_t)((((int)src[i]) - 128) * 256);
                                        }
                                    }
                                } else {
                                    if (src_channels == 1) {
                                        for (uint32_t i = 0; i < sample_count; ++i) {
                                            const int si = (int)i * 2;
                                            const int16_t s = (int16_t)(
                                                ((uint16_t)src[si] << 8) | src[si + 1]);
                                            audio_mix_buf[i * 2 + 0] = s;
                                            audio_mix_buf[i * 2 + 1] = s;
                                        }
                                    } else {
                                        for (int i = 0; i < out_samples; ++i) {
                                            const int si = i * 2;
                                            audio_mix_buf[i] = (int16_t)(
                                                ((uint16_t)src[si] << 8) | src[si + 1]);
                                        }
                                    }
                                }

                                /* Submit to codec. esp_codec_dev_write blocks
                                 * until DMA has consumed the buffer. */
                                esp_codec_dev_write(codec_dev, audio_mix_buf,
                                                    out_samples * sizeof(int16_t));
                            }
                        }
                    }
                }
            } else {
                D(bug("[AUDIO] Timeout waiting for AudioInterrupt\n"));
            }
        }

        vTaskDelay((AudioStatus.num_sources > 0) ? active_poll_interval : idle_poll_interval);
    }

    Serial.println("[AUDIO] Audio task exiting");
    vTaskDelete(NULL);
}

/* ----------------------------------------------------------------------- */
/* Lifecycle                                                               */
/* ----------------------------------------------------------------------- */

static bool open_audio(void)
{
    audio_sample_rates.clear();
    audio_sample_sizes.clear();
    audio_channel_counts.clear();
    audio_sample_rates.push_back(AUDIO_SAMPLE_RATE << 16);
    audio_sample_sizes.push_back(AUDIO_SAMPLE_SIZE);
    audio_channel_counts.push_back(AUDIO_CHANNELS);
    audio_sample_rate_index   = 0;
    audio_sample_size_index   = 0;
    audio_channel_count_index = 0;

    audio_frames_per_block = AUDIO_BUFFER_FRAMES;

    if (audio_mix_buf == NULL) {
        audio_mix_buf = (int16_t *)heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (audio_mix_buf == NULL) {
            Serial.println("[AUDIO] ERROR: failed to allocate PSRAM mix buffer");
            return false;
        }
        memset(audio_mix_buf, 0, AUDIO_BUFFER_SIZE);
        Serial.printf("[AUDIO] Allocated %d byte mix buffer in PSRAM\n", AUDIO_BUFFER_SIZE);
    }

    if (!init_speaker()) return false;

    set_audio_status_format();
    audio_open = true;
    return true;
}

static void close_audio(void)
{
    stop_speaker();
    if (audio_mix_buf) {
        heap_caps_free(audio_mix_buf);
        audio_mix_buf = NULL;
    }
    audio_open = false;
}

/* ----------------------------------------------------------------------- */
/* Public API                                                              */
/* ----------------------------------------------------------------------- */

void AudioInit(void)
{
    Serial.println("[AUDIO] Initializing audio subsystem (Waveshare)...");

    AudioStatus.sample_rate = AUDIO_SAMPLE_RATE << 16;
    AudioStatus.sample_size = AUDIO_SAMPLE_SIZE;
    AudioStatus.channels    = AUDIO_CHANNELS;
    AudioStatus.mixer       = 0;
    AudioStatus.num_sources = 0;
    audio_component_flags   = cmpWantsRegisterMessage | kStereoOut | k16BitOut;

    if (PrefsFindBool("nosound")) {
        Serial.println("[AUDIO] Sound disabled in preferences");
        return;
    }

    audio_irq_done_sem = xSemaphoreCreateBinary();
    if (audio_irq_done_sem == NULL) {
        Serial.println("[AUDIO] ERROR: failed to create semaphore");
        return;
    }

    if (!open_audio()) {
        Serial.println("[AUDIO] failed to open audio device");
        return;
    }

    audio_task_running = true;
    BaseType_t r = xTaskCreatePinnedToCore(
        audioTask, "AudioTask", AUDIO_TASK_STACK_SIZE, NULL,
        AUDIO_TASK_PRIORITY, &audio_task_handle, AUDIO_TASK_CORE);
    if (r != pdPASS) {
        Serial.println("[AUDIO] ERROR: failed to create audio task");
        audio_task_running = false;
        close_audio();
        return;
    }

    Serial.printf("[AUDIO] Audio task created on Core %d (emulation core: %d)\n",
                  AUDIO_TASK_CORE, EMULATION_TASK_CORE);
}

void AudioExit(void)
{
    Serial.println("[AUDIO] Shutting down audio subsystem...");
    if (audio_task_running) {
        audio_task_running = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        audio_task_handle = NULL;
    }
    close_audio();
    if (audio_irq_done_sem) {
        vSemaphoreDelete(audio_irq_done_sem);
        audio_irq_done_sem = NULL;
    }
    Serial.println("[AUDIO] Audio subsystem shut down");
}

void audio_enter_stream(void) { /* handled implicitly */ }

void audio_exit_stream(void)
{
    /* Best-effort: no direct analog to M5.Speaker.stop() - esp_codec_dev
     * draining is handled naturally by the blocking write model. */
}

void AudioInterrupt(void)
{
    if (AudioStatus.mixer) {
        M68kRegisters r;
        WriteMacInt32(audio_data + adatStreamInfo, 0);
        r.a[0] = audio_data + adatStreamInfo;
        r.a[1] = AudioStatus.mixer;
        Execute68k(audio_data + adatGetSourceData, &r);
        if (r.d[0] != 0) {
            WriteMacInt32(audio_data + adatStreamInfo, 0);
        }
    } else {
        WriteMacInt32(audio_data + adatStreamInfo, 0);
    }
    if (audio_irq_done_sem) xSemaphoreGive(audio_irq_done_sem);
}

bool audio_set_sample_rate(int index)
{
    if (index < 0 || index >= (int)audio_sample_rates.size()) return false;
    audio_sample_rate_index = index;
    set_audio_status_format();
    return true;
}

bool audio_set_sample_size(int index)
{
    if (index < 0 || index >= (int)audio_sample_sizes.size()) return false;
    audio_sample_size_index = index;
    set_audio_status_format();
    return true;
}

bool audio_set_channels(int index)
{
    if (index < 0 || index >= (int)audio_channel_counts.size()) return false;
    audio_channel_count_index = index;
    set_audio_status_format();
    return true;
}

bool  audio_get_main_mute(void)          { return main_mute; }
uint32 audio_get_main_volume(void)       { return main_volume; }
bool  audio_get_speaker_mute(void)       { return speaker_mute; }
uint32 audio_get_speaker_volume(void)    { return speaker_volume; }

void audio_set_main_mute(bool mute)
{
    main_mute = mute;
    if (codec_dev) esp_codec_dev_set_out_mute(codec_dev, mute || speaker_mute);
}

void audio_set_main_volume(uint32 vol)
{
    main_volume = vol;
    if (codec_dev) esp_codec_dev_set_out_vol(codec_dev, (float)get_effective_volume_percent());
}

void audio_set_speaker_mute(bool mute)
{
    speaker_mute = mute;
    if (codec_dev) esp_codec_dev_set_out_mute(codec_dev, mute || main_mute);
}

void audio_set_speaker_volume(uint32 vol)
{
    speaker_volume = vol;
    if (codec_dev) esp_codec_dev_set_out_vol(codec_dev, (float)get_effective_volume_percent());
}

/* AudioReset() is defined in src/basilisk/audio.cpp; don't duplicate it. */
