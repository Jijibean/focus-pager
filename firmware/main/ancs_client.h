/**
 * ancs_client.h — Apple Notification Center Service (ANCS) GATT client
 *
 * The ESP32 acts as a BLE peripheral that solicits ANCS from a bonded iPhone.
 * After bonding, iOS exposes the ANCS GATT service; this module discovers it,
 * subscribes to Notification Source, and fetches Title + Message via the
 * Control Point / Data Source round-trip.
 *
 * Usage:
 *   1. Call ancs_client_init() once at startup, passing a callback.
 *   2. Call ancs_client_start_advertising() to begin BLE advertising.
 *   3. Your callback fires whenever a fully-parsed notification arrives.
 */

#pragma once
#include <stdint.h>

/* ── Notification categories iOS sends over ANCS ─────────────────────────── */
typedef enum {
    ANCS_CAT_OTHER         = 0,
    ANCS_CAT_INCOMING_CALL = 1,
    ANCS_CAT_MISSED_CALL   = 2,
    ANCS_CAT_VOICEMAIL     = 3,
    ANCS_CAT_SOCIAL        = 4,
    ANCS_CAT_SCHEDULE      = 5,
    ANCS_CAT_EMAIL         = 6,
    ANCS_CAT_NEWS          = 7,
    ANCS_CAT_HEALTH_FITNESS= 8,
    ANCS_CAT_BUSINESS      = 9,
    ANCS_CAT_LOCATION      = 10,
    ANCS_CAT_ENTERTAINMENT = 11,
} ancs_category_t;

/* ── Event IDs (Notification Source byte 0) ─────────────────────────────── */
typedef enum {
    ANCS_EVT_ADDED    = 0,
    ANCS_EVT_MODIFIED = 1,
    ANCS_EVT_REMOVED  = 2,
} ancs_event_id_t;

/* ── Parsed notification delivered to the app ───────────────────────────── */
typedef struct {
    uint32_t       uid;
    ancs_event_id_t event_id;
    ancs_category_t category;
    uint8_t        flags;          /* EventFlags bitmask from ANCS spec */
    char           title[64];
    char           message[128];
    char           app_id[64];
} ancs_notification_t;

/* ── Callback type ──────────────────────────────────────────────────────── */
typedef void (*ancs_notification_cb_t)(const ancs_notification_t *notif);

/* ── Public API ─────────────────────────────────────────────────────────── */

/**
 * Initialize the ANCS client. Must be called after esp_bluedroid_enable().
 * @param cb  Callback invoked on every fully-parsed notification (may be NULL).
 */
void ancs_client_init(ancs_notification_cb_t cb);

/**
 * Begin BLE advertising with the ANCS solicitation UUID so iOS will connect.
 * Call once after ancs_client_init().
 */
void ancs_client_start_advertising(void);

/**
 * Return a human-readable category name (for logging).
 */
const char *ancs_category_name(ancs_category_t cat);
