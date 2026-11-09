#pragma once

// Application logging: debugI / debugW / debugE print to Serial and go into the
// crash-surviving persistent log (viewable from the web dashboard).
// (Replaces the old RemoteDebug telnet logger.)

#include <Arduino.h>
#include "persist_log.h"

#define _APPLOG(level, fmt, ...) do {                        \
        Serial.printf("[%c] " fmt "\n", level, ##__VA_ARGS__); \
        PersistLog::addf(level, fmt, ##__VA_ARGS__);           \
    } while (0)

#define debugV(fmt, ...) ((void)0)
#define debugD(fmt, ...) ((void)0)
#define debugI(fmt, ...) _APPLOG('I', fmt, ##__VA_ARGS__)
#define debugW(fmt, ...) _APPLOG('W', fmt, ##__VA_ARGS__)
#define debugE(fmt, ...) _APPLOG('E', fmt, ##__VA_ARGS__)
