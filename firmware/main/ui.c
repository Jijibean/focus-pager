/**
 * ui.c — ST7789 320x240 landscape display driver using ESP-IDF esp_lcd
 *
 * Pin assignments:
 *   DIN (MOSI) → GPIO23    CLK  → GPIO18
 *   CS         → GPIO5     DC   → GPIO2
 *   RST        → GPIO4     BL   → GPIO21
 */

#include "ui.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_log.h"

#define TAG "UI"

/* ── Pin definitions ─────────────────────────────────────────────────────── */
#define LCD_MOSI    23
#define LCD_CLK     18
#define LCD_CS       5
#define LCD_DC       2
#define LCD_RST      4
#define LCD_BL      21

/* ── Display geometry (landscape) ────────────────────────────────────────── */
#define LCD_W       320
#define LCD_H       240
#define LCD_SPI     SPI2_HOST
#define LCD_HZ      (40 * 1000 * 1000)

/* ── Font ────────────────────────────────────────────────────────────────── */
#define CHAR_W   6
#define CHAR_H   8

/* ── Layout constants (pixel Y) ──────────────────────────────────────────── */
#define STATUS_Y        0       /* status bar top */
#define STATUS_H       16       /* 2x scale → 16px */
#define DIV1_Y         17       /* first divider */
#define CLOCK_Y        26       /* clock top (4x) */
#define CLOCK_H        32       /* 4x scale → 32px */
#define AMPM_Y         38       /* AM/PM baseline (2x), vertically centred with clock */
#define DATE_Y         62       /* date top (2x) */
#define DATE_H         16       /* 2x scale → 16px */
#define DIV2_Y         81       /* second divider */
#define CONTENT_Y      86       /* content area top */
#define CONTENT_LINE_H 18       /* spacing between content lines (2x + 2px gap) */

#define MAX_TODOS       4
#define TODO_TEXT_LEN  32
#define MSG_TEXT_LEN   64

/* ── Simple 6x8 font (ASCII 32–127) ─────────────────────────────────────── *
 * Column-major, LSB=top row.
 */
