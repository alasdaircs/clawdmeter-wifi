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

// Two GTS Root R4 certificates — different key pairs, both needed for robustness.
//
// K1: self-signed original (2016-06-22 to 2036-06-22). Distributed in OS trust
//     stores; Windows/Edge build the chain leaf<-WE1<-K1 directly.
// K2: cross-certified by GlobalSign (2023-11-15 to 2028-01-28). Sent by the
//     server in its chain for clients that have GlobalSign but not K1.
//
// It is ambiguous which key signed WE1 (server says K2; OS clients use K1).
// mbedTLS tries both against the received chain and succeeds with whichever fits.
// Renew K2 before 2028-01-28.
static const char GTS_ROOT_R4_CA[] =
    // K1 -- self-signed, expires 2036-06-22
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCjCCAZGgAwIBAgIQbkepyIuUtui7OyrYorLBmTAKBggqhkjOPQQDAzBHMQsw\n"
    "CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
    "MBIGA1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
    "MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
    "Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQA\n"
    "IgNiAATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzu\n"
    "hXyiQHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/l\n"
    "xKvRHYqjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1Ud\n"
    "DgQWBBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNnADBkAjBqUFJ0\n"
    "CMRw3J5QdCHojXohw0+WbhXRIjVhLfoIN+4Zba3bssx9BzT1YBkstTTZbyACMANx\n"
    "sbqjYAuG7ZoIapVon+Kz4ZNkfF6Tpt95LY2F45TPI11xzPKwTdb+mciUqXWi4w==\n"
    "-----END CERTIFICATE-----\n"
    // K2 -- cross-certified by GlobalSign, expires 2028-01-28
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDejCCAmKgAwIBAgIQf+UwvzMTQ77dghYQST2KGzANBgkqhkiG9w0BAQsFADBX\n"
    "MQswCQYDVQQGEwJCRTEZMBcGA1UEChMQR2xvYmFsU2lnbiBudi1zYTEQMA4GA1UE\n"
    "CxMHUm9vdCBDQTEbMBkGA1UEAxMSR2xvYmFsU2lnbiBSb290IENBMB4XDTIzMTEx\n"
    "NTAzNDMyMVoXDTI4MDEyODAwMDA0MlowRzELMAkGA1UEBhMCVVMxIjAgBgNVBAoT\n"
    "GUdvb2dsZSBUcnVzdCBTZXJ2aWNlcyBMTEMxFDASBgNVBAMTC0dUUyBSb290IFI0\n"
    "MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAE83Rzp2iLYK5DuDXFgTB7S0md+8Fhzube\n"
    "Rr1r1WEYNa5A3XP3iZEwWus87oV8okB2O6nGuEfYKueSkWpz6bFyOZ8pn6KY019e\n"
    "WIZlD6GEZQbR3IvJx3PIjGov5cSr0R2Ko4H/MIH8MA4GA1UdDwEB/wQEAwIBhjAd\n"
    "BgNVHSUEFjAUBggrBgEFBQcDAQYIKwYBBQUHAwIwDwYDVR0TAQH/BAUwAwEB/zAd\n"
    "BgNVHQ4EFgQUgEzW63T/STaj1dj8tT7FavCUHYwwHwYDVR0jBBgwFoAUYHtmGkUN\n"
    "l8qJUC99BM00qP/8/UswNgYIKwYBBQUHAQEEKjAoMCYGCCsGAQUFBzAChhpodHRw\n"
    "Oi8vaS5wa2kuZ29vZy9nc3IxLmNydDAtBgNVHR8EJjAkMCKgIKAehhxodHRwOi8v\n"
    "Yy5wa2kuZ29vZy9yL2dzcjEuY3JsMBMGA1UdIAQMMAowCAYGZ4EMAQIBMA0GCSqG\n"
    "SIb3DQEBCwUAA4IBAQAYQrsPBtYDh5bjP2OBDwmkoWhIDDkic574y04tfzHpn+cJ\n"
    "odI2D4SseesQ6bDrarZ7C30ddLibZatoKiws3UL9xnELz4ct92vID24FfVbiI1hY\n"
    "+SW6FoVHkNeWIP0GCbaM4C6uVdF5dTUsMVs/ZbzNnIdCp5Gxmx5ejvEau8otR/Cs\n"
    "kGN+hr/W5GvT1tMBjgWKZ1i4//emhA1JG1BbPzoLJQvyEotc03lXjTaCzv8mEbep\n"
    "8RqZ7a2CPsgRbuvTPBwcOMBBmuFeU88+FSBX6+7iP0il8b4Z0QFqIwwMHfs/L6K1\n"
    "vepuoxtGzi4CZ68zJpiq1UvSqTbFJjtbD4seiMHl\n"
    "-----END CERTIFICATE-----\n";

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

