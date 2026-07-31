#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "splash.h"
#include "usage_rate.h"
#include "idle.h"
#include "brightness.h"
#include "chime_pref.h"
#include "idle_cfg.h"
#include "provisioning.h"
#include "wifi_poller.h"
#include "captive_portal.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/sound_hal.h"

static UsageData usage = {};

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}



// ---- Serial command buffer ----
#define CMD_BUF_SIZE 256
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (cmd_pos > 0) {
                if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
                else if (strcmp(cmd_buf, "buzz") == 0)  sound_hal_play_reset();
                else if (strcmp(cmd_buf, "chime on") == 0)  { chime_pref_set(true);  ui_settings_refresh(); }
                else if (strcmp(cmd_buf, "chime off") == 0) { chime_pref_set(false); ui_settings_refresh(); }
                else if (strcmp(cmd_buf, "chime") == 0)
                    Serial.printf("chime: %s\n", chime_pref_get() ? "on" : "off");
                else {
                    provisioning_handle_cmd(cmd_buf);
                    ui_update_wifi_creds(captive_portal_is_active());
                }
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();
    provisioning_init();
    chime_pref_load();

    display_hal_init();
    display_hal_begin();
    idle_init();       // takes over brightness and starts the idle timer
    brightness_init(); // load the user's saved brightness level, applied via idle

    power_hal_init();
    imu_hal_init();
    sound_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();
    wifi_poller_init();
    captive_portal_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    ui_update_wifi_creds(captive_portal_is_active());
    if (captive_portal_is_active()) {
        ui_set_nav_locked(true);
        ui_show_screen(SCREEN_WIFI);
    } else {
        ui_show_screen(SCREEN_SPLASH);
    }

    Serial.printf("Dashboard ready (%s, %dx%d)\n", board_caps().name, W, H);
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

void loop() {
    idle_tick();
    lv_timer_handler();
    wifi_poller_tick();
    captive_portal_tick();
    ui_tick_anim();
    ble_tick();
    power_hal_tick();
    imu_hal_tick();
    sound_hal_tick();
    splash_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    //   PRIMARY   → HID Space  (Claude Code voice-mode PTT)
    //   SECONDARY → HID Shift+Tab  (mode toggle; only if the board has one)
    //   PWR       → cycle screens; on splash, cycle animations
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); the normal action fires from the second
    // press. Activity bookkeeping happens inside idle_consume_wake_press
    // so no separate idle_note_activity() call is needed here.
    {
        static bool primary_was = false;
        static bool primary_wake_swallowed = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now != primary_was) {
            if (primary_now) {
                if (idle_consume_wake_press()) primary_wake_swallowed = true;
                else                            ble_keyboard_press(0x2C, 0);  // HID Space, no mods
            } else {
                if (primary_wake_swallowed) primary_wake_swallowed = false;
                else                        ble_keyboard_release();
            }
            primary_was = primary_now;
        }

        if (board_caps().button_count >= 2) {
            static bool secondary_was = false;
            static bool secondary_wake_swallowed = false;
            bool secondary_now = input_hal_is_held(INPUT_BTN_SECONDARY);
            if (secondary_now != secondary_was) {
                if (secondary_now) {
                    if (idle_consume_wake_press()) secondary_wake_swallowed = true;
                    else                            ble_keyboard_press(0x2B, 0x02);  // HID Tab + LEFT_SHIFT
                } else {
                    if (secondary_wake_swallowed) secondary_wake_swallowed = false;
                    else                          ble_keyboard_release();
                }
                secondary_was = secondary_now;
            }
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
                else                                          ui_cycle_screen();
            }
        }

        // PWR long-press (~1.5s) cycles display brightness; persisted to NVS.
        // Short-press keeps its screen/animation cycling role on this fork.
        if (power_hal_pwr_long_pressed()) {
            if (!idle_consume_wake_press()) {
                brightness_cycle();
                ui_settings_refresh();  // keep the settings slider in sync
            }
        }
        power_hal_pwr_released();  // drain the release edge (unused here)
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    // Start hotspot: manual button on Wi-Fi screen, or 3 successive connect
    // failures — but only while the stored credentials are unproven. Once
    // they've connected successfully this boot, failures mean out-of-range /
    // AP down, and the poller retries forever instead (portal is one-way
    // until reboot, which stranded the device after a walk out of range).
    if (!captive_portal_is_active()) {
        bool manual = ui_hotspot_requested();
        bool auto_fail = (wifi_poller_get_fail_count() >= 3) &&
                         !wifi_poller_has_ever_connected();
        if (manual || auto_fail) {
            if (auto_fail) Serial.println("wifi: 3 failures, switching to hotspot");
            wifi_poller_stop();
            captive_portal_start();
            ui_update_wifi_creds(true);
            ui_set_nav_locked(true);
            ui_show_screen(SCREEN_WIFI);
        }
    }

    check_serial_cmd();

    if (wifi_poller_has_new_data()) {
        wifi_poller_consume_data(&usage);
        int g_before = usage_rate_group();
        bool session_reset = usage_rate_sample(usage.session_pct);
        int g_after = usage_rate_group();
        // 5-hour session limit refilled → chime so the user knows they can
        // use Claude again (no-op on boards without a speaker). Gated on the
        // NVS `chime on` opt-in; the `buzz` serial cmd ignores it.
        if (session_reset && chime_pref_get()) {
            Serial.println("session reset detected — chime");
            sound_hal_play_reset();
        }
        if (g_after != g_before) {
            Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                g_before, g_after, usage.session_pct);
            if (splash_is_active()) splash_pick_for_current_rate();
        }
        ui_update(&usage);
    }

    // Wifi status overlay — update whenever status or data-validity changes.
    {
        static wifi_poll_status_t last_ws   = WIFI_POLL_INIT;
        static bool               ws_first  = true;
        static bool               last_valid = false;
        wifi_poll_status_t ws = wifi_poller_get_status();
        if (ws_first || ws != last_ws || usage.valid != last_valid) {
            ws_first   = false;
            last_ws    = ws;
            last_valid = usage.valid;
            char msg[80] = {};
            ui_status_level_t lvl = UI_STATUS_INFO;
            switch (ws) {
                case WIFI_POLL_NO_TOKEN:
                    strlcpy(msg, "Setup: token <sk-ant-...> via serial", sizeof(msg));
                    lvl = UI_STATUS_WARN; break;
                case WIFI_POLL_CONNECTING:
                    // ASCII only: lbl_status renders in Styrene 20, which was
                    // compiled without the em-dash/ellipsis glyphs (they tofu).
                    strlcpy(msg, "Connecting to Wi-Fi...", sizeof(msg)); break;
                case WIFI_POLL_WIFI_FAIL:
                    strlcpy(msg, "Wi-Fi error - check credentials", sizeof(msg));
                    lvl = UI_STATUS_ERROR; break;
                case WIFI_POLL_TOKEN_INVALID:
                    strlcpy(msg, "Token invalid - re-provision", sizeof(msg));
                    lvl = UI_STATUS_ERROR; break;
                case WIFI_POLL_LIMIT_REACHED:
                    strlcpy(msg, "Usage limit reached", sizeof(msg));
                    lvl = UI_STATUS_ERROR; break;
                case WIFI_POLL_API_DOWN:
                    strlcpy(msg, "Anthropic API down", sizeof(msg));
                    lvl = UI_STATUS_ERROR; break;
                case WIFI_POLL_API_ERROR: {
                    int c = wifi_poller_get_last_http_code();
                    if (c < 0) strlcpy(msg, "API unreachable", sizeof(msg));
                    else        snprintf(msg, sizeof(msg), "API error %d", c);
                    lvl = UI_STATUS_ERROR; break;
                }
                default: break;  // INIT, IDLE, OK — msg stays ""
            }
            // Keep overlay until first data arrives even if wifi status is OK/IDLE.
            if (msg[0] == '\0' && !usage.valid) {
                strlcpy(msg, "Connecting...", sizeof(msg));
            }
            ui_set_status(lvl, msg[0] ? msg : nullptr);
        }
    }

    delay(5);
}
