/**
 * brick_service.c — Brick Control GATT server (ESP-IDF Bluedroid)
 *
 * Registers a custom 128-bit service with four characteristics.
 * Auth uses a simple 4-byte truncated HMAC-SHA256 challenge-response
 * with the PSK stored in NVS (set once at pairing time).
 *
 * Base UUID: B41C00xx-9E5A-4C7B-9D2F-0A1B2C3D4E5F
 */

#include "brick_service.h"
#include "pager_state.h"
#include "ui.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_defs.h"
#include "nvs_flash.h"
#include "nvs.h"
/* Expose mbedtls_md_hmac_* which are gated as private in mbedtls 3.x headers */
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/md.h"

#define TAG "BRICK"

/* ── UUIDs ───────────────────────────────────────────────────────────────── *
 * Base: B41C00xx-9E5A-4C7B-9D2F-0A1B2C3D4E5F  (little-endian)
 */
#define UUID_BASE { \
    0x5F,0x4E,0x3D,0x2C,0x1B,0x0A,0x2F,0x9D, \
    0x7B,0x4C,0x5A,0x9E,0x00,0x00,0x1C,0xB4  \
}

/* Characteristic xx bytes */
#define XX_SVC          0x01
#define XX_BRICK_STATE  0x02
#define XX_UNBRICK_EVT  0x03
#define XX_AUTH         0x04
#define XX_COMMAND      0x05
#define XX_DISPLAY      0x06

/* ── GATT attribute table indices ───────────────────────────────────────── */
enum {
    IDX_SVC,

    IDX_BRICK_STATE_DECL,
    IDX_BRICK_STATE_VAL,
    IDX_BRICK_STATE_CCCD,

    IDX_UNBRICK_DECL,
    IDX_UNBRICK_VAL,
    IDX_UNBRICK_CCCD,

    IDX_AUTH_DECL,
    IDX_AUTH_VAL,

    IDX_CMD_DECL,
    IDX_CMD_VAL,

    IDX_DISP_DECL,
    IDX_DISP_VAL,

    ATTR_COUNT,
};

static uint16_t s_handles[ATTR_COUNT];
static uint16_t s_conn_id  = 0xFFFF;
static uint8_t  s_gatts_if = 0xFF;

/* UnbrickEvent counter */
static uint8_t s_unbrick_count = 0;

/* BrickState value (1 byte) */
static uint8_t s_brick_state_val = 0;

/* Auth: last challenge written by central, response computed by us */
static uint8_t s_auth_challenge[4] = {0};
static uint8_t s_auth_response[4]  = {0};
static bool    s_challenge_valid   = false;  /* true after Auth write, cleared after Command */

/* ── PSK (stored in NVS, compiled-in default used if NVS is empty) ────────── */
#define NVS_NS_BRICK  "brick"
#define NVS_KEY_PSK   "psk"
#define PSK_LEN       16

static uint8_t s_psk[PSK_LEN] = {
    0x2E, 0x8F, 0x41, 0xA6, 0xF4, 0xE3, 0x67, 0x69,
    0xA0, 0x3E, 0x79, 0xC9, 0xF6, 0x6B, 0x0E, 0xB7,
};

static void load_psk(void)
{
    nvs_handle_t h;
    size_t len = PSK_LEN;
    if (nvs_open(NVS_NS_BRICK, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_blob(h, NVS_KEY_PSK, s_psk, &len);
        nvs_close(h);
    }
    /* If NVS has no key, the compiled-in default is used */
}

/* ── HMAC-SHA256 truncated to 4 bytes ────────────────────────────────────── */
static void compute_hmac4(const uint8_t *psk, const uint8_t *msg, size_t msg_len,
                          uint8_t out[4])
{
    uint8_t hmac[32];
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 1 /* is_hmac */);
    mbedtls_md_hmac_starts(&ctx, psk, PSK_LEN);
    mbedtls_md_hmac_update(&ctx, msg, msg_len);
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);
    memcpy(out, hmac, 4);
}

