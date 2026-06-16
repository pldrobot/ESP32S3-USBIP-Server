//go:build windows

package vhci

import (
	"fmt"
	"unsafe"

	"golang.org/x/sys/windows"

	"github.com/pldrobot/usbip-client/usbip"
)

var (
	cfgmgr32                     = windows.NewLazySystemDLL("cfgmgr32.dll")
	procCMGetDevIfaceListSize     = cfgmgr32.NewProc("CM_Get_Device_Interface_List_SizeW")
	procCMGetDevIfaceList         = cfgmgr32.NewProc("CM_Get_Device_Interface_ListW")
)

// GUID_DEVINTERFACE_USB_HOST_CONTROLLER for usbip2_ude (from usbip-win2 vhci.h).
var guidVHCI = windows.GUID{
	Data1: 0xB4030C06,
	Data2: 0xDC5F,
	Data3: 0x4FCC,
	Data4: [8]byte{0x87, 0xEB, 0xE5, 0x51, 0x5A, 0x09, 0x35, 0xC0},
}

const (
	// IOCTL codes: CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, func, METHOD_BUFFERED=0, FILE_READ_DATA|FILE_WRITE_DATA=3)
	// = (0x22 << 16) | (3 << 14) | (func << 2)
	ioctlPluginHardware  = uint32(0x0022E000) // func = 0x800
	ioctlPlugoutHardware = uint32(0x0022E004) // func = 0x801
	ioctlGetImported     = uint32(0x0022E008) // func = 0x802

	cmDevIfaceListPresent = uint32(0x00000001)

	// plugin_hardware layout (bytes):
	//   [0:4]    base::size  = 1097
	//   [4:8]    port (int32, output — assigned port, 0 on input)
	//   [8:40]   busid[32]
	//   [40:72]  service[32]  (TCP port as ASCII string, e.g. "3240")
	//   [72:1097] host[1025]
	pluginSize = 1097
)

type windowsVHCI struct{}

func newPlatformVHCI() VHCI { return &windowsVHCI{} }

func (*windowsVHCI) IsInstalled() bool {
	path, err := findDevicePath()
	return err == nil && path != ""
}

func (*windowsVHCI) PlugIn(dev *usbip.AttachedDevice) (uint32, error) {
	path, err := findDevicePath()
	if err != nil {
		return 0, fmt.Errorf("vhci device: %w", err)
	}
	h, err := openDevice(path)
	if err != nil {
		return 0, fmt.Errorf("open vhci: %w", err)
	}
	defer windows.CloseHandle(h)

	var buf [pluginSize]byte
	*(*uint32)(unsafe.Pointer(&buf[0])) = uint32(pluginSize)
	// port at [4] stays 0 → driver auto-assigns
	copy(buf[8:40], dev.BusID)
	copy(buf[40:72], fmt.Sprintf("%d", dev.TCPPort))
	copy(buf[72:1097], dev.Host)

	var returned uint32
	// Output is first 8 bytes only: size(4) + port(4)
	if err := windows.DeviceIoControl(h, ioctlPluginHardware,
		&buf[0], uint32(pluginSize),
		&buf[0], 8,
		&returned, nil); err != nil {
		return 0, fmt.Errorf("plugin ioctl: %w", err)
	}

	port := *(*int32)(unsafe.Pointer(&buf[4]))
	if port <= 0 {
		return 0, fmt.Errorf("driver returned invalid port %d", port)
	}

	// The driver creates its own TCP connection; our probe connection is no longer needed.
	dev.Detach()
	return uint32(port), nil
}

func (*windowsVHCI) Unplug(port uint32) error {
	path, err := findDevicePath()
	if err != nil {
		return fmt.Errorf("vhci device: %w", err)
	}
	h, err := openDevice(path)
	if err != nil {
		return fmt.Errorf("open vhci: %w", err)
	}
	defer windows.CloseHandle(h)

	// plugout_hardware: size(4) + port(4)
	var buf [8]byte
	*(*uint32)(unsafe.Pointer(&buf[0])) = 8
	*(*int32)(unsafe.Pointer(&buf[4])) = int32(port)

	var returned uint32
	return windows.DeviceIoControl(h, ioctlPlugoutHardware,
		&buf[0], 8, nil, 0, &returned, nil)
}

func openDevice(path string) (windows.Handle, error) {
	pathPtr, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return 0, err
	}
	return windows.CreateFile(
		pathPtr,
		windows.GENERIC_READ|windows.GENERIC_WRITE,
		windows.FILE_SHARE_READ|windows.FILE_SHARE_WRITE,
		nil,
		windows.OPEN_EXISTING,
		windows.FILE_ATTRIBUTE_NORMAL,
		0,
	)
}

// findDevicePath enumerates the usbip2_ude device interface via cfgmgr32.
func findDevicePath() (string, error) {
	var cch uint32
	r, _, _ := procCMGetDevIfaceListSize.Call(
		uintptr(unsafe.Pointer(&cch)),
		uintptr(unsafe.Pointer(&guidVHCI)),
		0,
		uintptr(cmDevIfaceListPresent),
	)
	if r != 0 {
		return "", fmt.Errorf("CM_Get_Device_Interface_List_SizeW returned %d", r)
	}
	if cch <= 1 {
		return "", fmt.Errorf("usbip2_ude driver not found — install usbip-win2 and enable test signing")
	}

	buf := make([]uint16, cch)
	r, _, _ = procCMGetDevIfaceList.Call(
		uintptr(unsafe.Pointer(&guidVHCI)),
		0,
		uintptr(unsafe.Pointer(&buf[0])),
		uintptr(cch),
		uintptr(cmDevIfaceListPresent),
	)
	if r != 0 {
		return "", fmt.Errorf("CM_Get_Device_Interface_ListW returned %d", r)
	}

	// Multi-sz: first entry is the device path we want
	return windows.UTF16ToString(buf), nil
}
