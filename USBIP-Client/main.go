package main

import (
	"fmt"
	"os"
	"strconv"

	"github.com/pldrobot/usbip-client/tray"
	"github.com/pldrobot/usbip-client/usbip"
)

func main() {
	if len(os.Args) > 1 {
		switch os.Args[1] {
		case "scan":
			if len(os.Args) < 3 {
				fmt.Fprintln(os.Stderr, "usage: usbip-client scan <host> [port]")
				os.Exit(1)
			}
			port := 3240
			if len(os.Args) >= 4 {
				if p, err := strconv.Atoi(os.Args[3]); err == nil {
					port = p
				}
			}
			cmdScan(os.Args[2], port)
			return

		case "-h", "--help", "help":
			fmt.Print(`usbip-client — USB/IP system tray client

Usage:
  usbip-client              Launch system tray application (default)
  usbip-client scan <host>  List USB devices on a USB/IP server
  usbip-client help         Show this help

Config file is loaded from next to the executable (portable) or
  Windows: %APPDATA%\usbip-client\config.json
  Linux:   ~/.config/usbip-client/config.json

Copy config.json.example to config.json to pre-configure connections.
`)
			return
		}
	}

	tray.Run()
}

func cmdScan(host string, port int) {
	fmt.Printf("Scanning %s:%d …\n", host, port)
	client := usbip.NewClient(host, port)
	devices, err := client.ListDevices()
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
	if len(devices) == 0 {
		fmt.Println("no devices found")
		return
	}
	fmt.Printf("%-10s  %-9s  %s\n", "BUS ID", "USB ID", "Speed")
	fmt.Println("----------  ---------  ------")
	for _, d := range devices {
		fmt.Printf("%-10s  %04X:%04X   %s\n",
			usbip.BusIDString(d.Info),
			d.Info.IDVendor, d.Info.IDProduct,
			usbip.SpeedString(d.Info.Speed),
		)
	}
}
