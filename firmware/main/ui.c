/**
 * ui.c — LVGL-based display driver for Focus Pager
 *
 * ST7789 320x240 landscape via esp_lcd + esp_lvgl_port.
 * All public functions are mutex-safe (lvgl_port_lock/unlock).
 *
 * Pin assignments:
 *   DIN (MOSI) → GPIO23    CLK  → GPIO18
 *   CS         → GPIO5     DC   → GPIO4
 *   RST        → GPIO2     BL   → GPIO21
 *
 * Screen / page model:
 *   Page 0            — home screen  (clock right, todos left)
 *   Pages 1 .. N      — notification thread list (2 per page)
 *   Thread detail     — full message history for one sender
 *   Call / Bricked    — override screens, navigation suspended
 *
 * Encoder navigation (see ui_navigate / ui_encoder_click):
 *   Turn CW  → page + 1  (older threads)
 *   Turn CCW → page - 1  (back toward home)
 *   Click on thread card → thread detail view
 *   Click in thread detail → back to thread list
 */

#include "ui.h"
#include "notif_store.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#define TAG "UI"

/* ── Pin definitions ─────────────────────────────────────────────────────── */
#define LCD_MOSI    23
#define LCD_CLK     18
#define LCD_CS       5
#define LCD_DC       4
#define LCD_RST      2
#define LCD_BL      21

/* ── Display geometry ────────────────────────────────────────────────────── */
#define LCD_W        320
#define LCD_H        240
#define LCD_SPI      SPI2_HOST
#define LCD_HZ       (40 * 1000 * 1000)

/* ── LVGL draw buffer ────────────────────────────────────────────────────── */
#define DRAW_BUF_LINES  10

/* ── Colours ─────────────────────────────────────────────────────────────── */
#define C_WHITE    lv_color_hex(0xFFFFFF)
#define C_GRAY     lv_color_hex(0x888888)
#define C_DIM      lv_color_hex(0x444444)
#define C_BLACK    lv_color_hex(0x000000)
#define C_DIMRED   lv_color_hex(0x882222)
#define C_GREEN    lv_color_hex(0x00CC88)
#define C_ACCENT   lv_color_hex(0x4488FF)

/* ── Layout constants ────────────────────────────────────────────────────── */
#define STATUS_H     20   /* status bar height */
#define H_PAD        10   /* standard horizontal padding */
#define COL_W        (LCD_W / 2)   /* 160px — each half-screen column */

/* Home screen — left column (todos) */
#define TODO_HEADER_Y  (STATUS_H + 8)    /* "TASKS" label top */
#define TODO_START_Y   (TODO_HEADER_Y + 20) /* first todo item top */
#define TODO_LINE_H    20                /* height per todo row */

/* Home screen — right column (clock) */
/* Right column center-x from screen center = COL_W/2 = 80 */
#define CLOCK_X_OFS    (COL_W / 2)      /* 80 — offset for LV_ALIGN_TOP_MID */
#define CLOCK_Y         86              /* top of clock label */
#define DATE_Y         132              /* top of date label */
#define HINT_Y         178              /* scroll hint below date */

/* Notification detail — two half-cards stacked vertically */
#define DETAIL_MARGIN       4
#define DETAIL_BORDER       3
#define DETAIL_PAD          8   /* internal padding inside each half-card */
#define DETAIL_CARD_X       DETAIL_MARGIN
#define DETAIL_CARD_W      (LCD_W - DETAIL_MARGIN * 2)   /* 312 */
/* (LCD_H - status - top_margin - gap - bottom_margin) / 2 = (240-20-12)/2 = 104 */
#define DETAIL_CARD_H      ((LCD_H - STATUS_H - DETAIL_MARGIN * 3) / 2)
#define DETAIL_CARD0_Y     (STATUS_H + DETAIL_MARGIN)
#define DETAIL_CARD1_Y     (DETAIL_CARD0_Y + DETAIL_CARD_H + DETAIL_MARGIN)
/* Usable content width inside a card */
#define DETAIL_BODY_W      (DETAIL_CARD_W - DETAIL_PAD * 2)
/* Title width (leaves 58px for timestamp on right) */
#define DETAIL_TITLE_W     (DETAIL_BODY_W - 18 - 58)

/* Thread detail — header + content area */
#define TD_HEADER_Y    (STATUS_H + 4)   /* header row top */
#define TD_DIVIDER_Y   (STATUS_H + 20)  /* horizontal divider */
#define TD_CONTENT_Y   (TD_DIVIDER_Y + 2)
#define TD_CONTENT_H   (LCD_H - TD_CONTENT_Y)
/* Buffer for formatted message history (newest-first) */
#define TD_BUF_SIZE    (NS_MAX_MSG_PER_THREAD * (NS_TS_LEN + NS_MSG_LEN + 8))

/* ── Screen state ────────────────────────────────────────────────────────── */
typedef enum {
    SCREEN_IDLE,
    SCREEN_BRICKED,
    SCREEN_INCOMING_CALL,
    SCREEN_CALL_ACTIVE,
    SCREEN_NOTIF_DETAIL,
    SCREEN_THREAD_DETAIL,
} screen_state_t;

static screen_state_t s_screen = SCREEN_IDLE;
static int            s_current_page = 0;  /* 0 = home, 1..N = thread list page */

/* ── Time state ──────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  hour, min, sec;
    uint8_t  dow;
    uint16_t year;
    uint8_t  month, day;
    bool     valid;
} pager_time_t;

static pager_time_t s_time = {0};

/* ── Status state ────────────────────────────────────────────────────────── */
static char s_ble_status[16] = "";