static const uint8_t font6x8[][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00,0x00}, /* '"' */
    {0x14,0x7F,0x14,0x7F,0x14,0x00}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12,0x00}, /* '$' */
    {0x23,0x13,0x08,0x64,0x62,0x00}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50,0x00}, /* '&' */
    {0x00,0x05,0x03,0x00,0x00,0x00}, /* ''' */
    {0x00,0x1C,0x22,0x41,0x00,0x00}, /* '(' */
    {0x00,0x41,0x22,0x1C,0x00,0x00}, /* ')' */
    {0x08,0x2A,0x1C,0x2A,0x08,0x00}, /* '*' */
    {0x08,0x08,0x3E,0x08,0x08,0x00}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00,0x00}, /* ',' */
    {0x08,0x08,0x08,0x08,0x08,0x00}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00,0x00}, /* '.' */
    {0x20,0x10,0x08,0x04,0x02,0x00}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, /* '0' */
    {0x00,0x42,0x7F,0x40,0x00,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46,0x00}, /* '2' */
    {0x21,0x41,0x45,0x4B,0x31,0x00}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10,0x00}, /* '4' */
    {0x27,0x45,0x45,0x45,0x39,0x00}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30,0x00}, /* '6' */
    {0x01,0x71,0x09,0x05,0x03,0x00}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36,0x00}, /* '8' */
    {0x06,0x49,0x49,0x29,0x1E,0x00}, /* '9' */
    {0x00,0x36,0x36,0x00,0x00,0x00}, /* ':' */
    {0x00,0x56,0x36,0x00,0x00,0x00}, /* ';' */
    {0x08,0x14,0x22,0x41,0x00,0x00}, /* '<' */
    {0x14,0x14,0x14,0x14,0x14,0x00}, /* '=' */
    {0x00,0x41,0x22,0x14,0x08,0x00}, /* '>' */
    {0x02,0x01,0x51,0x09,0x06,0x00}, /* '?' */
    {0x32,0x49,0x79,0x41,0x3E,0x00}, /* '@' */
    {0x7E,0x11,0x11,0x11,0x7E,0x00}, /* 'A' */
    {0x7F,0x49,0x49,0x49,0x36,0x00}, /* 'B' */
    {0x3E,0x41,0x41,0x41,0x22,0x00}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C,0x00}, /* 'D' */
    {0x7F,0x49,0x49,0x49,0x41,0x00}, /* 'E' */
    {0x7F,0x09,0x09,0x09,0x01,0x00}, /* 'F' */
    {0x3E,0x41,0x49,0x49,0x7A,0x00}, /* 'G' */
    {0x7F,0x08,0x08,0x08,0x7F,0x00}, /* 'H' */
    {0x00,0x41,0x7F,0x41,0x00,0x00}, /* 'I' */
    {0x20,0x40,0x41,0x3F,0x01,0x00}, /* 'J' */
    {0x7F,0x08,0x14,0x22,0x41,0x00}, /* 'K' */
    {0x7F,0x40,0x40,0x40,0x40,0x00}, /* 'L' */
    {0x7F,0x02,0x0C,0x02,0x7F,0x00}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F,0x00}, /* 'N' */
    {0x3E,0x41,0x41,0x41,0x3E,0x00}, /* 'O' */
    {0x7F,0x09,0x09,0x09,0x06,0x00}, /* 'P' */
    {0x3E,0x41,0x51,0x21,0x5E,0x00}, /* 'Q' */
    {0x7F,0x09,0x19,0x29,0x46,0x00}, /* 'R' */
    {0x46,0x49,0x49,0x49,0x31,0x00}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01,0x00}, /* 'T' */
    {0x3F,0x40,0x40,0x40,0x3F,0x00}, /* 'U' */
    {0x1F,0x20,0x40,0x20,0x1F,0x00}, /* 'V' */
    {0x3F,0x40,0x38,0x40,0x3F,0x00}, /* 'W' */
    {0x63,0x14,0x08,0x14,0x63,0x00}, /* 'X' */
    {0x07,0x08,0x70,0x08,0x07,0x00}, /* 'Y' */
    {0x61,0x51,0x49,0x45,0x43,0x00}, /* 'Z' */
    {0x00,0x7F,0x41,0x41,0x00,0x00}, /* '[' */
    {0x02,0x04,0x08,0x10,0x20,0x00}, /* '\' */
    {0x00,0x41,0x41,0x7F,0x00,0x00}, /* ']' */
    {0x04,0x02,0x01,0x02,0x04,0x00}, /* '^' */
    {0x40,0x40,0x40,0x40,0x40,0x00}, /* '_' */
    {0x00,0x01,0x02,0x04,0x00,0x00}, /* '`' */
    {0x20,0x54,0x54,0x54,0x78,0x00}, /* 'a' */
    {0x7F,0x48,0x44,0x44,0x38,0x00}, /* 'b' */
    {0x38,0x44,0x44,0x44,0x20,0x00}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F,0x00}, /* 'd' */
    {0x38,0x54,0x54,0x54,0x18,0x00}, /* 'e' */
    {0x08,0x7E,0x09,0x01,0x02,0x00}, /* 'f' */
    {0x0C,0x52,0x52,0x52,0x3E,0x00}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78,0x00}, /* 'h' */
    {0x00,0x44,0x7D,0x40,0x00,0x00}, /* 'i' */
    {0x20,0x40,0x44,0x3D,0x00,0x00}, /* 'j' */
    {0x7F,0x10,0x28,0x44,0x00,0x00}, /* 'k' */
    {0x00,0x41,0x7F,0x40,0x00,0x00}, /* 'l' */
    {0x7C,0x04,0x18,0x04,0x78,0x00}, /* 'm' */
    {0x7C,0x08,0x04,0x04,0x78,0x00}, /* 'n' */
    {0x38,0x44,0x44,0x44,0x38,0x00}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08,0x00}, /* 'p' */
    {0x08,0x14,0x14,0x18,0x7C,0x00}, /* 'q' */
    {0x7C,0x08,0x04,0x04,0x08,0x00}, /* 'r' */
    {0x48,0x54,0x54,0x54,0x20,0x00}, /* 's' */
    {0x04,0x3F,0x44,0x40,0x20,0x00}, /* 't' */
    {0x3C,0x40,0x40,0x40,0x7C,0x00}, /* 'u' */
    {0x1C,0x20,0x40,0x20,0x1C,0x00}, /* 'v' */
    {0x3C,0x40,0x30,0x40,0x3C,0x00}, /* 'w' */
    {0x44,0x28,0x10,0x28,0x44,0x00}, /* 'x' */
    {0x0C,0x50,0x50,0x50,0x3C,0x00}, /* 'y' */
    {0x44,0x64,0x54,0x4C,0x44,0x00}, /* 'z' */
    {0x00,0x08,0x36,0x41,0x00,0x00}, /* '{' */
    {0x00,0x00,0x7F,0x00,0x00,0x00}, /* '|' */
    {0x00,0x41,0x36,0x08,0x00,0x00}, /* '}' */
    {0x08,0x04,0x08,0x10,0x08,0x00}, /* '~' */
};

