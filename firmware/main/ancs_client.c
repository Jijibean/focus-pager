/**
 * ancs_client.c — ANCS GATT client for ESP32 (ESP-IDF, Bluedroid)
 *
 * Flow:
 *   advertise with ANCS solicitation → iOS connects + bonds →
 *   discover ANCS service → enable Notification Source notify →
 *   on each UID: write GetNotifAttributes to Control Point →
 *   reassemble Data Source packets → parse Title/Message → fire callback
 */

#include "ancs_client.h"
#include "ui.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_defs.h"
#include "esp_timer.h"

#define TAG "ANCS"

/* ── ANCS 128-bit UUIDs (little-endian byte arrays for ESP-IDF) ──────────
 *
 *  Service:            7905F431-B5CE-4E99-A40F-4B1E122D00D0
 *  Notification Source:9FBF120D-6301-42D9-8C58-25E699A21DBD
 *  Control Point:      69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9
 *  Data Source:        22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB
 */
static const uint8_t ANCS_SVC_UUID[16] = {
    0xD0,0x00,0x2D,0x12,0x1E,0x4B,0x0F,0xA4,
    0x99,0x4E,0xCE,0xB5,0x31,0xF4,0x05,0x79
};
static const uint8_t ANCS_NOTIF_SRC_UUID[16] = {
    0xBD,0x1D,0xA2,0x99,0xE6,0x25,0x58,0x8C,
    0xD9,0x42,0x01,0x63,0x0D,0x12,0xBF,0x9F
};
static const uint8_t ANCS_CTRL_PT_UUID[16] = {
    0xD9,0xD9,0xAA,0xFD,0xBD,0x9B,0x21,0x98,
    0xA8,0x49,0xE1,0x45,0xF3,0xD8,0xD1,0x69
};
static const uint8_t ANCS_DATA_SRC_UUID[16] = {
    0xFB,0x7B,0x7C,0xCE,0x6A,0xB3,0x44,0xBE,
    0xB5,0x4B,0xD6,0x24,0xE9,0xC6,0xEA,0x22
};

/* ── ANCS command + attribute IDs ───────────────────────────────────────── */
#define ANCS_CMD_GET_NOTIF_ATTR   0x00
#define ANCS_ATTR_APP_ID          0x00
#define ANCS_ATTR_TITLE           0x01
#define ANCS_ATTR_SUBTITLE        0x02
#define ANCS_ATTR_MESSAGE         0x03
#define ANCS_MAX_ATTR_LEN         127   /* max bytes to request per attribute */

/* ── GATT client app ID ─────────────────────────────────────────────────── */
#define ANCS_GATTC_APP_ID         0

/* ── BLE advertising payload (raw) ──────────────────────────────────────────
 *
 *  iOS requires AD type 0x15 (128-bit Service Solicitation UUID) to recognise
 *  an ANCS accessory. The esp_ble_adv_data_t helper only emits AD type 0x07
 *  (Service UUID), which iOS ignores for ANCS. We must use raw adv data.
 *
 *  Adv packet  (21 bytes):
 *    [02 01 06]           Flags: LE General Discoverable, no BR/EDR
 *    [11 15 <16-byte UUID>] 128-bit Service Solicitation UUID (ANCS)
 *
 *  Scan response (12 bytes):
 *    [0B 09 FocusPager]   Complete Local Name
 */
static uint8_t raw_adv_data[] = {
    0x02, 0x01, 0x06,           /* Flags */
    0x11, 0x15,                 /* Length=17, AD type=0x15 (Solicitation UUID) */
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,  /* ANCS UUID, LE */
    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79,
};

