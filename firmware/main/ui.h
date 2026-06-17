#pragma once

/**
 * ui.h — ST7789 display driver (240x320, SPI)
 *
 * Pins (set in ui.c):
 *   DIN  → GPIO23   CLK → GPIO18   CS → GPIO5
 *   DC   → GPIO2    RST → GPIO4    BL → GPIO21
 */

#include <stdint.h>

/* RGB565 colour helpers */
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0

/**
 * Initialise SPI bus and ST7789 panel.
 * Call once at startup before any other ui_ function.
 */
void ui_init(void);

/**
 * Fill the entire screen with a solid RGB565 colour.
 * Good for connection testing.
 */
void ui_fill(uint16_t color);

/**
 * Display a notification on screen.
 * category  — short label e.g. "MESSAGE", "CALL"
 * title     — sender / app name
 * message   — body text (truncated to fit)
 */
void ui_show_notification(const char *category, const char *title, const char *message);

/**
 * Show a status string centred on screen (e.g. "Advertising…").
 */
void ui_show_status(const char *status);
