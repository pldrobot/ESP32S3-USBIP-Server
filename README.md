# ESP32-S3 USB/IP Bridge

Share one Full/Low-Speed (12 Mbps) USB device over the network using an ESP32-S3.
The firmware exposes the physical device over the
[USB/IP protocol](https://usbip.sourceforge.net/), so Windows and Linux can attach
it as a virtual USB device and use the real vendor driver. Printers, USB serial
adapters (CDC-ACM, CH340, FTDI), HID devices and similar work as if the device were
plugged in directly.

Not supported: hubs and more than one device, isochronous devices (audio, webcams),
High-Speed devices (the ESP32-S3 USB PHY is Full/Low Speed only).

## Features

- **Any USB device, no filtering.** Every interface (alternate setting 0) is claimed and every
  bulk and interrupt endpoint is forwarded; the PC sees the device's real descriptors, so its
  own driver binds (printer, CDC-ACM, CH340/FTDI serial, HID, ...). Full/Low speed, one device.
- **Asynchronous, zero-copy transfers.** Transfers are submitted without waiting; completions only
  queue a response. Payloads move between the socket and the DMA transfer buffer without extra
  copies, from a pool preallocated at boot (no malloc per URB).
- **Serial-friendly read-ahead.** Streaming endpoints (vendor-specific, CDC) are kept polled so a
  serial adapter does not lose data between network round trips.
- **Large writes.** Bulk OUT bigger than one 8 KB buffer is sent through two alternating buffers,
  overlapping network receive with the USB transfer.
- **Safe unplug / detach.** Pending transfers are cancelled and completed in order (`CMD_UNLINK`
  semantics follow usbipdcpp); the device is released only after the host stack has finished with it.
- **Operations.** Web dashboard with login (show/hide password), live status pushed over a WebSocket
  (the page never polls), WiFi setup, USB/IP status and kick, OTA updates, and a crash-surviving log (RTC ring + LittleFS).

## How it works

```
[USB device] ──USB──► [ESP32-S3] ──WiFi──► [Windows/Linux PC]
                         TCP:3240              USB/IP client
                                               → real vendor driver
```

1. The ESP32-S3 acts as a USB Host and enumerates the device.
2. The USB/IP server (port 3240) forwards all USB requests transparently over TCP.
3. The PC binds the device's native driver to the virtual USB device.

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
| `OTA_IP` | Optional device IP used as the OTA upload target (overrides `OTA_HOSTNAME`; avoids mDNS problems) |
| `OTA_PASSWORD` | Password required to accept OTA firmware updates |
| `LED_PIN` | GPIO number of the status LED |
| `NVS_NAMESPACE` | Flash namespace for storing WiFi credentials |

`USB_DEVICE_VID` / `USB_DEVICE_PID` are informational only — the bridge
accepts any full/low-speed USB device without filtering by VID/PID.

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

### 5 — Connect the USB device

Plug your USB device into the ESP32-S3's USB-C port (the one connected to the
USB OTG controller — **not** the debug/serial port).

The status LED and web dashboard at **http://\<device-ip\>** confirm when the
device is detected.

## Connecting a client

### USBIP-Client (recommended)

[**USBIP-Client**](https://github.com/pldrobot/USBIP-Client) is a small cross-platform system-tray app
(Rust) built for this server. Right-click the tray icon to see the servers found on the LAN, check one to
attach and uncheck it to detach, with **Refresh** and **Exit** at the bottom.

- **Discovery:** it scans the local /24 for port 3240 (plus hosts listed in `servers.txt`), then asks each
  server for its device list.
- **Attach:** on Windows it drives `usbip.exe` from a bundled usbip-win2 install; on Linux it uses the kernel
  `vhci_hcd` sysfs interface directly.
- **While a device is attached,** its Refresh and scan open extra connections to the server. The firmware
  answers them instead of dropping them: `DEVLIST` returns an empty list (the device is in use) and `IMPORT`
  is refused with status `NA`, so the client's list stays clean.

Once attached, the PC detects the virtual USB device and loads the driver for it as if it were plugged in
locally.

### Other USB/IP clients

Any standard USB/IP client works; the tray app just automates discovery and attach.

- **Linux:** the kernel client from `usbip` / `linux-tools`:
  ```bash
  usbip attach -r <ESP32_IP> -b 1-1
  usbip detach -p <port>
  ```
- **Windows:** [usbip-win2](https://github.com/vadimgrn/usbip-win2):
  ```powershell
  # Replace <ESP32_IP> with the device's IP or mDNS hostname
  usbip.exe attach -r <ESP32_IP> -b 1-1
  usbip.exe detach -p <port>
  ```

## OTA updates (after first flash)

After the first flash the `esp32s3_ota` environment uses the hostname and
password from `.env` automatically:

```bash
pio run -e esp32s3_ota -t upload
```

Logs (including the log from before a crash/reboot) are on the web dashboard
— see the **Logs** card.

## Services

| Port | Protocol | Purpose |
|------|----------|---------|
| 3240 | TCP | USB/IP bridge |
| 80 | HTTP + WebSocket (`/ws`) | Web dashboard (WiFi config) and live status push (needs the dashboard login) |
| 3232 | TCP | OTA firmware upload (ArduinoOTA) |

## LED status

| Pattern | Meaning |
|---------|---------|
| Solid ON | WiFi connected, device idle |
| Slow blink (500 ms) | USB/IP client attached |
| OFF | No WiFi |

## Project structure

```
.
├── src/
│   ├── main.cpp          — Setup, loop, OTA, watchdog
│   ├── config.h          — Fixed protocol constants; includes config_env.h
│   ├── config_env.h      — Auto-generated from .env (gitignored)
│   ├── wifi_manager.h/.cpp — AP/Station WiFi with NVS credential storage
│   ├── web_ui.h/.cpp     — HTTP dashboard + REST API (port 80)
│   ├── applog.h          — debugI/W/E macros → Serial + persistent log
│   └── persist_log.h/.cpp — crash-surviving log (RTC ring + LittleFS)
├── tools/
│   └── load_env.py       — PlatformIO pre-build script; reads .env → config_env.h
├── .env                  — Your local config (gitignored — never commit)
├── .env.template         — Template to copy and fill in
└── platformio.ini        — Build environments (USB serial + OTA)
```

The USB host and USB/IP engine is the
[EspUsbIpServer](https://github.com/pldrobot/EspUsbIpServer) library, pulled in by `lib_deps` at the
`v1.0.0` release. The dashboard uses
[ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) and AsyncTCP.

## Design reference

The USB/IP engine follows the design of **usbipdcpp**, an open-source C++ USB/IP library, and its
ESP32 port:

- [yunsmall/usbipdcpp](https://github.com/yunsmall/usbipdcpp) — the core library (protocol, session,
  `UNLINK` handling, transfer ownership)
- [yunsmall/usbipdcpp_esp32](https://github.com/yunsmall/usbipdcpp_esp32) — its ESP-IDF glue for the
  ESP32-S3 / ESP32-P4 (`usb_transfer_t` buffers used end to end, async submit with completion
  callbacks, per-endpoint cancel and resubmit)

This project is an independent Arduino implementation of that design (no asio/spdlog/C++20), not a
copy of its source. usbipdcpp is licensed under LGPL-3.0; check its terms if you reuse its code.
Where this firmware differs on purpose: read-ahead for serial-style endpoints, a pipelined
two-buffer path for oversized bulk OUT, and bulk IN lengths rounded down to the packet size.

## License

MIT
