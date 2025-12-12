#include "audio_player.h"
#include "nvs.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "event_bus.h"
#include "esp_check.h"
#include "helix_mp3_wrapper.h"
#include "error_monitor.h"
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// I2S pins for PCM5102A. Configurable via menuconfig defaults.
#define I2S_BCK_PIN  CONFIG_BROKER_I2S_BCK_PIN
#define I2S_WS_PIN   CONFIG_BROKER_I2S_WS_PIN
#define I2S_DATA_PIN CONFIG_BROKER_I2S_DATA_PIN

// SD card (SPI mode) pins.
#define SD_PIN_MISO CONFIG_BROKER_SD_MISO_PIN
#define SD_PIN_MOSI CONFIG_BROKER_SD_MOSI_PIN
#define SD_PIN_CLK  CONFIG_BROKER_SD_CLK_PIN
#define SD_PIN_CS   CONFIG_BROKER_SD_CS_PIN

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BITS        I2S_DATA_BIT_WIDTH_16BIT
#define AUDIO_CHANNELS    2
#define AUDIO_QUEUE_LEN   4

static const char *TAG = "audio_player";

typedef enum {
    AUDIO_FMT_UNKNOWN = 0,
    AUDIO_FMT_WAV,
    AUDIO_FMT_MP3,
    AUDIO_FMT_OGG,
} audio_format_t;

typedef struct {
    char path[256];
    int volume;
    float seek_ratio;
} audio_cmd_t;

typedef struct {
    audio_format_t fmt;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
} audio_info_t;

static int s_volume = 70;
static int s_current_volume = 70;
static bool s_volume_loaded = false;
static volatile bool s_paused = false;
static QueueHandle_t s_queue = NULL;
static audio_player_status_t s_status = {0};
static SemaphoreHandle_t s_status_mutex = NULL;
static i2s_chan_handle_t s_tx_chan = NULL;
static bool s_tx_enabled = false;
static TaskHandle_t s_task = NULL;
static sdmmc_card_t *s_card = NULL;
static bool s_spi_init = false;
static TaskHandle_t s_reader_task = NULL;
static volatile bool s_reader_done = false;
static volatile bool s_stop_requested = false;
static int s_last_bitrate_kbps = 0;
static TaskHandle_t s_save_task = NULL;
static volatile int s_pending_volume = -1;
static SemaphoreHandle_t s_sd_mutex = NULL;

static void sd_lock(void)
{
    if (s_sd_mutex) {
        xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    }
}

static void sd_unlock(void)
{
    if (s_sd_mutex) {
        xSemaphoreGive(s_sd_mutex);
    }
}

static bool sd_card_available(void)
{
    bool present = false;
    sd_lock();
    present = (s_card != NULL);
    sd_unlock();
    return present;
}

static audio_player_format_t to_public_format(audio_format_t fmt)
{
    switch (fmt) {
    case AUDIO_FMT_WAV: return AUDIO_PLAYER_FMT_WAV;
    case AUDIO_FMT_MP3: return AUDIO_PLAYER_FMT_MP3;
    case AUDIO_FMT_OGG: return AUDIO_PLAYER_FMT_OGG;
    default: return AUDIO_PLAYER_FMT_UNKNOWN;
    }
}

static void status_lock(void)
{
    if (s_status_mutex) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    }
}

static void status_unlock(void)
{
    if (s_status_mutex) {
        xSemaphoreGive(s_status_mutex);
    }
}

static void status_reset(void)
{
    status_lock();
    memset(&s_status, 0, sizeof(s_status));
    s_status.volume = s_volume;
    s_status.bitrate_kbps = 0;
    s_status.message[0] = 0;
    status_unlock();
}

