#pragma once
#include "data.h"

void wifi_poller_init(void);
void wifi_poller_tick(void);
bool wifi_poller_is_connected(void);
bool wifi_poller_has_new_data(void);
void wifi_poller_consume_data(UsageData* out);
