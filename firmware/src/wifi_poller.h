#pragma once
#include "data.h"

typedef enum {
    WIFI_POLL_INIT = 0,
    WIFI_POLL_NO_CREDS,       // no SSID/pass stored
    WIFI_POLL_NO_TOKEN,       // no API token stored
    WIFI_POLL_CONNECTING,     // connecting to Wi-Fi
    WIFI_POLL_WIFI_FAIL,      // Wi-Fi connect timed out
    WIFI_POLL_IDLE,           // connected, waiting for next poll interval
    WIFI_POLL_OK,             // last poll succeeded
    WIFI_POLL_API_ERROR,      // HTTP non-200 (see wifi_poller_get_last_http_code)
    WIFI_POLL_TOKEN_INVALID,  // HTTP 401 — credentials expired (backs off, self-heals)
    WIFI_POLL_LIMIT_REACHED,  // HTTP 429 — account usage window exhausted (backs off, displays as blocked)
    WIFI_POLL_API_DOWN,       // HTTP 5xx — Anthropic API error (backs off)
} wifi_poll_status_t;

void wifi_poller_init(void);
void wifi_poller_tick(void);
bool wifi_poller_is_connected(void);
bool wifi_poller_has_new_data(void);
void wifi_poller_consume_data(UsageData* out);
wifi_poll_status_t wifi_poller_get_status(void);
int wifi_poller_get_last_http_code(void);
int wifi_poller_get_fail_count(void);

// True once the stored credentials have connected successfully since boot.
// Gates the auto captive-portal fallback: proven credentials mean connect
// failures are environmental (out of range, AP down), so the poller keeps
// retrying instead of surrendering to hotspot mode.
bool wifi_poller_has_ever_connected(void);
void wifi_poller_stop(void);
