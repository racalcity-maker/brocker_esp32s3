#include "status_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "led_strip.h"

#define STATUS_LED_GPIO 48
#define STATUS_LED_BRIGHTNESS 51  // ~20% duty (0-255 scale) for softer glow

static const char *TAG = "status_led";
static led_strip_handle_t s_strip = NULL;
static TaskHandle_t s_task = NULL;
static status_led_pattern_t s_pattern = STATUS_LED_PATTERN_OFF;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void apply_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) {
        return;
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void set_off(void)
{
    apply_rgb(0, 0, 0);
}

static status_led_pattern_t pattern_get(void)
{
    status_led_pattern_t p;
    taskENTER_CRITICAL(&s_lock);
    p = s_pattern;
    taskEXIT_CRITICAL(&s_lock);
    return p;
}

static void render_pattern(status_led_pattern_t pattern, bool on)
{
    switch (pattern) {
    case STATUS_LED_PATTERN_SOLID_RED:
    case STATUS_LED_PATTERN_BLINK_RED:
        if (on) {
            apply_rgb(STATUS_LED_BRIGHTNESS, 0, 0);
        } else {
            set_off();
        }
        break;
    case STATUS_LED_PATTERN_SOLID_GREEN:
        if (on) {
            apply_rgb(0, STATUS_LED_BRIGHTNESS, 0);
        } else {
            set_off();
        }
        break;
    case STATUS_LED_PATTERN_OFF:
    default:
        set_off();
        break;
    }
}

static void status_led_task(void *arg)
{
    (void)arg;
    bool blink_on = true;
    TickType_t last_toggle = xTaskGetTickCount();
    const TickType_t blink_period = pdMS_TO_TICKS(1000); // 0.5 Hz (toggle every 1s)
    status_led_pattern_t prev = STATUS_LED_PATTERN_OFF;
    status_led_pattern_t last_pattern = STATUS_LED_PATTERN_OFF;
    bool last_on = false;
    while (1) {
        status_led_pattern_t pattern = pattern_get();
        if (pattern != prev) {
            prev = pattern;
            blink_on = true;
            last_toggle = xTaskGetTickCount();
        }
        if (pattern == STATUS_LED_PATTERN_BLINK_RED) {
            TickType_t now = xTaskGetTickCount();
            if (now - last_toggle >= blink_period) {
                blink_on = !blink_on;
                last_toggle = now;
            }
            if (pattern != last_pattern || blink_on != last_on) {
                render_pattern(pattern, blink_on);
                last_pattern = pattern;
                last_on = blink_on;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            blink_on = true;
            if (pattern != last_pattern || !last_on) {
                render_pattern(pattern, true);
                last_pattern = pattern;
                last_on = true;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

esp_err_t status_led_init(void)
{
    if (s_strip) {
        return ESP_OK;
    }
    led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
#ifdef LED_STRIP_COLOR_COMPONENT_FMT_GRB
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
#endif
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), TAG, "create strip");
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
    if (!s_task) {
        BaseType_t ok = xTaskCreate(status_led_task, "status_led", 2048, NULL, 3, &s_task);
        if (ok != pdPASS) {
            led_strip_del(s_strip);
            s_strip = NULL;
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

void status_led_set_pattern(status_led_pattern_t pattern)
{
    taskENTER_CRITICAL(&s_lock);
    s_pattern = pattern;
    taskEXIT_CRITICAL(&s_lock);
}