static void status_set_play(const char *path, audio_format_t fmt)
{
    status_lock();
    s_status.playing = true;
    s_status.paused = false;
    s_status.progress = 0;
    s_status.pos_ms = 0;
    s_status.dur_ms = 0;
    s_status.bitrate_kbps = 0;
        s_status.volume = s_volume;
        s_status.fmt = to_public_format(fmt);
        s_status.message[0] = 0;
        s_status.path[0] = 0;
    if (path) {
        strncpy(s_status.path, path, sizeof(s_status.path) - 1);
        s_status.path[sizeof(s_status.path) - 1] = 0;
    }
    status_unlock();
}

static void status_set_paused(bool paused)
{
    status_lock();
    s_status.paused = paused;
    status_unlock();
}

static void status_set_playing(bool playing)
{
    status_lock();
    s_status.playing = playing;
    if (!playing) {
        s_status.paused = false;
        s_status.progress = 0;
        s_status.pos_ms = 0;
        s_status.dur_ms = 0;
        s_status.bitrate_kbps = 0;
    }
    status_unlock();
}

static void status_update_progress(size_t bytes_read, size_t total_bytes, uint32_t pos_ms, uint32_t est_total_ms)
{
    status_lock();
    if (total_bytes > 0) {
        s_status.progress = (int)((bytes_read * 100U) / total_bytes);
        if (s_status.progress > 100) s_status.progress = 100;
    }
    if (pos_ms > 0) {
        s_status.pos_ms = (int)pos_ms;
    }
    if (est_total_ms > 0) {
        s_status.dur_ms = (int)est_total_ms;
    }
    status_unlock();
}

static void status_update_bitrate(int kbps)
{
    status_lock();
    s_status.bitrate_kbps = kbps;
    status_unlock();
}

static void status_set_message(const char *msg)
{
    status_lock();
    if (msg) {
        strncpy(s_status.message, msg, sizeof(s_status.message) - 1);
        s_status.message[sizeof(s_status.message) - 1] = 0;
    } else {
        s_status.message[0] = 0;
    }
    status_unlock();
}

