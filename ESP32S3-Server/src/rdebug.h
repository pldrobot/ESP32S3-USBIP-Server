#pragma once

// Shared RemoteDebug instance — telnet debug on port 23.
// Connect: telnet canon-printserver.local
// Or use PlatformIO monitor with:  monitor_port = socket://canon-printserver.local:23

#include <RemoteDebug.h>

extern RemoteDebug Debug;
