#include "wifi_poller.h"
#include "provisioning.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#define CONNECT_TIMEOUT_MS  15000
#define RETRY_INTERVAL_MS   30000
#define POLL_INTERVAL_MS    60000
#define HTTP_TIMEOUT_MS     10000

static const char* API_HOST = "api.anthropic.com";

enum wifi_state_t {
    WIFI_ST_IDLE,
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_FAILED,
};

static wifi_state_t s_state     = WIFI_ST_IDLE;
static uint32_t     s_state_ts  = 0;
static uint32_t     s_last_poll = 0;  // 0 = never polled; triggers on first connect

static void begin_connect() {
    String ssid = provisioning_get_ssid();
    WiFi.begin(ssid.c_str(), provisioning_get_pass().c_str());
    s_state    = WIFI_ST_CONNECTING;
    s_state_ts = millis();
    Serial.printf("wifi: connecting to \"%s\"...\n", ssid.c_str());
}

static const char* POLL_HEADERS[] = {
    "anthropic-ratelimit-unified-5h-utilization",
    "anthropic-ratelimit-unified-7d-utilization",
    "anthropic-ratelimit-unified-status",
    "anthropic-ratelimit-unified-representative-claim",
    "anthropic-ratelimit-unified-5h-reset",
};
static const int POLL_HEADER_COUNT = sizeof(POLL_HEADERS) / sizeof(POLL_HEADERS[0]);

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

    if (code > 0) {
        for (int i = 0; i < POLL_HEADER_COUNT; i++) {
            if (http.hasHeader(POLL_HEADERS[i])) {
                Serial.printf("hdr: %s: %s\n", POLL_HEADERS[i], http.header(POLL_HEADERS[i]).c_str());
            }
        }
    }

    http.end();
    Serial.println("wifi: poll done");
}

void wifi_poller_init(void) {
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
            } else if (s_last_poll == 0 || now - s_last_poll >= POLL_INTERVAL_MS) {
                s_last_poll = now;
                do_poll();
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