/* ── UUID helpers ────────────────────────────────────────────────────────── */

static esp_bt_uuid_t make_uuid(uint8_t xx)
{
    esp_bt_uuid_t u = { .len = ESP_UUID_LEN_128 };
    uint8_t base[] = UUID_BASE;
    memcpy(u.uuid.uuid128, base, 16);
    u.uuid.uuid128[12] = xx;   /* byte 12 = xx field */
    return u;
}

/* ── Attribute table ─────────────────────────────────────────────────────── */

static const uint8_t char_prop_rn  = ESP_GATT_CHAR_PROP_BIT_READ
                                   | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t char_prop_n   = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t char_prop_rw  = ESP_GATT_CHAR_PROP_BIT_READ
                                   | ESP_GATT_CHAR_PROP_BIT_WRITE;
static const uint8_t char_prop_w   = ESP_GATT_CHAR_PROP_BIT_WRITE;
static const uint8_t cccd_val[2]   = {0x00, 0x00};
static const uint8_t zero1         = 0x00;

static esp_gatts_attr_db_t s_attr_db[ATTR_COUNT];

static void build_attr_db(void)
{
    /* Helper UUIDs */
    static esp_bt_uuid_t svc_uuid, bs_uuid, ue_uuid, au_uuid, cmd_uuid, disp_uuid;
    static esp_bt_uuid_t char_decl_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid.uuid16 = ESP_GATT_UUID_CHAR_DECLARE,
    };
    static esp_bt_uuid_t cccd_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG,
    };
    /* Must have static storage: the attribute table is read by
     * esp_ble_gatts_create_attr_tab() after this function returns, so a
     * compound-literal/stack value here would dangle. */
    static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;

    svc_uuid = make_uuid(XX_SVC);
    bs_uuid  = make_uuid(XX_BRICK_STATE);
    ue_uuid  = make_uuid(XX_UNBRICK_EVT);
    au_uuid  = make_uuid(XX_AUTH);
    cmd_uuid  = make_uuid(XX_COMMAND);
    disp_uuid = make_uuid(XX_DISPLAY);

    /* Service */
    s_attr_db[IDX_SVC] = (esp_gatts_attr_db_t){
        .attr_control = {.auto_rsp = ESP_GATT_AUTO_RSP},
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16,
            .uuid_p      = (uint8_t *)&primary_service_uuid,
            .perm        = ESP_GATT_PERM_READ,
            .max_length  = ESP_UUID_LEN_128,
            .length      = ESP_UUID_LEN_128,
            .value       = svc_uuid.uuid.uuid128,
        },
    };

    /* BrickState declaration */
    s_attr_db[IDX_BRICK_STATE_DECL] = (esp_gatts_attr_db_t){
        .attr_control = {.auto_rsp = ESP_GATT_AUTO_RSP},
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16,
            .uuid_p      = (uint8_t *)&char_decl_uuid.uuid.uuid16,
            .perm        = ESP_GATT_PERM_READ,
            .max_length  = sizeof(char_prop_rn),
            .length      = sizeof(char_prop_rn),
            .value       = (uint8_t *)&char_prop_rn,
        },
    };
    /* BrickState value */
    s_attr_db[IDX_BRICK_STATE_VAL] = (esp_gatts_attr_db_t){
        .attr_control = {.auto_rsp = ESP_GATT_AUTO_RSP},
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_128,
            .uuid_p      = bs_uuid.uuid.uuid128,
            .perm        = ESP_GATT_PERM_READ,
            .max_length  = 1,
            .length      = 1,
            .value       = &s_brick_state_val,
        },
    };
    /* BrickState CCCD */
    s_attr_db[IDX_BRICK_STATE_CCCD] = (esp_gatts_attr_db_t){
        .attr_control = {.auto_rsp = ESP_GATT_AUTO_RSP},
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16,
            .uuid_p      = (uint8_t *)&cccd_uuid.uuid.uuid16,
            .perm        = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            .max_length  = 2,
            .length      = 2,
            .value       = (uint8_t *)cccd_val,
        },
    };

    /* UnbrickEvent declaration */
    s_attr_db[IDX_UNBRICK_DECL] = (esp_gatts_attr_db_t){
        .attr_control = {.auto_rsp = ESP_GATT_AUTO_RSP},
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16,
            .uuid_p      = (uint8_t *)&char_decl_uuid.uuid.uuid16,
            .perm        = ESP_GATT_PERM_READ,
            .max_length  = sizeof(char_prop_n),
            .length      = sizeof(char_prop_n),
            .value       = (uint8_t *)&char_prop_n,
        },
    };
    /* UnbrickEvent value */
    s_attr_db[IDX_UNBRICK_VAL] = (esp_gatts_attr_db_t){
        .attr_control = {.auto_rsp = ESP_GATT_AUTO_RSP},
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_128,
            .uuid_p      = ue_uuid.uuid.uuid128,
            .perm        = ESP_GATT_PERM_READ,
            .max_length  = 1,
            .length      = 1,
            .value       = &s_unbrick_count,
        },
    };
    /* UnbrickEvent CCCD */
    s_attr_db[IDX_UNBRICK_CCCD] = (esp_gatts_attr_db_t){
        .attr_control = {.auto_rsp = ESP_GATT_AUTO_RSP},
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16,
            .uuid_p      = (uint8_t *)&cccd_uuid.uuid.uuid16,
            .perm        = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            .max_length  = 2,
            .length      = 2,
            .value       = (uint8_t *)cccd_val,
        },
    };

    /* Auth declaration */
    s_attr_db[IDX_AUTH_DECL] = (esp_gatts_attr_db_t){
        .attr_control = {.auto_rsp = ESP_GATT_AUTO_RSP},
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16,
            .uuid_p      = (uint8_t *)&char_decl_uuid.uuid.uuid16,
            .perm        = ESP_GATT_PERM_READ,
            .max_length  = sizeof(char_prop_rw),
            .length      = sizeof(char_prop_rw),
            .value       = (uint8_t *)&char_prop_rw,
        },
    };
    /* Auth value — manual response so we can compute HMAC on write */
    s_attr_db[IDX_AUTH_VAL] = (esp_gatts_attr_db_t){
        .attr_control = {.auto_rsp = ESP_GATT_RSP_BY_APP},
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_128,
            .uuid_p      = au_uuid.uuid.uuid128,
            .perm        = ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
            .max_length  = 4,
            .length      = 4,
            .value       = s_auth_response,
        },
    };

    /* Command declaration */
    s_attr_db[IDX_CMD_DECL] = (esp_gatts_attr_db_t){
        .attr_control = {.auto_rsp = ESP_GATT_AUTO_RSP},
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16,
            .uuid_p      = (uint8_t *)&char_decl_uuid.uuid.uuid16,
            .perm        = ESP_GATT_PERM_READ,
            .max_length  = sizeof(char_prop_w),
            .length      = sizeof(char_prop_w),
            .value       = (uint8_t *)&char_prop_w,
        },
    };
    /* Command value — manual response so we can act on the write.
     * Phase 5: accepts 5 bytes [cmd, hmac4] and verifies before executing. */
    s_attr_db[IDX_CMD_VAL] = (esp_gatts_attr_db_t){
        .attr_control = {.auto_rsp = ESP_GATT_RSP_BY_APP},
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_128,
            .uuid_p      = cmd_uuid.uuid.uuid128,
            .perm        = ESP_GATT_PERM_WRITE_ENCRYPTED,
            .max_length  = 5,
            .length      = 1,
            .value       = (uint8_t *)&zero1,
        },
    };

    /* Display Data declaration */
    s_attr_db[IDX_DISP_DECL] = (esp_gatts_attr_db_t){
        .attr_control = {.auto_rsp = ESP_GATT_AUTO_RSP},
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16,
            .uuid_p      = (uint8_t *)&char_decl_uuid.uuid.uuid16,
            .perm        = ESP_GATT_PERM_READ,
            .max_length  = sizeof(char_prop_w),
            .length      = sizeof(char_prop_w),
            .value       = (uint8_t *)&char_prop_w,
        },
    };
    /* Display Data value — manual response so WRITE_EVT fires with data */
    s_attr_db[IDX_DISP_VAL] = (esp_gatts_attr_db_t){
        .attr_control = {.auto_rsp = ESP_GATT_RSP_BY_APP},
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_128,
            .uuid_p      = disp_uuid.uuid.uuid128,
            .perm        = ESP_GATT_PERM_WRITE_ENCRYPTED,
            .max_length  = 128,
            .length      = 1,
            .value       = (uint8_t *)&zero1,
        },
    };
}

