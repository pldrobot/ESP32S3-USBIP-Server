#include "usbip_server.h"
#include "usb_device.h"
#include "rdebug.h"

// ── Big-endian helpers ────────────────────────────────────────────────────────
void     UsbIpServer::_w32(uint8_t* p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
void     UsbIpServer::_w16(uint8_t* p, uint16_t v) { p[0]=v>>8;  p[1]=v; }
uint32_t UsbIpServer::_r32(const uint8_t* p)       { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
uint16_t UsbIpServer::_r16(const uint8_t* p)       { return ((uint16_t)p[0]<<8)|p[1]; }

// ── I/O ───────────────────────────────────────────────────────────────────────

bool UsbIpServer::_readExact(uint8_t* buf, size_t len, uint32_t timeoutMs) {
    size_t got = 0;
    unsigned long deadline = millis() + timeoutMs;
    while (got < len) {
        if (!_client.connected()) return false;
        int avail = _client.available();
        if (avail > 0) {
            int n = _client.read(buf + got, min((int)(len - got), avail));
            if (n > 0) got += n;
        } else if (millis() > deadline) {
            return false;
        } else {
            delay(1);
        }
    }
    return true;
}

bool UsbIpServer::_writeAll(const uint8_t* buf, size_t len) {
    size_t wrote = 0;
    while (wrote < len) {
        int n = _client.write(buf + wrote, len - wrote);
        if (n <= 0) return false;
        wrote += n;
    }
    return true;
}

// ── Device info (312 bytes) ───────────────────────────────────────────────────

void UsbIpServer::_fillDevInfo(uint8_t out[312]) {
    memset(out, 0, 312);
    strncpy((char*)out,         "/sys/devices/esp32/usb/1-1", 255);
    strncpy((char*)(out + 256), USBIP_BUSID, 31);
    _w32(out + 288, USBIP_BUSNUM);
    _w32(out + 292, USBIP_DEVNUM);
    _w32(out + 296, USBIP_SPEED_HIGH);
    _w16(out + 300, _device->getVendorId());
    _w16(out + 302, _device->getProductId());
    _w16(out + 304, 0x0100);
    out[306] = 0; out[307] = 0; out[308] = 0;
    out[309] = 1; out[310] = 1; out[311] = 1;
}

// ── Session tracking ──────────────────────────────────────────────────────────

void UsbIpServer::_onConnect(const String& ip) {
    strncpy(_curIP, ip.c_str(), sizeof(_curIP) - 1);
    _connectedMs = millis();
    _attachedMs  = 0;
    debugI("USBIP Connected: %s", _curIP);
}

void UsbIpServer::_onAttach() {
    _attachedMs = millis();
    _attached   = true;
    debugI("USBIP Attached: %s", _curIP);
}

void UsbIpServer::_onDisconnect() {
    if (!_curIP[0]) return;

    UsbIpSessionRecord& r = _history[_histNext];
    r.valid       = true;
    strncpy(r.ip, _curIP, sizeof(r.ip) - 1);
    r.endedAt     = (uint32_t)(millis() / 1000);
    r.durationSec = _attachedMs ? (uint32_t)((millis() - _attachedMs) / 1000) : 0;
    _histNext = (_histNext + 1) % MAX_HISTORY;

    debugI("USBIP Disconnected: %s (attached %us)", _curIP, r.durationSec);
    memset(_curIP, 0, sizeof(_curIP));
    _connectedMs = 0;
    _attachedMs  = 0;
    _attached    = false;
}

// ── Public ────────────────────────────────────────────────────────────────────

void UsbIpServer::begin(UsbDevice* device) {
    _device = device;
    _server.begin(USBIP_PORT);
    _server.setNoDelay(true);
    debugI("USBIP Server on port %d — new client auto-kicks existing", USBIP_PORT);
}

void UsbIpServer::kickClient() {
    if (_client && _client.connected()) {
        _onDisconnect();
        _client.stop();
        debugI("USBIP Client kicked by admin");
    }
}

void UsbIpServer::task() {
    // New connection incoming — kick existing and take over
    if (_server.hasClient()) {
        WiFiClient incoming = _server.accept();
        if (_client && _client.connected()) {
            debugI("USBIP New client %s — kicking current %s",
                   incoming.remoteIP().toString().c_str(), _curIP);
            _onDisconnect();
            _client.stop();
        }
        _client = incoming;
        _onConnect(_client.remoteIP().toString());
    }

    // Detect drop
    if (_client && !_client.connected()) {
        if (_curIP[0]) _onDisconnect();
        _client.stop();
    }

    if (!_client || !_client.connected()) return;

    if (!_attached) {
        if (_client.available() >= 8) {
            uint8_t hdr[8];
            if (!_readExact(hdr, 8)) { _client.stop(); return; }
            uint16_t cmd = _r16(hdr + 2);
            if      (cmd == OP_REQ_DEVLIST) _handleDevList();
            else if (cmd == OP_REQ_IMPORT)  _handleImport();
        }
    } else {
        if (_client.available() >= 48) _handleUrbs();
    }
}

// ── Handshake ─────────────────────────────────────────────────────────────────

bool UsbIpServer::_handleDevList() {
    bool has = _device->isConnected();
    uint8_t rep[12];
    _w16(rep + 0, USBIP_VERSION);
    _w16(rep + 2, OP_REP_DEVLIST);
    _w32(rep + 4, 0);
    _w32(rep + 8, has ? 1 : 0);
    if (!_writeAll(rep, 12)) return false;
    if (has) {
        uint8_t devinfo[312]; _fillDevInfo(devinfo);
        if (!_writeAll(devinfo, 312)) return false;
        uint8_t iface[4] = { 0x07, 0x01, 0x02, 0x00 };
        _writeAll(iface, 4);
    }
    return true;
}

bool UsbIpServer::_handleImport() {
    uint8_t busid[32];
    if (!_readExact(busid, 32)) return false;

    uint8_t rep[8];
    _w16(rep + 0, USBIP_VERSION);
    _w16(rep + 2, OP_REP_IMPORT);

    if (!_device->isConnected()) {
        _w32(rep + 4, 1);
        _writeAll(rep, 8);
        debugE("USBIP IMPORT rejected: printer not connected");
        return false;
    }

    _w32(rep + 4, 0);
    if (!_writeAll(rep, 8)) return false;
    uint8_t devinfo[312]; _fillDevInfo(devinfo);
    if (!_writeAll(devinfo, 312)) return false;
    _onAttach();
    return true;
}

// ── URB forwarding ────────────────────────────────────────────────────────────

void UsbIpServer::_handleUrbs() {
    uint8_t hdr[48];
    if (!_readExact(hdr, 48)) { _client.stop(); return; }

    uint32_t command   = _r32(hdr +  0);
    uint32_t seqnum    = _r32(hdr +  4);
    uint32_t direction = _r32(hdr + 12);
    uint32_t ep        = _r32(hdr + 16);
    int32_t  bufLen    = (int32_t)_r32(hdr + 24);
    uint8_t  setup[8]; memcpy(setup, hdr + 40, 8);

    if (command == USBIP_CMD_SUBMIT) {
        _processSubmit(seqnum, direction, ep, setup, bufLen);
    } else if (command == USBIP_CMD_UNLINK) {
        uint8_t rep[48] = {};
        _w32(rep +  0, USBIP_RET_UNLINK);
        _w32(rep +  4, seqnum);
        _w32(rep + 20, (uint32_t)(-104));
        _writeAll(rep, 48);
    }
}

bool UsbIpServer::_processSubmit(uint32_t seqnum, uint32_t direction, uint32_t ep,
                                   const uint8_t setup[8], int32_t bufLen) {
    uint8_t* buf = nullptr;
    if (bufLen > 0) {
        buf = (uint8_t*)malloc(bufLen);
        if (!buf) { _client.stop(); return false; }
    }

    int actual = 0; int32_t status = 0;

    if (ep == 0) {
        if (direction == USBIP_DIR_OUT && bufLen > 0)
            if (!_readExact(buf, bufLen)) { free(buf); return false; }
        usb_setup_packet_t pkt; memcpy(&pkt, setup, 8);
        actual = _device->submitControlTransfer(&pkt, buf, bufLen, 3000);
        if (actual < 0) { status = -32; actual = 0; }
    } else if (direction == USBIP_DIR_OUT) {
        if (bufLen > 0 && !_readExact(buf, bufLen)) { free(buf); return false; }
        actual = _device->submitBulkOut(buf, bufLen, 8000);
        if (actual < 0) { status = -32; actual = 0; }
    } else {
        if (bufLen > 0) {
            actual = _device->submitBulkIn(buf, bufLen, 5000);
            if (actual < 0) { status = -32; actual = 0; }
        }
    }

    uint8_t rep[48] = {};
    _w32(rep +  0, USBIP_RET_SUBMIT);
    _w32(rep +  4, seqnum);
    _w32(rep + 20, (uint32_t)status);
    _w32(rep + 24, (uint32_t)actual);
    if (!_writeAll(rep, 48)) { free(buf); return false; }
    if (direction == USBIP_DIR_IN && actual > 0 && buf)
        if (!_writeAll(buf, actual)) { free(buf); return false; }

    free(buf);
    return true;
}

// ── Status JSON ───────────────────────────────────────────────────────────────

String UsbIpServer::statusJson() const {
    String j = "{";
    j += "\"attached\":" + String(_attached ? "true" : "false") + ",";

    if (_curIP[0]) {
        uint32_t connSec = _connectedMs ? (uint32_t)((millis() - _connectedMs) / 1000) : 0;
        uint32_t attSec  = _attachedMs  ? (uint32_t)((millis() - _attachedMs)  / 1000) : 0;
        j += "\"client\":{\"ip\":\"" + String(_curIP) + "\""
           + ",\"connectedSec\":"    + String(connSec)
           + ",\"attachedSec\":"     + String(attSec) + "},";
    } else {
        j += "\"client\":null,";
    }

    uint32_t nowSec = (uint32_t)(millis() / 1000);
    j += "\"history\":[";
    bool first = true;
    for (int i = 1; i <= MAX_HISTORY; i++) {
        int idx = (_histNext - i + MAX_HISTORY) % MAX_HISTORY;
        const UsbIpSessionRecord& r = _history[idx];
        if (!r.valid) continue;
        if (!first) j += ",";
        first = false;
        uint32_t agoSec = (r.endedAt <= nowSec) ? (nowSec - r.endedAt) : 0;
        j += "{\"ip\":\"" + String(r.ip) + "\""
           + ",\"durationSec\":" + String(r.durationSec)
           + ",\"agoSec\":"      + String(agoSec) + "}";
    }
    j += "]}";
    return j;
}