static uint8_t raw_scan_rsp_data[] = {
    0x0B, 0x09,                 /* Length=11, AD type=0x09 (Complete Local Name) */
    'F','o','c','u','s','P','a','g','e','r',
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min       = 0x20,
    .adv_int_max       = 0x40,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* ── Module state ────────────────────────────────────────────────────────── */
static ancs_notification_cb_t s_user_cb = NULL;

/* Connection + handle cache */
static uint16_t s_conn_id      = 0xFFFF;
static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;

static uint16_t s_notif_src_handle  = ESP_GATT_ILLEGAL_HANDLE;
static uint16_t s_ctrl_pt_handle    = ESP_GATT_ILLEGAL_HANDLE;
static uint16_t s_data_src_handle   = ESP_GATT_ILLEGAL_HANDLE;

static uint16_t s_cts_start_handle = ESP_GATT_ILLEGAL_HANDLE;
static uint16_t s_cts_end_handle   = ESP_GATT_ILLEGAL_HANDLE;
static uint16_t s_cts_char_handle  = ESP_GATT_ILLEGAL_HANDLE;

/* Which step of discovery we're on */
typedef enum {
    DISC_IDLE,
    DISC_NOTIF_SRC_CCCD,   /* writing CCCD for Notification Source */
    DISC_DATA_SRC_CCCD,    /* writing CCCD for Data Source */
    DISC_DONE,
} disc_state_t;
static disc_state_t s_disc_state = DISC_IDLE;

/* Data Source reassembly buffer */
#define DATA_SRC_BUF_SIZE 512
static uint8_t  s_ds_buf[DATA_SRC_BUF_SIZE];
static uint16_t s_ds_len = 0;

/* Pending notification waiting for attribute fetch */
static ancs_notification_t s_pending = {0};
static bool s_fetch_pending = false;

/* Remote device BDA stored at connect time (used in subsequent GATT calls) */
static esp_bd_addr_t s_remote_bda = {0};

/* Timer used to fire service discovery outside the BT callback context */
static esp_timer_handle_t s_discovery_timer = NULL;

/* Count of services seen during discovery (shown on screen for debugging) */
static int s_svc_count = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Called by timer — safe to call GATTC from here */
static void discovery_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "Starting service discovery (conn=%d if=%d)", s_conn_id, s_gattc_if);
    s_svc_count = 0;
    esp_err_t err = esp_ble_gattc_search_service(s_gattc_if, s_conn_id, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "search_service failed: %s", esp_err_to_name(err));
        ui_set_status_ble("Err");
        esp_ble_gap_disconnect(s_remote_bda);
    }
}

static void start_discovery_delayed(void)
{
    if (s_discovery_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback = discovery_timer_cb,
            .name     = "ancs_disc",
        };
        esp_timer_create(&args, &s_discovery_timer);
    }
    esp_timer_stop(s_discovery_timer);               /* cancel any pending */
    esp_timer_start_once(s_discovery_timer, 800000); /* 800 ms */
}

static bool uuid128_match(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 16) == 0;
}

/* Find a characteristic handle by 128-bit UUID after service search */
static uint16_t find_char_handle(esp_gatt_if_t gattc_if, uint16_t conn_id,
                                 uint16_t svc_start, uint16_t svc_end,
                                 const uint8_t *char_uuid128)
{
    esp_gattc_char_elem_t result[4];
    uint16_t count = 4;
    esp_bt_uuid_t uuid = {
        .len = ESP_UUID_LEN_128,
    };
    memcpy(uuid.uuid.uuid128, char_uuid128, 16);

    esp_err_t ret = esp_ble_gattc_get_char_by_uuid(
        gattc_if, conn_id, svc_start, svc_end, uuid, result, &count);
    if (ret != ESP_OK || count == 0) {
        return ESP_GATT_ILLEGAL_HANDLE;
    }
    return result[0].char_handle;
}

/* Request Title + Message for a notification UID */
static void fetch_notif_attributes(uint32_t uid)
{
    /*
     * GetNotificationAttributes command layout:
     *   [0]    CommandID = 0x00
     *   [1..4] NotificationUID (LE)
     *   [5]    AttributeID = TITLE (0x01)
     *   [6..7] MaxLen (LE) = ANCS_MAX_ATTR_LEN
     *   [8]    AttributeID = SUBTITLE (0x02)
     *   [9..10] MaxLen (LE)
     *   [11]   AttributeID = MESSAGE (0x03)
     *   [12..13] MaxLen (LE)
     *   [14]   AttributeID = APP_ID (0x00)  — no MaxLen field for APP_ID
     */
    uint8_t cmd[15];
    cmd[0]  = ANCS_CMD_GET_NOTIF_ATTR;
    cmd[1]  = (uid >>  0) & 0xFF;
    cmd[2]  = (uid >>  8) & 0xFF;
    cmd[3]  = (uid >> 16) & 0xFF;
    cmd[4]  = (uid >> 24) & 0xFF;
    cmd[5]  = ANCS_ATTR_TITLE;
    cmd[6]  = ANCS_MAX_ATTR_LEN & 0xFF;
    cmd[7]  = (ANCS_MAX_ATTR_LEN >> 8) & 0xFF;
    cmd[8]  = ANCS_ATTR_SUBTITLE;
    cmd[9]  = ANCS_MAX_ATTR_LEN & 0xFF;
    cmd[10] = (ANCS_MAX_ATTR_LEN >> 8) & 0xFF;
    cmd[11] = ANCS_ATTR_MESSAGE;
    cmd[12] = ANCS_MAX_ATTR_LEN & 0xFF;
    cmd[13] = (ANCS_MAX_ATTR_LEN >> 8) & 0xFF;
    cmd[14] = ANCS_ATTR_APP_ID;

    esp_err_t ret = esp_ble_gattc_write_char(
        s_gattc_if, s_conn_id,
        s_ctrl_pt_handle,
        sizeof(cmd), cmd,
        ESP_GATT_WRITE_TYPE_RSP,
        ESP_GATT_AUTH_REQ_NONE);  /* link already encrypted from bonding */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Control Point write failed: %s", esp_err_to_name(ret));
    }
    s_ds_len = 0; /* reset reassembly buffer */
}

