#include "brightness.h"
#include "idle.h"
#include <Preferences.h>
#include <Arduino.h>

// User brightness is a raw 0..255 PWM level persisted to NVS ("brt_lvl"),
// set from the settings-screen slider. Floor of 10 so the panel can't be
// slid to fully dark (which would look like a dead device). LEVELS survives
// only for migrating the pre-slider "brt_idx" preset storage.
static const uint8_t LEVELS[] = {64, 128, 200, 255};
#define LEVELS_COUNT (sizeof(LEVELS) / sizeof(LEVELS[0]))
#define DEFAULT_LEVEL 200   // == the old hard-coded DISPLAY_DEFAULT_BRIGHTNESS
#define MIN_LEVEL     10

static uint8_t cur_level = DEFAULT_LEVEL;

static void save_and_apply(void) {
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putUChar("brt_lvl", cur_level);
    prefs.end();
    idle_set_awake_brightness(cur_level);
}

void brightness_init(void) {
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    uint8_t saved = prefs.getUChar("brt_lvl", 0);
    if (saved == 0) {
        // Migrate from the pre-slider index storage ("brt_idx", 0..3).
        uint8_t idx = prefs.getUChar("brt_idx", 0xFF);
        if (idx < LEVELS_COUNT) saved = LEVELS[idx];
    }
    prefs.end();

    cur_level = (saved >= MIN_LEVEL) ? saved : DEFAULT_LEVEL;
    idle_set_awake_brightness(cur_level);
    Serial.printf("Brightness init: level=%u\n", cur_level);
}

void brightness_set(uint8_t level) {
    if (level < MIN_LEVEL) level = MIN_LEVEL;
    if (level == cur_level) return;
    cur_level = level;
    save_and_apply();
    Serial.printf("Brightness set: level=%u\n", cur_level);
}

uint8_t brightness_get(void) {
    return cur_level;
}
