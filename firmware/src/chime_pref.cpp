#include "chime_pref.h"
#include <Preferences.h>
#include <Arduino.h>

static bool g_chime_enabled = false;

void chime_pref_load(void) {
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    g_chime_enabled = prefs.getBool("chime_en", false);
    prefs.end();
}

void chime_pref_set(bool on) {
    if (on == g_chime_enabled) return;
    g_chime_enabled = on;
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putBool("chime_en", on);
    prefs.end();
    Serial.printf("chime: %s (saved)\n", on ? "on" : "off");
}

bool chime_pref_get(void) {
    return g_chime_enabled;
}
