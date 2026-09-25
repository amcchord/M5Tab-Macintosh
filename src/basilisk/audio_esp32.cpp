/*
 *  audio_esp32.cpp - Audio support for ESP32-P4 with M5Unified Speaker
 *
 *  BasiliskII ESP32 Port
 *
 *  Uses M5Unified Speaker class to interface with ES8388 codec on Tab5.
 *  Audio data is retrieved from the Mac OS Apple Mixer via 68k code execution,
 *  then converted from big-endian to little-endian and sent to the speaker.
 *
 *  Audio task runs on the non-emulation core.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "audio_guest_memory.h"
#include "main.h"
#include "prefs.h"
#include "audio.h"
#include "audio_defs.h"

#include <M5Unified.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_system.h"

#define DEBUG 0
#include "debug.h"

// ============================================================================
// Audio Configuration
// ============================================================================

// Audio task configuration
#define AUDIO_TASK_STACK_SIZE  4096
#define AUDIO_TASK_PRIORITY    2  // Slightly higher priority than video/input

// Pin audio work to the opposite core from Arduino/emulation loop work.
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

// Audio buffer configuration
// Using 22050 Hz for better ESP32 performance (lower CPU load)
// Mac OS will resample if needed
#define AUDIO_SAMPLE_RATE      22050
#define AUDIO_BUFFER_FRAMES    1024   // Number of frames per buffer
#define AUDIO_CHANNELS         2      // Stereo
#define AUDIO_SAMPLE_SIZE      16     // 16-bit samples

// Calculate buffer sizes
#define AUDIO_BYTES_PER_FRAME  (AUDIO_CHANNELS * (AUDIO_SAMPLE_SIZE / 8))
#define AUDIO_BUFFER_SIZE      (AUDIO_BUFFER_FRAMES * AUDIO_BYTES_PER_FRAME)

// Mac volume is 8.8 fixed point, max is 0x0100
#define MAC_MAX_VOLUME 0x0100

// ============================================================================
// Audio State
// ============================================================================

// The currently selected audio parameters (indices in audio_sample_rates[] etc. vectors)
static int audio_sample_rate_index = 0;
static int audio_sample_size_index = 0;
static int audio_channel_count_index = 0;

// Audio task handle and state
static TaskHandle_t audio_task_handle = NULL;
static volatile bool audio_task_running = false;

// Semaphore for synchronization between audio task and AudioInterrupt
static SemaphoreHandle_t audio_irq_done_sem = NULL;

// Audio mixing buffer - in PSRAM for larger size
static int16_t *audio_mix_buf = NULL;

// Volume and mute state
// Start at 50% to avoid distortion - Mac OS can adjust via Sound control panel
static int main_volume = MAC_MAX_VOLUME / 2;
static int speaker_volume = MAC_MAX_VOLUME / 2;
static bool main_mute = false;
static bool speaker_mute = false;

// Speaker initialization state
static bool speaker_initialized = false;

// ============================================================================
// Internal Functions
// ============================================================================

/*
 *  Set AudioStatus to reflect current audio stream format
 */
static void set_audio_status_format(void)
{
    AudioStatus.sample_rate = audio_sample_rates[audio_sample_rate_index];
    AudioStatus.sample_size = audio_sample_sizes[audio_sample_size_index];
    AudioStatus.channels = audio_channel_counts[audio_channel_count_index];
}

/*
 *  Calculate the effective volume (0-255 for M5Unified Speaker)
 */
static uint8_t get_effective_volume(void)
{
    if (main_mute || speaker_mute) {
        return 0;
    }
    
    // Combine main and speaker volume
    // Both are 8.8 fixed point, max 0x0100
    // Result should be 0-255 for M5Unified
    uint32_t combined = (main_volume * speaker_volume) / MAC_MAX_VOLUME;
    
    // Scale to 0-255 range
    uint32_t volume = (combined * 255) / MAC_MAX_VOLUME;
    if (volume > 255) {
        volume = 255;
    }
    
    return (uint8_t)volume;
}

