/**
 * hfp_client.c — HFP Hands-Free client + I2S audio routing
 *
 * Classic BT flow:
 *   1. ESP32 advertises as discoverable Classic BT device "FocusPager"
 *   2. User pairs iPhone with ESP32 via iOS Bluetooth settings (once)
 *   3. On AUTH_CMPL, ESP32 saves peer BDA to NVS and initiates HFP connection
 *   4. On subsequent boots, saved BDA is loaded and connect is attempted
 *   5. When a call arrives, iPhone notifies via HFP; ESP32 shows it on display
 *   6. Short button press → answer / hang up
 *
 * Audio path (HCI/software):
 *   incoming_cb  (phone → speaker): write PCM bytes to I2S TX (MAX98357A)
 *   outgoing_cb  (mic → phone)    : read PCM bytes from I2S RX (INMP441)
 *   Callbacks are registered when the audio SCO channel opens and
 *   unregistered when it closes.
 */

#include "hfp_client.h"
#include "ui.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"
#include "esp_hf_client_legacy_api.h"   /* legacy data callbacks (HCI path) */
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "HFP"

/* ── I2S pin definitions ─────────────────────────────────────────────────── */
#define I2S_BCLK   GPIO_NUM_26   /* shared BCLK: INMP441 SCK + MAX98357A BCLK */
#define I2S_WS     GPIO_NUM_25   /* shared WS:   INMP441 WS  + MAX98357A LRC  */
#define I2S_DOUT   GPIO_NUM_22   /* ESP32 → MAX98357A DIN (speaker)            */
#define I2S_DIN    GPIO_NUM_19   /* INMP441 SD → ESP32 (microphone)            */

/* HFP PCM: CVSD codec = 8 kHz, 16-bit mono */
#define HFP_SAMPLE_RATE 8000

/* ── NVS: persist peer BDA across reboots ───────────────────────────────── */
#define NVS_NS_HFP  "hfp"
#define NVS_KEY_BDA "peer_bda"

/* ── Module state ────────────────────────────────────────────────────────── */
typedef enum {
    HFP_DISCONNECTED,
    HFP_CONNECTED,
    HFP_CALL_INCOMING,
    HFP_CALL_ACTIVE,
} hfp_state_t;

static hfp_state_t   s_state    = HFP_DISCONNECTED;
static esp_bd_addr_t s_peer_bda = {0};
static bool          s_has_peer = false;

static i2s_chan_handle_t s_tx_chan     = NULL;
static i2s_chan_handle_t s_rx_chan     = NULL;
static bool              s_i2s_running = false;

/* ── NVS helpers ─────────────────────────────────────────────────────────── */

