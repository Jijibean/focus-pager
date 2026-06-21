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
 */

#include "ui.h"

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

/* ── LVGL draw buffer (double-buffered, 1/10 screen) ─────────────────────── */
#define DRAW_BUF_LINES  10

/* ── Colours (lv_color_hex) ──────────────────────────────────────────────── */
#define C_WHITE    lv_color_hex(0xFFFFFF)
#define C_GRAY     lv_color_hex(0x888888)
#define C_DIM      lv_color_hex(0x444444)
#define C_BLACK    lv_color_hex(0x000000)
#define C_DIMRED   lv_color_hex(0x882222)
#define C_GREEN    lv_color_hex(0x00CC88)  /* call active / incoming accent */
#define C_ACCENT   lv_color_hex(0x4488FF)  /* general accent / icon tint   */

/* ── Screen state ────────────────────────────────────────────────────────── */
typedef enum {
    SCREEN_IDLE,
    SCREEN_BRICKED,
    SCREEN_INCOMING_CALL,
    SCREEN_CALL_ACTIVE,
} screen_state_t;

static screen_state_t s_screen = SCREEN_IDLE;

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
static lv_obj_t *s_scr_idle    = NULL;   /* main / notification list screen */
static lv_obj_t *s_scr_bricked = NULL;
static lv_obj_t *s_scr_call    = NULL;   /* shared for incoming + active */

/* Main screen labels */
static lv_obj_t *s_lbl_status = NULL;   /* status bar: time + BT state */

/* Notification history — newest at index 0 */
#define MAX_NOTIFS    5
#define NOTIF_CAT_LEN 20
#define NOTIF_TTL_LEN 48
#define NOTIF_MSG_LEN 80
#define NOTIF_TS_LEN  12   /* "12:59 PM\0" */

typedef struct {
    char cat[NOTIF_CAT_LEN];
    char title[NOTIF_TTL_LEN];
    char message[NOTIF_MSG_LEN];
    char timestamp[NOTIF_TS_LEN];
} notif_item_t;

static notif_item_t s_notif_hist[MAX_NOTIFS];
static int          s_notif_count = 0;

/* Notification screen: fixed status bar + scrollable card list */
#define STATUS_H  22   /* height of the fixed status bar strip */
#define CARD_H    70   /* height of each notification card    */
#define CARD_PX   10   /* horizontal padding inside a card    */
#define CARD_PY    6   /* vertical padding inside a card      */

static lv_obj_t *s_notif_list = NULL;   /* scrollable container inside s_scr_idle */

/* Call screen labels */
static lv_obj_t *s_call_icon   = NULL;  /* LV_SYMBOL_CALL, colored */
static lv_obj_t *s_call_label  = NULL;
static lv_obj_t *s_call_name   = NULL;
static lv_obj_t *s_call_hint   = NULL;
static lv_obj_t *s_call_status = NULL;

/* Bricked screen labels */
static lv_obj_t *s_lbl_bricked      = NULL;
static lv_obj_t *s_lbl_bricked_hint = NULL;

