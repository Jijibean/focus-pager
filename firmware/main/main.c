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

#include "ancs_client.h"

#define TAG "MAIN"

/* ── Notification callback ───────────────────────────────────────────────── *
 * Called from the BT task for every fully-parsed ANCS notification.
 * Phase 1: just log it. Future phases will drive the OLED and brick state.
 */
static void on_notification(const ancs_notification_t *n)
{
    ESP_LOGI(TAG, "[NOTIF] cat=%-14s title='%s' msg='%s' app='%s'",
             ancs_category_name(n->category),
             n->title, n->message, n->app_id);
    /* TODO Phase 1+: render on OLED */
    /* TODO Phase 3+: check if bricked and apply filter */
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

    /* Everything else is event-driven inside the BT task. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