/* ── Screen state ────────────────────────────────────────────────────────── */
typedef enum {
    SCREEN_IDLE,
    SCREEN_BRICKED,
    SCREEN_NOTIFICATION,
    SCREEN_INCOMING_CALL,
    SCREEN_CALL_ACTIVE,
} screen_state_t;

static screen_state_t s_screen = SCREEN_IDLE;

/* ── Status bar state ────────────────────────────────────────────────────── */
static char s_ble_status[16] = "";
static char s_hfp_status[16] = "";
static bool s_is_bricked = false;  /* drives LOCKED/READY label */

/* ── Time state ──────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  hour, min, sec;
    uint8_t  dow;     /* 0=Sun..6=Sat */
    uint16_t year;
    uint8_t  month, day;
    bool     valid;   /* false until first TIME_SYNC */
} pager_time_t;

static pager_time_t s_time = {0};

/* ── Content state ───────────────────────────────────────────────────────── */
typedef struct {
    bool  active;
    bool  checked;
    char  text[TODO_TEXT_LEN + 1];
} todo_item_t;

static todo_item_t s_todos[MAX_TODOS] = {0};
static char s_message[MSG_TEXT_LEN + 1] = "";

/* ── Panel handle ────────────────────────────────────────────────────────── */
static esp_lcd_panel_handle_t s_panel = NULL;

/* Mutex: serialises all draw calls — esp_timer callbacks and BT callbacks
 * run on different tasks, so concurrent SPI access must be prevented. */
static SemaphoreHandle_t s_ui_mutex = NULL;

#define UI_LOCK()   xSemaphoreTakeRecursive(s_ui_mutex, portMAX_DELAY)
#define UI_UNLOCK() xSemaphoreGiveRecursive(s_ui_mutex)

/* ── Timers ──────────────────────────────────────────────────────────────── */
static esp_timer_handle_t s_clock_timer = NULL;
static esp_timer_handle_t s_notif_timer = NULL;

/* ── Strip buffer for fills ──────────────────────────────────────────────── */
static uint16_t s_strip[LCD_W * 16];

/* ── Drawing helpers ─────────────────────────────────────────────────────── */

/** Draw a single character at pixel (px, py) with integer scale factor. */
static void draw_char_scaled(int px, int py, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font6x8[c - 32];
    int w = CHAR_W * scale;
    int h = CHAR_H * scale;

    /* Clamp to screen bounds */
    if (px + w > LCD_W) w = LCD_W - px;
    if (py + h > LCD_H) h = LCD_H - py;
    if (w <= 0 || h <= 0) return;

    /* Render into strip buffer row by row in scale-row chunks */
    for (int sy = 0; sy < CHAR_H; sy++) {
        /* Build one scaled row into s_strip */
        for (int sx = 0; sx < CHAR_W; sx++) {
            uint8_t bits = glyph[sx];
            uint16_t color = (bits & (1 << sy)) ? fg : bg;
            for (int dx = 0; dx < scale; dx++) {
                s_strip[sx * scale + dx] = color;
            }
        }
        /* Duplicate the row for vertical scaling */
        for (int dy = 1; dy < scale; dy++) {
            memcpy(&s_strip[dy * w], s_strip, w * sizeof(uint16_t));
        }
        int row_y = py + sy * scale;
        if (row_y + scale > LCD_H) break;
        esp_lcd_panel_draw_bitmap(s_panel, px, row_y, px + w, row_y + scale, s_strip);
    }
}

