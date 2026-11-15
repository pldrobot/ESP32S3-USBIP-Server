// =============================================================
//  Generic USB/IP Server (device name set in .env)
//  Hardware: ESP32-S3-N16R8 or ESP32-S3 Super Mini
//
//  Exposes one Full/Low-Speed USB device (any class)
//  over WiFi via USB/IP so any PC on the network can attach it as if it were
//  plugged in directly.
//
//  Client setup:
//    usbip attach -r <ESP32_IP> -b 1-1   (or use the USBIP-Client tray app)
//
//  Ports:
//    3240 — USB/IP
//    80   — Web UI  (WiFi setup + status dashboard)
//  Logs: crash-surviving, viewable on the web dashboard (Logs card)
// =============================================================

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include <esp_system.h>

#include <EspUsbIpServer.h>

#include "config.h"
#include "applog.h"
#include "persist_log.h"
#include "wifi_manager.h"
#include "web_ui.h"

// ── Status LED ───────────────────────────────────────────────────────────────
// Plain GPIO LED, or WS2812 (Super Mini, GPIO48) when built with -DLED_RGB.
static bool ledState = false;
static void setLed(bool on) {
    ledState = on;
#ifdef LED_RGB
    neopixelWrite(LED_PIN, 0, on ? 32 : 0, 0);   // dim green
#else
    digitalWrite(LED_PIN, on ? HIGH : LOW);
#endif
}

// ── Global objects ────────────────────────────────────────────────────────────
UsbDevice   usbDevice;
WiFiManager wifiMgr;
UsbIpServer usbip;
WebUI       webUI(wifiMgr, usbDevice, usbip);

// OTA is started once WiFi is up (at boot or after a later reconnect), never twice.
static bool otaUp = false;
// Set by the USB/IP disconnect callback (runs on the USB/IP task); the slow flash
// write happens in loop() instead.
static volatile bool flushLogRequested = false;

void setupOTA() {
    if (otaUp) return;
    otaUp = true;
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]()  { debugI("OTA starting..."); });
    ArduinoOTA.onProgress([](unsigned p, unsigned t) {
        static unsigned lastStep = 101;
        unsigned pct = t >= 100 ? p / (t / 100) : 0;
        if (pct / 10 != lastStep) { lastStep = pct / 10; debugI("OTA %u%%", pct); }
    });
    ArduinoOTA.onEnd([]()    { debugI("OTA done — rebooting"); });
    ArduinoOTA.begin();
    debugI("OTA ready on %s.local (port 3232)", OTA_HOSTNAME);
}

// ── setup() ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);

    PersistLog::begin();   // first: captures reset reason, saves previous boot's log

    Serial.println();
    Serial.println("===========================================");
    Serial.println("  " DEVICE_NAME " — USB/IP Server");
    Serial.println("  Firmware: " __DATE__ "  " __TIME__);
    Serial.println("===========================================");
    Serial.printf("  Heap: %uKB   PSRAM: %uKB\n",
                  (unsigned)(ESP.getFreeHeap()/1024), (unsigned)(ESP.getFreePsram()/1024));
    // Why did we (re)boot? 3=SW 4=PANIC 5=INT_WDT 6=TASK_WDT 7=WDT 9=BROWNOUT
    Serial.printf("  Reset reason: %d\n\n", (int)esp_reset_reason());

#ifndef LED_RGB
    pinMode(LED_PIN, OUTPUT);
#endif
    setLed(false);

    // 1 — USB Host (its task starts here)
    // Show the IDF USB stack's own warnings/errors on Serial (enumeration failures etc.);
    // the Arduino core would otherwise hide them.
#ifdef USBIP_VERBOSE
    esp_log_level_set("*", ESP_LOG_VERBOSE);   // build with -DUSBIP_VERBOSE to see everything
#else
    esp_log_level_set("*", ESP_LOG_WARN);
#endif
    usbDevice.begin();

    // 2 — WiFi
    wifiMgr.begin();
    WiFi.setSleep(false);          // modem sleep adds milliseconds of latency to every URB

    // 3 — OTA
    if (wifiMgr.isConnected()) setupOTA();

    // 4 — USB/IP server (its tasks start here, port 3240)
    usbip.begin(&usbDevice);

    usbip.onClientConnect([](const char* ip) {
        debugI("Device claimed by %s", ip);
    });
    usbip.onClientDisconnect([](const char* ip, uint32_t sec) {
        debugI("Device released by %s after %us", ip, (unsigned)sec);
        flushLogRequested = true;  // keep the log of the job that just finished
    });

    // 5 — Web dashboard (port 80)
    webUI.begin();

    // 6 — Watchdog
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    // Arduino core 3.x (ESP-IDF 5): config-struct API
    // The core has already started the task watchdog (init fails with INVALID_STATE), so the
    // config must be applied with reconfigure(). idle_core_mask = 0: do not watch the idle
    // tasks. A server moving a fast URB stream legitimately keeps a core busy for seconds;
    // that is load, not a hang. loopTask below is still watched.
    esp_task_wdt_config_t wdtCfg = { .timeout_ms = WDT_TIMEOUT_SEC * 1000,
                                      .idle_core_mask = 0, .trigger_panic = true };
    if (esp_task_wdt_init(&wdtCfg) == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&wdtCfg);
#else
    // Arduino core 2.x (ESP-IDF 4): (timeout_sec, panic) API
    esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
#endif
    esp_task_wdt_add(NULL);

    if (wifiMgr.isConnected()) {
        debugI("IP:        %s",        wifiMgr.getIP().c_str());
        debugI("Dashboard: http://%s", wifiMgr.getIP().c_str());
        debugI("Attach:    usbip attach -r %s -b 1-1", wifiMgr.getIP().c_str());
    } else {
        debugW("No WiFi — connect to AP '%s', open http://192.168.4.1", AP_SSID);
    }

    setLed(wifiMgr.isConnected());
}

// ── loop() ───────────────────────────────────────────────────────────────────
void loop() {
    esp_task_wdt_reset();

    wifiMgr.task();
    webUI.task();
    PersistLog::task();

    if (flushLogRequested) {
        flushLogRequested = false;
        PersistLog::flushNow();
    }

    if (wifiMgr.isConnected()) {
        setupOTA();                // no-op once started
        ArduinoOTA.handle();
    } else {
        otaUp = false;             // restart OTA after the next reconnect
    }

    // LED: slow blink = device in use, solid = idle+ready, off = no WiFi
    static unsigned long lastBlink = 0;
    if (usbip.isClientAttached()) {
        if (millis() - lastBlink > 500) { lastBlink = millis(); setLed(!ledState); }
    } else {
        setLed(wifiMgr.isConnected());
    }

    delay(1);
}
