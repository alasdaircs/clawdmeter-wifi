#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_SETTINGS,
    SCREEN_BLUETOOTH,
    SCREEN_WIFI,
    SCREEN_COUNT,
};

typedef enum {
    UI_STATUS_NONE,
    UI_STATUS_INFO,
    UI_STATUS_WARN,
    UI_STATUS_ERROR,
} ui_status_level_t;

void ui_init(void);
void ui_update(const UsageData* data);
void ui_set_status(ui_status_level_t level, const char* msg);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
void ui_update_wifi_creds(bool portal_active);
bool ui_hotspot_requested(void);
void ui_set_nav_locked(bool locked);

// Re-sync the settings screen's widgets (brightness slider, chime toggle)
// from their backing stores. Call after an external change (e.g.
// `chime on|off` over serial).
void ui_settings_refresh(void);
