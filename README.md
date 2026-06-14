# ESP32-S3 USB/IP Bridge

Turn any USB printer into a networked printer using an ESP32-S3. The firmware
exposes the physical USB printer over the
[USB/IP protocol](https://usbip.sourceforge.net/), so Windows (and Linux/macOS
with the usbipd client) can attach it as a virtual USB device and use the real
vendor driver — full printing, maintenance utilities, ink monitoring, and scanner
access all work as if the printer were plugged in directly.

## How it works

```
[USB Printer] ──USB──► [ESP32-S3] ──WiFi──► [Windows PC]
                         TCP:3240              usbipd client
                                               → real vendor driver
```

1. The ESP32-S3 acts as a USB Host and enumerates the printer.
2. The USB/IP server (port 3240) forwards all USB requests transparently over TCP.
3. Windows installs the printer's native driver on the virtual USB device.

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-S3-N16R8 (16 MB flash, 8 MB PSRAM) |
| USB | Onboard USB OTG port wired as USB Host |
| LED | Status LED on GPIO 2 (configurable via `LED_PIN` in `.env`) |

Any ESP32-S3 DevKit-C board works. The firmware is tested on the **DevKitC-1**
variant.

## Setup

### 1 — Clone and configure

```bash
git clone https://github.com/your-username/ESP32S3-USBIP-Server
cd ESP32S3-USBIP-Server
cp .env.template .env
```

Edit `.env` with your values (see comments inside). Key fields:

| Key | Description |
|-----|-------------|
| `AP_SSID` | WiFi SSID the ESP32 broadcasts when unconfigured |
| `AP_PASSWORD` | Password for the setup AP |
| `MDNS_HOSTNAME` | mDNS hostname (device appears as `<hostname>.lan`) |
| `DEVICE_NAME` | Label shown in logs and the web dashboard |
| `OTA_HOSTNAME` | Hostname used for OTA uploads (usually same as `MDNS_HOSTNAME`) |
| `OTA_PASSWORD` | Password required to accept OTA firmware updates |
| `LED_PIN` | GPIO number of the status LED |
| `NVS_NAMESPACE` | Flash namespace for storing WiFi credentials |

`USB_DEVICE_VID` / `USB_DEVICE_PID` are informational only — the bridge
accepts any USB printer automatically without filtering by VID/PID.

### 2 — Set your serial port

Edit `platformio.ini` and uncomment the upload/monitor port lines in `[env:esp32s3]`:

```ini
upload_port  = COM3      ; Windows example
monitor_port = COM3
```

### 3 — Flash

```bash
# Initial flash via USB cable
pio run -e esp32s3 -t upload

# Monitor serial output
pio run -e esp32s3 -t monitor
```

The pre-build script reads `.env` and generates `src/config_env.h`
automatically before every build.

### 4 — WiFi setup (first boot)

If no WiFi credentials are saved, the ESP32 starts in AP mode:

1. Connect your PC to the `AP_SSID` network you set in `.env`.
2. Open **http://192.168.4.1** in a browser.
3. Use the **WiFi** card to scan and connect to your home/office network.
4. The ESP32 reboots and connects as a station. Its IP is shown in the serial log.

### 5 — Connect the printer

Plug your USB printer into the ESP32-S3's USB-C port (the one connected to the
USB OTG controller — **not** the debug/serial port).

The status LED and web dashboard at **http://\<device-ip\>** confirm when the
printer is detected.

## Windows client setup

### Install usbipd (one-time)

```powershell
winget install usbipd
```

### Attach the printer

```powershell
# Replace <ESP32_IP> with the device's IP or mDNS hostname
usbip attach -r <ESP32_IP> -b 1-1
```

Windows detects the virtual USB device and installs the printer driver
automatically. Print, scan, and run maintenance as normal.

To detach:

```powershell
usbip detach -p <port>
```

## OTA updates (after first flash)

After the first flash the `esp32s3_ota` environment uses the hostname and
password from `.env` automatically:

```bash
pio run -e esp32s3_ota -t upload
```

For the Telnet debug monitor:

```bash
pio run -e esp32s3_ota -t monitor
```

## Services

| Port | Protocol | Purpose |
|------|----------|---------|
| 3240 | TCP | USB/IP bridge |
| 80 | HTTP | Web dashboard (status + WiFi config) |
| 23 | Telnet | Live debug output (RemoteDebug) |
| 3232 | TCP | OTA firmware upload (ArduinoOTA) |

## LED status

| Pattern | Meaning |
|---------|---------|
| Solid ON | WiFi connected, printer idle |
| Slow blink (500 ms) | USB/IP client attached (printing/maintenance) |
| OFF | No WiFi |

## Project structure

```
.
├── src/
│   ├── main.cpp          — Setup, loop, OTA, watchdog
│   ├── config.h          — Fixed protocol constants; includes config_env.h
│   ├── config_env.h      — Auto-generated from .env (gitignored)
│   ├── usb_device.h/.cpp  — USB Host driver (class 0x07 printers)
│   ├── usbip_server.h/.cpp — USB/IP protocol bridge (port 3240)
│   ├── wifi_manager.h/.cpp — AP/Station WiFi with NVS credential storage
│   ├── web_ui.h/.cpp     — HTTP dashboard + REST API (port 80)
│   └── rdebug.h          — RemoteDebug Telnet wrapper (port 23)
├── tools/
│   └── load_env.py       — PlatformIO pre-build script; reads .env → config_env.h
├── .env                  — Your local config (gitignored — never commit)
├── .env.template         — Template to copy and fill in
└── platformio.ini        — Build environments (USB serial + OTA)
```

## License

MIT
