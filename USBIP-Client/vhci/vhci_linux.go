//go:build linux

package vhci

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"

	"github.com/pldrobot/usbip-client/usbip"
)

const sysfsBase = "/sys/bus/platform/drivers/vhci_hcd/vhci_hcd.0"

type linuxVHCI struct{}

func newPlatformVHCI() VHCI { return &linuxVHCI{} }

func (*linuxVHCI) IsInstalled() bool {
	_, err := os.Stat(sysfsBase)
	return err == nil
}

// PlugIn transfers the TCP socket to the vhci-hcd kernel module.
// The kernel increments the socket's reference count, so we can safely
// close our copies after writing to sysfs.
func (*linuxVHCI) PlugIn(dev *usbip.AttachedDevice) (uint32, error) {
	port, err := findFreePort()
	if err != nil {
		return 0, fmt.Errorf("find free vhci port: %w", err)
	}

	type syscallConner interface {
		SyscallConn() (syscall.RawConn, error)
	}
	sc, ok := dev.Conn.(syscallConner)
	if !ok {
		return 0, fmt.Errorf("connection does not expose raw syscall access")
	}
	rawConn, err := sc.SyscallConn()
	if err != nil {
		return 0, fmt.Errorf("get raw conn: %w", err)
	}

	// Duplicate the socket fd so the kernel can take ownership independently
	// of Go's runtime fd management.
	var dupFd int
	var dupErr error
	rawConn.Control(func(fd uintptr) {
		dupFd, dupErr = syscall.Dup(int(fd))
	})
	if dupErr != nil {
		return 0, fmt.Errorf("dup socket fd: %w", dupErr)
	}

	// vhci-hcd attach: "<sockfd> <busnum> <devnum> <speed>"
	content := fmt.Sprintf("%d %d %d %d\n",
		dupFd, dev.DevInfo.BusNum, dev.DevInfo.DevNum, dev.DevInfo.Speed)
	if err := os.WriteFile(filepath.Join(sysfsBase, "attach"), []byte(content), 0); err != nil {
		syscall.Close(dupFd)
		return 0, fmt.Errorf("write sysfs attach: %w", err)
	}

	// Kernel took ownership via sockfd_lookup; close our dup'd copy.
	syscall.Close(dupFd)
	// Close Go's connection — the kernel holds the socket via its own ref.
	dev.Conn.Close()
	dev.Conn = nil

	return port, nil
}

func (*linuxVHCI) Unplug(port uint32) error {
	content := fmt.Sprintf("%d\n", port)
	if err := os.WriteFile(filepath.Join(sysfsBase, "detach"), []byte(content), 0); err != nil {
		return fmt.Errorf("write sysfs detach: %w", err)
	}
	return nil
}

// findFreePort reads the vhci status file and returns the first free port.
// Status file columns: hub port status devid speed busnum devnum
func findFreePort() (uint32, error) {
	data, err := os.ReadFile(filepath.Join(sysfsBase, "status"))
	if err != nil {
		return 0, fmt.Errorf("read vhci status: %w", err)
	}
	for _, line := range strings.Split(string(data), "\n") {
		fields := strings.Fields(line)
		if len(fields) < 2 {
			continue
		}
		portNum, errP := strconv.ParseUint(fields[0], 10, 32)
		status, errS := strconv.ParseUint(fields[2], 16, 32)
		if errP != nil || errS != nil {
			continue
		}
		if status == 0 { // 0x00 = port free
			return uint32(portNum), nil
		}
	}
	return 0, fmt.Errorf("no free vhci ports available (is vhci-hcd loaded?)")
}