/** Draw a string at pixel (px, py) with scale. Returns pixel X after last char. */
static int draw_str_px(int px, int py, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    int cw = CHAR_W * scale;
    while (*s) {
        if (px + cw > LCD_W) break;
        draw_char_scaled(px, py, *s++, fg, bg, scale);
        px += cw;
    }
    return px;
}

/** Fill a rectangular region with a solid colour. */
static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;

    /* Fill using s_strip in chunks of up to 16 rows */
    for (int i = 0; i < w * 16 && i < LCD_W * 16; i++) s_strip[i] = color;
    for (int row = 0; row < h; row += 16) {
        int rows = (row + 16 <= h) ? 16 : (h - row);
        esp_lcd_panel_draw_bitmap(s_panel, x, y + row, x + w, y + row + rows, s_strip);
    }
}

/** Draw a 1px horizontal line across the full width. */
static void draw_hline(int y, uint16_t color)
{
    for (int i = 0; i < LCD_W; i++) s_strip[i] = color;
    esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + 1, s_strip);
}

/** Clear from pixel y to bottom of screen. */
static void clear_below(int y)
{
    if (y < LCD_H) fill_rect(0, y, LCD_W, LCD_H - y, COLOR_BLACK);
}

/* ── Status bar ──────────────────────────────────────────────────────────── */

static void draw_status_bar(void)
{
    /* Clear status bar area */
    fill_rect(0, STATUS_Y, LCD_W, STATUS_H, COLOR_BLACK);

    /* Left side: BLE and HFP labels */
    int x = 4;
    if (s_ble_status[0]) {
        x = draw_str_px(x, STATUS_Y, s_ble_status, COLOR_CYAN, COLOR_BLACK, 2);
        x += CHAR_W * 2; /* gap */
    }
    if (s_hfp_status[0]) {
        draw_str_px(x, STATUS_Y, s_hfp_status, COLOR_GREEN, COLOR_BLACK, 2);
    }

    /* Right side: LOCKED or READY */
    const char *label = s_is_bricked ? "LOCKED" : "READY";
    uint16_t color = s_is_bricked ? COLOR_RED : COLOR_GREEN;
    int label_w = (int)strlen(label) * CHAR_W * 2;
    draw_str_px(LCD_W - label_w - 4, STATUS_Y, label, color, COLOR_BLACK, 2);
}

