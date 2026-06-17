#pragma once

/**
 * brick_service.h — Brick Control GATT server
 *
 * Exposes four characteristics over BLE:
 *
 *   BrickState   (Read, Notify) — 0=unbricked, 1=bricked
 *   UnbrickEvent (Notify)       — monotonic counter, ++ on each button hold
 *   Auth         (Write, Read)  — 4-byte challenge in / 4-byte HMAC response out
 *   Command      (Write)        — 0=sync state, 1=force brick
 *
 * Base UUID: B41C00xx-9E5A-4C7B-9D2F-0A1B2C3D4E5F
 */

#include <stdint.h>
#include <stdbool.h>
#include "pager_state.h"

/**
 * Initialise and register the Brick Control GATT server.
 * Call after esp_bluedroid_enable().
 */
void brick_service_init(void);

/**
 * Notify connected central of a new BrickState value.
 * Call whenever pager_state changes.
 */
void brick_service_notify_state(pager_state_t state);

/**
 * Notify connected central of an UnbrickEvent.
 * Call from the button hold callback.
 */
void brick_service_notify_unbrick(void);