/* ── Day / month name tables (used for logging only) ────────────────────── */
static const char *dow_names[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *month_names[] = {
    "","Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

/* ════════════════════════════════════════════════════════════════════════════
   Internal helpers
   ════════════════════════════════════════════════════════════════════════════ */

/* Apply shared screen style: black background, no border, no padding */
static void style_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, C_BLACK, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
}

/* Create a label with given font, color, and position */
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

/* Format and update the status string (time + BT icon + conn state) */
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

/* Forward declaration — category_icon is defined after the helpers */
static const char *category_icon(const char *cat);

/* Capture the current time as a short timestamp string ("10:34 AM") */
static void capture_timestamp(char *buf, size_t sz)
{
    if (!s_time.valid) { buf[0] = '\0'; return; }
    int h = s_time.hour % 12;
    if (h == 0) h = 12;
    snprintf(buf, sz, "%d:%02d %s",
             h, s_time.min, s_time.hour < 12 ? "AM" : "PM");
}

/* Build one notification card inside the scrollable list container */
static void add_notif_card(lv_obj_t *parent, const notif_item_t *n, int y)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_size(card, LCD_W, CARD_H);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(card, C_DIM, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Row 1 — icon (14pt, accent) */
    lv_obj_t *icon = lv_label_create(card);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(icon, C_ACCENT, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(icon, CARD_PX, CARD_PY);
    lv_label_set_text(icon, category_icon(n->cat));

    /* Row 1 — category name (12pt, gray) */
    lv_obj_t *cat_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(cat_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cat_lbl, C_GRAY, 0);
    lv_obj_set_style_bg_opa(cat_lbl, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(cat_lbl, CARD_PX + 20, CARD_PY + 1);
    lv_label_set_text(cat_lbl, n->cat);

    /* Row 1 — timestamp (12pt, dim, right-aligned) */
    if (n->timestamp[0]) {
        lv_obj_t *ts_lbl = lv_label_create(card);
        lv_obj_set_style_text_font(ts_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(ts_lbl, C_DIM, 0);
        lv_obj_set_style_bg_opa(ts_lbl, LV_OPA_TRANSP, 0);
        lv_obj_align(ts_lbl, LV_ALIGN_TOP_RIGHT, -CARD_PX, CARD_PY);
        lv_label_set_text(ts_lbl, n->timestamp);
    }

    /* Row 2 — title (16pt, white) */
    lv_obj_t *title_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_lbl, C_WHITE, 0);
    lv_obj_set_style_bg_opa(title_lbl, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(title_lbl, CARD_PX, CARD_PY + 22);
    lv_obj_set_width(title_lbl, LCD_W - CARD_PX * 2);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(title_lbl, n->title);

    /* Row 3 — message preview (12pt, gray, 1 line clipped) */
    lv_obj_t *body_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(body_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(body_lbl, C_GRAY, 0);
    lv_obj_set_style_bg_opa(body_lbl, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(body_lbl, CARD_PX, CARD_PY + 46);
    lv_obj_set_width(body_lbl, LCD_W - CARD_PX * 2);
    lv_label_set_long_mode(body_lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(body_lbl, n->message);
}

/* Wipe all cards and rebuild from s_notif_hist — call inside lvgl_port_lock */
static void rebuild_notif_list(void)
{
    if (!s_notif_list) return;
    lv_obj_clean(s_notif_list);
    if (s_notif_count == 0) {
        lv_obj_t *empty = lv_label_create(s_notif_list);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(empty, C_DIM, 0);
        lv_obj_set_style_bg_opa(empty, LV_OPA_TRANSP, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(empty, "No messages");
        return;
    }
    for (int i = 0; i < s_notif_count; i++) {
        add_notif_card(s_notif_list, &s_notif_hist[i], i * CARD_H);
    }
    lv_obj_scroll_to_y(s_notif_list, 0, LV_ANIM_OFF);  /* newest card on top */
}

/* Return the appropriate LVGL symbol for an ANCS category string */
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

/* Format a raw phone number into (XXX) XXX-XXXX — leaves non-digits intact */
static void format_phone_number(char *out, size_t outsz, const char *raw)
{
    if (!raw || !raw[0]) { snprintf(out, outsz, "Unknown"); return; }

    /* Strip everything except digits */
    char digits[16] = "";
    size_t nd = 0;
    for (const char *p = raw; *p && nd < sizeof(digits) - 1; p++) {
        if (*p >= '0' && *p <= '9') digits[nd++] = *p;
    }
    digits[nd] = '\0';

    /* Skip leading country code digit if 11 digits and starts with 1 */
    const char *d = digits;
    if (nd == 11 && d[0] == '1') { d++; nd = 10; }

    if (nd == 10) {
        snprintf(out, outsz, "(%c%c%c) %c%c%c-%c%c%c%c",
                 d[0],d[1],d[2], d[3],d[4],d[5], d[6],d[7],d[8],d[9]);
    } else {
        /* Not a standard number — show as-is */
        snprintf(out, outsz, "%s", raw);
    }
}

/* ════════════════════════════════════════════════════════════════════════════
   Screen builders — called once during ui_init
   ════════════════════════════════════════════════════════════════════════════ */

static void build_idle_screen(void)
{
    s_scr_idle = lv_obj_create(NULL);
    style_screen(s_scr_idle);

    /* Status bar — top: time + BT state */
    s_lbl_status = make_label(s_scr_idle,
        &lv_font_montserrat_12, C_GRAY,
        LV_ALIGN_TOP_LEFT, 10, 5, "");

    /* Scrollable notification list — fills area below status bar */
    s_notif_list = lv_obj_create(s_scr_idle);
    lv_obj_set_pos(s_notif_list, 0, STATUS_H);
    lv_obj_set_size(s_notif_list, LCD_W, LCD_H - STATUS_H);
    lv_obj_set_style_bg_opa(s_notif_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_notif_list, 0, 0);
    lv_obj_set_style_pad_all(s_notif_list, 0, 0);
    lv_obj_set_style_radius(s_notif_list, 0, 0);
    lv_obj_set_scrollbar_mode(s_notif_list, LV_SCROLLBAR_MODE_OFF);

    rebuild_notif_list();
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

    /* Status bar */
    s_call_status = make_label(s_scr_call,
        &lv_font_montserrat_12, C_GRAY,
        LV_ALIGN_TOP_LEFT, 10, 8, "");

    /* Phone icon — large, centred, coloured green for incoming */
    s_call_icon = make_label(s_scr_call,
        &lv_font_montserrat_48, C_GREEN,
        LV_ALIGN_CENTER, 0, -60, LV_SYMBOL_CALL);
    lv_obj_set_style_text_align(s_call_icon, LV_TEXT_ALIGN_CENTER, 0);

    /* "incoming call" / "call active" label — below icon */
    s_call_label = make_label(s_scr_call,
        &lv_font_montserrat_12, C_GRAY,
        LV_ALIGN_CENTER, 0, -8, "incoming call");
    lv_obj_set_style_text_align(s_call_label, LV_TEXT_ALIGN_CENTER, 0);

    /* Caller name — large white text, centred */
    s_call_name = make_label(s_scr_call,
        &lv_font_montserrat_24, C_WHITE,
        LV_ALIGN_CENTER, 0, 24, "");
    lv_label_set_long_mode(s_call_name, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_call_name, LCD_W - 20);
    lv_obj_set_style_text_align(s_call_name, LV_TEXT_ALIGN_CENTER, 0);

    /* Bottom hint */
    s_call_hint = make_label(s_scr_call,
        &lv_font_montserrat_12, C_DIM,
        LV_ALIGN_BOTTOM_MID, 0, -12, "press to answer");
    lv_obj_set_style_text_align(s_call_hint, LV_TEXT_ALIGN_CENTER, 0);
}

/* ════════════════════════════════════════════════════════════════════════════
   Timers
   ════════════════════════════════════════════════════════════════════════════ */

static void clock_tick_cb(void *arg)
{
    if (!s_time.valid) return;

    /* Advance time */
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

    /* Update status bar once per minute */
    if (s_time.sec != 0) return;
    if (!lvgl_port_lock(0)) return;
    update_status_label(s_lbl_status);
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
        /* Do NOT set LCD_RGB_DATA_ENDIAN_BIG here — the byte swap is handled
         * by swap_bytes=true in the LVGL port flush path. Setting both would
         * double-swap and produce wrong colors. */
        .bits_per_pixel  = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_set_gap(panel, 0, 0);
    /* Do NOT call swap_xy/mirror here — lvgl_port_add_disp calls
     * lvgl_port_disp_rotation_update() which overwrites any MADCTL we set.
     * Rotation is declared in disp_cfg.rotation below instead. */
    esp_lcd_panel_disp_on_off(panel, true);

    /* Backlight on */
    gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL, 1);
    ESP_LOGI(TAG, "Panel init done, free heap: %lu", esp_get_free_heap_size());

    /* ── esp_lvgl_port init ── */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));
    ESP_LOGI(TAG, "lvgl_port_init done, free heap: %lu", esp_get_free_heap_size());

    /* ── Register display with LVGL ──
     * lvgl_port_add_disp calls lvgl_port_disp_rotation_update internally,
     * which programs swap_xy/mirror into the panel from the values below.
     * swap_xy=true + mirror_x=true = 90° CW = landscape on this module. */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = LCD_W * DRAW_BUF_LINES,
        .double_buffer = false,
        .hres          = LCD_W,
        .vres          = LCD_H,
        .monochrome    = false,
        .rotation = {
            .swap_xy  = true,    /* landscape: exchange rows and columns */
            .mirror_x = true,    /* 90° CW — adjust if image is flipped */
            .mirror_y = false,
        },
        .flags = {
            .buff_dma    = true,
            .buff_spiram = false,
            .swap_bytes  = true,   /* RGB565: LVGL little-endian → ST7789 big-endian */
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp FAILED — out of memory?");
        return;
    }
    ESP_LOGI(TAG, "lvgl_port_add_disp OK, free heap: %lu", esp_get_free_heap_size());

    /* ── Build all screens ── */
    lv_display_set_default(s_disp);
    if (lvgl_port_lock(0)) {
        build_idle_screen();
        build_bricked_screen();
        build_call_screen();
        lv_screen_load(s_scr_idle);
        ESP_LOGI(TAG, "Screens built, free heap: %lu", esp_get_free_heap_size());
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for screen build");
    }
    /* Trigger initial render outside the lock — the LVGL port task handles flush */
    lv_refr_now(s_disp);

    /* ── Clock tick timer (1s periodic) — advances s_time and refreshes status bar ── */
    esp_timer_handle_t clock_timer;
    esp_timer_create_args_t clock_args = {
        .callback = clock_tick_cb,
        .name     = "ui_clock",
    };
    ESP_ERROR_CHECK(esp_timer_create(&clock_args, &clock_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(clock_timer, 1000000));

    ESP_LOGI(TAG, "LVGL UI initialised (%dx%d)", LCD_W, LCD_H);
}

/* ── Stub: kept for API compatibility, LVGL manages its own buffer ───────── */
void ui_fill(uint16_t color)
{
    /* LVGL redraws entire screen on lv_scr_load — nothing to do here */
    (void)color;
}

/* ── Status bar ──────────────────────────────────────────────────────────── */

void ui_set_status_ble(const char *text)
{
    strncpy(s_ble_status, text, sizeof(s_ble_status) - 1);
    s_ble_status[sizeof(s_ble_status) - 1] = '\0';
    if (lvgl_port_lock(0)) {
        update_status_label(s_lbl_status);
        update_status_label(s_call_status);
        lvgl_port_unlock();
    }
}

void ui_set_status_hfp(const char *text) { (void)text; }

/* ── Screen transitions ──────────────────────────────────────────────────── */

void ui_show_idle(void)
{
    if (!lvgl_port_lock(0)) return;
    s_screen = SCREEN_IDLE;
    update_status_label(s_lbl_status);
    lv_screen_load(s_scr_idle);
    lvgl_port_unlock();
}

void ui_show_bricked(void)
{
    if (!lvgl_port_lock(0)) return;
    s_screen = SCREEN_BRICKED;
    lv_screen_load(s_scr_bricked);
    lvgl_port_unlock();
}

void ui_show_notification(const char *category, const char *title, const char *message)
{
    /* Prepend to history (shift older entries back, drop the oldest if full) */
    if (s_notif_count < MAX_NOTIFS) s_notif_count++;
    memmove(&s_notif_hist[1], &s_notif_hist[0],
            (size_t)(s_notif_count - 1) * sizeof(notif_item_t));
    notif_item_t *n = &s_notif_hist[0];
    memset(n, 0, sizeof(*n));
    strncpy(n->cat,     category ? category : "",   NOTIF_CAT_LEN - 1);
    strncpy(n->title,   title    ? title    : "",   NOTIF_TTL_LEN - 1);
    strncpy(n->message, message  ? message  : "",   NOTIF_MSG_LEN - 1);
    capture_timestamp(n->timestamp, sizeof(n->timestamp));

    if (!lvgl_port_lock(0)) return;
    s_screen = SCREEN_IDLE;
    update_status_label(s_lbl_status);
    rebuild_notif_list();
    lv_screen_load(s_scr_idle);
    lvgl_port_unlock();
}

void ui_show_incoming_call(const char *caller)
{
    if (!lvgl_port_lock(0)) return;
    s_screen = SCREEN_INCOMING_CALL;

    /* Format caller: if it looks like digits, pretty-print the number.
     * If ANCS later provides a contact name it calls us again to override. */
    char display_name[32];
    if (caller && caller[0]) {
        format_phone_number(display_name, sizeof(display_name), caller);
    } else {
        snprintf(display_name, sizeof(display_name), "Unknown");
    }

    update_status_label(s_call_status);
    lv_label_set_text(s_call_icon, LV_SYMBOL_CALL);
    lv_obj_set_style_text_color(s_call_icon, C_GREEN, 0);
    lv_label_set_text(s_call_label, "incoming call");
    lv_label_set_text(s_call_name, display_name);
    lv_label_set_text(s_call_hint, "press to answer");

    lv_screen_load(s_scr_call);
    lvgl_port_unlock();
}

void ui_show_call_active(void)
{
    if (!lvgl_port_lock(0)) return;
    s_screen = SCREEN_CALL_ACTIVE;

    update_status_label(s_call_status);
    lv_label_set_text(s_call_icon, LV_SYMBOL_CALL);
    lv_obj_set_style_text_color(s_call_icon, C_GRAY, 0);  /* dim when active */
    lv_label_set_text(s_call_label, "call active");
    lv_label_set_text(s_call_hint, "press to hang up");

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
    update_status_label(s_lbl_status);
    lvgl_port_unlock();
}

void ui_set_todo(uint8_t index, bool checked, const char *text)
{
    (void)index; (void)checked; (void)text;
}

void ui_clear_todos(void) {}

void ui_set_message(const char *text) { (void)text; }

void ui_clear_message(void) {}
