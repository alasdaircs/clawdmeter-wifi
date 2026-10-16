#pragma once
// Heap diagnostics for chasing internal-SRAM pressure (e.g. TLS handshake
// failures on the PSRAM-free C6). Compiled in only with -DHEAP_DEBUG; without
// it every macro below expands to nothing.
//
// Build with it on, without editing platformio.ini:
//   bash:        PLATFORMIO_BUILD_FLAGS=-DHEAP_DEBUG pio run -d firmware -e <env> -t upload
//   PowerShell:  $env:PLATFORMIO_BUILD_FLAGS="-DHEAP_DEBUG"; pio run -d firmware -e <env> -t upload
//
// Every line starts with "heap:" so a serial log can be filtered with grep.
//   int free / largest — internal 8-bit-capable SRAM right now
//   min                — internal low-water mark since boot (captures the
//                        TLS handshake peak even though we never sample mid-handshake)
//   psram              — free PSRAM (0 on boards without it)
//   lvgl ...           — LVGL's own builtin pool (LV_MEM_SIZE), separate from the above
// A steadily falling "largest" across "poll done" lines means fragmentation
// or a leak; a flat one means the budget is simply too tight.

#ifdef HEAP_DEBUG
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

// Safe from any task — one Serial.printf per line so concurrent tasks don't interleave mid-line.
static inline void heap_log(const char* tag, int n = -1) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    char label[24];
    if (n >= 0) snprintf(label, sizeof(label), "%s #%d", tag, n);
    else        snprintf(label, sizeof(label), "%s", tag);
    Serial.printf("heap: %-16s t=%lus int free=%u largest=%u min=%u psram=%u\n",
        label, (unsigned long)(millis() / 1000),
        (unsigned)heap_caps_get_free_size(caps),
        (unsigned)heap_caps_get_largest_free_block(caps),
        (unsigned)heap_caps_get_minimum_free_size(caps),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// LVGL isn't thread-safe — call only from the main loop / setup().
static inline void heap_log_lvgl(const char* tag) {
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    Serial.printf("heap: %-16s lvgl total=%u free=%u biggest=%u used=%u%% frag=%u%%\n",
        tag, (unsigned)m.total_size, (unsigned)m.free_size,
        (unsigned)m.free_biggest_size, (unsigned)m.used_pct, (unsigned)m.frag_pct);
}

// Remaining headroom on the calling task's stack (bytes on ESP-IDF).
static inline void heap_log_stack(const char* tag) {
    Serial.printf("heap: %-16s stack hwm=%u\n", tag,
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

#define HEAP_LOG(tag)          heap_log(tag)
#define HEAP_LOG_N(tag, n)     heap_log(tag, n)
#define HEAP_LOG_LVGL(tag)     heap_log_lvgl(tag)
#define HEAP_LOG_STACK(tag)    heap_log_stack(tag)
#else
#define HEAP_LOG(tag)          ((void)0)
#define HEAP_LOG_N(tag, n)     ((void)(n))
#define HEAP_LOG_LVGL(tag)     ((void)0)
#define HEAP_LOG_STACK(tag)    ((void)0)
#endif
