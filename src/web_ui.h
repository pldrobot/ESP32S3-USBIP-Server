#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <EspUsbIpServer.h>
#include "wifi_manager.h"

class WebUI {
public:
    WebUI(WiFiManager& wifi, UsbDevice& device, UsbIpServer& usbip);
    void begin();
    void task();

private:
    WebServer    _server;
    WiFiManager& _wifi;
    UsbDevice&   _device;
    UsbIpServer& _usbip;

    void _handleRoot();
    void _handleStatus();
    void _handleUsbIpKick();
    void _handleWifiScan();
    void _handleWifiConnect();
    void _handleRestart();
    void _handleNotFound();
    void _handleLogin();
    void _handleLogout();
    String _generatePage();
    String _loginPage(bool failed);

    // Session auth
    static const int MAX_SESSIONS = 4;
    String _tokens[MAX_SESSIONS];
    int    _tokenNext = 0;
    bool   _authed();
    bool   _requireAuth();   // sends 401 and returns false when not logged in
    String _newToken();
    void   _dropToken(const String& t);
    String _cookieToken();
};
