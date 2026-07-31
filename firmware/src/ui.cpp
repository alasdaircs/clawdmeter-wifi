#include "ui.h"
#include "splash.h"
#include "brightness.h"
#include "chime_pref.h"
#include "idle.h"
#include "hal/sound_hal.h"
#include <lvgl.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "provisioning.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_mono_32);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;

    // Wi-Fi screen
    int16_t wifi_panel_h;
    int16_t wifi_val_x;
    int16_t wifi_row_h;
    int16_t wifi_btn_h;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
        L.wifi_panel_h     = 130;
        L.wifi_val_x       = 85;
        L.wifi_row_h       = 36;
        L.wifi_btn_h       = 70;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
        L.wifi_panel_h     = 110;
        L.wifi_val_x       = 70;
        L.wifi_row_h       = 30;
        L.wifi_btn_h       = 56;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly  = nullptr;
// Enterprise-only widgets inside panel_session (upstream fe7a7ee)
static lv_obj_t* lbl_session_pct_sym = nullptr;   // "%" in smaller font
static lv_obj_t* lbl_spending_desc   = nullptr;   // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under/On/Over pace"
static lv_obj_t* lbl_anim;
static lv_obj_t* lbl_status;

// ---- Live-data freshness → stale "Zzz" sub-view (ported from upstream
// 72524ef, adapted to the Wi-Fi fork's multi-screen UI). When no valid usage
// update has landed within DATA_FRESH_MS, hide the panels and show a small
// sleeping creature instead of rendering stale numbers as if they were live.
// 150s = 2.5 poll intervals, so one failed 60s poll doesn't flash the Zzz.
static lv_obj_t* panels_group;          // the two usage panels
static lv_obj_t* stale_group;           // the "Zzz" idle view
static uint32_t  last_data_ms = 0;      // lv_tick of the last valid usage update
static bool      data_received = false; // any valid update since boot
static bool      showing_stale = false;
static const uint32_t DATA_FRESH_MS = 150000;

// ---- Bluetooth screen widgets ----
static lv_obj_t* ble_container;
static lv_obj_t* lbl_ble_status;
static lv_obj_t* lbl_ble_device;
static lv_obj_t* lbl_ble_mac;

// ---- Settings screen widgets ----
static lv_obj_t* settings_container;
static lv_obj_t* settings_slider;
static lv_obj_t* lbl_brt_val;
static lv_obj_t* chime_switch;

// ---- Wi-Fi screen widgets ----
static lv_obj_t* wifi_container;
static lv_obj_t* lbl_wifi_ssid_val;
static lv_obj_t* lbl_wifi_pass_val;
static lv_obj_t* lbl_wifi_token_val;
static lv_obj_t* lbl_wifi_note;

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void ble_reset_click_cb(lv_event_t* e);
static void wifi_hotspot_click_cb(lv_event_t* e);

