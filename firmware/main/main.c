/**
 * main.c — Focus Pager firmware entry point (Phase 4: HFP call audio)
 *
 * Phase 1: ANCS read path — notifications display on ST7789
 * Phase 2: Brick Control GATT service + button hold unbrick event
 * Phase 4: Classic BT HFP — answer/hang up calls through pager speaker/mic
 *          Button: short press = answer/hang up, long press (≥2s) = unbrick
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
#include "hfp_client.h"
#include "pager_state.h"
#include "ui.h"

#define TAG      "MAIN"
#define LED_GPIO GPIO_NUM_13   /* moved off GPIO2; GPIO2 is now LCD RST */

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

    /* Incoming call: ANCS title = contact name (saved in Contacts).
     * Override whatever phone number HFP CLIP delivered earlier. */
    if (n->category == ANCS_CAT_INCOMING_CALL) {
        const char *name = n->title[0] ? n->title :
                           (n->message[0] ? n->message : NULL);
        ui_show_incoming_call(name);
        return;
    }

    ui_show_notification(ancs_category_name(n->category), n->title, n->message);
}

/* ── Button callbacks ────────────────────────────────────────────────────── */

/* Short press: answer incoming call or hang up active call */
static void on_short_press(void)
{
    ESP_LOGI(TAG, "Short press → hfp_client_button_press");
    hfp_client_button_press();
}

/* Long press (≥2s): fire unbrick event */
static void on_unbrick_event(void)
{
    ESP_LOGI(TAG, "Unbrick event fired");
    brick_service_notify_unbrick();

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
    ui_set_status_ble("Boot");
    xTaskCreate(led_task, "led", 1024, NULL, 1, NULL);

    /* BT controller — BTDM mode: BLE (ANCS + Brick) + Classic BT (HFP) */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));

    /* Bluedroid */
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* Brick Control GATT server (must register before ANCS client) */
    brick_service_init();

    /* ANCS GATT client + BLE advertising */
    ancs_client_init(on_notification);
    ancs_client_start_advertising();

    /* HFP Hands-Free + I2S audio */
    hfp_client_init();

    /* Button + brick state machine */
    pager_state_init(on_unbrick_event);
    pager_state_set_shortpress_cb(on_short_press);

    ESP_LOGI(TAG, "Focus Pager Phase 4 running");
    ui_set_status_ble("Adv");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