static void save_volume_to_nvs(uint8_t vol)
{
    nvs_handle_t h;
    if (nvs_open("audiocfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vol", vol);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void save_task(void *param)
{
    (void)param;
    while (1) {
        int v = s_pending_volume;
        if (v >= 0) {
            s_pending_volume = -1;
            save_volume_to_nvs((uint8_t)v);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static size_t mp3_i2s_write_cb(const uint8_t *data, size_t len, void *user)
{
    if (s_stop_requested) {
        return 0;
    }
    while (s_paused && !s_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    i2s_chan_handle_t chan = (i2s_chan_handle_t)user;
    size_t written = 0;
    if (!chan) {
        return 0;
    }
    int vol = s_volume;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    const bool unity = (vol == 100);
    const int16_t *in = (const int16_t *)data;
    size_t samples = len / sizeof(int16_t);
    uint8_t *tmp_buf = NULL;
    const void *out_ptr = data;
    if (!unity) {
        tmp_buf = heap_caps_malloc(len, MALLOC_CAP_DEFAULT);
        if (tmp_buf) {
            int16_t *out = (int16_t *)tmp_buf;
            float gain = vol / 100.0f;
            for (size_t i = 0; i < samples; ++i) {
                int32_t v = (int32_t)((float)in[i] * gain);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                out[i] = (int16_t)v;
            }
            out_ptr = tmp_buf;
        }
    }
    if (i2s_channel_write(chan, out_ptr, len, &written, portMAX_DELAY) != ESP_OK) {
        written = 0;
    }
    if (tmp_buf) {
        heap_caps_free(tmp_buf);
    }
    return written;
}

static void mp3_progress_cb(size_t bytes_read, size_t total_bytes, uint32_t elapsed_ms, uint32_t est_total_ms, void *user)
{
    int kbps = user ? *((int *)user) : 0;
    status_update_progress(bytes_read, total_bytes, elapsed_ms, est_total_ms);
    if (kbps > 0) {
        status_update_bitrate(kbps);
    }
}

static void i2s_setup(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(AUDIO_BITS, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DATA_PIN,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    s_tx_enabled = true;
}

static audio_format_t detect_format(const uint8_t *hdr, size_t len)
{
    if (len >= 12 && memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0) {
        return AUDIO_FMT_WAV;
    }
    if (len >= 3 && memcmp(hdr, "ID3", 3) == 0) {
        return AUDIO_FMT_MP3;
    }
    if (len >= 2 && hdr[0] == 0xFF && (hdr[1] & 0xE0) == 0xE0) {
        return AUDIO_FMT_MP3;
    }
    if (len >= 4 && memcmp(hdr, "OggS", 4) == 0) {
        return AUDIO_FMT_OGG;
    }
    return AUDIO_FMT_UNKNOWN;
}

static esp_err_t sd_mount(void)
{
    sd_lock();
    if (s_card) {
        sd_unlock();
        return ESP_OK;
    }
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    if (!s_spi_init) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = SD_PIN_MOSI,
            .miso_io_num = SD_PIN_MISO,
            .sclk_io_num = SD_PIN_CLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4000,
        };
        esp_err_t err = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (err != ESP_OK) {
            sd_unlock();
            ESP_RETURN_ON_ERROR(err, TAG, "spi init failed");
        }
        s_spi_init = true;
        ESP_LOGI(TAG, "SPI bus init for SD: MOSI=%d MISO=%d CLK=%d CS=%d freq=%dkHz",
                 SD_PIN_MOSI, SD_PIN_MISO, SD_PIN_CLK, SD_PIN_CS, host.max_freq_khz);
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = host.slot;

    const char mount_point[] = "/sdcard";
    esp_err_t ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to mount SD card: %s", esp_err_to_name(ret));
        error_monitor_set_sd_state(false);
        error_monitor_report_sd_fault();
    } else {
        ESP_LOGI(TAG, "SD mounted at %s", mount_point);
        error_monitor_set_sd_state(true);
    }
    sd_unlock();
    return ret;
}

static void play_tone_ms(int freq_hz, int duration_ms, int volume_percent)
{
    if (!s_tx_chan) {
        return;
    }
    const int total_samples = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    const int buf_samples = 256;
    int16_t *buf = heap_caps_malloc(buf_samples * AUDIO_CHANNELS * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "no mem for audio buf");
        error_monitor_report_audio_fault();
        return;
    }
    float phase = 0.0f;
    const float step = 2.0f * (float)M_PI * (float)freq_hz / (float)AUDIO_SAMPLE_RATE;
    const int32_t amplitude = (int32_t)(32767 * (volume_percent / 100.0f));

    int produced = 0;
    while (produced < total_samples) {
        int chunk = buf_samples;
        if (produced + chunk > total_samples) {
            chunk = total_samples - produced;
        }
        for (int i = 0; i < chunk; ++i) {
            int16_t sample = (int16_t)(sinf(phase) * amplitude);
            phase += step;
            if (phase > 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
            buf[2 * i] = sample;
            buf[2 * i + 1] = sample;
        }
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, buf, chunk * AUDIO_CHANNELS * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s write err %s", esp_err_to_name(err));
            break;
        }
        produced += chunk;
    }
    heap_caps_free(buf);
}

static esp_err_t parse_wav_header(FILE *f, audio_info_t *info, long *data_offset, uint32_t *data_size)
{
    struct __attribute__((packed)) wav_header {
        char riff[4];
        uint32_t size;
        char wave[4];
        char fmt[4];
        uint32_t fmt_size;
        uint16_t audio_format;
        uint16_t num_channels;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits_per_sample;
        char data_id[4];
        uint32_t data_size;
    } hdr;

    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        return ESP_FAIL;
    }
    if (strncmp(hdr.riff, "RIFF", 4) != 0 || strncmp(hdr.wave, "WAVE", 4) != 0) {
        return ESP_FAIL;
    }
    if (hdr.audio_format != 1 || hdr.bits_per_sample != 16) {
        return ESP_FAIL;
    }
    info->fmt = AUDIO_FMT_WAV;
    info->sample_rate = hdr.sample_rate;
    info->channels = hdr.num_channels;
    info->bits_per_sample = hdr.bits_per_sample;
    *data_offset = ftell(f);
    *data_size = hdr.data_size;
    return ESP_OK;
}

static size_t convert_pcm_to_output(const int16_t *in, size_t frames_in, const audio_info_t *info, int16_t *out, float gain)
{
    if (info->sample_rate == AUDIO_SAMPLE_RATE && info->channels == AUDIO_CHANNELS) {
        size_t samples = frames_in * AUDIO_CHANNELS;
        for (size_t i = 0; i < samples; ++i) {
            int32_t v = (int32_t)((float)in[i] * gain);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[i] = (int16_t)v;
        }
        return frames_in;
    }

    float ratio = (float)info->sample_rate / (float)AUDIO_SAMPLE_RATE;
    size_t out_frames = (size_t)((float)frames_in / ratio);
    for (size_t o = 0; o < out_frames; ++o) {
        size_t src_idx = (size_t)(o * ratio);
        int32_t l = 0, r = 0;
        if (info->channels == 1) {
            int16_t s = in[src_idx];
            l = r = s;
        } else {
            l = in[src_idx * 2];
            r = in[src_idx * 2 + 1];
        }
        l = (int32_t)((float)l * gain);
        r = (int32_t)((float)r * gain);
        if (l > 32767) {
            l = 32767;
        } else if (l < -32768) {
            l = -32768;
        }
        if (r > 32767) {
            r = 32767;
        } else if (r < -32768) {
            r = -32768;
        }
        out[o * 2] = (int16_t)l;
        out[o * 2 + 1] = (int16_t)r;
    }
    return out_frames;
}

static bool decode_wav_to_ring(FILE *f, const audio_info_t *info, int volume_percent, uint32_t data_size, size_t initial_bytes_done)
{
    const size_t in_buf_frames = 512;
    int16_t *in_buf = heap_caps_malloc(in_buf_frames * info->channels * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *out_buf = heap_caps_malloc(in_buf_frames * AUDIO_CHANNELS * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!in_buf || !out_buf) {
        ESP_LOGE(TAG, "no mem for wav decode");
        if (in_buf) heap_caps_free(in_buf);
        if (out_buf) heap_caps_free(out_buf);
        error_monitor_report_audio_fault();
        return false;
    }
    size_t bytes_read = 0;
    size_t bytes_done = initial_bytes_done;
    const uint32_t byte_rate = info->sample_rate * info->channels * (info->bits_per_sample / 8);
    while ((bytes_read = fread(in_buf, 1, in_buf_frames * info->channels * sizeof(int16_t), f)) > 0) {
        if (s_stop_requested) {
            break;
        }
        while (s_paused && !s_stop_requested) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        int vol = s_volume;
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        float gain = vol / 100.0f;
        size_t frames = bytes_read / (info->channels * sizeof(int16_t));
        size_t out_frames = convert_pcm_to_output(in_buf, frames, info, out_buf, gain);
        size_t out_bytes = out_frames * AUDIO_CHANNELS * sizeof(int16_t);
        size_t written = 0;
        if (i2s_channel_write(s_tx_chan, out_buf, out_bytes, &written, portMAX_DELAY) != ESP_OK) {
            ESP_LOGW(TAG, "i2s write failed");
            break;
        }
        bytes_done += bytes_read;
        if (byte_rate > 0) {
            uint32_t pos_ms = (uint32_t)((bytes_done * 1000ULL) / byte_rate);
            uint32_t dur_ms = data_size > 0 ? (uint32_t)((data_size * 1000ULL) / byte_rate) : 0;
            status_update_progress(bytes_done, data_size, pos_ms, dur_ms);
        } else {
            status_update_progress(bytes_done, data_size, 0, 0);
        }
    }
    heap_caps_free(in_buf);
    heap_caps_free(out_buf);
    return true;
}

static void reader_task(void *param)
{
    audio_cmd_t cmd = *(audio_cmd_t *)param;
    free(param);
    s_reader_done = false;
    s_stop_requested = false;

    FILE *f = fopen(cmd.path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", cmd.path);
        status_set_message("Cannot open file");
        error_monitor_report_audio_fault();
        s_reader_done = true;
        vTaskDelete(NULL);
        return;
    }
    fseek(f, 0, SEEK_END);
#if AUDIO_PLAYER_DEBUG
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t head[32] = {0};
    fread(head, 1, sizeof(head), f);
    fseek(f, 0, SEEK_SET);
    ESP_LOGI(TAG, "file %s size=%ld head=%02X %02X %02X %02X %02X %02X %02X %02X",
             cmd.path, fsize,
             head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);
#else
    fseek(f, 0, SEEK_END);
    fseek(f, 0, SEEK_SET);
#endif
    uint8_t hdr[16] = {0};
    size_t hdr_len = fread(hdr, 1, sizeof(hdr), f);
    fseek(f, 0, SEEK_SET);
    audio_format_t fmt = detect_format(hdr, hdr_len);
    audio_info_t info = {0};
    info.fmt = fmt;
    bool is_seek = (cmd.seek_ratio >= 0.0f);
    if (!is_seek) {
        status_set_play(cmd.path, fmt);
    } else {
        status_lock();
        s_status.playing = true;
        s_status.paused = false;
        s_status.fmt = to_public_format(fmt);
        s_status.message[0] = 0;
        status_unlock();
    }

    if (fmt == AUDIO_FMT_WAV) {
        long data_off = 0;
        uint32_t data_size = 0;
        if (parse_wav_header(f, &info, &data_off, &data_size) == ESP_OK) {
            uint32_t byte_rate = info.sample_rate * info.channels * (info.bits_per_sample / 8);
            size_t skip_bytes = 0;
            if (cmd.seek_ratio >= 0.0f && cmd.seek_ratio <= 1.0f) {
                skip_bytes = (size_t)((float)data_size * cmd.seek_ratio);
                if (skip_bytes >= data_size) {
                    skip_bytes = data_size > 0 ? data_size - 1 : 0;
                }
            }
            fseek(f, data_off + (long)skip_bytes, SEEK_SET);
            if (skip_bytes > 0 && byte_rate > 0) {
                uint32_t est_ms = (uint32_t)((skip_bytes * 1000ULL) / byte_rate);
                status_update_progress(skip_bytes, data_size, est_ms, (uint32_t)((data_size * 1000ULL) / byte_rate));
            }
            decode_wav_to_ring(f, &info, cmd.volume >= 0 ? cmd.volume : s_volume, data_size, skip_bytes);
        } else {
            ESP_LOGE(TAG, "bad wav header");
            status_set_message("Bad WAV header");
            error_monitor_report_audio_fault();
        }
    } else if (fmt == AUDIO_FMT_MP3) {
        fclose(f);
        f = NULL;
        int vol = cmd.volume >= 0 ? cmd.volume : s_volume;
        s_last_bitrate_kbps = 0;
        helix_mp3_decode_file(cmd.path, vol, mp3_i2s_write_cb, s_tx_chan, mp3_progress_cb, &s_last_bitrate_kbps, cmd.seek_ratio);
    } else if (fmt == AUDIO_FMT_OGG) {
        ESP_LOGW(TAG, "OGG decode not implemented yet");
        status_set_message("OGG not supported");
        error_monitor_report_audio_fault();
    } else {
        ESP_LOGW(TAG, "unknown audio format");
        status_set_message("Unknown format");
        error_monitor_report_audio_fault();
    }

    if (f) {
        fclose(f);
    }
    // stop I2S to avoid repeating the last DMA buffer forever
    if (!s_stop_requested && s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        s_tx_enabled = false;
    }
    status_set_playing(false);
    s_reader_done = true;
    s_reader_task = NULL;
    vTaskDelete(NULL);
}

static void stop_reader(void)
{
    s_stop_requested = true;
    if (s_reader_task) {
        // wait a bit for reader to exit
        for (int i = 0; i < 50 && !s_reader_done; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!s_reader_done) {
            vTaskDelete(s_reader_task); // force kill if stuck
        }
        s_reader_task = NULL;
    }
    s_reader_done = true;
    status_set_playing(false);
}

static void audio_task(void *param)
{
    audio_cmd_t cmd;
    while (xQueueReceive(s_queue, &cmd, portMAX_DELAY) == pdTRUE) {
        int v = cmd.volume >= 0 ? cmd.volume : s_volume;
        s_current_volume = v;
#if AUDIO_PLAYER_DEBUG
        ESP_LOGI(TAG, "play request: %s vol=%d", cmd.path, v);
#endif
        if (!sd_card_available()) {
            ESP_LOGE(TAG, "SD not mounted, beep");
            status_set_playing(false);
            play_tone_ms(660, 150, v);
            play_tone_ms(440, 150, v);
            error_monitor_report_sd_fault();
            continue;
        }
        s_paused = false;
        status_set_paused(false);
        if (s_tx_chan && !s_tx_enabled) {
            esp_err_t en = i2s_channel_enable(s_tx_chan); // ensure channel is running
            if (en != ESP_OK && en != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "i2s enable failed: %s", esp_err_to_name(en));
            } else {
                s_tx_enabled = true;
            }
        }
        audio_cmd_t *pcmd = malloc(sizeof(audio_cmd_t));
        if (!pcmd) {
            ESP_LOGE(TAG, "no mem for cmd");
            error_monitor_report_audio_fault();
            continue;
        }
        *pcmd = cmd;
        stop_reader();
        BaseType_t ok = xTaskCreatePinnedToCore(reader_task, "audio_reader", 8192, pcmd, 6, &s_reader_task, 1);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "reader task create failed");
            free(pcmd);
            play_tone_ms(660, 150, v);
            play_tone_ms(440, 150, v);
            error_monitor_report_audio_fault();
            continue;
        }

    }
    vTaskDelete(NULL);
}

static void on_event(const event_bus_message_t *msg)
{
    if (!msg) {
        return;
    }
    switch (msg->type) {
    case EVENT_AUDIO_PLAY:
        audio_player_play(msg->payload);
        break;
    case EVENT_VOLUME_SET:
        ESP_LOGW(TAG, "ignoring volume cmd topic='%s' payload='%s'", msg->topic, msg->payload);
        break;
    case EVENT_WEB_COMMAND:
        break;
    case EVENT_NONE:
        break;
    case EVENT_CARD_OK:
    case EVENT_CARD_BAD:
    case EVENT_RELAY_CMD:
    case EVENT_SYSTEM_STATUS:
        break;
    default:
        break;
    }
}

void audio_player_stop(void)
{
    s_paused = false;
    s_last_bitrate_kbps = 0;
    status_set_playing(false);
    stop_reader();
    if (s_tx_chan) {
        // fully reset I2S channel to avoid hanging frames
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        s_tx_enabled = false;
        i2s_setup();
    }
}

esp_err_t audio_player_mount_sd(void)
{
    return sd_mount();
}

esp_err_t audio_player_init(void)
{
    if (!s_status_mutex) {
        s_status_mutex = xSemaphoreCreateMutex();
    }
    if (!s_sd_mutex) {
        s_sd_mutex = xSemaphoreCreateMutex();
        if (!s_sd_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    status_reset();
    esp_err_t sd_err = sd_mount();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "sd mount failed, audio will beep only: %s", esp_err_to_name(sd_err));
        sd_lock();
        s_card = NULL;
        sd_unlock();
    }
    // load volume from NVS
    nvs_handle_t h;
    if (nvs_open("audiocfg", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "vol", &v) == ESP_OK) {
            s_volume = v;
            s_current_volume = v;
            ESP_LOGI(TAG, "volume loaded from NVS: %d", v);
        }
        nvs_close(h);
    }
    s_volume_loaded = true;
    status_lock();
    s_status.volume = s_volume;
    status_unlock();
    i2s_setup();
    if (!s_queue) {
        s_queue = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(audio_cmd_t));
    }
    if (!s_save_task) {
        xTaskCreate(save_task, "audio_save", 2048, NULL, 4, &s_save_task);
    }
    ESP_ERROR_CHECK(event_bus_register_handler(on_event));
    return s_queue ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t audio_player_start(void)
{
    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_task) {
        BaseType_t ok = xTaskCreate(audio_task, "audio_task", 8192, NULL, 6, &s_task);
        if (ok != pdPASS) {
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "audio player started");
    return ESP_OK;
}

