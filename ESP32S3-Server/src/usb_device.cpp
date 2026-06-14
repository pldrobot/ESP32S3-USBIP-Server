#include "usb_device.h"
#include "rdebug.h"

UsbDevice::UsbDevice()
    : _state(DEVICE_DISCONNECTED), _maintenanceMode(false),
      _vendorId(0), _productId(0),
      _clientHandle(nullptr), _deviceHandle(nullptr),
      _epOut(0), _epIn(0), _epOutMPS(64), _epInMPS(64), _ifaceNum(0),
      _devDescLen(0), _cfgDescLen(0) {}

bool UsbDevice::begin() {
    usb_host_config_t hostCfg = { .skip_phy_setup = false,
                                  .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    if (usb_host_install(&hostCfg) != ESP_OK) {
        debugE("USB Host install failed");
        return false;
    }

    usb_host_client_config_t clientCfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = _clientEventCallback,
                   .callback_arg = this }
    };
    if (usb_host_client_register(&clientCfg, &_clientHandle) != ESP_OK) {
        debugE("USB Client register failed");
        return false;
    }

    debugI("USB Host ready, waiting for device...");
    return true;
}

void UsbDevice::task() {
    if (!_clientHandle) return;
    usb_host_lib_handle_events(0, NULL);
    usb_host_client_handle_events(_clientHandle, 0);
}

void UsbDevice::_clientEventCallback(const usb_host_client_event_msg_t* msg, void* arg) {
    static_cast<UsbDevice*>(arg)->_handleClientEvent(msg);
}

void UsbDevice::_handleClientEvent(const usb_host_client_event_msg_t* msg) {
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        debugI("USB Device detected addr=%d", msg->new_dev.address);
        if (_state == DEVICE_DISCONNECTED)
            _openDevice(msg->new_dev.address);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        debugW("USB Device disconnected");
        _closeDevice();
    }
}

bool UsbDevice::_openDevice(uint8_t devAddr) {
    if (usb_host_device_open(_clientHandle, devAddr, &_deviceHandle) != ESP_OK) {
        debugE("USB Cannot open device");
        return false;
    }
    _state = DEVICE_CONNECTED;

    // Read and cache device descriptor
    const usb_device_desc_t* devDesc;
    if (usb_host_get_device_descriptor(_deviceHandle, &devDesc) == ESP_OK) {
        _vendorId  = devDesc->idVendor;
        _productId = devDesc->idProduct;
        _devDescLen = sizeof(usb_device_desc_t);
        memcpy(_devDescBuf, devDesc, _devDescLen);
        debugI("USB VID:0x%04X PID:0x%04X", _vendorId, _productId);

        _manufacturer = String("VID:0x") + String(_vendorId, HEX);
        _product      = String("PID:0x") + String(_productId, HEX);
        debugI("USB %s %s", _manufacturer.c_str(), _product.c_str());
    }

    // Read and cache config descriptor
    const usb_config_desc_t* cfgDesc;
    if (usb_host_get_active_config_descriptor(_deviceHandle, &cfgDesc) == ESP_OK) {
        _cfgDescLen = min((uint16_t)CFG_DESC_BUF_SIZE, cfgDesc->wTotalLength);
        memcpy(_cfgDescBuf, cfgDesc, _cfgDescLen);
    }

    if (!_findDeviceInterface() || !_claimInterface()) {
        debugE("USB No printer interface found or cannot claim — is a printer connected?");
        _closeDevice();
        return false;
    }

    _getDeviceId();
    _state = DEVICE_READY;
    debugI("USB Device ready");
    return true;
}