/* ── LVGL display handle ─────────────────────────────────────────────────── */
static lv_display_t *s_disp = NULL;

/* ── LVGL screen objects ─────────────────────────────────────────────────── */

/* Screens */
static lv_obj_t *s_scr_idle          = NULL;
static lv_obj_t *s_scr_bricked       = NULL;
static lv_obj_t *s_scr_call          = NULL;
static lv_obj_t *s_scr_notif_detail  = NULL;
static lv_obj_t *s_scr_thread_detail = NULL;

/* Home screen */
static lv_obj_t *s_lbl_status     = NULL;   /* status bar */
static lv_obj_t *s_lbl_clock      = NULL;   /* "10:34 AM" in right col */
static lv_obj_t *s_lbl_clock_date = NULL;   /* "Sat Jun 21" below clock */
static lv_obj_t *s_lbl_hint         = NULL;   /* pulsing scroll hint */
static lv_obj_t *s_lbl_unread_badge = NULL;   /* top-right unread dot + count */
static bool      s_hint_on          = false;  /* current pulse state */

/* Todo list — up to MAX_TODOS items in left column */
#define MAX_TODOS     5
#define TODO_TEXT_LEN 47
typedef struct { bool active; bool checked; char text[TODO_TEXT_LEN + 1]; } todo_item_t;
static todo_item_t  s_todos[MAX_TODOS];
static lv_obj_t    *s_lbl_todos[MAX_TODOS];

/* Notification detail screen — two half-cards stacked */
static lv_obj_t *s_detail_status    = NULL;
static lv_obj_t *s_detail_pager     = NULL;   /* "2/5" top-right  */
static lv_obj_t *s_card[2]          = {NULL, NULL};
static lv_obj_t *s_card_icon[2]     = {NULL, NULL};
static lv_obj_t *s_card_title[2]    = {NULL, NULL};
static lv_obj_t *s_card_ts[2]       = {NULL, NULL};
static lv_obj_t *s_card_body[2]     = {NULL, NULL};
static lv_obj_t *s_card_unread[2]   = {NULL, NULL};  /* unread count badge */

/* Thread detail screen */
static lv_obj_t *s_td_status  = NULL;  /* status bar */
static lv_obj_t *s_td_back    = NULL;  /* "< back" hint */
static lv_obj_t *s_td_sender  = NULL;  /* sender name header */
static lv_obj_t *s_td_icon    = NULL;  /* category icon */
static lv_obj_t *s_td_content = NULL;  /* message history label */

/* Call screen */
static lv_obj_t *s_call_icon   = NULL;
static lv_obj_t *s_call_label  = NULL;
static lv_obj_t *s_call_name   = NULL;
static lv_obj_t *s_call_hint   = NULL;
static lv_obj_t *s_call_status = NULL;

/* Bricked screen */
static lv_obj_t *s_lbl_bricked      = NULL;
static lv_obj_t *s_lbl_bricked_hint = NULL;

