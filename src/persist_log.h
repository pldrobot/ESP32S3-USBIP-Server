#pragma once
#include <Arduino.h>

// Crash-surviving log.
//  - Every line goes into a ring buffer in RTC memory (survives panic / watchdog /
//    software resets, so it is intact after a crash).
//  - On boot, the previous boot's ring is saved to flash (LittleFS), keeping the
//    last PLOG_KEEP boots.
//  - Warnings/errors also trigger a flush of the ring to flash (at most once per
//    PLOG_FLUSH_MIN_MS) so a power loss / brownout still leaves recent history.
namespace PersistLog {
    void begin();                                    // call first thing in setup()
    void task();                                     // call from loop(): deferred flash flush
    void add(char level, const char* line);          // thread-safe
    void addf(char level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void flushNow();                                 // write current ring to flash
    String dump();                                   // current boot + saved previous boots
    const char* resetReasonName(int r);
}