/*
 *  Reset ES8388 codec via I2C with proper timing.
 *
 *  Why this exists: the ES8388 stays powered through ESP32 software resets,
 *  so after a crash, panic, or `esp_restart()` it retains whatever state it
 *  was in - mid-DMA, mid-PLL-lock, charge pump active, DAC powered up with
 *  garbage configuration. M5Unified's normal init writes the chip-reset
 *  register back-to-back with no delay, which can leave the codec stuck
 *  enough that audio is silent until the user pulls power. This routine
 *  forces a full power-down + reset cycle before the normal init runs.
 *
 *  Sequence:
 *    1. Mute the NS4150 power amp via PI4IO1 GPIO so the user doesn't
 *       hear pops during the rest of the dance.
 *    2. Power down the DAC, ADC, charge pump, and analog reference in
 *       known order so the chip is quiescent when we toggle the reset.
 *       Doing this with the chip running in an unknown state was the
 *       leading cause of the post-crash audio dead-state.
 *    3. Assert software reset (chip control 1, bit 7) for 100 ms - the
 *       datasheet says >= 1 ms but bench testing shows the DAC PLL needs
 *       longer to settle if it had been actively producing samples.
 *    4. Release reset and wait 50 ms for the internal POR to complete
 *       and the I2C interface to come back online.
 *    5. Read back register 0 to verify the chip is responsive. If it
 *       isn't (bus error, 0xFF, etc.) we log it but still continue -
 *       Speaker.begin() will get its own crack at it.
 */
static constexpr uint8_t ES8388_I2C_ADDR  = 0x10;
static constexpr uint8_t PI4IO1_I2C_ADDR  = 0x43;

/* ES8388 register map (subset). Names match the ES8388 datasheet. */
static constexpr uint8_t ES8388_REG_CHIP_CTRL1   = 0x00;
static constexpr uint8_t ES8388_REG_CHIP_CTRL2   = 0x01;
static constexpr uint8_t ES8388_REG_CHIP_PWR     = 0x02;
static constexpr uint8_t ES8388_REG_ADC_PWR      = 0x03;
static constexpr uint8_t ES8388_REG_DAC_PWR      = 0x04;
static constexpr uint8_t ES8388_REG_DAC_CTRL3    = 0x19;  /* DAC mute */
static constexpr uint8_t ES8388_REG_LOUT1_VOL    = 0x2E;
static constexpr uint8_t ES8388_REG_ROUT1_VOL    = 0x2F;

