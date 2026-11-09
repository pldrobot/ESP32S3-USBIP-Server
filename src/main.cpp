// =============================================================
//  Generic USB/IP Server (device name set in .env)
//  Hardware: ESP32-S3-N16R8 or ESP32-S3 Super Mini
//
//  Exposes a USB device (e.g. a printer) over WiFi via USB/IP so any
//  PC on the network can attach it as if it were plugged in directly.
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

void setupOTA() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]()  { debugI("OTA starting..."); });
    ArduinoOTA.onProgress([](unsigned p, unsigned t) { debugI("OTA %u%%", p/(t/100)); });
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
    Serial.printf("  Heap: %dKB   PSRAM: %dKB\n",
                  ESP.getFreeHeap()/1024, ESP.getFreePsram()/1024);
    // Why did we (re)boot? 3=SW 4=PANIC 5=INT_WDT 6=TASK_WDT 7=WDT 9=BROWNOUT
    Serial.printf("  Reset reason: %d\n\n", (int)esp_reset_reason());

#ifndef LED_RGB
    pinMode(LED_PIN, OUTPUT);
#endif
    setLed(false);

    // 1 — USB Host (spawns task on opposite core automatically)
    usbDevice.begin();

    // 2 — WiFi
    wifiMgr.begin();


    if (wifiMgr.isConnected()) setupOTA();

    // 4 — USB/IP bridge (spawns its own task on this core, port 3240)
    usbip.begin(&usbDevice);

    usbip.onClientConnect([](const char* ip) {
        debugI("Device claimed by %s", ip);
    });
    usbip.onClientDisconnect([](const char* ip, uint32_t sec) {
        debugI("Device released by %s after %us", ip, sec);
        PersistLog::flushNow();   // keep the log of the job that just finished
    });

    // 5 — Web dashboard (port 80)
    webUI.begin();

    // 6 — Watchdog
    esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
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

    static bool otaUp = false;
    if (wifiMgr.isConnected()) {
        if (!otaUp) { setupOTA(); otaUp = true; }
        ArduinoOTA.handle();
    } else {
        otaUp = false;
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
