# Clawdmeter — Wi-Fi native fork

A fork of [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter)
targeting the **Waveshare ESP32-S3-Touch-AMOLED-2.16** board.

This fork replaces the host daemon + BLE data channel with Wi-Fi polling directly
from the device. The ESP32 calls the Anthropic API itself, removing all host
dependencies. No daemon. No host PC required for the display to function.

**Read `CLAUDE.md` first** for hardware pin assignments, build/flash commands,
display gotchas, and the general firmware architecture. This file covers only
what changes.

---

## What changes vs upstream

| Concern | Upstream | This fork |
|---|---|---|
| Usage data source | Host daemon → BLE GATT RX | ESP32 → Wi-Fi → Anthropic API |
| Token storage | Host filesystem / Keychain | ESP32 NVS (`Preferences`) |
| BLE data service | NimBLE custom GATT service | **Removed** |
| BLE HID keyboard | NimBLE HID (buttons) | **Kept unchanged** |
| Wi-Fi | Not used | Added |
| Provisioning | Host-side install script | Serial command interface |

The splash animations, UI screens, display driver, touch, IMU, power management,
and HID keyboard are all **untouched**. The delta is confined to:

- `ble.{h,cpp}` — strip the custom data GATT service; keep HID
- `wifi_poller.{h,cpp}` — new: Wi-Fi connection and API polling loop
- `provisioning.{h,cpp}` — new: serial command handler for first-time setup
- `data.h` — `UsageData` struct stays; remove any BLE-receive callbacks

---

## Architecture

```
On-device NVS
(Wi-Fi SSID/password, OAuth token)
        │
        ▼
  wifi_poller
  ├── WiFi.begin() → connected
  ├── WiFiClientSecure → api.anthropic.com:443
  ├── POST /v1/messages (1 token, claude-haiku-4-5-20251001)
  ├── Read response headers:
  │     anthropic-ratelimit-unified-5h-utilization
  │     anthropic-ratelimit-unified-7d-utilization
  │     anthropic-ratelimit-unified-status
  │     anthropic-ratelimit-unified-representative-claim
  │     anthropic-ratelimit-unified-5h-reset
  └── Populate UsageData → existing UI + splash logic unchanged
```

The existing `UsageData` struct and all downstream consumers (UI, splash mood
selection) are unchanged. `wifi_poller` is a drop-in replacement for the BLE
receive path — it populates the same struct.

---

## API call

**Endpoint:** `POST https://api.anthropic.com/v1/messages`

