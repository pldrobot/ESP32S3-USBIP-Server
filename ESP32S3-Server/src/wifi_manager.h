#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "config.h"

enum WifiState { WSTATE_NONE, WSTATE_AP_SETUP, WSTATE_STATION };

class WiFiManager {
public:
    WiFiManager();
    void begin();
    void task();

    bool setCredentials(const String& ssid, const String& password);

    WifiState getMode()      const { return _mode; }
    bool      isConnected()  const { return WiFi.status() == WL_CONNECTED; }
    String    getIP()        const;
    String    getSSID()      const { return _ssid; }
    String    getHostname()  const { return _hostname; }
    int       getRSSI()      const { return WiFi.RSSI(); }

    String    scanNetworks();
    void      reconnect();

private:
    WifiState   _mode;
    String      _ssid, _password, _hostname;
    Preferences _prefs;
    unsigned long _lastReconnect;
    static const unsigned long RECONNECT_INTERVAL = 10000;

    bool _loadCredentials();
    void _saveCredentials();
    bool _connectStation();
    void _startAP();
};