/* Parse a fully-reassembled Data Source buffer for one GetNotifAttributes
 * response and fill in s_pending. Returns true on success. */
static bool parse_data_source(const uint8_t *buf, uint16_t len)
{
    if (len < 5) return false;                /* CommandID + UID = 5 bytes min */
    if (buf[0] != ANCS_CMD_GET_NOTIF_ATTR) return false;

    uint32_t uid = buf[1] | ((uint32_t)buf[2] << 8) |
                   ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 24);
    if (uid != s_pending.uid) {
        ESP_LOGW(TAG, "UID mismatch: expected %lu got %lu",
                 (unsigned long)s_pending.uid, (unsigned long)uid);
        return false;
    }

    uint16_t pos = 5;
    while (pos + 3 <= len) {
        uint8_t  attr_id   = buf[pos++];
        uint16_t attr_len  = buf[pos] | ((uint16_t)buf[pos+1] << 8);
        pos += 2;
        if (pos + attr_len > len) break;

        switch (attr_id) {
        case ANCS_ATTR_TITLE:
            snprintf(s_pending.title, sizeof(s_pending.title),
                     "%.*s", (int)attr_len, &buf[pos]);
            break;
        case ANCS_ATTR_MESSAGE:
            snprintf(s_pending.message, sizeof(s_pending.message),
                     "%.*s", (int)attr_len, &buf[pos]);
            break;
        case ANCS_ATTR_APP_ID:
            snprintf(s_pending.app_id, sizeof(s_pending.app_id),
                     "%.*s", (int)attr_len, &buf[pos]);
            break;
        default:
            break;
        }
        pos += attr_len;
    }
    return true;
}

/* ── GATTC event handler ─────────────────────────────────────────────────── */

