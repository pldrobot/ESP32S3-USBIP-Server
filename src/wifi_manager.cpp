#include "wifi_manager.h"
#include "rdebug.h"

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
    _ssid = ssid; _password = pass;
    _saveCredentials();
    WiFi.softAPdisconnect(true);
    if (_connectStation()) {
        debugI("WiFi Connected, IP: %s", getIP().c_str());
        return true;
    }
    _startAP();
    return false;
}

String WiFiManager::getIP() const {
    if (_mode == WSTATE_STATION)  return WiFi.localIP().toString();
    if (_mode == WSTATE_AP_SETUP) return WiFi.softAPIP().toString();
    return "0.0.0.0";
}

String WiFiManager::scanNetworks() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i) json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",";
        json += "\"rssi\":"  + String(WiFi.RSSI(i)) + ",";
        json += "\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    WiFi.scanDelete();
    return json + "]";
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