static void nvs_save_bda(const esp_bd_addr_t bda)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_HFP, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_BDA, bda, sizeof(esp_bd_addr_t));
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool nvs_load_bda(esp_bd_addr_t bda)
{
    nvs_handle_t h;
    size_t len = sizeof(esp_bd_addr_t);
    if (nvs_open(NVS_NS_HFP, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_blob(h, NVS_KEY_BDA, bda, &len);
        nvs_close(h);
        return (err == ESP_OK && len == sizeof(esp_bd_addr_t));
    }
    return false;
}

/* ── I2S ─────────────────────────────────────────────────────────────────── */

static void i2s_audio_init(void)
{
    /* Single I2S bus for both TX (speaker) and RX (mic).
     * slot_bit_width=32 satisfies INMP441's 64-BCLK-per-WS-cycle requirement
     * while data_bit_width=16 matches HFP's PCM format directly. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                            I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(HFP_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk         = I2S_GPIO_UNUSED,
            .bclk         = I2S_BCLK,
            .ws           = I2S_WS,
            .dout         = I2S_DOUT,
            .din          = I2S_DIN,
            .invert_flags = {0},
        },
    };
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.slot_mask      = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));
    ESP_LOGI(TAG, "I2S init (8kHz, 16-bit data, 32-bit slot, mono left)");
}

static void i2s_audio_start(void)
{
    if (s_i2s_running) return;
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
    s_i2s_running = true;
    ESP_LOGI(TAG, "I2S audio started");
}

static void i2s_audio_stop(void)
{
    if (!s_i2s_running) return;
    i2s_channel_disable(s_tx_chan);
    i2s_channel_disable(s_rx_chan);
    s_i2s_running = false;
    ESP_LOGI(TAG, "I2S audio stopped");
}

/* ── HFP audio data callbacks (legacy HCI path) ──────────────────────────── *
 * Both run in the BT task — keep them fast and non-blocking.
 */

/* Phone → speaker: write PCM to I2S TX (MAX98357A amp) */
static void hfp_incoming_cb(const uint8_t *buf, uint32_t len)
{
    if (s_i2s_running) {
        size_t written = 0;
        i2s_channel_write(s_tx_chan, buf, len, &written, 0); /* non-blocking */
    }
    /* Trigger the BT stack to pull the next mic frame */
    esp_hf_client_outgoing_data_ready();
}

/* Mic → phone: read PCM from I2S RX (INMP441) and return byte count.
 * Use zero timeout — if the mic has no data ready, send silence rather than
 * blocking the BT task (which causes SCO transmit queue overflow). */
static uint32_t hfp_outgoing_cb(uint8_t *buf, uint32_t len)
{
    if (!s_i2s_running) {
        memset(buf, 0, len);
        return len;
    }
    size_t bytes_read = 0;
    i2s_channel_read(s_rx_chan, buf, len, &bytes_read, 0); /* non-blocking */
    if (bytes_read < len) {
        memset(buf + bytes_read, 0, len - bytes_read);
    }
    return len;
}

/* ── HFP event handler ───────────────────────────────────────────────────── */

static void hfp_client_cb(esp_hf_client_cb_event_t event,
                           esp_hf_client_cb_param_t *param)
{
    ESP_LOGD(TAG, "HFP event: %d", (int)event);
    switch (event) {

    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        switch (param->conn_stat.state) {
        case ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED:
        case ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED:
            s_state = HFP_CONNECTED;
            memcpy(s_peer_bda, param->conn_stat.remote_bda,
                   sizeof(esp_bd_addr_t));
            ESP_LOGI(TAG, "HFP SLC connected");
            ui_show_status("Ready - send a text!");
            break;
        case ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED:
            s_state = HFP_DISCONNECTED;
            i2s_audio_stop();
            ESP_LOGI(TAG, "HFP disconnected");
            break;
        default:
            break;
        }
        break;

    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ||
            param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            /* Register data callbacks and open I2S when the SCO channel opens */
            esp_hf_client_register_data_callback(hfp_incoming_cb,
                                                  hfp_outgoing_cb);
            i2s_audio_start();
            ESP_LOGI(TAG, "Audio SCO open (frame_size=%d)",
                     param->audio_stat.preferred_frame_size);
        } else if (param->audio_stat.state ==
                   ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            i2s_audio_stop();
            ESP_LOGI(TAG, "Audio SCO closed");
        }
        break;

    case ESP_HF_CLIENT_RING_IND_EVT:
        ESP_LOGI(TAG, "Incoming call — ringing");
        s_state = HFP_CALL_INCOMING;
        ui_show_incoming_call("");   /* caller ID arrives via CLIP */
        break;

    case ESP_HF_CLIENT_CLIP_EVT:
        if (param->clip.number) {
            ESP_LOGI(TAG, "Caller ID: %s", param->clip.number);
            ui_show_incoming_call(param->clip.number);
        }
        break;

    case ESP_HF_CLIENT_CIND_CALL_EVT:
        ESP_LOGI(TAG, "CIND_CALL status=%d", (int)param->call.status);
        if (param->call.status == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS) {
            s_state = HFP_CALL_ACTIVE;
            ui_show_call_active();
        } else if (param->call.status == ESP_HF_CALL_STATUS_NO_CALLS) {
            if (s_state != HFP_DISCONNECTED) {
                s_state = HFP_CONNECTED;
                ui_show_status("Ready - send a text!");
            }
        }
        break;

    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
        ESP_LOGI(TAG, "CIND_CALL_SETUP status=%d", (int)param->call_setup.status);
        /* Incoming call setup — treat as incoming if not already set */
        if (param->call_setup.status == ESP_HF_CALL_SETUP_STATUS_INCOMING &&
            s_state == HFP_CONNECTED) {
            s_state = HFP_CALL_INCOMING;
            ui_show_incoming_call("");
        } else if (param->call_setup.status == ESP_HF_CALL_SETUP_STATUS_IDLE &&
                   s_state == HFP_CALL_INCOMING) {
            /* Call was rejected/missed */
            s_state = HFP_CONNECTED;
            ui_show_status("Ready - send a text!");
        }
        break;

    default:
        break;
    }
}

/* ── Forward declaration ─────────────────────────────────────────────────── */
static void reconnect_task(void *arg);

/* ── Classic BT GAP handler ──────────────────────────────────────────────── */

static void gap_bt_cb(esp_bt_gap_cb_event_t event,
                      esp_bt_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Classic BT paired: %s",
                     param->auth_cmpl.device_name);
            memcpy(s_peer_bda, param->auth_cmpl.bda, sizeof(esp_bd_addr_t));
            s_has_peer = true;
            nvs_save_bda(s_peer_bda);
            /* iOS needs a moment after pairing before it will accept the HFP
             * RFCOMM connection — connect via a delayed task instead of here */
            xTaskCreate(reconnect_task, "hfp_post_pair", 2048, NULL, 3, NULL);
        } else {
            ESP_LOGE(TAG, "Pairing failed: %d", param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        /* SSP numeric comparison — auto-confirm (Just Works, no MITM) */
        ESP_LOGI(TAG, "SSP confirm: %06" PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey: %06" PRIu32, param->key_notif.passkey);
        break;

    default:
        break;
    }
}

/* ── Reconnect task ──────────────────────────────────────────────────────── */

static void reconnect_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000));
    if (s_has_peer) {
        ESP_LOGI(TAG, "Connecting HFP to peer");
        esp_hf_client_connect(s_peer_bda);
    }
    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void hfp_client_init(void)
{
    i2s_audio_init();

    /* Classic BT GAP — name, Just Works SSP, discoverable */
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_bt_cb));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name("FocusPager"));

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    /* HFP HF profile */
    ESP_ERROR_CHECK(esp_hf_client_register_callback(hfp_client_cb));
    ESP_ERROR_CHECK(esp_hf_client_init());
    /* Audio data callbacks are registered dynamically in AUDIO_STATE_EVT */

    /* Reconnect to a previously paired phone after Bluedroid settles */
    s_has_peer = nvs_load_bda(s_peer_bda);
    if (s_has_peer) {
        ESP_LOGI(TAG, "Saved peer BDA found — reconnecting in 3s");
        xTaskCreate(reconnect_task, "hfp_reconnect", 2048, NULL, 3, NULL);
    } else {
        ESP_LOGI(TAG, "No saved peer — pair via iPhone Bluetooth settings");
    }

    ESP_LOGI(TAG, "HFP client init complete");
}

void hfp_client_button_press(void)
{
    static const char *state_names[] = {
        "DISCONNECTED", "CONNECTED", "CALL_INCOMING", "CALL_ACTIVE"
    };
    ESP_LOGI(TAG, "Button press — HFP state: %s",
             state_names[(int)s_state]);

    switch (s_state) {
    case HFP_CALL_INCOMING:
        ESP_LOGI(TAG, "Answering call");
        esp_hf_client_answer_call();
        s_state = HFP_CALL_ACTIVE;
        ui_show_call_active();
        break;
    case HFP_CALL_ACTIVE:
        ESP_LOGI(TAG, "Hanging up");
        esp_hf_client_reject_call();
        s_state = HFP_CONNECTED;
        ui_show_status("Ready - send a text!");
        break;
    case HFP_DISCONNECTED:
        ESP_LOGW(TAG, "Button pressed but HFP not connected — is iPhone paired?");
        break;
    case HFP_CONNECTED:
        ESP_LOGW(TAG, "Button pressed but no call in progress");
        break;
    }
}
