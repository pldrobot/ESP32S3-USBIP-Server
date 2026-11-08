#pragma once

// Shared RemoteDebug instance — telnet debug on port 23.
// Connect: telnet <MDNS_HOSTNAME>.local
// Or use PlatformIO monitor with:  monitor_port = socket://<MDNS_HOSTNAME>.local:23

#include <RemoteDebug.h>

extern RemoteDebug Debug;
