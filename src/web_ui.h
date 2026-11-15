#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <EspUsbIpServer.h>
#include "wifi_manager.h"

// Dashboard on ESPAsyncWebServer. Request handlers run on the AsyncTCP task and must never
// block: anything slow (joining WiFi, restarting) is only requested there and carried out by
// task(), which runs from loop(). Live status is pushed over a WebSocket at /ws on the same
// port; the upgrade request must carry the login cookie.
class WebUI {
public:
    WebUI(WiFiManager& wifi, UsbDevice& device, UsbIpServer& usbip);
    void begin();
    void task();

private:
    AsyncWebServer _server;
    AsyncWebSocket _ws;
    WiFiManager&   _wifi;
    UsbDevice&     _device;
    UsbIpServer&   _usbip;

    // Handlers (AsyncTCP task)
    void _handleRoot(AsyncWebServerRequest* r);
    void _handleLogin(AsyncWebServerRequest* r);
    void _handleLogout(AsyncWebServerRequest* r);
    void _handleUsbIpKick(AsyncWebServerRequest* r);
    void _handleWifiScan(AsyncWebServerRequest* r);
    void _handleWifiConnect(AsyncWebServerRequest* r);
    void _handleWifiResult(AsyncWebServerRequest* r);
    void _handleRestart(AsyncWebServerRequest* r);

    String _statusJson();
    String _generatePage();
    String _loginPage(bool failed);

    // Session auth
    static const int MAX_SESSIONS = 4;
    String _tokens[MAX_SESSIONS];
    int    _tokenNext = 0;
    unsigned long _lastLoginFailMs = 0;   // throttles brute force without blocking
    bool   _authedReq(AsyncWebServerRequest* r);
    bool   _requireAuth(AsyncWebServerRequest* r);   // sends 401 and returns false when not logged in
    String _newToken();
    void   _dropToken(const String& t);
    String _cookieToken(AsyncWebServerRequest* r);
    static String _arg(AsyncWebServerRequest* r, const char* name);

    // Work started by a handler and done in task()
    unsigned long _lastPush = 0;
    enum ConnState : uint8_t { CONN_IDLE, CONN_PENDING, CONN_RUNNING, CONN_OK, CONN_FAILED };
    volatile uint8_t _connState = CONN_IDLE;
    String _connSsid, _connPass, _connIp;
    volatile unsigned long _restartAt = 0;
};