/* ── Day-of-week names ───────────────────────────────────────────────────── */
static const char *dow_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *month_names[] = {
    "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* ── Idle screen drawing ─────────────────────────────────────────────────── */

static void draw_clock(void)
{
    /* Clear clock area */
    fill_rect(0, CLOCK_Y, LCD_W, CLOCK_H, COLOR_BLACK);

    if (!s_time.valid) {
        /* No time synced yet — show placeholder */
        int pw = 7 * CHAR_W * 4; /* "--:--" = 5 chars at 4x */
        int px = (LCD_W - pw) / 2;
        draw_str_px(px, CLOCK_Y, "--:--", COLOR_GRAY, COLOR_BLACK, 4);
        return;
    }

    /* 12-hour format */
    int hour12 = s_time.hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char *ampm = (s_time.hour < 12) ? "AM" : "PM";

    char clock_str[8];
    snprintf(clock_str, sizeof(clock_str), "%2d:%02d", hour12, s_time.min);

    /* Centre the clock: "12:45" = 5 chars at 4x = 120px, + gap + "PM" at 2x = 24px */
    int clock_w = 5 * CHAR_W * 4;  /* 120px */
    int ampm_w = 2 * CHAR_W * 2;   /* 24px */
    int gap = CHAR_W * 2;          /* 12px gap */
    int total = clock_w + gap + ampm_w;
    int start_x = (LCD_W - total) / 2;

    draw_str_px(start_x, CLOCK_Y, clock_str, COLOR_WHITE, COLOR_BLACK, 4);
    draw_str_px(start_x + clock_w + gap, AMPM_Y, ampm, COLOR_GRAY, COLOR_BLACK, 2);
}

static void draw_date(void)
{
    fill_rect(0, DATE_Y, LCD_W, DATE_H, COLOR_BLACK);

    if (!s_time.valid) return;

    char date_str[24];
    const char *dow = (s_time.dow < 7) ? dow_names[s_time.dow] : "???";
    const char *mon = (s_time.month >= 1 && s_time.month <= 12) ? month_names[s_time.month] : "???";
    snprintf(date_str, sizeof(date_str), "%s, %s %d", dow, mon, s_time.day);

    int date_w = (int)strlen(date_str) * CHAR_W * 2;
    int start_x = (LCD_W - date_w) / 2;
    draw_str_px(start_x, DATE_Y, date_str, COLOR_GRAY, COLOR_BLACK, 2);
}

static void draw_content(void)
{
    /* Clear content area */
    clear_below(CONTENT_Y);

    int y = CONTENT_Y;
    int left_margin = 8;

    /* TO-DO items */
    for (int i = 0; i < MAX_TODOS; i++) {
        if (!s_todos[i].active) continue;
        if (y + CONTENT_LINE_H > LCD_H) break;

        char line[48];
        snprintf(line, sizeof(line), "[%c] %s",
                 s_todos[i].checked ? 'x' : ' ',
                 s_todos[i].text);
        uint16_t color = s_todos[i].checked ? COLOR_GRAY : COLOR_WHITE;
        draw_str_px(left_margin, y, line, color, COLOR_BLACK, 2);
        y += CONTENT_LINE_H;
    }

    /* Custom message */
    if (s_message[0]) {
        if (y + CONTENT_LINE_H <= LCD_H) {
            draw_str_px(left_margin, y, s_message, COLOR_YELLOW, COLOR_BLACK, 2);
        }
    }
}

static void draw_idle_screen(void)
{
    /* Clear below status bar */
    clear_below(DIV1_Y);

    draw_hline(DIV1_Y, COLOR_GRAY);
    draw_clock();
    draw_date();
    draw_hline(DIV2_Y, COLOR_GRAY);
    draw_content();
}

/* ── Notification timeout ────────────────────────────────────────────────── */

static void notif_timeout_cb(void *arg)
{
    UI_LOCK();
    if (s_screen == SCREEN_NOTIFICATION) {
        s_screen = SCREEN_IDLE;
        draw_idle_screen();
    }
    UI_UNLOCK();
}

/* ── Clock tick ──────────────────────────────────────────────────────────── */

static void advance_time(void)
{
    if (!s_time.valid) return;

    s_time.sec++;
    if (s_time.sec < 60) return;
    s_time.sec = 0;
    s_time.min++;
    if (s_time.min < 60) return;
    s_time.min = 0;
    s_time.hour++;
    if (s_time.hour < 24) return;
    s_time.hour = 0;
    /* Advance day — simplified, doesn't handle month/year rollover precisely
     * since we'll get a fresh TIME_SYNC from the app regularly */
    s_time.dow = (s_time.dow + 1) % 7;
    s_time.day++;
}

static void clock_tick_cb(void *arg)
{
    UI_LOCK();
    advance_time();

    /* Only redraw clock on idle screen */
    if (s_screen == SCREEN_IDLE) {
        draw_clock();
        /* Redraw date at midnight (when hour just rolled to 0 and min/sec are 0) */
        if (s_time.hour == 0 && s_time.min == 0 && s_time.sec == 0) {
            draw_date();
        }
    }
    UI_UNLOCK();
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void ui_init(void)
{
    /* SPI bus */
    spi_bus_config_t buscfg = {
        .mosi_io_num     = LCD_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = LCD_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI, &buscfg, SPI_DMA_CH_AUTO));

    /* Panel IO (SPI) */
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = LCD_DC,
        .cs_gpio_num       = LCD_CS,
        .pclk_hz           = LCD_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI, &io_cfg, &io));

    /* ST7789 panel */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num  = LCD_RST,
        .data_endian     = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel  = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_set_gap(s_panel, 0, 0);

    /* Landscape rotation: swap X/Y, then mirror X */
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, true, false);

    esp_lcd_panel_disp_on_off(s_panel, true);

    /* Backlight on */
    gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL, 1);

    /* Drawing mutex — must be created before timers start */
    s_ui_mutex = xSemaphoreCreateRecursiveMutex();
    assert(s_ui_mutex);

    /* Clock tick timer — 1 second periodic */
    esp_timer_create_args_t clock_args = {
        .callback = clock_tick_cb,
        .name     = "ui_clock",
    };
    ESP_ERROR_CHECK(esp_timer_create(&clock_args, &s_clock_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_clock_timer, 1000000)); /* 1s */

    /* Notification timeout timer — one-shot, created once, started per notification */
    esp_timer_create_args_t notif_args = {
        .callback = notif_timeout_cb,
        .name     = "ui_notif",
    };
    ESP_ERROR_CHECK(esp_timer_create(&notif_args, &s_notif_timer));

    ESP_LOGI(TAG, "ST7789 initialised (%dx%d landscape)", LCD_W, LCD_H);
}

