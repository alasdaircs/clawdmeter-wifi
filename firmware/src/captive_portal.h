#pragma once

void captive_portal_init(void);   // auto-starts if no credentials stored
void captive_portal_start(void);  // start immediately (call wifi_poller_stop first)
void captive_portal_stop(void);   // tear down AP + portal (call wifi_poller_restart after)
void captive_portal_tick(void);
bool captive_portal_is_active(void);
