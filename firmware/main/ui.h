#pragma once

/**
 * ui.h — ST7789 display driver (320x240 landscape, SPI)
 *
 * Pins (set in ui.c):
 *   DIN  → GPIO23   CLK → GPIO18   CS → GPIO5
 *   DC   → GPIO4    RST → GPIO2    BL → GPIO21
 */

#include <stdint.h>
#include <stdbool.h>

/* RGB565 colour helpers */
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_GRAY    0x7BEF
#define COLOR_CYAN    0x07FF
#define COLOR_DIM     0x39E7   /* ~23% gray — hints, tertiary text */
#define COLOR_DIMRED  0xA000   /* dim red — bricked state */

/**
 * Initialise SPI bus, ST7789 panel (landscape), and 1-second clock timer.
 * Call once at startup before any other ui_ function.
 */
void ui_init(void);

/**
 * Fill the entire screen with a solid RGB565 colour.
 */
void ui_fill(uint16_t color);

/* ── Status bar ─────────────────────────────────────────────────────────── */

/** Update the BLE status label in the status bar (e.g. "Adv", "Bond", "BLE"). */
void ui_set_status_ble(const char *text);

/** Update the HFP status label in the status bar (e.g. "HFP", "Call"). */
void ui_set_status_hfp(const char *text);

/* ── Screen modes ───────────────────────────────────────────────────────── */

/** Show the idle screen (clock, date, content area). Called when unbricked. */
void ui_show_idle(void);

/** Show the bricked screen (large LOCKED label). */
void ui_show_bricked(void);

/**
 * Display a notification overlay (keeps status bar).
 * Auto-dismisses after 8 seconds back to idle.
 */
void ui_show_notification(const char *category, const char *title, const char *message);

/** Show incoming call screen (keeps status bar). */
void ui_show_incoming_call(const char *caller);

/** Show active call screen (keeps status bar). */
void ui_show_call_active(void);

/* ── Display data (set from BLE) ────────────────────────────────────────── */

/** Sync time from the companion app. dow = 0=Sun..6=Sat. */
void ui_set_time(uint8_t hour, uint8_t min, uint8_t sec,
                 uint8_t dow, uint16_t year, uint8_t month, uint8_t day);

/** Set a to-do item (index 0–3). checked=true shows [x], else [ ]. */
void ui_set_todo(uint8_t index, bool checked, const char *text);

/** Clear all to-do items. */
void ui_clear_todos(void);

/** Set the custom message line. */
void ui_set_message(const char *text);

/** Clear the custom message. */
void ui_clear_message(void);

/* ── Encoder navigation ─────────────────────────────────────────────────── */

/**
 * Navigate the notification list.
 * +1 scrolls to the next (older) notification; -1 scrolls back toward home.
 * Page 0 is always the home screen; pages 1..N are notification detail views.
 * No-ops when a call or bricked screen is active.
 */
void ui_navigate(int delta);
