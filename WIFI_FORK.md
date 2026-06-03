# Clawdmeter — Wi-Fi native fork: technical reference

A fork of [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter)
targeting the **Waveshare ESP32-S3-Touch-AMOLED-2.16** board.

This fork replaces the host daemon + BLE data channel with Wi-Fi polling directly from
the device. The ESP32 calls the Anthropic API itself — no host PC, no daemon, no BLE
data channel required. The BLE HID keyboard (physical buttons) is kept unchanged.

---

## What changes vs upstream

| Concern | Upstream | This fork |
|---|---|---|
| Usage data source | Host daemon → BLE GATT RX | ESP32 → Wi-Fi → Anthropic API |
| Token storage | Host filesystem / Keychain | ESP32 NVS (`Preferences`) |
| Provisioning | Host-side install script | Captive portal (primary) or serial |
| BLE data service | NimBLE custom GATT service | **Removed** |
| BLE HID keyboard | NimBLE HID (buttons) | **Kept unchanged** |
| Wi-Fi | Not used | Added |
| Screens | Splash, Usage, Bluetooth | + Wi-Fi credentials/hotspot screen |

The splash animations, display driver, touch, IMU, power management, and HID keyboard
are all **untouched**. The delta is confined to:

- `ble.{h,cpp}` — custom data GATT service stripped; HID kept
- `wifi_poller.{h,cpp}` — Wi-Fi connection state machine and API polling loop
- `provisioning.{h,cpp}` — NVS credential storage and serial command handler
- `captive_portal.{h,cpp}` — AP-mode setup form for first-time and re-provisioning
- `ui.{h,cpp}` — `ui_set_status()` error overlay; new `SCREEN_WIFI` credentials screen
- `main.cpp` — wires new modules into setup/loop; hotspot fallback logic

---

## Architecture

```
On-device NVS
(Wi-Fi SSID/password, OAuth token)
        │
        ▼
  wifi_poller (FreeRTOS task on Core 0)
  ├── WiFi.begin() → connected
  ├── NTP sync (pool.ntp.org)
  ├── WiFiClientSecure + HTTPClient
  ├── POST api.anthropic.com/v1/messages
  │     (1 token, claude-haiku-4-5-20251001)
  ├── Read response headers
  └── Populate UsageData → existing UI + splash logic unchanged

  captive_portal (active only when no credentials stored,
                  or manually triggered via Wi-Fi screen button)
  ├── WiFi.softAP("ClawdMeter")
  ├── DNSServer (wildcard → 192.168.4.1)
  └── WebServer → HTML form → save to NVS → ESP.restart()
```

---

## API call

**Endpoint:** `POST https://api.anthropic.com/v1/messages`

**Headers:**
```
Authorization: Bearer <oauth-token>
anthropic-version: 2023-06-01
Content-Type: application/json
```

**Body** (minimal — 1 output token, cheapest model):
```json
{
  "model": "claude-haiku-4-5-20251001",
  "max_tokens": 1,
  "messages": [{"role": "user", "content": "Hi"}]
}
```

**Data comes from response headers, not the body:**

| Header | Use |
|---|---|
| `anthropic-ratelimit-unified-5h-utilization` | Session % (0.0–1.0) |
| `anthropic-ratelimit-unified-7d-utilization` | Weekly % (0.0–1.0) |
| `anthropic-ratelimit-unified-status` | `allowed` / `throttled` / `exceeded` |
| `anthropic-ratelimit-unified-representative-claim` | `five_hour` or `seven_day` |
| `anthropic-ratelimit-unified-5h-reset` | Unix timestamp of next 5h reset |

**Important:** `Authorization: Bearer` works. `X-Api-Key` returns HTTP 401.

**Poll interval:** 60 seconds. A 401 stops polling permanently until the device is
re-provisioned (no point hammering with a known-bad token).

---

## TLS / memory

`WiFiClientSecure` with `setInsecure()` is used (no cert pinning). Acceptable for a
personal desk device with no sensitive data flowing back from the API.

**Critical:** NimBLE + HTTPS together exhaust internal SRAM. Fix applied in
`wifi_poller.cpp`:

```cpp
// Route mbedTLS allocations ≥512B to PSRAM
static void* tls_calloc_psram(size_t n, size_t size) {
    void* ptr = nullptr;
    if (n * size >= 512)
        ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = calloc(n, size);
    return ptr;
}
mbedtls_platform_set_calloc_free(tls_calloc_psram, free);
```

