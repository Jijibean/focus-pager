/**
 * pager_state.c — Brick state machine + button task
 *
 * Button on GPIO27, active-low with internal pullup.
 * Hold ≥ HOLD_MS fires the unbrick callback.
 * BrickState is persisted in NVS under key "brick_state".
 */

#include "pager_state.h"
#include "ui.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#define TAG         "STATE"
#define BUTTON_GPIO GPIO_NUM_27
#define HOLD_MS     2000          /* hold duration to trigger unbrick */
#define POLL_MS     50            /* button poll interval */
#define NVS_NS      "pager"       /* NVS namespace */
#define NVS_KEY     "brick_state"

static unbrick_event_cb_t s_unbrick_cb = NULL;
static pager_state_t      s_state      = PAGER_UNBRICKED;

/* ── NVS helpers ─────────────────────────────────────────────────────────── */

static void nvs_save_state(pager_state_t state)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY, (uint8_t)state);
        nvs_commit(h);
        nvs_close(h);
    }
}

static pager_state_t nvs_load_state(void)
{
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY, &val);
        nvs_close(h);
    }
    return (val == 1) ? PAGER_BRICKED : PAGER_UNBRICKED;
}

/* ── Display helper ──────────────────────────────────────────────────────── */

static void update_display(pager_state_t state)
{
    if (state == PAGER_BRICKED) {
        ui_show_status("BRICKED\nHold button\nto unbrick");
    } else {
        ui_show_status("Ready - send a text!");
    }
}

/* ── Button task ─────────────────────────────────────────────────────────── */

static void button_task(void *arg)
{
    uint32_t held_ms = 0;
    bool     was_pressed = false;

    while (1) {
        bool pressed = (gpio_get_level(BUTTON_GPIO) == 0); /* active-low */

        if (pressed) {
            held_ms += POLL_MS;
            if (!was_pressed) {
                ESP_LOGI(TAG, "Button down");
            }
            if (held_ms >= HOLD_MS && !was_pressed) {
                /* Held long enough — fire the event exactly once per hold */
                ESP_LOGI(TAG, "Button held %lums — firing unbrick event",
                         (unsigned long)held_ms);
                was_pressed = true; /* prevent repeat until released */
                if (s_unbrick_cb) s_unbrick_cb();
            }
        } else {
            if (was_pressed || held_ms > 0) {
                ESP_LOGI(TAG, "Button released after %lums", (unsigned long)held_ms);
            }
            held_ms     = 0;
            was_pressed = false;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void pager_state_init(unbrick_event_cb_t cb)
{
    s_unbrick_cb = cb;

    /* Load persisted state */
    s_state = nvs_load_state();
    ESP_LOGI(TAG, "Loaded state: %s",
             s_state == PAGER_BRICKED ? "BRICKED" : "UNBRICKED");

    /* Configure button GPIO */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    /* Start button polling task */
    xTaskCreate(button_task, "button", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Pager state init complete");
}

void pager_state_set(pager_state_t state)
{
    s_state = state;
    nvs_save_state(state);
    update_display(state);
    ESP_LOGI(TAG, "State → %s", state == PAGER_BRICKED ? "BRICKED" : "UNBRICKED");
}

pager_state_t pager_state_get(void)
{
    return s_state;
}
