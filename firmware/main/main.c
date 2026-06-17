/**
 * main.c — Focus Pager firmware entry point (Phase 2: Brick Control)
 *
 * Phase 1: ANCS read path — notifications display on ST7789
 * Phase 2: Brick Control GATT service + button hold unbrick event
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "driver/gpio.h"

#include "ancs_client.h"
#include "brick_service.h"
#include "pager_state.h"
#include "ui.h"

#define TAG      "MAIN"
#define LED_GPIO GPIO_NUM_2

/* ── LED blink task ──────────────────────────────────────────────────────── */
static void led_task(void *arg)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    while (1) {
        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ── ANCS notification callback ──────────────────────────────────────────── */
static void on_notification(const ancs_notification_t *n)
{
    ESP_LOGI(TAG, "[NOTIF] cat=%-14s title='%s' msg='%s'",
             ancs_category_name(n->category), n->title, n->message);

    /* Only show notifications when unbricked (Phase 3 will enforce this) */
    ui_show_notification(ancs_category_name(n->category), n->title, n->message);
}

/* ── Button hold callback ────────────────────────────────────────────────── *
 * Fired by pager_state when the button is held ≥2s.
 * Increments UnbrickEvent and notifies the iOS app.
 * The app is responsible for verifying auth and lowering the shield.
 */
static void on_unbrick_event(void)
{
    ESP_LOGI(TAG, "Unbrick event fired");
    brick_service_notify_unbrick();

    /* If we're bricked, the iOS app will send Command=0 after verifying auth.
     * Optimistically update local state now — app will correct if auth fails. */
    if (pager_state_get() == PAGER_BRICKED) {
        pager_state_set(PAGER_UNBRICKED);
        brick_service_notify_state(PAGER_UNBRICKED);
    }
}

/* ── app_main ────────────────────────────────────────────────────────────── */
void app_main(void)
{
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Display — up first so user sees boot progress */
    ui_init();
    ui_show_status("Booting...");
    xTaskCreate(led_task, "led", 1024, NULL, 1, NULL);

    /* BT controller — BLE only (Phase 4 switches to BTDM for HFP) */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    /* Bluedroid */
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* Brick Control GATT server (must register before ANCS client) */
    brick_service_init();

    /* ANCS GATT client */
    ancs_client_init(on_notification);
    ancs_client_start_advertising();

    /* Button + state machine */
    pager_state_init(on_unbrick_event);

    ESP_LOGI(TAG, "Focus Pager Phase 2 running");
    ui_show_status("Advertising...");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