The poll task stack (12 KB) is also allocated from PSRAM at runtime via
`heap_caps_malloc`. `EXT_RAM_BSS_ATTR` for static stack arrays does NOT work with
this pioarduino build — the stack ends up in SRAM regardless.

---

## Token storage (NVS)

Arduino `Preferences` library, namespace `"clawdmeter"`:

| Key | Value |
|-----|-------|
| `"ssid"` | Wi-Fi network name |
| `"pass"` | Wi-Fi password |
| `"token"` | Anthropic OAuth token (`sk-ant-oat01-…`) |

NVS survives firmware updates. Wiped by `pio run -t erase` or the `clear` serial command.

**Security note:** NVS is not encrypted on this board. The token is extractable with
physical access via `esptool.py read_flash`. Acceptable for a personal desk device —
treat it accordingly.

---

## Provisioning

### Captive portal (primary)

The device starts a Wi-Fi AP named **ClawdMeter** when no SSID+password are stored in
NVS. A DNS server redirects all hostnames to `192.168.4.1`. Any browser on a device
connected to ClawdMeter reaches the setup form; Android and iOS also show a
*"Sign into network"* notification automatically via captive portal detection.

The form accepts SSID, password, and an optional Anthropic token. On submit it writes
to NVS and calls `ESP.restart()`.

The portal can also be triggered at runtime (e.g., when moving to a new network) via
the **Start Hotspot** button on the Wi-Fi screen, or automatically after three
consecutive Wi-Fi connection failures. When active, the device locks navigation to the
Wi-Fi screen until rebooted with valid credentials.

### Serial (secondary)

Commands over UART at 115200 baud:

```
ssid  <value>   store Wi-Fi SSID
pass  <value>   store Wi-Fi password
token <value>   store Anthropic OAuth token
status          print stored values (token masked to first 20 chars)
clear           wipe all NVS keys
screenshot      dump LVGL framebuffer as raw RGB565 over serial
```

---

## Error states

| Condition | `wifi_poller` status | Usage screen overlay |
|---|---|---|
| No SSID/pass in NVS | `WIFI_POLL_NO_CREDS` | *(nav locked to Wi-Fi screen; portal active)* |
| No token in NVS | `WIFI_POLL_NO_TOKEN` | "Setup: token `<sk-ant-...>` via serial" |
| Connecting to AP | `WIFI_POLL_CONNECTING` | "Connecting to Wi-Fi…" |
| Connect timeout | `WIFI_POLL_WIFI_FAIL` | "Wi-Fi error — check credentials" |
| HTTP 200 | `WIFI_POLL_OK` | *(overlay cleared)* |
| HTTP 401 | `WIFI_POLL_TOKEN_INVALID` | "Token invalid — re-provision" (polling stops) |
| Other HTTP error | `WIFI_POLL_API_ERROR` | "API error `<code>`" or "API unreachable" |
| Waiting for first data | — | "Connecting…" (shown until first successful poll) |

Last successfully received `UsageData` is retained across failed polls so the display
shows stale-but-useful data rather than blanking.

After **three consecutive connection timeouts** `wifi_poller` reports fail count ≥ 3;
`main.cpp` calls `wifi_poller_stop()` + `captive_portal_start()` automatically.

---

## Wi-Fi screen

`SCREEN_WIFI` is the third non-splash screen (Usage → Bluetooth → Wi-Fi → Usage via
PWR button). It shows:

- **SSID** — stored network name in full
- **Pass** — first character, `***`, last character (e.g. `C***5`)
- **Token** — first 14 characters + `...` (e.g. `sk-ant-oat01-m...`)
- **Start Hotspot** button — triggers `captive_portal_start()` immediately
- Note line — "Configure via serial at 115200 baud" normally; "Join Wi-Fi: ClawdMeter
  / then open 192.168.4.1" when portal is active

When hotspot mode is active (portal started for any reason), navigation is locked to
this screen until the device reboots with valid credentials.

---

## PlatformIO environment

`waveshare_amoled_216` in `firmware/platformio.ini`. `WiFi.h`, `WiFiClientSecure.h`,
`WebServer.h`, and `DNSServer.h` are all part of the ESP32 Arduino core — no extra
`lib_deps` entries needed beyond what upstream already uses.