static void reset_es8388(void)
{
    Serial.println("[AUDIO] Force-resetting ES8388 codec...");

    /* 1) Hard mute the NS4150 power amp before we touch anything else.
     * The PI4IO1 GPIO expander on the Tab5 gates the amp's enable line
     * through bit 1 of register 0x05. Driving it low isolates the
     * speaker from any residual DC offset on the codec line outputs. */
    M5.In_I2C.bitOff(PI4IO1_I2C_ADDR, 0x05, 0b00000010, 400000);
    delay(5);

    /* 2) Software-mute the DAC outputs and zero LOUT1/ROUT1 so the
     * line drivers are silent before we cut their power. Best-effort -
     * if the codec is wedged these will fail and that's OK. */
    M5.In_I2C.writeRegister8(ES8388_I2C_ADDR, ES8388_REG_DAC_CTRL3, 0x04, 400000);
    M5.In_I2C.writeRegister8(ES8388_I2C_ADDR, ES8388_REG_LOUT1_VOL, 0x00, 400000);
    M5.In_I2C.writeRegister8(ES8388_I2C_ADDR, ES8388_REG_ROUT1_VOL, 0x00, 400000);

    /* 3) Power down DAC, ADC, charge pump, analog reference. The order
     * matters per the ES8388 power-down sequence: outputs first, then
     * DAC core, then ADC core, then chip-wide power. Each write fires
     * even if the previous one failed; we want to push the chip toward
     * a known state regardless of how it started. */
    M5.In_I2C.writeRegister8(ES8388_I2C_ADDR, ES8388_REG_DAC_PWR,  0xC0, 400000);  /* DAC L+R off */
    M5.In_I2C.writeRegister8(ES8388_I2C_ADDR, ES8388_REG_ADC_PWR,  0xFF, 400000);  /* ADC fully off */
    M5.In_I2C.writeRegister8(ES8388_I2C_ADDR, ES8388_REG_CHIP_PWR, 0xFF, 400000);  /* analog/digital powered down */
    delay(20);

    /* 4) Assert software reset (chip control 1 bit 7). Hold long
     * enough that any in-flight I2S frame, PLL lock, and DAC zero-
     * crossing detector all finish. */
    M5.In_I2C.writeRegister8(ES8388_I2C_ADDR, ES8388_REG_CHIP_CTRL1, 0x80, 400000);
    delay(100);

    /* 5) Release reset and let internal POR complete. */
    M5.In_I2C.writeRegister8(ES8388_I2C_ADDR, ES8388_REG_CHIP_CTRL1, 0x00, 400000);
    delay(50);

    /* 6) Sanity-read chip control 1. Anything other than 0x00/0x06
     * means the chip didn't accept our writes; log but proceed so
     * Speaker.begin() can still try. */
    uint8_t ctrl1 = M5.In_I2C.readRegister8(ES8388_I2C_ADDR, ES8388_REG_CHIP_CTRL1, 400000);
    if (ctrl1 == 0xFF) {
        Serial.println("[AUDIO] WARN: ES8388 not responding on I2C after reset");
    } else {
        Serial.printf("[AUDIO] ES8388 reset OK, chip ctrl1=0x%02X\n", ctrl1);
    }
}

/*
 *  Initialize M5Unified Speaker
 */
static bool init_speaker(void)
{
    Serial.println("[AUDIO] Initializing M5Unified Speaker...");

    /* Always run a full ES8388 reset before Speaker.begin(), regardless
     * of esp_reset_reason. The codec keeps its previous state through
     * any kind of ESP32 reset (it has its own power rail), and the
     * full power-down + reset sequence is harmless when the chip is
     * already cold-booted. Previously we gated this on warm-boot only,
     * which left "first boot after a brownout" cases relying on
     * M5Unified's compressed init path - leading to occasional
     * post-crash silent-audio reports that only a power cycle could
     * fix. Logging the reset reason for diagnostics. */
    esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("[AUDIO] esp_reset_reason=%d\n", (int)reason);
    reset_es8388();

    // Get current speaker config
    auto spk_cfg = M5.Speaker.config();
    
    // Configure for our audio format
    spk_cfg.sample_rate = AUDIO_SAMPLE_RATE;
    spk_cfg.stereo = (AUDIO_CHANNELS == 2);
    spk_cfg.buzzer = false;  // Not using buzzer mode
    spk_cfg.use_dac = false; // Use I2S, not DAC
    
    // Apply configuration
    M5.Speaker.config(spk_cfg);
    
    // Start the speaker
    if (!M5.Speaker.begin()) {
        Serial.println("[AUDIO] ERROR: Failed to start M5Unified Speaker");
        return false;
    }
    
    // Set initial volume
    M5.Speaker.setVolume(get_effective_volume());
    
    Serial.printf("[AUDIO] Speaker initialized: %d Hz, %s\n",
                  AUDIO_SAMPLE_RATE,
                  AUDIO_CHANNELS == 2 ? "stereo" : "mono");
    
    speaker_initialized = true;
    return true;
}

/*
 *  Stop M5Unified Speaker
 */
static void stop_speaker(void)
{
    if (speaker_initialized) {
        M5.Speaker.stop();
        M5.Speaker.end();
        speaker_initialized = false;
        Serial.println("[AUDIO] Speaker stopped");
    }
}

