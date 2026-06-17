/**
 * main.c — Focus Pager firmware entry point (Phase 1: ANCS read path)
 *
 * Initialises NVS, Bluetooth (Classic disabled, BLE enabled via Bluedroid),
 * wires the ANCS client, and starts advertising so an iPhone will bond and
 * forward notifications.
 *
 * Parsed notifications are printed over UART (idf_component_log / ESP_LOG).
 * Phase 2 will add the Brick Control GATT service; Phase 4 adds HFP.
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
#include "ui.h"

#define TAG        "MAIN"
#define LED_GPIO   GPIO_NUM_2   /* onboard LED on most ESP32-WROOM-32 dev boards */

/* ── LED blink task ─────────────────────────────────────────────────────── *
 * Blinks the onboard LED so we can confirm the firmware is actually running.
 * Fast blink (200 ms) = advertising. Will change pattern in later phases.
 */
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

/* ── Notification callback ───────────────────────────────────────────────── *
 * Called from the BT task for every fully-parsed ANCS notification.
 * Phase 1: just log it. Future phases will drive the OLED and brick state.
 */
static void on_notification(const ancs_notification_t *n)
{
    ESP_LOGI(TAG, "[NOTIF] cat=%-14s title='%s' msg='%s' app='%s'",
             ancs_category_name(n->category),
             n->title, n->message, n->app_id);
    ui_show_notification(ancs_category_name(n->category), n->title, n->message);
}

void app_main(void)
{
    /* ── NVS (required by BT stack) ─────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── Bluetooth controller ────────────────────────────────────────────── *
     * Phase 1 is BLE-only. Classic BT is released to save ~30 KB of IRAM.
     * Phase 4 (HFP) changes this to ESP_BT_MODE_BTDM.
     */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    /* ── Bluedroid host stack ────────────────────────────────────────────── */
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_LOGI(TAG, "Bluetooth stack up — initialising ANCS client");

    /* ── ANCS client ─────────────────────────────────────────────────────── */
    ancs_client_init(on_notification);
    ancs_client_start_advertising();

    ESP_LOGI(TAG, "Focus Pager Phase 1 running — open iPhone Bluetooth settings");
    ESP_LOGI(TAG, "and pair with 'FocusPager'. Send yourself a notification.");

    /* Blink onboard LED to confirm firmware is running */
    xTaskCreate(led_task, "led", 1024, NULL, 1, NULL);

    /* Display init + wiring test: cycle red → green → blue → status screen */
    ui_init();
    ESP_LOGI(TAG, "Display test: RED");
    ui_fill(COLOR_RED);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Display test: GREEN");
    ui_fill(COLOR_GREEN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Display test: BLUE");
    ui_fill(COLOR_BLUE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ui_show_status("Advertising...");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