/* ── GATTS event handler ─────────────────────────────────────────────────── */

static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    switch (event) {

    case ESP_GATTS_REG_EVT:
        ESP_LOGI(TAG, "GATTS registered, if=%d status=%d",
                 gatts_if, param->reg.status);
        s_gatts_if = gatts_if;
        build_attr_db();
        esp_ble_gatts_create_attr_tab(s_attr_db, gatts_if,
                                      ATTR_COUNT, 0);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Attr tab create failed: %d",
                     param->add_attr_tab.status);
            break;
        }
        memcpy(s_handles, param->add_attr_tab.handles,
               sizeof(uint16_t) * ATTR_COUNT);
        ESP_LOGI(TAG, "Attr table created — starting GATTS service");
        esp_ble_gatts_start_service(s_handles[IDX_SVC]);
        break;

    case ESP_GATTS_START_EVT:
        ESP_LOGI(TAG, "Brick Control service started");
        break;

    case ESP_GATTS_CONNECT_EVT:
        ESP_LOGI(TAG, "Central connected, conn_id=%d", param->connect.conn_id);
        s_conn_id = param->connect.conn_id;
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "Central disconnected");
        s_conn_id = 0xFFFF;
        s_challenge_valid = false;
        memset(s_auth_challenge, 0, 4);
        break;

    case ESP_GATTS_READ_EVT: {
        uint16_t h = param->read.handle;
        esp_gatt_rsp_t rsp = {0};

        if (h == s_handles[IDX_AUTH_VAL]) {
            /* Return the computed HMAC response */
            rsp.attr_value.len = 4;
            memcpy(rsp.attr_value.value, s_auth_response, 4);
            ESP_LOGI(TAG, "Auth read — response %02x%02x%02x%02x",
                     s_auth_response[0], s_auth_response[1],
                     s_auth_response[2], s_auth_response[3]);
        } else {
            rsp.attr_value.len = 1;
            rsp.attr_value.value[0] = 0;
        }
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                                    param->read.trans_id,
                                    ESP_GATT_OK, &rsp);
        break;
    }

    case ESP_GATTS_WRITE_EVT: {
        uint16_t h   = param->write.handle;
        uint8_t *val = param->write.value;
        uint16_t len = param->write.len;

        if (h == s_handles[IDX_AUTH_VAL] && len == 4) {
            /* Central wrote a 4-byte challenge — compute HMAC response */
            memcpy(s_auth_challenge, val, 4);
            s_challenge_valid = true;
            compute_hmac4(s_psk, s_auth_challenge, 4, s_auth_response);
            ESP_LOGI(TAG, "Auth challenge %02x%02x%02x%02x → response %02x%02x%02x%02x",
                     val[0], val[1], val[2], val[3],
                     s_auth_response[0], s_auth_response[1],
                     s_auth_response[2], s_auth_response[3]);
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id,
                                            ESP_GATT_OK, NULL);
            }

        } else if (h == s_handles[IDX_CMD_VAL] && len == 5) {
            /* Command = [cmd_byte, hmac4] where hmac4 = HMAC(PSK, challenge||cmd)[0:4] */
            uint8_t cmd = val[0];

            if (!s_challenge_valid) {
                ESP_LOGW(TAG, "Command 0x%02x rejected — no active auth challenge", cmd);
                if (param->write.need_rsp) {
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                                param->write.trans_id,
                                                ESP_GATT_WRITE_NOT_PERMIT, NULL);
                }
                break;
            }

            /* Verify: HMAC(PSK, challenge || cmd)[0:4] */
            uint8_t msg[5];
            memcpy(msg, s_auth_challenge, 4);
            msg[4] = cmd;
            uint8_t expected[4];
            compute_hmac4(s_psk, msg, sizeof(msg), expected);

            if (memcmp(&val[1], expected, 4) != 0) {
                ESP_LOGW(TAG, "Command 0x%02x rejected — HMAC mismatch", cmd);
                if (param->write.need_rsp) {
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                                param->write.trans_id,
                                                ESP_GATT_WRITE_NOT_PERMIT, NULL);
                }
                break;
            }

            /* Challenge consumed — one-time use */
            s_challenge_valid = false;
            memset(s_auth_challenge, 0, 4);

            ESP_LOGI(TAG, "Command 0x%02x authenticated OK", cmd);
            if (cmd == 0x01) {
                /* Force brick */
                pager_state_set(PAGER_BRICKED);
                brick_service_notify_state(PAGER_BRICKED);
            } else if (cmd == 0x00) {
                /* Sync — send back current state */
                brick_service_notify_state(pager_state_get());
            }
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id,
                                            ESP_GATT_OK, NULL);
            }

        } else if (h == s_handles[IDX_DISP_VAL] && len >= 1) {
            /* Display Data — parse opcode + payload */
            uint8_t opcode = val[0];
            switch (opcode) {
            case 0x01:  /* TIME_SYNC: [op, hour, min, sec, dow, year_hi, year_lo, month, day] */
                if (len >= 9) {
                    uint16_t year = ((uint16_t)val[5] << 8) | val[6];
                    ui_set_time(val[1], val[2], val[3], val[4], year, val[7], val[8]);
                }
                break;
            case 0x02:  /* TODO_SET: [op, index, checked, text...] */
                if (len >= 4) {
                    char text[33] = {0};
                    int tlen = len - 3;
                    if (tlen > 32) tlen = 32;
                    memcpy(text, &val[3], tlen);
                    text[tlen] = '\0';
                    ui_set_todo(val[1], val[2] != 0, text);
                }
                break;
            case 0x03:  /* TODO_CLEAR */
                ui_clear_todos();
                break;
            case 0x04:  /* MESSAGE_SET: [op, text...] */
                if (len >= 2) {
                    char text[65] = {0};
                    int tlen = len - 1;
                    if (tlen > 64) tlen = 64;
                    memcpy(text, &val[1], tlen);
                    text[tlen] = '\0';
                    ui_set_message(text);
                }
                break;
            case 0x05:  /* MESSAGE_CLR */
                ui_clear_message();
                break;
            default:
                ESP_LOGW(TAG, "Unknown display opcode 0x%02x", opcode);
                break;
            }
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id,
                                            ESP_GATT_OK, NULL);
            }
        }
        break;
    }

    default:
        break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void brick_service_init(void)
{
    load_psk();
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(1));
    ESP_LOGI(TAG, "Brick Control service init complete");
}

void brick_service_notify_state(pager_state_t state)
{
    if (s_conn_id == 0xFFFF) return;
    s_brick_state_val = (uint8_t)state;
    uint8_t val = (uint8_t)state;
    esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                                s_handles[IDX_BRICK_STATE_VAL],
                                1, &val, false);
}

void brick_service_notify_unbrick(void)
{
    s_unbrick_count++;
    ESP_LOGI(TAG, "UnbrickEvent #%d", s_unbrick_count);
    if (s_conn_id == 0xFFFF) {
        ESP_LOGW(TAG, "No central connected — event stored, not notified");
        return;
    }
    esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                                s_handles[IDX_UNBRICK_VAL],
                                1, &s_unbrick_count, false);
}