static void gattc_event_handler(esp_gattc_cb_event_t event,
                                 esp_gatt_if_t gattc_if,
                                 esp_ble_gattc_cb_param_t *param)
{
    switch (event) {

    case ESP_GATTC_REG_EVT:
        ESP_LOGI(TAG, "GATTC registered, if=%d status=%d",
                 gattc_if, param->reg.status);
        s_gattc_if = gattc_if;
        /* Set security parameters for bonding */
        esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE,
            &(uint8_t){ESP_LE_AUTH_REQ_SC_MITM_BOND}, 1);
        esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,
            &(uint8_t){ESP_IO_CAP_NONE}, 1);
        esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,
            &(uint8_t){16}, 1);
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,
            &(uint8_t){ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK}, 1);
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,
            &(uint8_t){ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK}, 1);
        break;

    case ESP_GATTC_CONNECT_EVT:
        ESP_LOGI(TAG, "Connected, conn_id=%d", param->connect.conn_id);
        s_conn_id = param->connect.conn_id;
        s_disc_state = DISC_IDLE;
        memcpy(s_remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        ui_set_status_ble("GATTC");
        /* Must open the GATTC client channel before service discovery.
         * Without this, search_service fires into a void and SEARCH_CMPL
         * never arrives. is_direct=true reuses the existing BLE link. */
        esp_ble_gattc_open(gattc_if, param->connect.remote_bda,
                           param->connect.ble_addr_type, true);
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "GATTC open failed: %d", param->open.status);
            ui_set_status_ble("Retry");
            esp_ble_gap_disconnect(s_remote_bda);
        } else {
            ESP_LOGI(TAG, "GATTC open OK — requesting encryption");
            ui_set_status_ble("Bond");
            esp_ble_set_encryption(param->open.remote_bda,
                                   ESP_BLE_SEC_ENCRYPT_MITM);
        }
        break;

    case ESP_GATTC_SEARCH_RES_EVT: {
        s_svc_count++;
        ESP_LOGI(TAG, "Service #%d found (uuid_len=%d)", s_svc_count,
                 param->search_res.srvc_id.uuid.len);
        /* Check if this is the ANCS service */
        esp_gatt_id_t *srvc = &param->search_res.srvc_id;
        if (srvc->uuid.len == ESP_UUID_LEN_128 &&
            uuid128_match(srvc->uuid.uuid.uuid128, ANCS_SVC_UUID))
        {
            ESP_LOGI(TAG, "ANCS service found: start=0x%04x end=0x%04x",
                     param->search_res.start_handle,
                     param->search_res.end_handle);

            s_notif_src_handle = find_char_handle(
                gattc_if, s_conn_id,
                param->search_res.start_handle,
                param->search_res.end_handle,
                ANCS_NOTIF_SRC_UUID);

            s_ctrl_pt_handle = find_char_handle(
                gattc_if, s_conn_id,
                param->search_res.start_handle,
                param->search_res.end_handle,
                ANCS_CTRL_PT_UUID);

            s_data_src_handle = find_char_handle(
                gattc_if, s_conn_id,
                param->search_res.start_handle,
                param->search_res.end_handle,
                ANCS_DATA_SRC_UUID);

            ESP_LOGI(TAG, "  NotifSrc=0x%04x CtrlPt=0x%04x DataSrc=0x%04x",
                     s_notif_src_handle, s_ctrl_pt_handle, s_data_src_handle);
        }
        /* Check if this is the Current Time Service (CTS) */
        if (srvc->uuid.len == ESP_UUID_LEN_16 &&
            srvc->uuid.uuid.uuid16 == 0x1805)
        {
            s_cts_start_handle = param->search_res.start_handle;
            s_cts_end_handle   = param->search_res.end_handle;
            ESP_LOGI(TAG, "CTS service found: start=0x%04x end=0x%04x",
                     s_cts_start_handle, s_cts_end_handle);
        }
        break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        char buf[48];
        snprintf(buf, sizeof(buf), "Found %d services", s_svc_count);
        ESP_LOGI(TAG, "Service discovery complete — %s", buf);
        if (s_notif_src_handle == ESP_GATT_ILLEGAL_HANDLE) {
            /* iOS withholds ANCS on the first post-bond connection.
             * Show count so we know if ANY services were returned,
             * then disconnect so iOS reconnects and exposes ANCS. */
            ui_set_status_ble("Retry");
            esp_ble_gap_disconnect(s_remote_bda);
            break;
        }
        /* Enable Notification Source notifications first */
        s_disc_state = DISC_NOTIF_SRC_CCCD;
        esp_ble_gattc_register_for_notify(gattc_if,
            s_remote_bda, s_notif_src_handle);
        break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        if (param->reg_for_notify.status != ESP_GATT_OK) {
            ui_set_status_ble("Err");
            ESP_LOGE(TAG, "register_for_notify failed: %d",
                     param->reg_for_notify.status);
            break;
        }

        uint8_t  notify_en[2] = {0x01, 0x00};
        uint16_t char_h       = param->reg_for_notify.handle;
        uint16_t cccd_handle  = ESP_GATT_ILLEGAL_HANDLE;

        /* Find the CCCD descriptor */
        esp_gattc_descr_elem_t descr[4];
        uint16_t descr_count = 4;
        esp_bt_uuid_t cccd_uuid = {
            .len = ESP_UUID_LEN_16,
            .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG,
        };
        esp_ble_gattc_get_descr_by_char_handle(
            gattc_if, s_conn_id, char_h, cccd_uuid, descr, &descr_count);

        if (descr_count > 0) {
            cccd_handle = descr[0].handle;
            ESP_LOGI(TAG, "Writing CCCD h=0x%04x for char h=0x%04x",
                     cccd_handle, char_h);
        } else {
            /* Fallback: CCCD is almost always char_handle + 1 in ANCS */
            cccd_handle = char_h + 1;
            ESP_LOGW(TAG, "CCCD not found via lookup — using fallback h=0x%04x",
                     cccd_handle);
        }

        esp_err_t werr = esp_ble_gattc_write_char_descr(
            gattc_if, s_conn_id, cccd_handle,
            sizeof(notify_en), notify_en,
            ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        if (werr != ESP_OK) {
            ESP_LOGE(TAG, "CCCD write err: %s", esp_err_to_name(werr));
        }

        /* Advance to next step */
        if (s_disc_state == DISC_NOTIF_SRC_CCCD) {
            s_disc_state = DISC_DATA_SRC_CCCD;
            ESP_LOGI(TAG, "Registering DataSrc notify (h=0x%04x)", s_data_src_handle);
            esp_ble_gattc_register_for_notify(gattc_if,
                s_remote_bda, s_data_src_handle);
        } else if (s_disc_state == DISC_DATA_SRC_CCCD) {
            s_disc_state = DISC_DONE;
            ESP_LOGI(TAG, "ANCS fully subscribed");
            ui_set_status_ble("BLE");
            ui_show_idle();
            /* Read current time from CTS if available */
            if (s_cts_start_handle != ESP_GATT_ILLEGAL_HANDLE) {
                esp_bt_uuid_t cts_char_uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = 0x2A2B };
                esp_gattc_char_elem_t result[2];
                uint16_t count = 2;
                if (esp_ble_gattc_get_char_by_uuid(gattc_if, s_conn_id,
                        s_cts_start_handle, s_cts_end_handle,
                        cts_char_uuid, result, &count) == ESP_OK && count > 0) {
                    s_cts_char_handle = result[0].char_handle;
                    esp_ble_gattc_read_char(gattc_if, s_conn_id,
                                            s_cts_char_handle, ESP_GATT_AUTH_REQ_NONE);
                    ESP_LOGI(TAG, "Reading CTS char h=0x%04x", s_cts_char_handle);
                }
            }
        }
        break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
        uint16_t handle = param->notify.handle;
        uint8_t *data   = param->notify.value;
        uint16_t len    = param->notify.value_len;

        if (handle == s_notif_src_handle) {
            /* Notification Source: 8-byte fixed format */
            if (len < 8) break;
            uint8_t event_id  = data[0];
            uint8_t flags     = data[1];
            uint8_t cat       = data[2];
            uint32_t uid = data[4] | ((uint32_t)data[5] << 8) |
                           ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);

            ESP_LOGI(TAG, "NotifSrc event=%d cat=%s uid=%lu",
                     event_id, ancs_category_name((ancs_category_t)cat),
                     (unsigned long)uid);

            /* Ignore REMOVED events — don't touch the display */
            if (event_id == ANCS_EVT_REMOVED) break;

            memset(&s_pending, 0, sizeof(s_pending));
            s_pending.uid      = uid;
            s_pending.event_id = (ancs_event_id_t)event_id;
            s_pending.category = (ancs_category_t)cat;
            s_pending.flags    = flags;
            s_fetch_pending    = true;

            fetch_notif_attributes(uid);

        } else if (handle == s_data_src_handle) {

            if (len >= 5 && data[0] == ANCS_CMD_GET_NOTIF_ATTR) {
                uint32_t uid_in = data[1] | ((uint32_t)data[2] << 8) |
                                  ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
                if (uid_in == s_pending.uid) {
                    s_ds_len = 0;
                }
            }
            uint16_t space = DATA_SRC_BUF_SIZE - s_ds_len;
            uint16_t copy  = (len < space) ? len : space;
            memcpy(s_ds_buf + s_ds_len, data, copy);
            s_ds_len += copy;

            if (s_fetch_pending && parse_data_source(s_ds_buf, s_ds_len)) {
                s_fetch_pending = false;
                ESP_LOGI(TAG, "Parsed: cat=%s title='%s' msg='%s'",
                         ancs_category_name(s_pending.category),
                         s_pending.title, s_pending.message);
                if (s_user_cb) s_user_cb(&s_pending);
            } else if (s_fetch_pending) {
                /* Parse failed — show raw first bytes for debugging */
                ESP_LOGW(TAG, "Parse fail len=%d b0=%02x b1=%02x",
                         s_ds_len, s_ds_buf[0], s_ds_len > 1 ? s_ds_buf[1] : 0);
            }
        }
        break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "Write char status: %d", param->write.status);
        }
        break;

    case ESP_GATTC_READ_CHAR_EVT:
        if (param->read.status == ESP_GATT_OK &&
            param->read.handle == s_cts_char_handle &&
            param->read.value_len >= 7) {
            const uint8_t *v = param->read.value;
            uint16_t year = (uint16_t)v[0] | ((uint16_t)v[1] << 8);
            uint8_t month = v[2], day = v[3];
            uint8_t hours = v[4], minutes = v[5], seconds = v[6];
            uint8_t cts_dow = (param->read.value_len >= 8) ? v[7] : 0;
            uint8_t our_dow = (cts_dow == 7) ? 0 : cts_dow;
            ESP_LOGI(TAG, "CTS time: %04d-%02d-%02d %02d:%02d:%02d dow=%d",
                     year, month, day, hours, minutes, seconds, our_dow);
            ui_set_time(hours, minutes, seconds, our_dow, year, month, day);
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGW(TAG, "Disconnected (reason=%d) — restarting advertising",
                 param->disconnect.reason);
        ui_set_status_ble("Adv");
        s_conn_id         = 0xFFFF;
        s_notif_src_handle= ESP_GATT_ILLEGAL_HANDLE;
        s_ctrl_pt_handle  = ESP_GATT_ILLEGAL_HANDLE;
        s_data_src_handle = ESP_GATT_ILLEGAL_HANDLE;
        s_cts_start_handle= ESP_GATT_ILLEGAL_HANDLE;
        s_cts_end_handle  = ESP_GATT_ILLEGAL_HANDLE;
        s_cts_char_handle = ESP_GATT_ILLEGAL_HANDLE;
        s_disc_state      = DISC_IDLE;
        s_fetch_pending   = false;
        ancs_client_start_advertising();
        break;

    default:
        break;
    }
}

