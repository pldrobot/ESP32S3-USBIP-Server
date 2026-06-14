// =============================================================
//  ESP32-S3 USB/IP Bridge
//  Hardware: ESP32-S3-N16R8 (16MB Flash / 8MB PSRAM)
//
//  Design: Pure USB/IP bridge.
//    Windows installs the real printer driver on the virtual USB
//    device provided by USB/IP.  All printing, maintenance, and
//    vendor utilities work exactly as if the printer were plugged
//    directly into the PC via USB.
//
//  Windows setup (one-time):
//    winget install usbipd
//    usbip attach -r <ESP32_IP> -b 1-1
//    → Windows detects the printer, installs driver, done.
//
//  Ports:
//    3240 — USB/IP  (primary — printing + maintenance)
//    80   — Web UI  (WiFi setup + status dashboard)
//    23   — Telnet  (RemoteDebug — live debug output)
// =============================================================

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "rdebug.h"
#include "usb_device.h"
#include "wifi_manager.h"
#include "usbip_server.h"
#include "web_ui.h"

// ── Global objects ────────────────────────────────────────────────────────────
RemoteDebug Debug;

UsbDevice   usbDevice;
WiFiManager wifiMgr;
UsbIpServer usbip;
WebUI       webUI(wifiMgr, usbDevice, usbip);

TaskHandle_t usbTaskHandle = NULL;

// ── USB Host background task (Core 1) ─────────────────────────────────────────
void usbHostTask(void*) {
    debugI("USB Host task running on Core 1");
    while (true) {
        usbDevice.task();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setupOTA() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]()  { debugI("OTA starting..."); });
    ArduinoOTA.onProgress([](unsigned p, unsigned t) { debugI("OTA %u%%", p/(t/100)); });
    ArduinoOTA.onEnd([]()    { debugI("OTA done — rebooting"); });
    ArduinoOTA.begin();
    debugI("OTA ready on %s.local (port 3232)", OTA_HOSTNAME);
}

// ── setup() ───────────────────────────────────────────────────────────────────
void setup() {
    // Keep Serial for early boot messages via hardware UART (GPIO43/44).
    // After WiFi is up, RemoteDebug Telnet takes over. With setSerialEnabled(true)
    // all debug macros also echo to Serial, so nothing is lost.
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("===========================================");
    Serial.println("  USB/IP Bridge  v2.0  [" DEVICE_NAME "]");
    Serial.println("  Full USB over WiFi -- print + maintain");
    Serial.println("===========================================");
    Serial.printf("  Heap: %dKB   PSRAM: %dKB\n\n",
                  ESP.getFreeHeap()/1024, ESP.getFreePsram()/1024);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // 1 — USB Host (runs on Core 1)
    if (usbDevice.begin()) {
        xTaskCreatePinnedToCore(usbHostTask, "usb_host",
                                USB_HOST_TASK_STACK_SIZE, NULL,
                                USB_HOST_TASK_PRIORITY, &usbTaskHandle, 1);
    }

    // 2 — WiFi
    wifiMgr.begin();

    // 3 — RemoteDebug (Telnet port 23, only after WiFi is up)
    Debug.begin(MDNS_HOSTNAME);
    Debug.setResetCmdEnabled(true);
    Debug.setSerialEnabled(true);   // mirror all debugX() calls to Serial too
    debugI("RemoteDebug started — connect: telnet %s.local", MDNS_HOSTNAME);

    if (wifiMgr.isConnected()) setupOTA();

    // 4 — USB/IP bridge (port 3240)
    usbip.begin(&usbDevice);

    // 5 — Web dashboard (port 80)
    webUI.begin();

    // 6 — Watchdog (ESP-IDF 4.x API: seconds, panic-on-timeout)
    esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
    esp_task_wdt_add(NULL);

    if (wifiMgr.isConnected()) {
        debugI("IP:        %s",        wifiMgr.getIP().c_str());
        debugI("Dashboard: http://%s", wifiMgr.getIP().c_str());
        debugI("USB/IP:    usbip attach -r %s -b 1-1", wifiMgr.getIP().c_str());
        debugI("Debug:     telnet %s:23", wifiMgr.getIP().c_str());
    } else {
        debugW("No WiFi — connect to AP '%s', then open http://%s",
               AP_SSID, wifiMgr.getIP().c_str());
    }

    digitalWrite(LED_PIN, wifiMgr.isConnected() ? HIGH : LOW);
}

// ── loop() ────────────────────────────────────────────────────────────────────
void loop() {
    esp_task_wdt_reset();

    wifiMgr.task();
    usbip.task();
    webUI.task();
    Debug.handle();

    // Re-init OTA if WiFi reconnected after a drop
    static bool otaUp = false;
    if (wifiMgr.isConnected()) {
        if (!otaUp) { setupOTA(); otaUp = true; }
        ArduinoOTA.handle();
    } else {
        otaUp = false;
    }

    // LED: slow blink = USB/IP attached, solid = idle+ready, off = no WiFi
    static unsigned long lastBlink = 0;
    if (usbip.isClientAttached()) {
        if (millis() - lastBlink > 500) { lastBlink = millis(); digitalWrite(LED_PIN, !digitalRead(LED_PIN)); }
    } else {
        digitalWrite(LED_PIN, wifiMgr.isConnected() ? HIGH : LOW);
    }

    delay(1);
}