/*
 *  Audio streaming task - runs on non-emulation core
 *  Periodically requests audio data from Mac OS and sends to speaker
 */
static void audioTask(void *param)
{
    (void)param;
    Serial.printf("[AUDIO] Audio task started on Core %d\n", xPortGetCoreID());

    // Poll quickly, but only request new mixer data when the speaker queue has room.
    const TickType_t active_poll_interval = pdMS_TO_TICKS(2);
    const TickType_t idle_poll_interval = pdMS_TO_TICKS(20);
    
    while (audio_task_running) {
        // Check if there are active sound sources
        if (AudioStatus.num_sources > 0 &&
            audio_open &&
            audio_irq_done_sem != NULL &&
            speaker_initialized &&
            !main_mute &&
            !speaker_mute) {

            // Only fetch when channel 0 has fully drained current buffer.
            // This better matches pull-based backends that request one block
            // at a time at playback cadence.
            if (M5.Speaker.isPlaying(0) != 0) {
                vTaskDelay(active_poll_interval);
                continue;
            }

            // Drop any stale completion signal before issuing a fresh request.
            while (xSemaphoreTake(audio_irq_done_sem, 0) == pdTRUE) {
            }

            // Trigger audio interrupt to get new buffer from Mac OS
            D(bug("[AUDIO] Triggering audio interrupt\n"));
            SetInterruptFlag(INTFLAG_AUDIO);
            TriggerInterrupt();
            
            // Wait for AudioInterrupt to complete (with timeout)
            if (xSemaphoreTake(audio_irq_done_sem, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (!AudioGuestRAMRange(audio_data, adatStreamInfo + 4)) continue;
                // Get stream info from Apple Mixer
                uint32_t apple_stream_info = ReadMacInt32(audio_data + adatStreamInfo);
                // Consume this stream-info slot exactly once.
                WriteMacInt32(audio_data + adatStreamInfo, 0);

                if (apple_stream_info && audio_mix_buf != NULL &&
                    AudioGuestRAMRange(apple_stream_info, scd_buffer + 4)) {
                    const uint32_t sample_count = ReadMacInt32(apple_stream_info + scd_sampleCount);
                    const uint32_t src_channels = ReadMacInt16(apple_stream_info + scd_numChannels);
                    const uint32_t src_sample_size = ReadMacInt16(apple_stream_info + scd_sampleSize);
                    const uint32_t src_buffer_mac = ReadMacInt32(apple_stream_info + scd_buffer);
                    const uint32_t src_rate_fixed = ReadMacInt32(apple_stream_info + scd_sampleRate);
                    uint32_t src_rate_hz = src_rate_fixed >> 16;
                    if (src_rate_hz == 0 || src_rate_hz > 96000) {
                        src_rate_hz = AUDIO_SAMPLE_RATE;
                    }

                    D(bug("[AUDIO] Got %d samples, %d channels, %d bits\n",
                          sample_count, src_channels, src_sample_size));

                    bool format_ok = true;
                    if (sample_count == 0 || sample_count > AUDIO_BUFFER_FRAMES) {
                        D(bug("[AUDIO] Dropping block: unsupported sample_count=%d\n", sample_count));
                        format_ok = false;
                    }
                    if ((src_channels != 1 && src_channels != 2) ||
                        (src_sample_size != 8 && src_sample_size != 16)) {
                        D(bug("[AUDIO] Dropping block: unsupported format %dch/%dbit\n",
                              src_channels, src_sample_size));
                        format_ok = false;
                    }

                    if (format_ok) {
                        // Get source buffer pointer
                        const uint32_t source_bytes = sample_count * src_channels * (src_sample_size / 8);
                        const uint8_t *src = AudioGuestSamples(src_buffer_mac, source_bytes);
                        if (src != NULL) {
                            const int out_samples = static_cast<int>(sample_count) * AUDIO_CHANNELS;
                            if ((out_samples * static_cast<int>(sizeof(int16_t))) <= AUDIO_BUFFER_SIZE) {
                                if (src_sample_size == 8) {
                                    if (src_channels == 1) {
                                        for (uint32_t i = 0; i < sample_count; ++i) {
                                            const int16_t sample = (static_cast<int16_t>(src[i]) - 128) * 256;
                                            audio_mix_buf[i * 2] = sample;
                                            audio_mix_buf[i * 2 + 1] = sample;
                                        }
                                    } else {
                                        for (int i = 0; i < out_samples; ++i) {
                                            audio_mix_buf[i] = (static_cast<int16_t>(src[i]) - 128) * 256;
                                        }
                                    }
                                } else {
                                    if (src_channels == 1) {
                                        for (uint32_t i = 0; i < sample_count; ++i) {
                                            const int src_index = static_cast<int>(i) * 2;
                                            const int16_t sample = static_cast<int16_t>(
                                                (static_cast<uint16_t>(src[src_index]) << 8) | src[src_index + 1]);
                                            audio_mix_buf[i * 2] = sample;
                                            audio_mix_buf[i * 2 + 1] = sample;
                                        }
                                    } else {
                                        for (int i = 0; i < out_samples; ++i) {
                                            const int src_index = i * 2;
                                            audio_mix_buf[i] = static_cast<int16_t>(
                                                (static_cast<uint16_t>(src[src_index]) << 8) | src[src_index + 1]);
                                        }
                                    }
                                }

                                // Stream to speaker. M5 handles I2S buffering internally.
                                M5.Speaker.playRaw(audio_mix_buf, out_samples, src_rate_hz, true, 1, 0, false);
                            } else {
                                D(bug("[AUDIO] Dropping block: output size too large (%d samples)\n", out_samples));
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

/*
 *  Open audio device
 */
static bool open_audio(void)
{
    // Advertise one stable output format that matches the speaker stream config.
    audio_sample_rates.clear();
    audio_sample_sizes.clear();
    audio_channel_counts.clear();
    audio_sample_rates.push_back(AUDIO_SAMPLE_RATE << 16);
    audio_sample_sizes.push_back(AUDIO_SAMPLE_SIZE);
    audio_channel_counts.push_back(AUDIO_CHANNELS);
    audio_sample_rate_index = 0;
    audio_sample_size_index = 0;
    audio_channel_count_index = 0;
    
    // Set audio frames per block
    audio_frames_per_block = AUDIO_BUFFER_FRAMES;
    
    // Allocate audio buffer in PSRAM
    if (audio_mix_buf == NULL) {
        audio_mix_buf = (int16_t *)heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (audio_mix_buf == NULL) {
            Serial.println("[AUDIO] ERROR: Failed to allocate audio buffer");
            return false;
        }
        memset(audio_mix_buf, 0, AUDIO_BUFFER_SIZE);
        Serial.printf("[AUDIO] Allocated %d byte audio buffer in PSRAM\n", AUDIO_BUFFER_SIZE);
    }
    
    // Initialize speaker
    if (!init_speaker()) {
        return false;
    }
    
    // Set AudioStatus to reflect format
    set_audio_status_format();
    
    audio_open = true;
    return true;
}

/*
 *  Close audio device
 */
static void close_audio(void)
{
    stop_speaker();
    
    if (audio_mix_buf != NULL) {
        heap_caps_free(audio_mix_buf);
        audio_mix_buf = NULL;
    }
    
    audio_open = false;
}

// ============================================================================
// Public API (called from audio.cpp and emulator core)
// ============================================================================

/*
 *  Initialization
 */
void AudioInit(void)
{
    Serial.println("[AUDIO] Initializing audio subsystem...");
    
    // Init audio status and feature flags
    AudioStatus.sample_rate = AUDIO_SAMPLE_RATE << 16;  // 16.16 fixed point
    AudioStatus.sample_size = AUDIO_SAMPLE_SIZE;
    AudioStatus.channels = AUDIO_CHANNELS;
    AudioStatus.mixer = 0;
    AudioStatus.num_sources = 0;
    audio_component_flags = cmpWantsRegisterMessage | kStereoOut | k16BitOut;
    
    // Sound disabled in prefs? Then do nothing
    if (PrefsFindBool("nosound")) {
        Serial.println("[AUDIO] Sound disabled in preferences");
        return;
    }
    
    // Create semaphore for audio interrupt synchronization
    audio_irq_done_sem = xSemaphoreCreateBinary();
    if (audio_irq_done_sem == NULL) {
        Serial.println("[AUDIO] ERROR: Failed to create audio semaphore");
        return;
    }
    
    // Open audio device
    if (!open_audio()) {
        Serial.println("[AUDIO] Failed to open audio device");
        return;
    }
    
    // Start audio task on non-emulation core
    audio_task_running = true;
    BaseType_t result = xTaskCreatePinnedToCore(
        audioTask,
        "AudioTask",
        AUDIO_TASK_STACK_SIZE,
        NULL,
        AUDIO_TASK_PRIORITY,
        &audio_task_handle,
        AUDIO_TASK_CORE
    );
    
    if (result != pdPASS) {
        Serial.println("[AUDIO] ERROR: Failed to create audio task");
        audio_task_running = false;
        close_audio();
        return;
    }
    
    Serial.printf("[AUDIO] Audio task created on Core %d (emulation core: %d)\n",
                  AUDIO_TASK_CORE, EMULATION_TASK_CORE);
    Serial.println("[AUDIO] Audio subsystem initialized successfully");
}

/*
 *  Deinitialization
 */
void AudioExit(void)
{
    Serial.println("[AUDIO] Shutting down audio subsystem...");
    
    // Stop audio task
    if (audio_task_running) {
        audio_task_running = false;
        // Give task time to exit
        vTaskDelay(pdMS_TO_TICKS(100));
        audio_task_handle = NULL;
    }
    
    // Close audio device
    close_audio();
    
    // Delete semaphore
    if (audio_irq_done_sem != NULL) {
        vSemaphoreDelete(audio_irq_done_sem);
        audio_irq_done_sem = NULL;
    }
    
    Serial.println("[AUDIO] Audio subsystem shut down");
}

/*
 *  First source added, start audio stream
 */
void audio_enter_stream(void)
{
    D(bug("[AUDIO] audio_enter_stream\n"));
    // Audio task handles this automatically via AudioStatus.num_sources
}

/*
 *  Last source removed, stop audio stream
 */
void audio_exit_stream(void)
{
    D(bug("[AUDIO] audio_exit_stream\n"));
    // Audio task handles this automatically via AudioStatus.num_sources
    
    // Stop any current playback
    if (speaker_initialized) {
        M5.Speaker.stop();
    }
}

/*
 *  MacOS audio interrupt, read next data block
 *  Called from emul_op.cpp when INTFLAG_AUDIO is set
 */
void AudioInterrupt(void)
{
    D(bug("[AUDIO] AudioInterrupt\n"));
    
    // Get data from Apple Mixer
    if (AudioStatus.mixer) {
        M68kRegisters r;
        // Clear previous pointer so stale buffers are never replayed.
        WriteMacInt32(audio_data + adatStreamInfo, 0);
        r.a[0] = audio_data + adatStreamInfo;
        r.a[1] = AudioStatus.mixer;
        Execute68k(audio_data + adatGetSourceData, &r);
        D(bug("[AUDIO] GetSourceData() returns %08lx\n", r.d[0]));

        // Non-zero return means the mixer did not provide fresh source data.
        // Force stream info to 0 so callers never reuse stale buffer pointers.
        if (r.d[0] != 0) {
            WriteMacInt32(audio_data + adatStreamInfo, 0);
        }
    } else {
        WriteMacInt32(audio_data + adatStreamInfo, 0);
    }
    
    // Signal audio task that data is ready
    if (audio_irq_done_sem != NULL) {
        xSemaphoreGive(audio_irq_done_sem);
    }
    
    D(bug("[AUDIO] AudioInterrupt done\n"));
}

/*
 *  Set sampling parameters
 *  "index" is an index into the audio_sample_rates[] etc. vectors
 *  It is guaranteed that AudioStatus.num_sources == 0
 */
bool audio_set_sample_rate(int index)
{
    if (index < 0 || index >= (int)audio_sample_rates.size()) {
        return false;
    }
    
    audio_sample_rate_index = index;
    set_audio_status_format();
    
    Serial.printf("[AUDIO] Sample rate set to %d Hz\n",
                  audio_sample_rates[index] >> 16);
    return true;
}

bool audio_set_sample_size(int index)
{
    if (index < 0 || index >= (int)audio_sample_sizes.size()) {
        return false;
    }
    
    audio_sample_size_index = index;
    set_audio_status_format();
    
    Serial.printf("[AUDIO] Sample size set to %d bits\n",
                  audio_sample_sizes[index]);
    return true;
}

bool audio_set_channels(int index)
{
    if (index < 0 || index >= (int)audio_channel_counts.size()) {
        return false;
    }
    
    audio_channel_count_index = index;
    set_audio_status_format();
    
    Serial.printf("[AUDIO] Channels set to %d\n",
                  audio_channel_counts[index]);
    return true;
}

/*
 *  Get/set volume controls
 *  Volume values use 8.8 fixed point with 0x0100 meaning "maximum volume"
 *  Left channel in upper 16 bits, right channel in lower 16 bits
 */

bool audio_get_main_mute(void)
{
    return main_mute;
}

uint32 audio_get_main_volume(void)
{
    uint32 chan = main_volume;
    return (chan << 16) | chan;
}

bool audio_get_speaker_mute(void)
{
    return speaker_mute;
}

uint32 audio_get_speaker_volume(void)
{
    uint32 chan = speaker_volume;
    return (chan << 16) | chan;
}

void audio_set_main_mute(bool mute)
{
    main_mute = mute;
    
    // Update M5Unified Speaker volume
    if (speaker_initialized) {
        M5.Speaker.setVolume(get_effective_volume());
    }
    
    D(bug("[AUDIO] Main mute set to %d\n", mute));
}

void audio_set_main_volume(uint32 vol)
{
    // Average left and right channels
    main_volume = ((vol >> 16) + (vol & 0xffff)) / 2;
    if (main_volume > MAC_MAX_VOLUME) {
        main_volume = MAC_MAX_VOLUME;
    }
    
    // Update M5Unified Speaker volume
    if (speaker_initialized) {
        M5.Speaker.setVolume(get_effective_volume());
    }
    
    D(bug("[AUDIO] Main volume set to %d\n", main_volume));
}

void audio_set_speaker_mute(bool mute)
{
    speaker_mute = mute;
    
    // Update M5Unified Speaker volume
    if (speaker_initialized) {
        M5.Speaker.setVolume(get_effective_volume());
    }
    
    D(bug("[AUDIO] Speaker mute set to %d\n", mute));
}

void audio_set_speaker_volume(uint32 vol)
{
    // Average left and right channels
    speaker_volume = ((vol >> 16) + (vol & 0xffff)) / 2;
    if (speaker_volume > MAC_MAX_VOLUME) {
        speaker_volume = MAC_MAX_VOLUME;
    }
    
    // Update M5Unified Speaker volume
    if (speaker_initialized) {
        M5.Speaker.setVolume(get_effective_volume());
    }
    
    D(bug("[AUDIO] Speaker volume set to %d\n", speaker_volume));
}
