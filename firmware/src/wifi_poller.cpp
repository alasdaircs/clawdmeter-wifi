#include "wifi_poller.h"
#include "provisioning.h"
#include <WiFi.h>

#define CONNECT_TIMEOUT_MS  15000
#define RETRY_INTERVAL_MS   30000

enum wifi_state_t {
    WIFI_ST_IDLE,
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_FAILED,
};

static wifi_state_t s_state = WIFI_ST_IDLE;
static uint32_t s_state_ts = 0;

static void begin_connect() {
    String ssid = provisioning_get_ssid();
    WiFi.begin(ssid.c_str(), provisioning_get_pass().c_str());
    s_state = WIFI_ST_CONNECTING;
    s_state_ts = millis();
    Serial.printf("wifi: connecting to \"%s\"...\n", ssid.c_str());
}

void wifi_poller_init(void) {
    if (!provisioning_has_wifi()) {
        Serial.println("wifi: no credentials, skipping");
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);  // we manage retries ourselves
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
                s_state = WIFI_ST_FAILED;
                s_state_ts = now;
                Serial.printf("wifi: timeout (status=%d), retry in %us\n",
                    WiFi.status(), RETRY_INTERVAL_MS / 1000);
            }
            break;

        case WIFI_ST_CONNECTED:
            if (WiFi.status() != WL_CONNECTED) {
                s_state = WIFI_ST_FAILED;
                s_state_ts = now;
                Serial.println("wifi: lost connection, retry in 30s");
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
