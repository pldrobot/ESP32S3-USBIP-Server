#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include "wifi_manager.h"
#include "usb_device.h"
#include "usbip_server.h"

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
    String _generatePage();
};
