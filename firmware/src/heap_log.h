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
//
// The same flag adds CPU load lines ("cpu:", see cpu_log below) to the 60 s
// loop log, plus a "cpu" serial command that reports the interval since the
// previous sample — send it twice, a few seconds apart, to time one screen.

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

// CPU load over the interval since the previous call, from FreeRTOS run-time
// stats (enabled in the prebuilt Arduino sdkconfig). Lines start with "cpu:":
// per-core load (100% minus that core's idle task), then every task that used
// at least 1% of one core. loopTask is LVGL rendering + UI; the first call
// only takes a baseline. The run-time counter is a u32 of microseconds, so
// intervals must stay under ~71 min (the 60 s loop log guarantees that).
// Keeps static snapshots — call only from the main loop / setup().
#define CPU_LOG_MAX_TASKS 32
static inline void cpu_log(const char* tag) {
    static TaskStatus_t prev[CPU_LOG_MAX_TASKS], cur[CPU_LOG_MAX_TASKS];
    static UBaseType_t  prev_n = 0;
    static uint32_t     prev_total = 0;

    uint32_t    total = 0;
    UBaseType_t n = uxTaskGetSystemState(cur, CPU_LOG_MAX_TASKS, &total);
    uint32_t    dt = total - prev_total;
    bool        have_prev = prev_n > 0 && dt > 0;

    // A task's run time this interval, or 0 if it didn't exist last time.
    auto delta = [&](const TaskStatus_t& t) -> uint32_t {
        for (UBaseType_t j = 0; j < prev_n; j++)
            if (prev[j].xHandle == t.xHandle) return t.ulRunTimeCounter - prev[j].ulRunTimeCounter;
        return 0;
    };
    // Tenths of a percent of one core.
    auto pct10 = [&](uint32_t d) -> unsigned { return (unsigned)((uint64_t)d * 1000 / dt); };

    if (!have_prev) {
        Serial.printf("cpu: %-16s baseline (%u tasks)\n", tag, (unsigned)n);
    } else {
        char line[96];
        int  len = snprintf(line, sizeof(line), "cpu: %-16s t=%lus dt=%lums",
                            tag, (unsigned long)(millis() / 1000), (unsigned long)(dt / 1000));
        for (int core = 0; core < portNUM_PROCESSORS; core++) {
            TaskHandle_t idle = xTaskGetIdleTaskHandleForCore(core);
            for (UBaseType_t i = 0; i < n; i++) {
                if (cur[i].xHandle != idle) continue;
                unsigned busy = 1000 - min(pct10(delta(cur[i])), 1000u);
                len += snprintf(line + len, sizeof(line) - len, " core%d=%u.%u%%",
                                core, busy / 10, busy % 10);
            }
        }
        Serial.println(line);

        // Busiest first: repeatedly pick the largest remaining delta.
        bool shown[CPU_LOG_MAX_TASKS] = {};
        while (true) {
            int      best = -1;
            uint32_t best_d = 0;
            for (UBaseType_t i = 0; i < n; i++) {
                if (shown[i]) continue;
                uint32_t d = delta(cur[i]);
                if (best < 0 || d > best_d) { best = (int)i; best_d = d; }
            }
            if (best < 0 || pct10(best_d) < 10) break;
            shown[best] = true;
            unsigned p = pct10(best_d);
            Serial.printf("cpu:   %-16s %u.%u%%\n", cur[best].pcTaskName, p / 10, p % 10);
        }
    }

    memcpy(prev, cur, n * sizeof(TaskStatus_t));
    prev_n = n;
    prev_total = total;
}

#define HEAP_LOG(tag)          heap_log(tag)
#define HEAP_LOG_N(tag, n)     heap_log(tag, n)
#define HEAP_LOG_LVGL(tag)     heap_log_lvgl(tag)
#define HEAP_LOG_STACK(tag)    heap_log_stack(tag)
#define CPU_LOG(tag)           cpu_log(tag)
#else
#define HEAP_LOG(tag)          ((void)0)
#define HEAP_LOG_N(tag, n)     ((void)(n))
#define HEAP_LOG_LVGL(tag)     ((void)0)
#define HEAP_LOG_STACK(tag)    ((void)0)
#define CPU_LOG(tag)           ((void)0)
#endif
