#include "wifi_poller.h"
#include "provisioning.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "mbedtls/platform.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <time.h>

#define CONNECT_TIMEOUT_MS  15000
#define RETRY_INTERVAL_MS   30000
#define POLL_INTERVAL_MS    60000
#define HTTP_TIMEOUT_MS     10000

static const char* API_HOST = "api.anthropic.com";

static const char* POLL_HEADERS[] = {
    "anthropic-ratelimit-unified-5h-utilization",
    "anthropic-ratelimit-unified-7d-utilization",
    "anthropic-ratelimit-unified-status",
    "anthropic-ratelimit-unified-representative-claim",
    "anthropic-ratelimit-unified-5h-reset",
};
static const int POLL_HEADER_COUNT = sizeof(POLL_HEADERS) / sizeof(POLL_HEADERS[0]);

enum wifi_state_t {
    WIFI_ST_IDLE,
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_FAILED,
};

static wifi_state_t s_state     = WIFI_ST_IDLE;
static uint32_t     s_state_ts  = 0;
static uint32_t     s_last_poll = 0;

static UsageData          s_data         = {};
static volatile bool      s_has_new_data = false;
static volatile bool      s_poll_busy    = false;
static TaskHandle_t       s_poll_task    = nullptr;
static SemaphoreHandle_t  s_mutex        = nullptr;

// Task stack allocated from PSRAM at runtime — keeps SRAM free for TLS/DMA.
static StackType_t* s_poll_stack = nullptr;
static StaticTask_t s_poll_tcb;

// Route large mbedTLS allocations (SSL record buffers, handshake state) to PSRAM,
// leaving internal SRAM free for the hardware AES engine's DMA descriptors.
static void* tls_calloc_psram(size_t n, size_t size) {
    void* ptr = nullptr;
    if (n * size >= 512) {
        ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!ptr) ptr = calloc(n, size);
    return ptr;
}

static void begin_connect() {
    String ssid = provisioning_get_ssid();
    WiFi.begin(ssid.c_str(), provisioning_get_pass().c_str());
    s_state    = WIFI_ST_CONNECTING;
    s_state_ts = millis();
    Serial.printf("wifi: connecting to \"%s\"...\n", ssid.c_str());
}

static int unix_to_reset_mins(const String& ts_str) {
    if (ts_str.length() == 0) return -1;
    time_t now = time(nullptr);
    if (now < 1000000000L) return -1;  // NTP not synced yet
    long ts = ts_str.toInt();
    if (ts <= 0) return -1;
    long diff = (long)ts - (long)now;
    return diff > 0 ? (int)(diff / 60) : 0;
}

static void do_poll() {
    if (!provisioning_has_token()) {
        Serial.println("wifi: no token, skipping poll");
        return;
    }

    Serial.println("wifi: poll start");

    WiFiClientSecure tls;
    tls.setInsecure();

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.collectHeaders(POLL_HEADERS, POLL_HEADER_COUNT);

    if (!http.begin(tls, "https://" + String(API_HOST) + "/v1/messages")) {
        Serial.println("wifi: http.begin failed");
        return;
    }

    http.addHeader("Authorization", "Bearer " + provisioning_get_token());
    http.addHeader("anthropic-version", "2023-06-01");
    http.addHeader("Content-Type", "application/json");

    String body = "{\"model\":\"claude-haiku-4-5-20251001\","
                  "\"max_tokens\":1,"
                  "\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}]}";

    int code = http.POST(body);
    Serial.printf("wifi: HTTP %d\n", code);

    if (code == 200) {
        UsageData d = {};
        d.session_pct        = http.header(POLL_HEADERS[0]).toFloat() * 100.0f;
        d.weekly_pct         = http.header(POLL_HEADERS[1]).toFloat() * 100.0f;
        strlcpy(d.status, http.header(POLL_HEADERS[2]).c_str(), sizeof(d.status));
        d.session_reset_mins = unix_to_reset_mins(http.header(POLL_HEADERS[4]));
        d.weekly_reset_mins  = -1;
        d.ok    = true;
        d.valid = true;
        Serial.printf("wifi: s=%.1f%% w=%.1f%% status=%s reset=%dm\n",
            d.session_pct, d.weekly_pct, d.status, d.session_reset_mins);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_data         = d;
        s_has_new_data = true;
        xSemaphoreGive(s_mutex);
    }

    http.end();
    Serial.println("wifi: poll done");
}

static void poll_task_fn(void* param) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        do_poll();
        s_poll_busy = false;
    }
}

void wifi_poller_init(void) {
    mbedtls_platform_set_calloc_free(tls_calloc_psram, free);

    s_mutex = xSemaphoreCreateMutex();

    // Allocate task stack from PSRAM so it doesn't fragment internal SRAM.
    s_poll_stack = (StackType_t*)heap_caps_malloc(12288, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_poll_stack) {
        Serial.println("wifi: PSRAM stack alloc failed, falling back to SRAM");
        s_poll_stack = (StackType_t*)malloc(12288);
    }
    s_poll_task = xTaskCreateStaticPinnedToCore(
        poll_task_fn, "wifi_poll",
        12288 / sizeof(StackType_t),
        nullptr, 1,
        s_poll_stack, &s_poll_tcb, 0
    );

    if (!provisioning_has_wifi()) {
        Serial.println("wifi: no credentials, skipping");
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    begin_connect();
}

void wifi_poller_tick(void) {
    uint32_t now = millis();

    switch (s_state) {
        case WIFI_ST_IDLE:
            break;

        case WIFI_ST_CONNECTING:
            if (WiFi.status() == WL_CONNECTED) {
                s_state = WIFI_ST_CONNECTED;
                Serial.printf("wifi: connected, IP=%s\n",
                    WiFi.localIP().toString().c_str());
                configTime(0, 0, "pool.ntp.org");
            } else if (now - s_state_ts >= CONNECT_TIMEOUT_MS) {
                WiFi.disconnect();
                s_state    = WIFI_ST_FAILED;
                s_state_ts = now;
                Serial.printf("wifi: timeout (status=%d), retry in %us\n",
                    WiFi.status(), RETRY_INTERVAL_MS / 1000);
            }
            break;

        case WIFI_ST_CONNECTED:
            if (WiFi.status() != WL_CONNECTED) {
                s_state    = WIFI_ST_FAILED;
                s_state_ts = now;
                Serial.println("wifi: lost connection, retry in 30s");
            } else if (!s_poll_busy &&
                       (s_last_poll == 0 || now - s_last_poll >= POLL_INTERVAL_MS)) {
                s_last_poll = now;
                s_poll_busy = true;
                xTaskNotifyGive(s_poll_task);
            }
            break;

        case WIFI_ST_FAILED:
            if (now - s_state_ts >= RETRY_INTERVAL_MS) {
                begin_connect();
            }
            break;
    }
}

bool wifi_poller_is_connected(void) {
    return s_state == WIFI_ST_CONNECTED;
}

bool wifi_poller_has_new_data(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool r = s_has_new_data;
    xSemaphoreGive(s_mutex);
    return r;
}

void wifi_poller_consume_data(UsageData* out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out           = s_data;
    s_has_new_data = false;
    xSemaphoreGive(s_mutex);
}