bool UsbDevice::_findDeviceInterface() {
    const usb_config_desc_t* cfgDesc;
    if (usb_host_get_active_config_descriptor(_deviceHandle, &cfgDesc) != ESP_OK)
        return false;

    int offset = 0;
    const uint8_t* p = (const uint8_t*)cfgDesc;
    uint16_t total  = cfgDesc->wTotalLength;

    while (offset < total) {
        const usb_standard_desc_t* desc = (const usb_standard_desc_t*)(p + offset);
        if (desc->bLength == 0) break;

        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* iface = (const usb_intf_desc_t*)desc;
            if (iface->bInterfaceClass    == USB_CLASS_PRINTER &&
                iface->bInterfaceSubClass == USB_SUBCLASS_PRINTER) {

                _ifaceNum = iface->bInterfaceNumber;
                debugI("USB Device interface #%d protocol=%d",
                       _ifaceNum, iface->bInterfaceProtocol);

                int epOff = offset + desc->bLength;
                for (int i = 0; i < iface->bNumEndpoints && epOff < total; i++) {
                    const usb_standard_desc_t* raw = (const usb_standard_desc_t*)(p + epOff);
                    if (raw->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
                        const usb_ep_desc_t* ep = (const usb_ep_desc_t*)raw;
                        if ((ep->bmAttributes & 0x03) == USB_BM_ATTRIBUTES_XFER_BULK) {
                            if (ep->bEndpointAddress & 0x80) {
                                _epIn    = ep->bEndpointAddress;
                                _epInMPS = ep->wMaxPacketSize;
                                debugI("USB Bulk IN  ep=0x%02X mps=%d", _epIn, _epInMPS);
                            } else {
                                _epOut    = ep->bEndpointAddress;
                                _epOutMPS = ep->wMaxPacketSize;
                                debugI("USB Bulk OUT ep=0x%02X mps=%d", _epOut, _epOutMPS);
                            }
                        }
                    }
                    epOff += raw->bLength;
                }
                return (_epOut != 0);
            }
        }
        offset += desc->bLength;
    }
    return false;
}