static bool s_hotspot_requested = false;
static bool s_nav_locked        = false;

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc(lv_image_dsc_t* dsc, int w, int h, const uint16_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.stride = w * 2;
    dsc->data = (const uint8_t*)data;
    dsc->data_size = w * h * 2;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y, L.content_w - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

    return panel;
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    // Panels live in their own transparent full-size group so the stale
    // "Zzz" view can swap with them without touching the title/status line.
    panels_group = lv_obj_create(usage_container);
    lv_obj_set_size(panels_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(panels_group, 0, 0);
    lv_obj_set_style_bg_opa(panels_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panels_group, 0, 0);
    lv_obj_set_style_pad_all(panels_group, 0, 0);
    lv_obj_clear_flag(panels_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panels_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(panels_group, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Enterprise-only overlays inside panel_session — hidden until
    // enterprise data arrives (upstream fe7a7ee).
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of your monthly budget");
    lv_obj_set_style_text_font(lbl_spending_desc, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, &font_styrene_16, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(panels_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so the enterprise period box can color pace and reset
    // date separately in one label.
    lv_label_set_recolor(lbl_weekly_reset, true);

    // Stale/Zzz view — a shrunk sleeping creature (claudepix "expression
    // sleep") centered where the panels normally sit. Hidden by default.
    stale_group = lv_obj_create(usage_container);
    lv_obj_set_size(stale_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(stale_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(stale_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stale_group, 0, 0);
    lv_obj_set_style_pad_all(stale_group, 0, 0);
    lv_obj_clear_flag(stale_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(stale_group, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t* creature = splash_mini_create(stale_group, "expression sleep", 160);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(stale_group, LV_OBJ_FLAG_HIDDEN);

    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);

    lbl_status = lv_label_create(usage_container);
    lv_label_set_text(lbl_status, "");
    lv_obj_set_style_text_font(lbl_status, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_status, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_status, L.content_w);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
}

// ======== Settings Screen ========

static void brt_slider_cb(lv_event_t* e) {
    int32_t v = lv_slider_get_value(settings_slider);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        // Live preview while dragging — apply to the panel but don't touch
        // NVS on every pixel of travel.
        idle_set_awake_brightness((uint8_t)v);
        lv_label_set_text_fmt(lbl_brt_val, "%d%%", (int)(v * 100 / 255));
    } else {  // LV_EVENT_RELEASED — persist once
        brightness_set((uint8_t)v);
    }
}

static void chime_switch_cb(lv_event_t* e) {
    (void)e;
    bool on = lv_obj_has_state(chime_switch, LV_STATE_CHECKED);
    chime_pref_set(on);
    if (on) sound_hal_play_reset();  // audible confirmation (no-op without speaker)
}

static void init_settings_screen(lv_obj_t* scr) {
    settings_container = lv_obj_create(scr);
    lv_obj_set_size(settings_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(settings_container, 0, 0);
    lv_obj_set_style_bg_opa(settings_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_container, 0, 0);
    lv_obj_set_style_pad_all(settings_container, 0, 0);
    lv_obj_clear_flag(settings_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(settings_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(settings_container);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 16, L.title_y);

    // Brightness panel: label + live % + slider. No EVENT_BUBBLE — a tap
    // inside the panel must not cycle the screen mid-adjustment.
    lv_obj_t* p1 = make_panel(settings_container, L.margin, L.content_y,
                              L.content_w, L.usage_panel_h);
    lv_obj_clear_flag(p1, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* lbl_b = lv_label_create(p1);
    lv_label_set_text(lbl_b, "Brightness");
    lv_obj_set_style_text_font(lbl_b, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_b, COL_TEXT, 0);
    lv_obj_set_pos(lbl_b, 0, 0);

    lbl_brt_val = lv_label_create(p1);
    lv_obj_set_style_text_font(lbl_brt_val, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_brt_val, COL_DIM, 0);
    lv_obj_align(lbl_brt_val, LV_ALIGN_TOP_RIGHT, 0, 0);

    settings_slider = lv_slider_create(p1);
    lv_slider_set_range(settings_slider, 10, 255);
    lv_obj_set_size(settings_slider, L.content_w - 32 - 24, 20);
    lv_obj_set_pos(settings_slider, 12, L.usage_bar_y + 8);
    lv_obj_set_style_bg_color(settings_slider, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(settings_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(settings_slider, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(settings_slider, COL_TEXT, LV_PART_KNOB);
    // Widen the touch target well beyond the visual track.
    lv_obj_set_ext_click_area(settings_slider, 24);
    lv_obj_add_event_cb(settings_slider, brt_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(settings_slider, brt_slider_cb, LV_EVENT_RELEASED, NULL);

    // Chime panel: label + switch.
    lv_obj_t* p2 = make_panel(settings_container, L.margin,
                              L.content_y + L.usage_panel_h + L.usage_panel_gap,
                              L.content_w, L.usage_panel_h);
    lv_obj_clear_flag(p2, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Top-aligned to mirror the brightness panel's layout.
    lv_obj_t* lbl_c = lv_label_create(p2);
    lv_label_set_text(lbl_c, "Reset chime");
    lv_obj_set_style_text_font(lbl_c, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_c, COL_TEXT, 0);
    lv_obj_set_pos(lbl_c, 0, 0);

    lv_obj_t* lbl_c2 = lv_label_create(p2);
    lv_label_set_text(lbl_c2, "Rings when your session resets");
    lv_obj_set_style_text_font(lbl_c2, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_c2, COL_DIM, 0);
    lv_obj_set_pos(lbl_c2, 0, 52);

    chime_switch = lv_switch_create(p2);
    lv_obj_set_size(chime_switch, 84, 44);
    lv_obj_align(chime_switch, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(chime_switch, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chime_switch, COL_ACCENT,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(chime_switch, COL_TEXT, LV_PART_KNOB);
    lv_obj_set_ext_click_area(chime_switch, 16);
    lv_obj_add_event_cb(chime_switch, chime_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_add_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
}

void ui_settings_refresh(void) {
    if (!settings_container) return;
    uint8_t b = brightness_get();
    lv_slider_set_value(settings_slider, b, LV_ANIM_OFF);
    lv_label_set_text_fmt(lbl_brt_val, "%d%%", (int)(b * 100 / 255));
    if (chime_pref_get()) lv_obj_add_state(chime_switch, LV_STATE_CHECKED);
    else                  lv_obj_remove_state(chime_switch, LV_STATE_CHECKED);
}

// ======== Wi-Fi Screen ========

static void redact_password(const String& pass, char* buf, size_t len) {
    if (pass.length() == 0) { strlcpy(buf, "(none)", len); return; }
    if (pass.length() == 1) { strlcpy(buf, "*", len); return; }
    if (pass.length() == 2) { strlcpy(buf, "**", len); return; }
    snprintf(buf, len, "%c***%c", pass[0], pass[pass.length() - 1]);
}

static void redact_token(const String& token, char* buf, size_t len) {
    if (token.length() == 0)   { strlcpy(buf, "(none)", len); return; }
    if (token.length() <= 14)  { strlcpy(buf, token.c_str(), len); return; }
    snprintf(buf, len, "%.14s...", token.c_str());
}

static lv_obj_t* make_wifi_val_label(lv_obj_t* parent, int y) {
    int val_w = L.content_w - 32 - L.wifi_val_x;
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    // Fixed size + CLIP enforces single-line; text is pre-truncated in software.
    lv_obj_set_size(lbl, val_w, L.wifi_row_h);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(lbl, "(none)");
    lv_obj_set_pos(lbl, L.wifi_val_x, y);
    return lbl;
}

static void init_wifi_screen(lv_obj_t* scr) {
    wifi_container = lv_obj_create(scr);
    lv_obj_set_size(wifi_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(wifi_container, 0, 0);
    lv_obj_set_style_bg_opa(wifi_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_container, 0, 0);
    lv_obj_set_style_pad_all(wifi_container, 0, 0);
    lv_obj_clear_flag(wifi_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(wifi_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_title = lv_label_create(wifi_container);
    lv_label_set_text(lbl_title, "Wi-Fi");
    lv_obj_set_style_text_font(lbl_title, L.bt_title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    lv_obj_t* p = make_panel(wifi_container, L.margin, L.content_y,
                             L.content_w, L.wifi_panel_h);

    static const char* const keys[] = { "SSID", "Pass", "Token" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t* k = lv_label_create(p);
        lv_label_set_text(k, keys[i]);
        lv_obj_set_style_text_font(k, L.bt_device_font, 0);
        lv_obj_set_style_text_color(k, COL_DIM, 0);
        lv_obj_set_pos(k, 0, i * L.wifi_row_h);
    }

    lbl_wifi_ssid_val  = make_wifi_val_label(p, 0);
    lbl_wifi_pass_val  = make_wifi_val_label(p, L.wifi_row_h);
    lbl_wifi_token_val = make_wifi_val_label(p, 2 * L.wifi_row_h);

    int btn_y  = L.content_y + L.wifi_panel_h + 16;
    int note_y = btn_y + L.wifi_btn_h + 12;

    lv_obj_t* btn_zone = lv_obj_create(wifi_container);
    lv_obj_set_pos(btn_zone, L.margin, btn_y);
    lv_obj_set_size(btn_zone, L.content_w, L.wifi_btn_h);
    lv_obj_set_style_bg_color(btn_zone, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(btn_zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_zone, 8, 0);
    lv_obj_set_style_border_width(btn_zone, 0, 0);
    lv_obj_set_flex_flow(btn_zone, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_zone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_zone, wifi_hotspot_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* btn_lbl = lv_label_create(btn_zone);
    lv_label_set_text(btn_lbl, "Start Hotspot");
    lv_obj_set_style_text_font(btn_lbl, L.bt_device_font, 0);
    lv_obj_set_style_text_color(btn_lbl, COL_AMBER, 0);

    lbl_wifi_note = lv_label_create(wifi_container);
    lv_obj_set_style_text_font(lbl_wifi_note, L.bt_credit_1_font, 0);
    lv_obj_set_style_text_color(lbl_wifi_note, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_wifi_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_wifi_note, L.content_w);
    lv_label_set_long_mode(lbl_wifi_note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_wifi_note, "Configure via serial at 115200 baud");
    lv_obj_set_pos(lbl_wifi_note, L.margin, note_y);

    lv_obj_add_flag(wifi_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Bluetooth Screen ========

static void init_bluetooth_screen(lv_obj_t* scr) {
    ble_container = lv_obj_create(scr);
    lv_obj_set_size(ble_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(ble_container, 0, 0);
    lv_obj_set_style_bg_opa(ble_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ble_container, 0, 0);
    lv_obj_set_style_pad_all(ble_container, 0, 0);
    lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ble_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_ble_title = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_title, "Bluetooth");
    lv_obj_set_style_text_font(lbl_ble_title, L.bt_title_font, 0);
    lv_obj_set_style_text_color(lbl_ble_title, COL_TEXT, 0);
    lv_obj_align(lbl_ble_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    lv_obj_t* p_info = make_panel(ble_container, L.margin, L.content_y,
                                  L.content_w, L.bt_info_panel_h);

    static lv_image_dsc_t icon_bt_dsc;
    init_icon_dsc(&icon_bt_dsc, ICON_BLUETOOTH_W, ICON_BLUETOOTH_H, icon_bluetooth_data);

    lv_obj_t* bt_img = lv_image_create(p_info);
    lv_image_set_src(bt_img, &icon_bt_dsc);
    lv_obj_set_pos(bt_img, 0, 0);

    lbl_ble_status = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_status, "Initializing...");
    lv_obj_set_style_text_font(lbl_ble_status, L.bt_status_font, 0);
    lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_status, 56, 2);

    lbl_ble_device = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_device, "Device: ---");
    lv_obj_set_style_text_font(lbl_ble_device, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_ble_device, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_device, 0, 64);

    lbl_ble_mac = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_mac, "Address: ---");
    lv_obj_set_style_text_font(lbl_ble_mac, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_ble_mac, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_mac, 0, 100);

    int reset_y = L.content_y + L.bt_info_panel_h + 16;
    lv_obj_t* reset_zone = lv_obj_create(ble_container);
    lv_obj_set_pos(reset_zone, L.margin, reset_y);
    lv_obj_set_size(reset_zone, L.content_w, L.bt_reset_zone_h);
    lv_obj_set_style_bg_color(reset_zone, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(reset_zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(reset_zone, 8, 0);
    lv_obj_set_style_border_width(reset_zone, 0, 0);
    lv_obj_set_style_pad_column(reset_zone, 14, 0);
    lv_obj_set_flex_flow(reset_zone, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reset_zone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(reset_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(reset_zone, ble_reset_click_cb, LV_EVENT_CLICKED, NULL);

    static lv_image_dsc_t icon_trash_dsc;
    init_icon_dsc(&icon_trash_dsc, ICON_TRASH2_W, ICON_TRASH2_H, icon_trash2_data);
    lv_obj_t* trash_img = lv_image_create(reset_zone);
    lv_image_set_src(trash_img, &icon_trash_dsc);

    lv_obj_t* reset_lbl = lv_label_create(reset_zone);
    lv_label_set_text(reset_lbl, "Reset Bluetooth");
    lv_obj_set_style_text_font(reset_lbl, L.bt_device_font, 0);
    lv_obj_set_style_text_color(reset_lbl, COL_DIM, 0);

    lv_obj_t* lbl_credit = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit, "Built by @hermannbjorgvin");
    lv_obj_set_style_text_font(lbl_credit, L.bt_credit_1_font, 0);
    lv_obj_set_style_text_color(lbl_credit, COL_DIM, 0);
    lv_obj_align(lbl_credit, LV_ALIGN_BOTTOM_MID, 0, -46);

    lv_obj_t* lbl_credit2 = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit2, "Clawd animation by @amaanbuilds");
    lv_obj_set_style_text_font(lbl_credit2, L.bt_credit_2_font, 0);
    lv_obj_set_style_text_color(lbl_credit2, COL_DIM, 0);
    lv_obj_align(lbl_credit2, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_battery_icons();

    init_usage_screen(scr);
    init_settings_screen(scr);
    init_bluetooth_screen(scr);
    init_wifi_screen(scr);
    ui_settings_refresh();
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);
}

// Pill highlight for the currently-binding rate-limit window (from the
// anthropic-ratelimit-unified-representative-claim header): the binding
// window's pill goes brand terracotta so you can see at a glance which
// limit you'd hit first.
static void style_binding_pill(lv_obj_t* pill, bool binding) {
    if (!pill) return;
    lv_obj_set_style_bg_color(pill, binding ? COL_ACCENT : COL_BAR_BG, 0);
    lv_obj_set_style_text_color(pill, binding ? COL_BG : COL_TEXT, 0);
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    last_data_ms  = lv_tick_get();   // a valid usage update just landed
    data_received = true;

    int s_pct = (int)(data->session_pct + 0.5f);
    char buf[48];

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc
        lv_obj_set_style_text_font(lbl_session_pct, &font_tiempos_56, 0);
        lv_label_set_text(lbl_session_label, "Spending");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        style_binding_pill(lbl_session_label, false);
        style_binding_pill(lbl_weekly_label,  false);

        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, &font_styrene_48, 0);
        lv_label_set_text(lbl_session_label, "Current");
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        style_binding_pill(lbl_session_label, data->session_binding);
        style_binding_pill(lbl_weekly_label,  data->weekly_binding);

        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(data->session_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    if (data->enterprise) {
        // Period box: time % + pace color + "Resets <date>" (fe7a7ee).
        const char* pace_text = "Under pace";
        const char* pace_hex  = "788c5d";   // THEME_GREEN
        if (data->session_pct > (float)data->time_pct + 15.0f) {
            pace_text = "Over pace"; pace_hex = "c0392b";   // THEME_RED
        } else if (data->session_pct > (float)data->time_pct - 15.0f) {
            pace_text = "On pace";   pace_hex = "d97757";   // THEME_AMBER
        }
        lv_label_set_text(lbl_weekly_label, "Period");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        lv_color_t bar_pace =
            (data->session_pct <= (float)data->time_pct)         ? COL_GREEN :
            (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                                                                   COL_RED;
        lv_obj_set_style_bg_color(bar_weekly, bar_pace, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "#%s %s# - #faf9f5 Resets %s#",
                 pace_hex, pace_text, data->reset_date);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text(lbl_weekly_label, "Weekly");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
        format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_weekly_reset, buf);
    }
}

// Swap panels <-> stale Zzz view based on data freshness. Only re-lays-out on
// an actual change. Stale only ever shows after at least one valid update —
// before that the panels show "---%" and the status overlay explains itself.
static void update_stale_view(void) {
    if (!panels_group || !stale_group) return;
    bool stale = data_received &&
                 (lv_tick_get() - last_data_ms) >= DATA_FRESH_MS;
    if (stale == showing_stale) return;
    showing_stale = stale;
    if (stale) {
        lv_obj_add_flag(panels_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(stale_group, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(stale_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(panels_group, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;
    update_stale_view();
    if (showing_stale) splash_mini_tick();  // animate the sleeping creature
    if (lv_obj_has_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN)) return;

    uint32_t now = lv_tick_get();

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms >= spinner_ms[anim_spinner_idx]) {
        anim_last_ms = now;
        anim_phase = (anim_phase + 1) % SPINNER_PHASES;
        anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                        : (SPINNER_PHASES - anim_phase);

        static char buf[80];
        // On the stale view, alternate "Listening…"/"No data…" so the screen
        // reads as alive AND explicitly data-less (upstream 72524ef).
        const char* word = showing_stale
            ? ((anim_msg_idx & 1) ? "No data" : "Listening")
            : anim_messages[anim_msg_idx];
        snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
                 spinner_frames[anim_spinner_idx], word);
        lv_label_set_text(lbl_anim, buf);
    }
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (s_nav_locked) return;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

static void ble_reset_click_cb(lv_event_t* e) {
    (void)e;
    ble_clear_bonds();
}

static void wifi_hotspot_click_cb(lv_event_t* e) {
    (void)e;
    s_hotspot_requested = true;
}

bool ui_hotspot_requested(void) {
    if (!s_hotspot_requested) return false;
    s_hotspot_requested = false;
    return true;
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:     splash_show(); break;
    case SCREEN_USAGE:      lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_SETTINGS:
        ui_settings_refresh();  // widgets reflect current state on entry
        lv_obj_clear_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
        break;
    case SCREEN_BLUETOOTH:  lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_WIFI:       lv_obj_clear_flag(wifi_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (logo_img) {
        if (screen == SCREEN_SPLASH) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_set_nav_locked(bool locked) { s_nav_locked = locked; }

void ui_cycle_screen(void) {
    if (s_nav_locked) return;
    screen_t next;
    switch (current_screen) {
    case SCREEN_USAGE:     next = SCREEN_SETTINGS;  break;
    case SCREEN_SETTINGS:  next = SCREEN_BLUETOOTH; break;
    case SCREEN_BLUETOOTH: next = SCREEN_WIFI;      break;
    case SCREEN_WIFI:      next = SCREEN_USAGE;     break;
    default:               next = SCREEN_USAGE;     break;
    }
    ui_show_screen(next);
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_set_status(ui_status_level_t level, const char* msg) {
    if (!msg || msg[0] == '\0' || level == UI_STATUS_NONE) {
        lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_color_t col;
    switch (level) {
        case UI_STATUS_ERROR: col = COL_RED;   break;
        case UI_STATUS_WARN:  col = COL_AMBER; break;
        default:              col = COL_DIM;   break;
    }
    lv_obj_set_style_text_color(lbl_status, col, 0);
    lv_label_set_text(lbl_status, msg);
    lv_obj_clear_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
}

void ui_update_wifi_creds(bool portal_active) {
    String ssid  = provisioning_get_ssid();
    String pass  = provisioning_get_pass();
    String token = provisioning_get_token();

    char ssid_buf[20];
    if (ssid.length() == 0)       strlcpy(ssid_buf, "(none)", sizeof(ssid_buf));
    else if (ssid.length() <= 16) strlcpy(ssid_buf, ssid.c_str(), sizeof(ssid_buf));
    else                          snprintf(ssid_buf, sizeof(ssid_buf), "%.13s...", ssid.c_str());
    lv_label_set_text(lbl_wifi_ssid_val, ssid_buf);

    char buf[32];
    redact_password(pass, buf, sizeof(buf));
    lv_label_set_text(lbl_wifi_pass_val, buf);

    redact_token(token, buf, sizeof(buf));
    lv_label_set_text(lbl_wifi_token_val, buf);

    lv_label_set_text(lbl_wifi_note, portal_active
        ? "Join Wi-Fi: ClawdMeter\nthen open 192.168.4.1"
        : "Configure via serial at 115200 baud");
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    switch (state) {
    case BLE_STATE_CONNECTED:
        lv_label_set_text(lbl_ble_status, "Connected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_GREEN, 0);
        break;
    case BLE_STATE_ADVERTISING:
        lv_label_set_text(lbl_ble_status, "Advertising...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_AMBER, 0);
        break;
    case BLE_STATE_DISCONNECTED:
        lv_label_set_text(lbl_ble_status, "Disconnected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_RED, 0);
        break;
    default:
        lv_label_set_text(lbl_ble_status, "Initializing...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
        break;
    }

    if (name) {
        static char nbuf[48];
        snprintf(nbuf, sizeof(nbuf), "Device: %s", name);
        lv_label_set_text(lbl_ble_device, nbuf);
    }
    if (mac) {
        static char mbuf[48];
        snprintf(mbuf, sizeof(mbuf), "Address: %s", mac);
        lv_label_set_text(lbl_ble_mac, mbuf);
    }
}

void ui_update_battery(int percent, bool charging) {
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