void ui_fill(uint16_t color)
{
    UI_LOCK();
    for (int i = 0; i < LCD_W * 16; i++) s_strip[i] = color;
    for (int y = 0; y < LCD_H; y += 16) {
        int h = (y + 16 <= LCD_H) ? 16 : (LCD_H - y);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + h, s_strip);
    }
    UI_UNLOCK();
}

void ui_set_status_ble(const char *text)
{
    UI_LOCK();
    strncpy(s_ble_status, text, sizeof(s_ble_status) - 1);
    s_ble_status[sizeof(s_ble_status) - 1] = '\0';
    draw_status_bar();
    UI_UNLOCK();
}

void ui_set_status_hfp(const char *text)
{
    UI_LOCK();
    strncpy(s_hfp_status, text, sizeof(s_hfp_status) - 1);
    s_hfp_status[sizeof(s_hfp_status) - 1] = '\0';
    draw_status_bar();
    UI_UNLOCK();
}

void ui_show_idle(void)
{
    UI_LOCK();
    s_screen = SCREEN_IDLE;
    s_is_bricked = false;
    ui_fill(COLOR_BLACK);
    draw_status_bar();
    draw_idle_screen();
    UI_UNLOCK();
}

void ui_show_bricked(void)
{
    UI_LOCK();
    s_screen = SCREEN_BRICKED;
    s_is_bricked = true;
    ui_fill(COLOR_BLACK);
    draw_status_bar();

    /* Large "LOCKED" centred below status bar */
    clear_below(DIV1_Y);
    draw_hline(DIV1_Y, COLOR_RED);

    const char *msg = "LOCKED";
    int msg_w = (int)strlen(msg) * CHAR_W * 4;
    int cx = (LCD_W - msg_w) / 2;
    int cy = 80;
    draw_str_px(cx, cy, msg, COLOR_RED, COLOR_BLACK, 4);

    /* Hint text */
    const char *hint = "Hold button to unbrick";
    int hw = (int)strlen(hint) * CHAR_W * 2;
    draw_str_px((LCD_W - hw) / 2, 130, hint, COLOR_GRAY, COLOR_BLACK, 2);
    UI_UNLOCK();
}

void ui_show_notification(const char *category, const char *title, const char *message)
{
    UI_LOCK();
    s_screen = SCREEN_NOTIFICATION;

    /* Keep status bar, clear below */
    clear_below(DIV1_Y);
    draw_hline(DIV1_Y, COLOR_YELLOW);

    /* Category label at 2x */
    draw_str_px(8, DIV1_Y + 4, category, COLOR_YELLOW, COLOR_BLACK, 2);

    /* Title at 2x */
    int title_y = DIV1_Y + 24;
    draw_str_px(8, title_y, title, COLOR_WHITE, COLOR_BLACK, 2);

    /* Message body at 2x, word-wrapped */
    int max_chars = (LCD_W - 16) / (CHAR_W * 2);
    int msg_len = (int)strlen(message);
    int y = title_y + 24;
    int pos = 0;
    char line[54];  /* max_chars + 1 */
    while (pos < msg_len && y + 16 < LCD_H) {
        int take = msg_len - pos;
        if (take > max_chars) take = max_chars;
        memcpy(line, message + pos, take);
        line[take] = '\0';
        draw_str_px(8, y, line, COLOR_WHITE, COLOR_BLACK, 2);
        y += 18;
        pos += take;
    }

    /* Start 8-second auto-dismiss timer */
    esp_timer_stop(s_notif_timer);  /* safe even if not running */
    esp_timer_start_once(s_notif_timer, 8000000);  /* 8s */
    UI_UNLOCK();
}