static wifi_state_t       s_state          = WIFI_ST_IDLE;
static uint32_t           s_state_ts       = 0;
static uint32_t           s_last_poll      = 0;

static UsageData          s_data           = {};
static volatile bool      s_has_new_data   = false;
static volatile bool      s_poll_busy      = false;
static bool               s_stop_polling   = false;
static TaskHandle_t       s_poll_task      = nullptr;
static SemaphoreHandle_t  s_mutex          = nullptr;
static StackType_t*       s_poll_stack     = nullptr;
static StaticTask_t       s_poll_tcb;

static wifi_poll_status_t s_status         = WIFI_POLL_INIT;
static int                s_last_http_code = 0;
static int                s_fail_count     = 0;  // successive connection timeouts

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
    s_status   = WIFI_POLL_CONNECTING;
    Serial.printf("wifi: connecting to \"%s\"...\n", ssid.c_str());
}

static int unix_to_reset_mins(const String& ts_str) {
    if (ts_str.length() == 0) return -1;
    time_t now = time(nullptr);
    if (now < 1000000000L) return -1;
    long ts = ts_str.toInt();
    if (ts <= 0) return -1;
    long diff = (long)ts - (long)now;
    return diff > 0 ? (int)(diff / 60) : 0;
}

static void do_poll() {
    if (!provisioning_has_token()) {
        s_status = WIFI_POLL_NO_TOKEN;
        Serial.println("wifi: no token, skipping poll");
        return;
    }

    Serial.println("wifi: poll start");

    WiFiClientSecure tls;
    tls.setCACert(GTS_ROOT_R4_CA);

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.collectHeaders(POLL_HEADERS, POLL_HEADER_COUNT);

    if (!http.begin(tls, "https://" + String(API_HOST) + "/v1/messages")) {
        s_status         = WIFI_POLL_API_ERROR;
        s_last_http_code = -1;
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

        s_status = WIFI_POLL_OK;

    } else if (code == 401) {
        s_status       = WIFI_POLL_TOKEN_INVALID;
        s_stop_polling = true;
        Serial.println("wifi: 401 — token invalid, polling stopped");

    } else {
        s_status         = WIFI_POLL_API_ERROR;
        s_last_http_code = code;
        Serial.printf("wifi: API error %d\n", code);
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
        s_status = WIFI_POLL_NO_CREDS;
        Serial.println("wifi: no credentials, skipping");
        return;
    }
    begin_connect();
}

void wifi_poller_tick(void) {
    uint32_t now = millis();

    switch (s_state) {
        case WIFI_ST_IDLE:
            break;

        case WIFI_ST_CONNECTING:
            if (WiFi.status() == WL_CONNECTED) {
                s_state      = WIFI_ST_CONNECTED;
                s_status     = WIFI_POLL_IDLE;
                s_fail_count = 0;
                Serial.printf("wifi: connected, IP=%s\n",
                    WiFi.localIP().toString().c_str());
                configTime(0, 0, "pool.ntp.org");
            } else if (now - s_state_ts >= CONNECT_TIMEOUT_MS) {
                WiFi.disconnect();
                s_state    = WIFI_ST_FAILED;
                s_state_ts = now;
                s_status   = WIFI_POLL_WIFI_FAIL;
                s_fail_count++;
                Serial.printf("wifi: timeout (status=%d), fail #%d, retry in %us\n",
                    WiFi.status(), s_fail_count, RETRY_INTERVAL_MS / 1000);
            }
            break;

        case WIFI_ST_CONNECTED:
            if (WiFi.status() != WL_CONNECTED) {
                s_state    = WIFI_ST_FAILED;
                s_state_ts = now;
                s_status   = WIFI_POLL_WIFI_FAIL;
                Serial.println("wifi: lost connection, retry in 30s");
            } else if (!s_poll_busy && !s_stop_polling &&
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

wifi_poll_status_t wifi_poller_get_status(void)    { return s_status; }
int wifi_poller_get_last_http_code(void)           { return s_last_http_code; }
int wifi_poller_get_fail_count(void)               { return s_fail_count; }

void wifi_poller_stop(void) {
    s_state        = WIFI_ST_IDLE;
    s_status       = WIFI_POLL_NO_CREDS;
    s_stop_polling = true;
    s_fail_count   = 0;
    WiFi.disconnect();
    Serial.println("wifi: stopped");
}
