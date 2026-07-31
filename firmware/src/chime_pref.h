#pragma once
#include <stdbool.h>

// Session-reset chime opt-in, persisted to NVS. Default off (matches
// upstream's daemon-config default). Toggled from the settings screen or
// the `chime on|off` serial command; read by main.cpp's reset trigger.
void chime_pref_load(void);
void chime_pref_set(bool on);
bool chime_pref_get(void);
