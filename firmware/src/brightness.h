#pragma once
#include <stdint.h>

// User-controlled display brightness, persisted to NVS. Set from the
// settings-screen slider. idle owns the actual panel brightness, so this
// routes the chosen level through idle_set_awake_brightness().
void    brightness_init(void);    // load saved level from NVS and apply
void    brightness_set(uint8_t);  // set an arbitrary level (settings slider); min 10
uint8_t brightness_get(void);     // current PWM level (10..255)