/* ── Name tables ─────────────────────────────────────────────────────────── */
static const char *dow_names[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *month_names[] = {
    "","Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

/* ════════════════════════════════════════════════════════════════════════════
   Internal helpers
   ════════════════════════════════════════════════════════════════════════════ */

static void style_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, C_BLACK, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent,
                             const lv_font_t *font,
                             lv_color_t color,
                             lv_align_t align,
                             int x_ofs, int y_ofs,
                             const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
    lv_obj_align(lbl, align, x_ofs, y_ofs);
    lv_label_set_text(lbl, text);
    return lbl;
}

static void update_status_label(lv_obj_t *lbl)
{
    if (!lbl) return;
    char buf[64];
    if (s_time.valid) {
        int h = s_time.hour % 12;
        if (h == 0) h = 12;
        snprintf(buf, sizeof(buf), "%d:%02d %s",
                 h, s_time.min, s_time.hour < 12 ? "AM" : "PM");
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    if (s_ble_status[0]) {
        size_t used = strlen(buf);
        snprintf(buf + used, sizeof(buf) - used,
                 "  " LV_SYMBOL_BLUETOOTH " %s", s_ble_status);
    }
    lv_label_set_text(lbl, buf);
}

/* Update the large clock and date labels — call inside lvgl_port_lock */
static void redraw_clock(void)
{
    if (!s_lbl_clock) return;

    char time_buf[16];
    if (s_time.valid) {
        int h = s_time.hour % 12;
        if (h == 0) h = 12;
        snprintf(time_buf, sizeof(time_buf), "%d:%02d %s",
                 h, s_time.min, s_time.hour < 12 ? "AM" : "PM");
    } else {
        snprintf(time_buf, sizeof(time_buf), "--:--");
    }
    lv_label_set_text(s_lbl_clock, time_buf);

    if (s_lbl_clock_date) {
        if (s_time.valid) {
            char date_buf[20];
            snprintf(date_buf, sizeof(date_buf), "%s %s %d",
                     dow_names[s_time.dow % 7],
                     month_names[s_time.month <= 12 ? s_time.month : 0],
                     s_time.day);
            lv_label_set_text(s_lbl_clock_date, date_buf);
        } else {
            lv_label_set_text(s_lbl_clock_date, "");
        }
    }
}

static void redraw_todos(void)
{
    for (int i = 0; i < MAX_TODOS; i++) {
        if (!s_lbl_todos[i]) continue;
        if (s_todos[i].active) {
            char line[TODO_TEXT_LEN + 8];
            snprintf(line, sizeof(line), "%s %s",
                     s_todos[i].checked ? LV_SYMBOL_OK : LV_SYMBOL_BULLET,
                     s_todos[i].text);
            lv_label_set_text(s_lbl_todos[i], line);
            lv_obj_set_style_text_color(s_lbl_todos[i],
                s_todos[i].checked ? C_DIM : C_GRAY, 0);
            lv_obj_remove_flag(s_lbl_todos[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_todos[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* Forward declaration */
static const char *category_icon(const char *cat);

/* Refresh the top-right unread badge on the idle screen.
 * Safe to call from any screen — updates will render when idle screen shows. */
static void redraw_unread_badge(void)
{
    if (!s_lbl_unread_badge) return;
    int n = notif_store_total_unread();
    if (n > 0) {
        char buf[20];
        snprintf(buf, sizeof(buf), LV_SYMBOL_BULLET " %d", n);
        lv_label_set_text(s_lbl_unread_badge, buf);
        lv_obj_remove_flag(s_lbl_unread_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_lbl_unread_badge, LV_OBJ_FLAG_HIDDEN);
    }
}

static void capture_timestamp(char *buf, size_t sz)
{
    if (!s_time.valid) { buf[0] = '\0'; return; }
    int h = s_time.hour % 12;
    if (h == 0) h = 12;
    snprintf(buf, sz, "%d:%02d %s",
             h, s_time.min, s_time.hour < 12 ? "AM" : "PM");
}

static const char *category_icon(const char *cat)
{
    if (!cat) return LV_SYMBOL_BELL;
    if (strcmp(cat, "IncomingCall") == 0) return LV_SYMBOL_CALL;
    if (strcmp(cat, "MissedCall")   == 0) return LV_SYMBOL_CALL;
    if (strcmp(cat, "Voicemail")    == 0) return LV_SYMBOL_VOLUME_MAX;
    if (strcmp(cat, "Email")        == 0) return LV_SYMBOL_ENVELOPE;
    if (strcmp(cat, "Social")       == 0) return LV_SYMBOL_BELL;
    if (strcmp(cat, "Schedule")     == 0) return LV_SYMBOL_OK;
    if (strcmp(cat, "News")         == 0) return LV_SYMBOL_FILE;
    if (strcmp(cat, "Location")     == 0) return LV_SYMBOL_GPS;
    return LV_SYMBOL_BELL;
}

static void format_phone_number(char *out, size_t outsz, const char *raw)
{
    if (!raw || !raw[0]) { snprintf(out, outsz, "Unknown"); return; }
    char digits[16] = "";
    size_t nd = 0;
    for (const char *p = raw; *p && nd < sizeof(digits) - 1; p++) {
        if (*p >= '0' && *p <= '9') digits[nd++] = *p;
    }
    digits[nd] = '\0';
    const char *d = digits;
    if (nd == 11 && d[0] == '1') { d++; nd = 10; }
    if (nd == 10) {
        snprintf(out, outsz, "(%c%c%c) %c%c%c-%c%c%c%c",
                 d[0],d[1],d[2], d[3],d[4],d[5], d[6],d[7],d[8],d[9]);
    } else {
        snprintf(out, outsz, "%s", raw);
    }
}

/* Replace iOS smart-quotes / typographic punctuation with ASCII equivalents
 * so LVGL's Montserrat bitmap font (basic Latin only) can render them.
 * Covers the most common sequences seen in iMessage/iOS push payloads. */
static void sanitize_utf8(char *dst, size_t dstsz, const char *src)
{
    size_t di = 0;
    while (*src && di < dstsz - 1) {
        unsigned char c0 = (unsigned char)src[0];
        /* 3-byte UTF-8 in General Punctuation block: E2 80 XX */
        if (c0 == 0xE2 &&
            (unsigned char)src[1] == 0x80 &&
            src[2] != '\0') {
            unsigned char c2 = (unsigned char)src[2];
            const char *rep = NULL;
            switch (c2) {
                case 0x98: case 0x99:
                case 0x9A: case 0x9B: rep = "'";   break; /* ' '  ‚ ‛ → ' */
                case 0x9C: case 0x9D: rep = "\"";  break; /* " "       → " */
                case 0x93: case 0x94: rep = "-";   break; /* – —        → - */
                case 0xA6:            rep = "..."; break; /* …          → ... */
                default: src += 3; continue;               /* skip unknown   */
            }
            src += 3;
            while (*rep && di < dstsz - 1) dst[di++] = *rep++;
            continue;
        }
        /* Skip any other non-ASCII multi-byte sequence */
        if (c0 >= 0x80) {
            int skip = (c0 >= 0xF0) ? 4 : (c0 >= 0xE0) ? 3 : 2;
            src += skip;
            continue;
        }
        dst[di++] = (char)c0;
        src++;
    }
    dst[di] = '\0';
}

/* ════════════════════════════════════════════════════════════════════════════
   Screen builders
   ════════════════════════════════════════════════════════════════════════════ */

static void build_idle_screen(void)
{
    s_scr_idle = lv_obj_create(NULL);
    style_screen(s_scr_idle);

    /* ── Status bar ── */
    s_lbl_status = make_label(s_scr_idle,
        &lv_font_montserrat_12, C_GRAY,
        LV_ALIGN_TOP_LEFT, H_PAD, 4, "");

    /* Unread notification badge — top-right, accent dot + count */
    s_lbl_unread_badge = make_label(s_scr_idle,
        &lv_font_montserrat_12, C_ACCENT,
        LV_ALIGN_TOP_RIGHT, -H_PAD, 4, "");
    lv_obj_add_flag(s_lbl_unread_badge, LV_OBJ_FLAG_HIDDEN);

    /* ── Vertical divider between columns ── */
    lv_obj_t *vdiv = lv_obj_create(s_scr_idle);
    lv_obj_set_pos(vdiv, COL_W, STATUS_H);
    lv_obj_set_size(vdiv, 1, LCD_H - STATUS_H);
    lv_obj_set_style_bg_color(vdiv, C_DIM, 0);
    lv_obj_set_style_bg_opa(vdiv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vdiv, 0, 0);
    lv_obj_set_style_pad_all(vdiv, 0, 0);
    lv_obj_clear_flag(vdiv, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Left column: todos ── */

    /* "TASKS" header */
    make_label(s_scr_idle, &lv_font_montserrat_12, C_DIM,
        LV_ALIGN_TOP_LEFT, H_PAD, TODO_HEADER_Y, "TASKS");

    /* Todo item labels */
    for (int i = 0; i < MAX_TODOS; i++) {
        s_lbl_todos[i] = lv_label_create(s_scr_idle);
        lv_obj_set_style_text_font(s_lbl_todos[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_lbl_todos[i], C_GRAY, 0);
        lv_obj_set_style_bg_opa(s_lbl_todos[i], LV_OPA_TRANSP, 0);
        lv_obj_align(s_lbl_todos[i], LV_ALIGN_TOP_LEFT,
                     H_PAD, TODO_START_Y + i * TODO_LINE_H);
        lv_label_set_long_mode(s_lbl_todos[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_lbl_todos[i], COL_W - H_PAD - 4);
        lv_obj_add_flag(s_lbl_todos[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* ── Right column: clock + date ──
     * LV_ALIGN_TOP_MID + x_ofs = CLOCK_X_OFS (80) centres objects in the
     * right half: parent centre (160) + 80 = 240 = mid of 160..319. */

    s_lbl_clock = make_label(s_scr_idle,
        &lv_font_montserrat_32, C_WHITE,
        LV_ALIGN_TOP_MID, CLOCK_X_OFS, CLOCK_Y, "--:--");
    lv_obj_set_style_text_align(s_lbl_clock, LV_TEXT_ALIGN_CENTER, 0);

    s_lbl_clock_date = make_label(s_scr_idle,
        &lv_font_montserrat_12, C_GRAY,
        LV_ALIGN_TOP_MID, CLOCK_X_OFS, DATE_Y, "");
    lv_obj_set_style_text_align(s_lbl_clock_date, LV_TEXT_ALIGN_CENTER, 0);

    /* ── Scroll hint — pulses when notifications are waiting ── */
    s_lbl_hint = make_label(s_scr_idle,
        &lv_font_montserrat_14, C_ACCENT,
        LV_ALIGN_TOP_MID, CLOCK_X_OFS, HINT_Y, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_align(s_lbl_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
}

static void build_half_card(int ci, int y)
{
    s_card[ci] = lv_obj_create(s_scr_notif_detail);
    lv_obj_set_pos(s_card[ci], DETAIL_CARD_X, y);
    lv_obj_set_size(s_card[ci], DETAIL_CARD_W, DETAIL_CARD_H);
    lv_obj_set_style_bg_color(s_card[ci], C_BLACK, 0);
    lv_obj_set_style_bg_opa(s_card[ci], LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_card[ci], C_DIM, 0);
    lv_obj_set_style_border_width(s_card[ci], DETAIL_BORDER, 0);
    lv_obj_set_style_radius(s_card[ci], 6, 0);
    lv_obj_set_style_pad_all(s_card[ci], 0, 0);
    lv_obj_set_scrollbar_mode(s_card[ci], LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_card[ci], LV_OBJ_FLAG_SCROLLABLE);

    s_card_icon[ci] = lv_label_create(s_card[ci]);
    lv_obj_set_style_text_font(s_card_icon[ci], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_card_icon[ci], C_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_card_icon[ci], LV_OPA_TRANSP, 0);
    lv_obj_set_pos(s_card_icon[ci], DETAIL_PAD, DETAIL_PAD);
    lv_label_set_text(s_card_icon[ci], LV_SYMBOL_BELL);

    s_card_title[ci] = lv_label_create(s_card[ci]);
    lv_obj_set_style_text_font(s_card_title[ci], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_card_title[ci], C_WHITE, 0);
    lv_obj_set_style_bg_opa(s_card_title[ci], LV_OPA_TRANSP, 0);
    lv_obj_set_pos(s_card_title[ci], DETAIL_PAD + 18, DETAIL_PAD + 1);
    lv_obj_set_width(s_card_title[ci], DETAIL_TITLE_W);
    lv_label_set_long_mode(s_card_title[ci], LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_card_title[ci], "");

    s_card_ts[ci] = lv_label_create(s_card[ci]);
    lv_obj_set_style_text_font(s_card_ts[ci], &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_card_ts[ci], C_DIM, 0);
    lv_obj_set_style_bg_opa(s_card_ts[ci], LV_OPA_TRANSP, 0);
    lv_obj_align(s_card_ts[ci], LV_ALIGN_TOP_RIGHT, -DETAIL_PAD, DETAIL_PAD + 2);
    lv_label_set_text(s_card_ts[ci], "");

    s_card_body[ci] = lv_label_create(s_card[ci]);
    lv_obj_set_style_text_font(s_card_body[ci], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_card_body[ci], C_GRAY, 0);
    lv_obj_set_style_bg_opa(s_card_body[ci], LV_OPA_TRANSP, 0);
    lv_obj_set_pos(s_card_body[ci], DETAIL_PAD, DETAIL_PAD + 22);
    lv_obj_set_size(s_card_body[ci], DETAIL_BODY_W,
                    DETAIL_CARD_H - DETAIL_PAD * 2 - 22);
    lv_label_set_long_mode(s_card_body[ci], LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_card_body[ci], "");

    /* Unread count badge — bottom-right corner of card */
    s_card_unread[ci] = lv_label_create(s_card[ci]);
    lv_obj_set_style_text_font(s_card_unread[ci], &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_card_unread[ci], C_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_card_unread[ci], LV_OPA_TRANSP, 0);
    lv_obj_align(s_card_unread[ci], LV_ALIGN_BOTTOM_RIGHT, -DETAIL_PAD, -DETAIL_PAD + 2);
    lv_label_set_text(s_card_unread[ci], "");
    lv_obj_add_flag(s_card_unread[ci], LV_OBJ_FLAG_HIDDEN);
}

static void build_notif_detail_screen(void)
{
    s_scr_notif_detail = lv_obj_create(NULL);
    style_screen(s_scr_notif_detail);

    s_detail_status = make_label(s_scr_notif_detail,
        &lv_font_montserrat_12, C_GRAY,
        LV_ALIGN_TOP_LEFT, H_PAD, 4, "");

    s_detail_pager = make_label(s_scr_notif_detail,
        &lv_font_montserrat_12, C_DIM,
        LV_ALIGN_TOP_RIGHT, -H_PAD, 4, "");

    build_half_card(0, DETAIL_CARD0_Y);
    build_half_card(1, DETAIL_CARD1_Y);
}

static void build_thread_detail_screen(void)
{
    s_scr_thread_detail = lv_obj_create(NULL);
    style_screen(s_scr_thread_detail);

    /* Status bar */
    s_td_status = make_label(s_scr_thread_detail,
        &lv_font_montserrat_12, C_GRAY,
        LV_ALIGN_TOP_LEFT, H_PAD, 4, "");

    /* Header row */
    s_td_back = make_label(s_scr_thread_detail,
        &lv_font_montserrat_12, C_ACCENT,
        LV_ALIGN_TOP_LEFT, H_PAD, TD_HEADER_Y,
        LV_SYMBOL_LEFT " back");

    s_td_icon = make_label(s_scr_thread_detail,
        &lv_font_montserrat_14, C_ACCENT,
        LV_ALIGN_TOP_RIGHT, -H_PAD, TD_HEADER_Y, LV_SYMBOL_BELL);

    s_td_sender = lv_label_create(s_scr_thread_detail);
    lv_obj_set_style_text_font(s_td_sender, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_td_sender, C_WHITE, 0);
    lv_obj_set_style_bg_opa(s_td_sender, LV_OPA_TRANSP, 0);
    lv_obj_align(s_td_sender, LV_ALIGN_TOP_MID, 0, TD_HEADER_Y);
    lv_obj_set_width(s_td_sender, LCD_W - 110);
    lv_label_set_long_mode(s_td_sender, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_td_sender, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_td_sender, "");

    /* Horizontal divider below header */
    lv_obj_t *hdiv = lv_obj_create(s_scr_thread_detail);
    lv_obj_set_pos(hdiv, 0, TD_DIVIDER_Y);
    lv_obj_set_size(hdiv, LCD_W, 1);
    lv_obj_set_style_bg_color(hdiv, C_DIM, 0);
    lv_obj_set_style_bg_opa(hdiv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdiv, 0, 0);
    lv_obj_set_style_pad_all(hdiv, 0, 0);
    lv_obj_clear_flag(hdiv, LV_OBJ_FLAG_SCROLLABLE);

    /* Message history — wrapping label, clipped by screen boundary */
    s_td_content = lv_label_create(s_scr_thread_detail);
    lv_obj_set_style_text_font(s_td_content, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_td_content, C_GRAY, 0);
    lv_obj_set_style_bg_opa(s_td_content, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(s_td_content, H_PAD, TD_CONTENT_Y + 4);
    lv_obj_set_size(s_td_content, LCD_W - H_PAD * 2, TD_CONTENT_H - 4);
    lv_label_set_long_mode(s_td_content, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_td_content, "");
}

static void build_bricked_screen(void)
{
    s_scr_bricked = lv_obj_create(NULL);
    style_screen(s_scr_bricked);

    s_lbl_bricked = make_label(s_scr_bricked,
        &lv_font_montserrat_32, C_DIMRED,
        LV_ALIGN_CENTER, 0, -20, "locked");

    s_lbl_bricked_hint = make_label(s_scr_bricked,
        &lv_font_montserrat_12, C_DIM,
        LV_ALIGN_CENTER, 0, 24, "hold to unbrick");
}

static void build_call_screen(void)
{
    s_scr_call = lv_obj_create(NULL);
    style_screen(s_scr_call);

    s_call_status = make_label(s_scr_call,
        &lv_font_montserrat_12, C_GRAY,
        LV_ALIGN_TOP_LEFT, 10, 8, "");

    s_call_icon = make_label(s_scr_call,
        &lv_font_montserrat_48, C_GREEN,
        LV_ALIGN_CENTER, 0, -60, LV_SYMBOL_CALL);
    lv_obj_set_style_text_align(s_call_icon, LV_TEXT_ALIGN_CENTER, 0);

    s_call_label = make_label(s_scr_call,
        &lv_font_montserrat_12, C_GRAY,
        LV_ALIGN_CENTER, 0, -8, "incoming call");
    lv_obj_set_style_text_align(s_call_label, LV_TEXT_ALIGN_CENTER, 0);

    s_call_name = make_label(s_scr_call,
        &lv_font_montserrat_24, C_WHITE,
        LV_ALIGN_CENTER, 0, 24, "");
    lv_label_set_long_mode(s_call_name, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_call_name, LCD_W - 20);
    lv_obj_set_style_text_align(s_call_name, LV_TEXT_ALIGN_CENTER, 0);

    s_call_hint = make_label(s_scr_call,
        &lv_font_montserrat_12, C_DIM,
        LV_ALIGN_BOTTOM_MID, 0, -12, "press to answer");
    lv_obj_set_style_text_align(s_call_hint, LV_TEXT_ALIGN_CENTER, 0);
}

/* ════════════════════════════════════════════════════════════════════════════
   Navigation helpers
   ════════════════════════════════════════════════════════════════════════════ */

/* Fill one half-card with the latest message from a thread.
 * Call inside lvgl_port_lock. */
static void populate_card(int ci, const ns_thread_t *t, bool highlighted)
{
    lv_color_t border_col = highlighted ? C_ACCENT : C_DIM;
    lv_color_t icon_col   = highlighted ? C_ACCENT : C_DIM;
    lv_obj_set_style_border_color(s_card[ci], border_col, 0);
    lv_obj_set_style_text_color(s_card_icon[ci], icon_col, 0);
    lv_label_set_text(s_card_icon[ci],  category_icon(t->cat));
    lv_label_set_text(s_card_title[ci], t->sender[0] ? t->sender : t->cat);
    lv_label_set_text(s_card_ts[ci],    t->count > 0 ? t->msgs[0].ts : "");
    lv_label_set_text(s_card_body[ci],
                      (t->count > 0 && t->msgs[0].text[0])
                      ? t->msgs[0].text : " ");

    /* Unread badge */
    if (t->unread > 0) {
        char ubuf[12];
        snprintf(ubuf, sizeof(ubuf), "%d", t->unread);
        lv_label_set_text(s_card_unread[ci], ubuf);
        lv_obj_remove_flag(s_card_unread[ci], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_card_unread[ci], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_remove_flag(s_card[ci], LV_OBJ_FLAG_HIDDEN);
}

/* Load the thread-list detail screen for the given 1-based page.
 *
 * Newest thread always at the top (highlighted); the next older thread
 * is shown below it (dimmed) for context:
 *
 *   page 1  →  top = thread[0] (highlighted), bottom = thread[1] (dimmed)
 *   page 2  →  top = thread[1] (highlighted), bottom = thread[2] (dimmed)
 *   page N  →  top = thread[N-1] (highlighted), bottom hidden if no thread[N]
 *
 * card[0] = top half, card[1] = bottom half.
 * Call inside lvgl_port_lock. */
static void load_notif_detail_locked(int page)
{
    int total = notif_store_count();
    update_status_label(s_detail_status);

    char pager_buf[16];
    snprintf(pager_buf, sizeof(pager_buf), "%d/%d", page, total);
    lv_label_set_text(s_detail_pager, pager_buf);

    /* Always hide both cards first, then show what exists */
    lv_obj_add_flag(s_card[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_card[1], LV_OBJ_FLAG_HIDDEN);

    const ns_thread_t *top  = notif_store_get(page - 1);  /* highlighted */
    const ns_thread_t *next = notif_store_get(page);       /* dimmed below */

    if (top)  populate_card(0, top,  true);
    if (next) populate_card(1, next, false);

    lv_screen_load(s_scr_notif_detail);
}

/* Load thread detail screen for thread at idx.
 * Shows full message history (newest first), then marks thread as read.
 * Call inside lvgl_port_lock. */
static void load_thread_detail_locked(int idx)
{
    const ns_thread_t *t = notif_store_get(idx);
    if (!t) return;

    update_status_label(s_td_status);
    lv_label_set_text(s_td_icon,   category_icon(t->cat));
    lv_label_set_text(s_td_sender, t->sender[0] ? t->sender : t->cat);

    /* Build formatted history — newest at top (index 0) */
    static char buf[TD_BUF_SIZE];
    buf[0] = '\0';
    size_t pos = 0;
    for (int i = 0; i < t->count && pos < sizeof(buf) - 1; i++) {
        int n = snprintf(buf + pos, sizeof(buf) - pos,
                         "%s\n%s\n\n",
                         t->msgs[i].ts[0]   ? t->msgs[i].ts   : "--:--",
                         t->msgs[i].text[0] ? t->msgs[i].text : " ");
        if (n > 0 && (size_t)n < sizeof(buf) - pos)
            pos += (size_t)n;
        else
            break;
    }
    lv_label_set_text(s_td_content, buf);

    /* Mark this thread as read and refresh the badge on the idle screen */
    notif_store_mark_read(idx);
    redraw_unread_badge();

    lv_screen_load(s_scr_thread_detail);
}

/* ════════════════════════════════════════════════════════════════════════════
   Timers
   ════════════════════════════════════════════════════════════════════════════ */

/* 500ms pulse: show/hide the scroll hint and keep the unread badge current */
static void hint_tick_cb(void *arg)
{
    if (!lvgl_port_lock(0)) return;
    if (s_screen == SCREEN_IDLE && notif_store_count() > 0) {
        if (s_lbl_hint) {
            s_hint_on = !s_hint_on;
            if (s_hint_on)
                lv_obj_remove_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
        }
        redraw_unread_badge();
    } else {
        if (s_lbl_hint) {
            s_hint_on = false;
            lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lvgl_port_unlock();
}

static void clock_tick_cb(void *arg)
{
    if (!s_time.valid) return;

    s_time.sec++;
    if (s_time.sec >= 60) {
        s_time.sec = 0;
        s_time.min++;
        if (s_time.min >= 60) {
            s_time.min = 0;
            s_time.hour++;
            if (s_time.hour >= 24) {
                s_time.hour = 0;
                s_time.dow = (s_time.dow + 1) % 7;
                s_time.day++;
            }
        }
    }

    /* Redraw once per minute */
    if (s_time.sec != 0) return;
    if (!lvgl_port_lock(0)) return;
    redraw_clock();
    update_status_label(s_lbl_status);
    update_status_label(s_detail_status);
    lvgl_port_unlock();
}

/* ════════════════════════════════════════════════════════════════════════════
   Public API
   ════════════════════════════════════════════════════════════════════════════ */

void ui_init(void)
{
    ESP_LOGI(TAG, "ui_init start, free heap: %lu", esp_get_free_heap_size());

    /* ── SPI bus ── */
    spi_bus_config_t buscfg = {
        .mosi_io_num     = LCD_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = LCD_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_W * DRAW_BUF_LINES * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI, &buscfg, SPI_DMA_CH_AUTO));

    /* ── LCD panel IO ── */
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

    /* ── ST7789 panel ── */
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num  = LCD_RST,
        .bits_per_pixel  = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_set_gap(panel, 0, 0);
    esp_lcd_panel_disp_on_off(panel, true);

    gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL, 1);
    ESP_LOGI(TAG, "Panel init done, free heap: %lu", esp_get_free_heap_size());

    /* ── esp_lvgl_port ── */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    /* ── Register display ── */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = LCD_W * DRAW_BUF_LINES,
        .double_buffer = false,
        .hres          = LCD_W,
        .vres          = LCD_H,
        .monochrome    = false,
        .rotation = {
            .swap_xy  = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma    = true,
            .buff_spiram = false,
            .swap_bytes  = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp FAILED");
        return;
    }

    /* ── Build all screens ── */
    lv_display_set_default(s_disp);
    if (lvgl_port_lock(0)) {
        build_idle_screen();
        build_notif_detail_screen();
        build_bricked_screen();
        build_call_screen();
        build_thread_detail_screen();
        lv_screen_load(s_scr_idle);
        ESP_LOGI(TAG, "Screens built, free heap: %lu", esp_get_free_heap_size());
        lvgl_port_unlock();
    }
    lv_refr_now(s_disp);

    /* ── Clock tick timer (1s) ── */
    esp_timer_handle_t clock_timer;
    esp_timer_create_args_t clock_args = {
        .callback = clock_tick_cb,
        .name     = "ui_clock",
    };
    ESP_ERROR_CHECK(esp_timer_create(&clock_args, &clock_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(clock_timer, 1000000));

    /* ── Scroll hint pulse timer (500ms) ── */
    esp_timer_handle_t hint_timer;
    esp_timer_create_args_t hint_args = {
        .callback = hint_tick_cb,
        .name     = "ui_hint",
    };
    ESP_ERROR_CHECK(esp_timer_create(&hint_args, &hint_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(hint_timer, 500000));

    ESP_LOGI(TAG, "LVGL UI initialised (%dx%d)", LCD_W, LCD_H);
}

void ui_fill(uint16_t color) { (void)color; }

/* ── Status bar ──────────────────────────────────────────────────────────── */

void ui_set_status_ble(const char *text)
{
    strncpy(s_ble_status, text, sizeof(s_ble_status) - 1);
    s_ble_status[sizeof(s_ble_status) - 1] = '\0';
    if (lvgl_port_lock(0)) {
        update_status_label(s_lbl_status);
        update_status_label(s_call_status);
        update_status_label(s_detail_status);
        lvgl_port_unlock();
    }
}

void ui_set_status_hfp(const char *text) { (void)text; }

/* ── Screen transitions ──────────────────────────────────────────────────── */

void ui_show_idle(void)
{
    if (!lvgl_port_lock(0)) return;
    s_screen       = SCREEN_IDLE;
    s_current_page = 0;
    update_status_label(s_lbl_status);
    lv_screen_load(s_scr_idle);
    lvgl_port_unlock();
}

void ui_show_bricked(void)
{
    if (!lvgl_port_lock(0)) return;
    s_screen       = SCREEN_BRICKED;
    s_current_page = 0;
    lv_screen_load(s_scr_bricked);
    lvgl_port_unlock();
}

void ui_show_notification(const char *category, const char *title, const char *message)
{
    /* Sanitize and store — title becomes sender key for thread grouping */
    char san_sender[NS_SENDER_LEN];
    char san_msg[NS_MSG_LEN];
    sanitize_utf8(san_sender, sizeof(san_sender), title   ? title   : "");
    sanitize_utf8(san_msg,    sizeof(san_msg),    message ? message : "");

    char ts[NS_TS_LEN];
    capture_timestamp(ts, sizeof(ts));

    notif_store_add(category ? category : "", san_sender, san_msg, ts);

    /* Return to home — notification reachable via scroll; refresh badge */
    if (!lvgl_port_lock(0)) return;
    s_screen       = SCREEN_IDLE;
    s_current_page = 0;
    update_status_label(s_lbl_status);
    redraw_unread_badge();
    lv_screen_load(s_scr_idle);
    lvgl_port_unlock();
}

void ui_show_incoming_call(const char *caller)
{
    if (!lvgl_port_lock(0)) return;
    s_screen       = SCREEN_INCOMING_CALL;
    s_current_page = 0;

    char display_name[32];
    if (caller && caller[0])
        format_phone_number(display_name, sizeof(display_name), caller);
    else
        snprintf(display_name, sizeof(display_name), "Unknown");

    update_status_label(s_call_status);
    lv_label_set_text(s_call_icon,  LV_SYMBOL_CALL);
    lv_obj_set_style_text_color(s_call_icon, C_GREEN, 0);
    lv_label_set_text(s_call_label, "incoming call");
    lv_label_set_text(s_call_name,  display_name);
    lv_label_set_text(s_call_hint,  "press to answer");
    lv_screen_load(s_scr_call);
    lvgl_port_unlock();
}

void ui_show_call_active(void)
{
    if (!lvgl_port_lock(0)) return;
    s_screen       = SCREEN_CALL_ACTIVE;
    s_current_page = 0;

    update_status_label(s_call_status);
    lv_label_set_text(s_call_icon,  LV_SYMBOL_CALL);
    lv_obj_set_style_text_color(s_call_icon, C_GRAY, 0);
    lv_label_set_text(s_call_label, "call active");
    lv_label_set_text(s_call_hint,  "press to hang up");
    lv_screen_load(s_scr_call);
    lvgl_port_unlock();
}

/* ── Display data setters ────────────────────────────────────────────────── */

void ui_set_time(uint8_t hour, uint8_t min, uint8_t sec,
                 uint8_t dow, uint16_t year, uint8_t month, uint8_t day)
{
    s_time.hour  = hour;
    s_time.min   = min;
    s_time.sec   = sec;
    s_time.dow   = dow;
    s_time.year  = year;
    s_time.month = month;
    s_time.day   = day;
    s_time.valid = true;

    ESP_LOGI(TAG, "Time synced: %02d:%02d:%02d %s %s %d %d",
             hour, min, sec,
             dow_names[dow % 7],
             month_names[month <= 12 ? month : 0],
             day, year);

    if (!lvgl_port_lock(0)) return;
    redraw_clock();
    update_status_label(s_lbl_status);
    lvgl_port_unlock();
}

void ui_set_todo(uint8_t index, bool checked, const char *text)
{
    if (index >= MAX_TODOS) return;
    s_todos[index].active  = true;
    s_todos[index].checked = checked;
    strncpy(s_todos[index].text, text, TODO_TEXT_LEN);
    s_todos[index].text[TODO_TEXT_LEN] = '\0';
    if (lvgl_port_lock(0)) { redraw_todos(); lvgl_port_unlock(); }
}

void ui_clear_todos(void)
{
    memset(s_todos, 0, sizeof(s_todos));
    if (lvgl_port_lock(0)) { redraw_todos(); lvgl_port_unlock(); }
}

void ui_set_message(const char *text)   { (void)text; }
void ui_clear_message(void)             {}

/* ── Encoder navigation ──────────────────────────────────────────────────── */

void ui_navigate(int delta)
{
    /* Only navigate when on the home or thread-list screens */
    if (s_screen != SCREEN_IDLE && s_screen != SCREEN_NOTIF_DETAIL) return;
    int total = notif_store_count();
    if (total == 0 && delta > 0) return;

    int new_page = s_current_page + delta;
    if (new_page < 0)      new_page = 0;
    if (new_page > total)  new_page = total;
    if (new_page == s_current_page) return;

    s_current_page = new_page;

    if (new_page == 0) {
        if (!lvgl_port_lock(0)) return;
        s_screen = SCREEN_IDLE;
        update_status_label(s_lbl_status);
        lv_screen_load(s_scr_idle);
        lvgl_port_unlock();
    } else {
        if (!lvgl_port_lock(0)) return;
        s_screen = SCREEN_NOTIF_DETAIL;
        load_notif_detail_locked(new_page);
        lvgl_port_unlock();
    }
}

bool ui_has_unread(void)
{
    return notif_store_count() > 0;
}

bool ui_screen_is_call(void)
{
    return s_screen == SCREEN_INCOMING_CALL || s_screen == SCREEN_CALL_ACTIVE;
}

void ui_encoder_click(void)
{
    if (s_screen == SCREEN_NOTIF_DETAIL) {
        /* Drill into thread detail — highlighted card is always thread[page-1] */
        int thread_idx = s_current_page - 1;
        if (!lvgl_port_lock(0)) return;
        s_screen = SCREEN_THREAD_DETAIL;
        load_thread_detail_locked(thread_idx);
        lvgl_port_unlock();
    } else if (s_screen == SCREEN_THREAD_DETAIL) {
        /* Back to thread list */
        if (!lvgl_port_lock(0)) return;
        s_screen = SCREEN_NOTIF_DETAIL;
        load_notif_detail_locked(s_current_page);
        lvgl_port_unlock();
    } else {
        /* Default: return home */
        ui_navigate(-10);
    }
}
