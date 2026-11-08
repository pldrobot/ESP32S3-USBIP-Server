#pragma once

// =============================================================
// USB/IP Server Configuration
// Hardware: ESP32-S3-N16R8 or ESP32-S3 Super Mini
//
// Device-specific settings live in .env (see .env.template).
// Run 'pio run' to auto-generate src/config_env.h from .env.
// =============================================================

#include "config_env.h"

// ---- Web server (fixed) ----
#define WEB_SERVER_PORT          80

// ---- Web UI login (override in .env) ----
#ifndef WEB_USER
#define WEB_USER                 "admin"
#endif
#ifndef WEB_PASSWORD
#define WEB_PASSWORD             "admin"
#endif

// ---- Watchdog (fixed) ----
#define WDT_TIMEOUT_SEC          30

// ---- NVS storage keys (fixed — changing breaks saved credentials) ----
#define NVS_KEY_SSID        "wifi_ssid"
#define NVS_KEY_PASS        "wifi_pass"
#define NVS_KEY_HOSTNAME    "hostname"
