// Package vhci provides an interface to the OS virtual USB host controller.
// Windows: uses the usbip2_ude.sys driver (usbip-win2).
// Linux:   uses the vhci-hcd kernel module via sysfs.
package vhci

import "github.com/pldrobot/usbip-client/usbip"

// VHCI controls a virtual USB host controller.
type VHCI interface {
	// PlugIn registers a remote USB device. On Windows the driver creates its
	// own TCP connection to the server; dev.Conn is closed before return.
	// Returns the virtual port number assigned by the driver.
	PlugIn(dev *usbip.AttachedDevice) (port uint32, err error)

	// Unplug disconnects the virtual device on the given port.
	Unplug(port uint32) error

	// IsInstalled reports whether the platform driver is installed and reachable.
	IsInstalled() bool
}

// New returns the platform-specific VHCI implementation.
func New() VHCI {
	return newPlatformVHCI()
}
