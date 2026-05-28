#pragma once
#include <Arduino.h>

void provisioning_init(void);
void provisioning_handle_cmd(const char* cmd);

bool     provisioning_has_wifi(void);
bool     provisioning_has_token(void);
String   provisioning_get_ssid(void);
String   provisioning_get_pass(void);
String   provisioning_get_token(void);
