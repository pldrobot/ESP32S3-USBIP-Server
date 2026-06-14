#pragma once
#include <Arduino.h>
#include <WiFi.h>

class UsbDevice;

#define USBIP_PORT        3240
#define USBIP_VERSION     0x0111
#define OP_REQ_DEVLIST    0x8005
#define OP_REP_DEVLIST    0x0005
#define OP_REQ_IMPORT     0x8003
#define OP_REP_IMPORT     0x0003
#define USBIP_CMD_SUBMIT  0x00000001
#define USBIP_CMD_UNLINK  0x00000002
#define USBIP_RET_SUBMIT  0x00000003
#define USBIP_RET_UNLINK  0x00000004
#define USBIP_DIR_OUT     0
#define USBIP_DIR_IN      1
#define USBIP_SPEED_HIGH  3
#define USBIP_BUSNUM      1
#define USBIP_DEVNUM      2
#define USBIP_BUSID       "1-1"

struct UsbIpSessionRecord {
    bool     valid       = false;
    char     ip[16]      = {};
    uint32_t endedAt     = 0;   // uptime seconds
    uint32_t durationSec = 0;
};

class UsbIpServer {
public:
    void   begin(UsbDevice* device);
    void   task();
    bool   isClientAttached() const { return _attached; }
    void   kickClient();
    String statusJson() const;

private:
    WiFiServer  _server;
    WiFiClient  _client;
    UsbDevice*  _device     = nullptr;
    bool        _attached   = false;

    char          _curIP[16]   = {};
    unsigned long _connectedMs = 0;
    unsigned long _attachedMs  = 0;

    static const int MAX_HISTORY = 6;
    UsbIpSessionRecord _history[MAX_HISTORY];
    int                _histNext = 0;

    void _onConnect(const String& ip);
    void _onAttach();
    void _onDisconnect();

    bool _handleDevList();
    bool _handleImport();
    void _handleUrbs();
    bool _processSubmit(uint32_t seqnum, uint32_t direction, uint32_t ep,
                        const uint8_t setup[8], int32_t bufLen);

    bool     _readExact(uint8_t* buf, size_t len, uint32_t timeoutMs = 5000);
    bool     _writeAll(const uint8_t* buf, size_t len);
    void     _fillDevInfo(uint8_t out[312]);

    static void     _w32(uint8_t* p, uint32_t v);
    static void     _w16(uint8_t* p, uint16_t v);
    static uint32_t _r32(const uint8_t* p);
    static uint16_t _r16(const uint8_t* p);
};
