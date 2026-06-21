/**
 * encoder.c — Rotary encoder driver (CLK=GPIO32, DT=GPIO33, SW=GPIO34)
 *
 * CLK ISR fires on any edge; direction is read from DT on the rising edge.
 * A 10 ms task consumes the accumulated delta and polls the SW button.
 */

#include "encoder.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"

#define TAG      "ENC"
#define ENC_CLK  GPIO_NUM_32
#define ENC_DT   GPIO_NUM_33
#define ENC_SW   GPIO_NUM_34

#define POLL_MS     10   /* task poll interval — 100 Hz */
#define SW_DEBOUNCE 30   /* min press duration to register a click (ms) */

static encoder_rotate_cb_t s_rotate_cb = NULL;
static encoder_click_cb_t  s_click_cb  = NULL;

/* Written in ISR, read in task — volatile is sufficient (Xtensa int R/W atomic) */
static volatile int     s_delta    = 0;
static volatile uint8_t s_last_clk = 0;

/* ISR: trigger on any CLK edge, sample DT on the rising edge for direction */
static void IRAM_ATTR clk_isr(void *arg)
{
    uint8_t clk = (uint8_t)gpio_get_level(ENC_CLK);
    if (clk && !s_last_clk) {               /* rising edge */
        if (gpio_get_level(ENC_DT) == 0)
            s_delta++;                       /* CW  */
        else
            s_delta--;                       /* CCW */
    }
    s_last_clk = clk;
}

static void encoder_task(void *arg)
{
    uint32_t sw_held  = 0;
    bool     sw_fired = false;

    while (1) {
        /* Consume rotation — fire once per poll cycle with net direction */
        int d = s_delta;
        if (d != 0) {
            s_delta = 0;
            if (s_rotate_cb)
                s_rotate_cb(d > 0 ? 1 : -1);
        }

        /* SW button — active-low, external 10kΩ pull-up on GPIO34 */
        bool pressed = (gpio_get_level(ENC_SW) == 0);
        if (pressed) {
            sw_held += POLL_MS;
            if (sw_held >= SW_DEBOUNCE && !sw_fired) {
                sw_fired = true;
                if (s_click_cb) s_click_cb();
            }
        } else {
            sw_held  = 0;
            sw_fired = false;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void encoder_init(encoder_rotate_cb_t on_rotate, encoder_click_cb_t on_click)
{
    s_rotate_cb = on_rotate;
    s_click_cb  = on_click;

    /* CLK + DT: input with internal pull-up, interrupt on any edge */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << ENC_CLK) | (1ULL << ENC_DT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    /* SW: GPIO34 is input-only — no internal pull-up available */
    gpio_config_t sw_cfg = {
        .pin_bit_mask = (1ULL << ENC_SW),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sw_cfg));

    s_last_clk = (uint8_t)gpio_get_level(ENC_CLK);

    /* Install ISR service — ignore if already installed by another driver */
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(gpio_isr_handler_add(ENC_CLK, clk_isr, NULL));

    xTaskCreate(encoder_task, "encoder", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "init: CLK=GPIO%d  DT=GPIO%d  SW=GPIO%d",
             ENC_CLK, ENC_DT, ENC_SW);
}
