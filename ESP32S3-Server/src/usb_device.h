#pragma once
#include <Arduino.h>
#include "usb/usb_host.h"

#define USB_CLASS_PRINTER       0x07
#define USB_SUBCLASS_PRINTER    0x01
#define USB_PROTOCOL_UNIDIR     0x01
#define USB_PROTOCOL_BIDIR      0x02
#define USB_PROTOCOL_1284       0x03

#define USB_PRINTER_GET_DEVICE_ID   0x00
#define USB_PRINTER_GET_PORT_STATUS 0x01
#define USB_PRINTER_SOFT_RESET      0x02

// USB standard request codes (for USB/IP intercept)
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_SET_INTERFACE     0x0B

enum UsbDeviceState {
    DEVICE_DISCONNECTED,
    DEVICE_CONNECTED,
    DEVICE_READY,
    DEVICE_BUSY,
    DEVICE_MAINTENANCE,   // Locked for maintenance or USB/IP
    DEVICE_ERROR
};

// Max descriptor buffer sizes
#define DEV_DESC_BUF_SIZE   18
#define CFG_DESC_BUF_SIZE   512

class UsbDevice {
public:
    UsbDevice();
    bool begin();
    void task();

    // Normal print data — rejected while in maintenance/USB/IP mode
    int sendData(const uint8_t* data, size_t length);

    // Read response from device via Bulk IN endpoint
    int readData(uint8_t* buffer, size_t maxLength, uint32_t timeoutMs = 2000);

    // Send a maintenance command and optionally read back a response
    int sendMaintCommand(const uint8_t* cmd, size_t cmdLen,
                         uint8_t* response = nullptr,
                         size_t responseMaxLen = 0,
                         uint32_t responseTimeoutMs = 2000);

    // Generic transfers for USB/IP forwarding
    int submitControlTransfer(usb_setup_packet_t* setup,
                              uint8_t* data, size_t dataLen,
                              uint32_t timeoutMs = 3000);
    int submitBulkOut(const uint8_t* data, size_t len, uint32_t timeoutMs = 5000);
    int submitBulkIn(uint8_t* buf, size_t maxLen, uint32_t timeoutMs = 3000);

    bool softReset();

    // Exclusive lock used by maintenance and USB/IP server
    void setMaintenanceMode(bool active);
    bool isInMaintenanceMode() const { return _maintenanceMode; }

    // State queries
    UsbDeviceState getState()   const { return _state; }
    bool isConnected() const { return _state >= DEVICE_CONNECTED; }
    bool isReady()     const { return _state == DEVICE_READY; }
    bool isBusy()      const { return _state == DEVICE_BUSY; }

    // Identity
    String   getManufacturer() const { return _manufacturer; }
    String   getProduct()      const { return _product; }
    String   getDeviceId()     const { return _deviceId; }
    uint16_t getVendorId()     const { return _vendorId; }
    uint16_t getProductId()    const { return _productId; }
    uint8_t  getEpOut()        const { return _epOut; }
    uint8_t  getEpIn()         const { return _epIn; }
    uint8_t  getIfaceNum()     const { return _ifaceNum; }

    // Raw descriptor bytes — needed by USB/IP server to answer GET_DESCRIPTOR
    const uint8_t* getDevDescRaw()  const { return _devDescBuf; }
    size_t         getDevDescLen()  const { return _devDescLen; }
    const uint8_t* getCfgDescRaw()  const { return _cfgDescBuf; }
    size_t         getCfgDescLen()  const { return _cfgDescLen; }

private:
    UsbDeviceState _state;
    bool _maintenanceMode;

    String _manufacturer, _product, _deviceId;
    uint16_t _vendorId, _productId;

    usb_host_client_handle_t _clientHandle;
    usb_device_handle_t      _deviceHandle;
    uint8_t  _epOut, _epIn;
    uint16_t _epOutMPS, _epInMPS;
    uint8_t  _ifaceNum;

    // Cached raw descriptors for USB/IP
    uint8_t _devDescBuf[DEV_DESC_BUF_SIZE];
    size_t  _devDescLen;
    uint8_t _cfgDescBuf[CFG_DESC_BUF_SIZE];
    size_t  _cfgDescLen;

    static void _clientEventCallback(const usb_host_client_event_msg_t*, void*);
    void _handleClientEvent(const usb_host_client_event_msg_t*);
    bool _openDevice(uint8_t devAddr);
    bool _findDeviceInterface();
    bool _claimInterface();
    void _closeDevice();
    bool _getDeviceId();

    // Internal: submit a bulk transfer on any endpoint
    int _bulkTransfer(uint8_t endpoint, uint8_t* buffer, size_t length,
                      uint32_t timeoutMs);
};