bool UsbDevice::_claimInterface() {
    esp_err_t err = usb_host_interface_claim(_clientHandle, _deviceHandle, _ifaceNum, 0);
    if (err != ESP_OK) {
        debugE("USB Claim interface failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void UsbDevice::_closeDevice() {
    if (_deviceHandle) {
        usb_host_interface_release(_clientHandle, _deviceHandle, _ifaceNum);
        usb_host_device_close(_clientHandle, _deviceHandle);
        _deviceHandle = nullptr;
    }
    _state        = DEVICE_DISCONNECTED;
    _maintenanceMode = false;
    _epOut = _epIn = 0;
    _devDescLen = _cfgDescLen = 0;
    _deviceId = _manufacturer = _product = "";
    debugW("USB Device closed");
}

bool UsbDevice::_getDeviceId() {
    if (!_deviceHandle) return false;

    usb_transfer_t* xfer;
    if (usb_host_transfer_alloc(264, 0, &xfer) != ESP_OK) return false;

    xfer->device_handle     = _deviceHandle;
    xfer->bEndpointAddress  = 0;
    xfer->timeout_ms        = 1000;

    usb_setup_packet_t* s = (usb_setup_packet_t*)xfer->data_buffer;
    s->bmRequestType = 0xA1;  // Class | Interface | IN
    s->bRequest      = USB_PRINTER_GET_DEVICE_ID;
    s->wValue        = 0;
    s->wIndex        = _ifaceNum;
    s->wLength       = 256;
    xfer->num_bytes  = 256 + sizeof(usb_setup_packet_t);

    volatile bool done = false;
    xfer->callback = [](usb_transfer_t* t){ *(volatile bool*)t->context = true; };
    xfer->context  = (void*)&done;

    if (usb_host_transfer_submit_control(_clientHandle, xfer) == ESP_OK) {
        for (int i = 0; i < 100 && !done; i++)
            usb_host_client_handle_events(_clientHandle, 10);

        if (done && xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            uint8_t* d = xfer->data_buffer + sizeof(usb_setup_packet_t);
            int len = (d[0] << 8) | d[1];
            if (len > 2 && len < 254)
                _deviceId = String((char*)(d + 2), len - 2);
            debugI("USB Device ID: %s", _deviceId.c_str());
        }
    }
    usb_host_transfer_free(xfer);
    return !_deviceId.isEmpty();
}

// ── Internal bulk transfer helper ─────────────────────────────────────────────

int UsbDevice::_bulkTransfer(uint8_t endpoint, uint8_t* buffer,
                               size_t length, uint32_t timeoutMs) {
    if (!_deviceHandle || length == 0) return -1;

    uint16_t mps      = (endpoint & 0x80) ? _epInMPS : _epOutMPS;
    size_t allocSize  = ((length + mps - 1) / mps) * mps;

    usb_transfer_t* xfer;
    if (usb_host_transfer_alloc(allocSize, 0, &xfer) != ESP_OK) return -1;

    bool isIn = (endpoint & 0x80);
    if (!isIn) memcpy(xfer->data_buffer, buffer, length);

    xfer->device_handle    = _deviceHandle;
    xfer->bEndpointAddress = endpoint;
    xfer->num_bytes        = length;
    xfer->timeout_ms       = timeoutMs;

    volatile bool done = false;
    xfer->callback = [](usb_transfer_t* t){ *(volatile bool*)t->context = true; };
    xfer->context  = (void*)&done;

    esp_err_t err = usb_host_transfer_submit(xfer);
    if (err != ESP_OK) { usb_host_transfer_free(xfer); return -1; }

    unsigned long deadline = millis() + timeoutMs;
    while (!done && millis() < deadline)
        usb_host_client_handle_events(_clientHandle, 10);

    int result = -1;
    if (done && xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        result = xfer->actual_num_bytes;
        if (isIn) memcpy(buffer, xfer->data_buffer, result);
    }
    usb_host_transfer_free(xfer);
    return result;
}

// ── Public transfer APIs ───────────────────────────────────────────────────────

int UsbDevice::sendData(const uint8_t* data, size_t length) {
    if (_maintenanceMode) {
        debugW("USB sendData rejected: locked for USB/IP");
        return -1;
    }
    if (_state != DEVICE_READY || !_epOut) return -1;

    _state = DEVICE_BUSY;
    size_t sent = 0;
    while (sent < length) {
        size_t chunk = min((size_t)_epOutMPS, length - sent);
        int n = _bulkTransfer(_epOut, (uint8_t*)(data + sent), chunk, 5000);
        if (n < 0) { _state = DEVICE_READY; return (int)sent; }
        sent += n;
    }
    _state = DEVICE_READY;
    return (int)sent;
}

int UsbDevice::readData(uint8_t* buffer, size_t maxLength, uint32_t timeoutMs) {
    if (!_epIn || !_deviceHandle) return -1;
    return _bulkTransfer(_epIn, buffer, maxLength, timeoutMs);
}

int UsbDevice::sendMaintCommand(const uint8_t* cmd, size_t cmdLen,
                                  uint8_t* response, size_t responseMaxLen,
                                  uint32_t responseTimeoutMs) {
    if (!_deviceHandle || !_epOut) return -1;
    int n = _bulkTransfer(_epOut, (uint8_t*)cmd, cmdLen, 3000);
    if (n < 0) return -1;

    if (response && responseMaxLen > 0 && _epIn) {
        delay(80);
        return _bulkTransfer(_epIn, response, responseMaxLen, responseTimeoutMs);
    }
    return n;
}

// ── USB/IP generic transfer methods ───────────────────────────────────────────

int UsbDevice::submitControlTransfer(usb_setup_packet_t* setup,
                                       uint8_t* data, size_t dataLen,
                                       uint32_t timeoutMs) {
    if (!_deviceHandle) return -1;

    // Intercept SET_CONFIGURATION and SET_INTERFACE: device already configured,
    // return success to Windows without forwarding.
    if ((setup->bmRequestType & 0x1F) == 0x00) {
        if (setup->bRequest == USB_REQ_SET_CONFIGURATION ||
            setup->bRequest == USB_REQ_SET_INTERFACE)
            return 0;
    }

    bool isIn = (setup->bmRequestType & 0x80) != 0;
    size_t bufSize = sizeof(usb_setup_packet_t) + max(dataLen, (size_t)64);

    usb_transfer_t* xfer;
    if (usb_host_transfer_alloc(bufSize, 0, &xfer) != ESP_OK) return -1;

    memcpy(xfer->data_buffer, setup, sizeof(usb_setup_packet_t));
    if (!isIn && dataLen > 0 && data)
        memcpy(xfer->data_buffer + sizeof(usb_setup_packet_t), data, dataLen);

    xfer->device_handle    = _deviceHandle;
    xfer->bEndpointAddress = 0;
    xfer->num_bytes        = sizeof(usb_setup_packet_t) + (isIn ? max(dataLen, (size_t)0) : dataLen);
    xfer->timeout_ms       = timeoutMs;

    volatile bool done = false;
    xfer->callback = [](usb_transfer_t* t){ *(volatile bool*)t->context = true; };
    xfer->context  = (void*)&done;

    if (usb_host_transfer_submit_control(_clientHandle, xfer) != ESP_OK) {
        usb_host_transfer_free(xfer); return -1;
    }

    unsigned long deadline = millis() + timeoutMs;
    while (!done && millis() < deadline)
        usb_host_client_handle_events(_clientHandle, 10);

    int result = -1;
    if (done && xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        result = (int)xfer->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
        if (result < 0) result = 0;
        if (isIn && data && result > 0)
            memcpy(data, xfer->data_buffer + sizeof(usb_setup_packet_t), result);
    }
    usb_host_transfer_free(xfer);
    return result;
}

int UsbDevice::submitBulkOut(const uint8_t* data, size_t len, uint32_t timeoutMs) {
    if (!_epOut || !_deviceHandle) return -1;
    return _bulkTransfer(_epOut, (uint8_t*)data, len, timeoutMs);
}

int UsbDevice::submitBulkIn(uint8_t* buf, size_t maxLen, uint32_t timeoutMs) {
    if (!_epIn || !_deviceHandle) return -1;
    return _bulkTransfer(_epIn, buf, maxLen, timeoutMs);
}

bool UsbDevice::softReset() {
    if (!_deviceHandle) return false;
    usb_transfer_t* xfer;
    if (usb_host_transfer_alloc(8, 0, &xfer) != ESP_OK) return false;

    xfer->device_handle    = _deviceHandle;
    xfer->bEndpointAddress = 0;
    xfer->timeout_ms       = 1000;

    usb_setup_packet_t* s = (usb_setup_packet_t*)xfer->data_buffer;
    s->bmRequestType = 0x21;  // Class | Interface | OUT
    s->bRequest      = USB_PRINTER_SOFT_RESET;
    s->wValue = s->wIndex = s->wLength = 0;
    s->wIndex    = _ifaceNum;
    xfer->num_bytes = sizeof(usb_setup_packet_t);

    volatile bool done = false;
    xfer->callback = [](usb_transfer_t* t){ *(volatile bool*)t->context = true; };
    xfer->context  = (void*)&done;

    bool ok = false;
    if (usb_host_transfer_submit_control(_clientHandle, xfer) == ESP_OK) {
        for (int i = 0; i < 100 && !done; i++)
            usb_host_client_handle_events(_clientHandle, 10);
        ok = done && xfer->status == USB_TRANSFER_STATUS_COMPLETED;
    }
    usb_host_transfer_free(xfer);
    if (ok) debugI("USB Soft reset OK");
    return ok;
}

void UsbDevice::setMaintenanceMode(bool active) {
    _maintenanceMode = active;
    if (active && _state == DEVICE_READY)
        _state = DEVICE_MAINTENANCE;
    else if (!active && _state == DEVICE_MAINTENANCE)
        _state = DEVICE_READY;
    debugI("USB Maintenance lock: %s", active ? "ON" : "OFF");
}