esp_err_t audio_player_play(const char *path)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    audio_cmd_t cmd = {0};
    strncpy(cmd.path, path, sizeof(cmd.path) - 1);
    cmd.path[sizeof(cmd.path) - 1] = 0;
    cmd.volume = -1; // use current volume
    cmd.seek_ratio = -1.0f;
    return xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_player_seek(uint32_t pos_ms)
{
    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    char current_path[256] = {0};
    float ratio = -1.0f;
    bool can_seek = false;
    status_lock();
    if ((s_status.playing || s_status.paused) && s_status.path[0] != 0) {
        can_seek = true;
        strncpy(current_path, s_status.path, sizeof(current_path) - 1);
        current_path[sizeof(current_path) - 1] = 0;
        if (s_status.dur_ms > 0) {
            ratio = (float)pos_ms / (float)s_status.dur_ms;
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
        }
        if (ratio >= 0.0f) {
            s_status.pos_ms = (int)pos_ms;
            s_status.progress = (s_status.dur_ms > 0) ? (int)((float)pos_ms * 100.0f / (float)s_status.dur_ms) : 0;
        }
    }
    status_unlock();
    if (!can_seek || ratio < 0.0f) {
        return ESP_ERR_INVALID_STATE;
    }
    audio_cmd_t cmd = {0};
    strncpy(cmd.path, current_path, sizeof(cmd.path) - 1);
    cmd.path[sizeof(cmd.path) - 1] = 0;
    cmd.volume = -1;
    cmd.seek_ratio = ratio;
    return xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_player_set_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    if (percent == s_volume) {
        return ESP_OK;
    }
    s_volume = percent;
    s_current_volume = percent;
    status_lock();
    s_status.volume = s_volume;
    status_unlock();
    if (s_volume_loaded) {
        s_pending_volume = s_volume;
    }
    return ESP_OK;
}

void audio_player_pause(void)
{
    if (s_paused) {
        return;
    }
    s_paused = true;
    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        s_tx_enabled = false;
    }
    status_set_paused(true);
    s_last_bitrate_kbps = 0;
}

void audio_player_resume(void)
{
    if (!s_paused) {
        return;
    }
    s_paused = false;
    if (s_tx_chan) {
        if (!s_tx_enabled) {
            esp_err_t err = i2s_channel_enable(s_tx_chan);
            if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
                s_tx_enabled = true;
            } else {
                ESP_LOGW(TAG, "i2s resume enable failed: %s", esp_err_to_name(err));
            }
        }
    }
    status_set_paused(false);
}

int audio_player_get_volume(void)
{
    return s_volume;
}

void audio_player_get_status(audio_player_status_t *out)
{
    if (!out) {
        return;
    }
    status_lock();
    *out = s_status;
    status_unlock();
}
