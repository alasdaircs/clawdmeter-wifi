#include "provisioning.h"
#include <Preferences.h>

#define NVS_NS "clawdmeter"

static String s_token;
static String s_ssid;
static String s_pass;

static void save_and_cache(const char* key, const char* value, String& cache) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putString(key, value);
    prefs.end();
    cache = value;
    Serial.printf("prov: %s saved\n", key);
}

void provisioning_init(void) {
    Preferences prefs;
    prefs.begin(NVS_NS, true);  // read-only
    s_token = prefs.getString("token", "");
    s_ssid  = prefs.getString("ssid",  "");
    s_pass  = prefs.getString("pass",  "");
    prefs.end();
    Serial.printf("prov: token=%s ssid=%s pass=%s\n",
        s_token.length() ? s_token.substring(0, 20).c_str() : "(none)",
        s_ssid.length()  ? s_ssid.c_str()                   : "(none)",
        s_pass.length()  ? "***"                             : "(none)");
}

void provisioning_handle_cmd(const char* cmd) {
    // Split on first space: verb [value]
    const char* sp = strchr(cmd, ' ');
    String verb = sp ? String(cmd).substring(0, sp - cmd) : String(cmd);
    const char* value = sp ? sp + 1 : "";

    if (verb == "token") {
        if (*value == '\0') { Serial.println("prov: usage: token <value>"); return; }
        save_and_cache("token", value, s_token);
    } else if (verb == "ssid") {
        if (*value == '\0') { Serial.println("prov: usage: ssid <value>"); return; }
        save_and_cache("ssid", value, s_ssid);
    } else if (verb == "pass") {
        if (*value == '\0') { Serial.println("prov: usage: pass <value>"); return; }
        save_and_cache("pass", value, s_pass);
    } else if (verb == "status") {
        Serial.printf("token: %s\n", s_token.length() ? s_token.substring(0, 20).c_str() : "(none)");
        Serial.printf("ssid:  %s\n", s_ssid.length()  ? s_ssid.c_str()                   : "(none)");
        Serial.printf("pass:  %s\n", s_pass.length()  ? "***"                             : "(none)");
    } else if (verb == "clear") {
        Preferences prefs;
        prefs.begin(NVS_NS, false);
        prefs.clear();
        prefs.end();
        s_token = "";
        s_ssid  = "";
        s_pass  = "";
        Serial.println("prov: cleared");
    } else {
        Serial.printf("prov: unknown command: %s\n", cmd);
    }
}

bool provisioning_has_wifi(void) {
    return s_ssid.length() > 0 && s_pass.length() > 0;
}

bool provisioning_has_token(void) {
    return s_token.length() > 0;
}

String provisioning_get_ssid(void)  { return s_ssid; }
String provisioning_get_pass(void)  { return s_pass; }
String provisioning_get_token(void) { return s_token; }
