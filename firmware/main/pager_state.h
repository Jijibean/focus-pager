#pragma once

/**
 * pager_state.h — Brick state machine + button task
 *
 * Monitors GPIO27 for a ≥2s hold and fires an unbrick event.
 * State is persisted in NVS so it survives power cycles.
 */

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PAGER_UNBRICKED = 0,
    PAGER_BRICKED   = 1,
} pager_state_t;

/* Callback fired when the button is held ≥2s */
typedef void (*unbrick_event_cb_t)(void);

/**
 * Initialise the state machine and button GPIO.
 * Call once after NVS is initialised.
 * @param cb  Called on every valid button hold (may be NULL).
 */
void pager_state_init(unbrick_event_cb_t cb);

/** Set brick state (persisted to NVS, updates display) */
void pager_state_set(pager_state_t state);

/** Get current brick state */
pager_state_t pager_state_get(void);
