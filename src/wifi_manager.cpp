#include "wifi_manager.h"
#include "applog.h"
#include <esp_task_wdt.h>

WiFiManager::WiFiManager() : _mode(WSTATE_NONE), _lastReconnect(0) {}

void WiFiManager::begin() {
    _hostname = MDNS_HOSTNAME;
    if (_loadCredentials() && _ssid.length() > 0) {
        debugI("WiFi Connecting to '%s'...", _ssid.c_str());
        if (_connectStation()) {
            debugI("WiFi Connected, IP: %s", getIP().c_str());
            return;
        }
        debugW("WiFi Connection failed, starting AP mode");
    } else {
        debugW("WiFi No credentials, starting AP mode");
    }
    _startAP();
}

void WiFiManager::task() {
    if (_mode == WSTATE_STATION && !isConnected()) {
        unsigned long now = millis();
        if (now - _lastReconnect > RECONNECT_INTERVAL) {
            _lastReconnect = now;
            debugW("WiFi Reconnecting...");
            WiFi.reconnect();
        }
    }
}

bool WiFiManager::setCredentials(const String& ssid, const String& pass) {
    const String oldSsid = _ssid, oldPass = _password;
    _ssid = ssid; _password = pass;
    WiFi.softAPdisconnect(true);
    if (_connectStation()) {
        _saveCredentials();                // only remember credentials that actually worked
        debugI("WiFi Connected, IP: %s", getIP().c_str());
        return true;
    }
    _ssid = oldSsid; _password = oldPass;
    _startAP();
    return false;
}

String WiFiManager::getIP() const {
    if (_mode == WSTATE_STATION)  return WiFi.localIP().toString();
    if (_mode == WSTATE_AP_SETUP) return WiFi.softAPIP().toString();
    return "0.0.0.0";
}

bool WiFiManager::scanPoll(String& json) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return false;
    if (n < 0) { WiFi.scanNetworks(true); return false; }     // none started yet (or it failed): start one
    json = "[";
    for (int i = 0; i < n; i++) {
        if (i) json += ",";
        String ssid = WiFi.SSID(i);          // arbitrary bytes: escape for JSON
        ssid.replace("\\", "\\\\");
        ssid.replace("\"", "\\\"");
        for (unsigned k = 0; k < ssid.length(); k++)
            if ((uint8_t)ssid[k] < 0x20) ssid.setCharAt(k, ' ');
        json += "{\"ssid\":\"" + ssid + "\",";
        json += "\"rssi\":"  + String(WiFi.RSSI(i)) + ",";
        json += "\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    WiFi.scanDelete();
    json += "]";
    return true;
}

void WiFiManager::reconnect() {
    if (_mode == WSTATE_STATION) { WiFi.disconnect(); delay(500); _connectStation(); }
}

bool WiFiManager::_loadCredentials() {
    _prefs.begin(NVS_NAMESPACE, true);
    _ssid     = _prefs.getString(NVS_KEY_SSID, "");
    _password = _prefs.getString(NVS_KEY_PASS, "");
    _hostname = _prefs.getString(NVS_KEY_HOSTNAME, MDNS_HOSTNAME);
    _prefs.end();
    return _ssid.length() > 0;
}

void WiFiManager::_saveCredentials() {
    _prefs.begin(NVS_NAMESPACE, false);
    _prefs.putString(NVS_KEY_SSID,     _ssid);
    _prefs.putString(NVS_KEY_PASS,     _password);
    _prefs.putString(NVS_KEY_HOSTNAME, _hostname);
    _prefs.end();
}

bool WiFiManager::_connectStation() {
    WiFi.persistent(false);         // we manage NVS ourselves
    WiFi.setAutoReconnect(true);    // stack retries after transient drops
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(_hostname.c_str());
    WiFi.begin(_ssid.c_str(), _password.c_str());
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();   // up to 15 s here; only once subscribed
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) { _mode = WSTATE_STATION; return true; }
    WiFi.disconnect();
    return false;
}

void WiFiManager::_startAP() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    _mode = WSTATE_AP_SETUP;
    debugI("WiFi AP: %s  Setup: http://%s", AP_SSID, WiFi.softAPIP().toString().c_str());
}