**Headers required:**
```
Authorization: Bearer <token>
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

**Data comes from response headers, not the body.** Parse headers as they arrive
rather than buffering the full response. Key headers:

| Header | Use |
|---|---|
| `anthropic-ratelimit-unified-5h-utilization` | Primary display value (0.0–1.0) |
| `anthropic-ratelimit-unified-7d-utilization` | Secondary display value |
| `anthropic-ratelimit-unified-status` | `allowed` / `throttled` / `exceeded` |
| `anthropic-ratelimit-unified-representative-claim` | `five_hour` or `seven_day` — which window is primary |
| `anthropic-ratelimit-unified-5h-reset` | Unix timestamp of next reset |

**Validated:** `Authorization: Bearer` works. `X-Api-Key` returns HTTP 401.
Do not use `X-Api-Key`.

**Poll interval:** 60 seconds, matching upstream. Use `millis()` comparison in
the main loop — do not use `delay()` or `vTaskDelay()` in a way that blocks
the LVGL tick or touch handling.

---

## TLS

`WiFiClientSecure` requires a root CA cert to verify `api.anthropic.com`.
Anthropic's API is served behind Cloudflare; the root is **ISRG Root X1**
(Let's Encrypt) or Cloudflare's own root depending on the edge node — verify
empirically with a test build before hardcoding.

Options (in preference order):
1. `client.setInsecure()` — acceptable for a personal desk device with no
   sensitive data flowing the other direction. Simple. Use this first.
2. `client.setCACert(root_ca)` — proper cert pinning. Add the PEM as a `const
   char*` in `wifi_poller.cpp`. Requires updating when the cert rotates.

Start with `setInsecure()` to prove the flow, then optionally harden.

---

## Token storage (NVS)

Use the Arduino `Preferences` library (wraps ESP-IDF NVS):

```cpp
#include <Preferences.h>
Preferences prefs;
prefs.begin("clawdmeter", false);          // namespace, read-write
prefs.putString("token", "sk-ant-oat01-...");
String token = prefs.getString("token", "");
prefs.end();
```

Store under namespace `"clawdmeter"`, keys:
- `"token"` — OAuth token (`sk-ant-oat01-...`)
- `"ssid"` — Wi-Fi SSID
- `"pass"` — Wi-Fi password

NVS survives firmware updates but is wiped by `pio run -t erase`. Flash size
on the ESP32-S3-Touch-AMOLED-2.16 is 16 MB; NVS partition is ample.

**Security note:** NVS is not encrypted by default on this board. The token is
extractable with physical access via `esptool.py read_flash`. This is an
acceptable risk for a personal desk device — treat the device accordingly.
If the device is lost, revoke by changing your Anthropic account password
(there is no documented per-token revocation endpoint).

---

## Provisioning

The device needs Wi-Fi credentials and a token before it can poll. Use the
existing serial port (`/dev/ttyACM0` at 115200 baud — same as the `screenshot`
command) for a minimal command interface.

Commands (newline-terminated):

```
token sk-ant-oat01-...        # store OAuth token to NVS
ssid MyNetwork                # store Wi-Fi SSID to NVS
pass MyPassword               # store Wi-Fi password to NVS
status                        # print current NVS values (mask token to first 20 chars)
clear                         # wipe all NVS keys
```

Implement in `provisioning.{h,cpp}`, called from `loop()` when
`Serial.available()`. Keep it simple — no framing, no CRC, plain text.

On first boot (no token in NVS), show a "needs setup" state on the display.
The splash screen can remain; add a text overlay indicating setup is required.

**On Windows**, the serial port appears as a COM port. Use PuTTY, the Arduino
Serial Monitor, or PowerShell:
```powershell
$port = New-Object System.IO.Ports.SerialPort "COM3", 115200
$port.Open()
$port.WriteLine("token sk-ant-oat01-...")
$port.Close()
```

---

## PlatformIO environment

The existing `waveshare_amoled_216` environment in `firmware/platformio.ini`
is the target. `WiFi.h` and `WiFiClientSecure.h` are part of the ESP32 Arduino
core — no additional `lib_deps` entries needed.

---

## Error handling and display states

| Condition | Display behaviour |
|---|---|
| No token in NVS | Overlay: "Run: token \<your-token\> via serial" |
| Wi-Fi connect failed | Overlay: "Wi-Fi error" + SSID; retry every 30 s |
| HTTP non-200 | Overlay: "API error \<status\>"; retain last good data |
| No data yet on boot | Spinner or "Connecting…" on usage screen |
| HTTP 401 | Overlay: "Token invalid — re-provision"; stop polling |

Retain the last successfully received `UsageData` across failed polls so the
display shows stale-but-useful data rather than blanking.

---

## What to build first

1. **`provisioning.cpp`** — serial command handler, NVS read/write, `status`
   command. Verify credentials survive a reboot before touching networking.
2. **`wifi_poller.cpp` (connect only)** — `WiFi.begin()`, wait for IP, print
   to serial. Verify Wi-Fi connects reliably before adding HTTPS.
3. **`wifi_poller.cpp` (HTTPS + headers)** — make the API call with
   `setInsecure()`, print raw response headers to serial. Verify you can see
   `anthropic-ratelimit-unified-5h-utilization`.
4. **Populate `UsageData`** from headers and pass to existing UI.
5. **`ble.cpp` cleanup** — remove the custom data GATT service, keep HID.
6. **Display error states** for missing token / Wi-Fi failure.
7. **Polish:** cert pinning (optional), reconnect logic, display overlays.