void ui_show_incoming_call(const char *caller)
{
    UI_LOCK();
    s_screen = SCREEN_INCOMING_CALL;

    /* Stop notification timer if it was running */
    esp_timer_stop(s_notif_timer);

    /* Keep status bar, clear below */
    clear_below(DIV1_Y);
    draw_hline(DIV1_Y, COLOR_GREEN);

    const char *label = "INCOMING CALL";
    int lw = (int)strlen(label) * CHAR_W * 2;
    draw_str_px((LCD_W - lw) / 2, DIV1_Y + 8, label, COLOR_GREEN, COLOR_BLACK, 2);

    /* Caller ID */
    const char *name = (caller && caller[0]) ? caller : "Unknown";
    int nw = (int)strlen(name) * CHAR_W * 2;
    draw_str_px((LCD_W - nw) / 2, DIV1_Y + 40, name, COLOR_WHITE, COLOR_BLACK, 2);

    /* Hint */
    const char *hint = "Press button to answer";
    int hw = (int)strlen(hint) * CHAR_W * 2;
    draw_str_px((LCD_W - hw) / 2, DIV1_Y + 80, hint, COLOR_YELLOW, COLOR_BLACK, 2);
    UI_UNLOCK();
}

void ui_show_call_active(void)
{
    UI_LOCK();
    s_screen = SCREEN_CALL_ACTIVE;

    esp_timer_stop(s_notif_timer);

    clear_below(DIV1_Y);
    draw_hline(DIV1_Y, COLOR_GREEN);

    const char *label = "CALL ACTIVE";
    int lw = (int)strlen(label) * CHAR_W * 2;
    draw_str_px((LCD_W - lw) / 2, DIV1_Y + 40, label, COLOR_GREEN, COLOR_BLACK, 2);

    const char *hint = "Press button to hang up";
    int hw = (int)strlen(hint) * CHAR_W * 2;
    draw_str_px((LCD_W - hw) / 2, DIV1_Y + 80, hint, COLOR_YELLOW, COLOR_BLACK, 2);
    UI_UNLOCK();
}

/* ── Display data setters ────────────────────────────────────────────────── */

void ui_set_time(uint8_t hour, uint8_t min, uint8_t sec,
                 uint8_t dow, uint16_t year, uint8_t month, uint8_t day)
{
    UI_LOCK();
    s_time.hour  = hour;
    s_time.min   = min;
    s_time.sec   = sec;
    s_time.dow   = dow;
    s_time.year  = year;
    s_time.month = month;
    s_time.day   = day;
    s_time.valid = true;

    ESP_LOGI(TAG, "Time synced: %02d:%02d:%02d %s %s %d %d",
             hour, min, sec, dow_names[dow % 7],
             month_names[month <= 12 ? month : 0], day, year);

    if (s_screen == SCREEN_IDLE) {
        draw_clock();
        draw_date();
    }
    UI_UNLOCK();
}

void ui_set_todo(uint8_t index, bool checked, const char *text)
{
    if (index >= MAX_TODOS) return;
    UI_LOCK();
    s_todos[index].active  = true;
    s_todos[index].checked = checked;
    strncpy(s_todos[index].text, text, TODO_TEXT_LEN);
    s_todos[index].text[TODO_TEXT_LEN] = '\0';
    if (s_screen == SCREEN_IDLE) draw_content();
    UI_UNLOCK();
}

void ui_clear_todos(void)
{
    UI_LOCK();
    memset(s_todos, 0, sizeof(s_todos));
    if (s_screen == SCREEN_IDLE) draw_content();
    UI_UNLOCK();
}

void ui_set_message(const char *text)
{
    UI_LOCK();
    strncpy(s_message, text, MSG_TEXT_LEN);
    s_message[MSG_TEXT_LEN] = '\0';
    if (s_screen == SCREEN_IDLE) draw_content();
    UI_UNLOCK();
}

void ui_clear_message(void)
{
    UI_LOCK();
    s_message[0] = '\0';
    if (s_screen == SCREEN_IDLE) draw_content();
    UI_UNLOCK();
}