/* ── GAP event handler ───────────────────────────────────────────────────── */

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "ADV_DATA_RAW_SET status=%d — setting scan response",
                 param->adv_data_raw_cmpl.status);
        esp_ble_gap_config_scan_rsp_data_raw(raw_scan_rsp_data,
                                             sizeof(raw_scan_rsp_data));
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "SCAN_RSP_DATA_RAW_SET status=%d — starting advertising",
                 param->scan_rsp_data_raw_cmpl.status);
        esp_ble_gap_start_advertising(&adv_params);
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "ADV_START_COMPLETE — advertising is live");
        } else {
            ESP_LOGE(TAG, "ADV_START_COMPLETE failed, status=%d",
                     param->adv_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        ESP_LOGI(TAG, "Security request from " ESP_BD_ADDR_STR,
                 ESP_BD_ADDR_HEX(param->ble_security.ble_req.bd_addr));
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
        esp_bd_addr_t bd_addr;
        memcpy(bd_addr, param->ble_security.auth_cmpl.bd_addr, sizeof(esp_bd_addr_t));
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "Bonded with " ESP_BD_ADDR_STR,
                     ESP_BD_ADDR_HEX(bd_addr));
            ui_set_status_ble("ANCS");
            /* Fire discovery from a timer — calling GATTC directly inside
             * a BT event callback can deadlock the stack on ESP-IDF v6.x */
            start_discovery_delayed();
        } else {
            /* Bond key mismatch — happens when firmware was reflashed and NVS
             * was wiped but iPhone still holds the old keys. Remove stale bond
             * info so iOS can re-bond cleanly on the next connection. */
            ESP_LOGE(TAG, "Auth failed (reason=0x%02x) — clearing stale bond",
                     param->ble_security.auth_cmpl.fail_reason);
            esp_ble_remove_bond_device(bd_addr);
            ui_set_status_ble("Auth!");
        }
        break;
    }

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGI(TAG, "Passkey: %06lu",
                 (unsigned long)param->ble_security.key_notif.passkey);
        break;

    default:
        ESP_LOGD(TAG, "GAP event %d (unhandled)", event);
        break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void ancs_client_init(ancs_notification_cb_t cb)
{
    s_user_cb = cb;

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(ANCS_GATTC_APP_ID));

    /* Device name stored in GATT device name attribute */
    esp_ble_gap_set_device_name("FocusPager");

    ESP_LOGI(TAG, "ANCS client initialised");
}

void ancs_client_start_advertising(void)
{
    /* Raw adv data with AD type 0x15 (solicitation) — required for iOS ANCS.
     * Scan response is set in ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT. */
    ESP_ERROR_CHECK(esp_ble_gap_config_adv_data_raw(raw_adv_data,
                                                     sizeof(raw_adv_data)));
}

const char *ancs_category_name(ancs_category_t cat)
{
    switch (cat) {
    case ANCS_CAT_INCOMING_CALL: return "IncomingCall";
    case ANCS_CAT_MISSED_CALL:   return "MissedCall";
    case ANCS_CAT_VOICEMAIL:     return "Voicemail";
    case ANCS_CAT_SOCIAL:        return "Social";
    case ANCS_CAT_EMAIL:         return "Email";
    case ANCS_CAT_NEWS:          return "News";
    case ANCS_CAT_SCHEDULE:      return "Schedule";
    default:                     return "Other";
    }
}
