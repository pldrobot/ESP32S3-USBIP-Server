#pragma once

// =============================================================
// ESP32-S3 USB/IP Bridge — Configuration
// Hardware: ESP32-S3-N16R8 (16MB Flash / 8MB PSRAM)
//
// Device-specific settings live in .env (see .env.template).
// Run 'pio run' to auto-generate src/config_env.h from .env.
// =============================================================

#include "config_env.h"

// ---- Web server (fixed) ----
#define WEB_SERVER_PORT          80

// ---- USB Host task (fixed) ----
#define USB_HOST_TASK_PRIORITY   5
#define USB_HOST_TASK_STACK_SIZE 4096

// ---- Watchdog (fixed) ----
#define WDT_TIMEOUT_SEC          30

// ---- NVS storage keys (fixed — changing breaks saved credentials) ----
#define NVS_KEY_SSID        "wifi_ssid"
#define NVS_KEY_PASS        "wifi_pass"
#define NVS_KEY_HOSTNAME    "hostname"
