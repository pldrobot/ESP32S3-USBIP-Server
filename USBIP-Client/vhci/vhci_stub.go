//go:build !windows && !linux

package vhci

import (
	"fmt"

	"github.com/pldrobot/usbip-client/usbip"
)

type stubVHCI struct{}

func newPlatformVHCI() VHCI { return &stubVHCI{} }

func (*stubVHCI) IsInstalled() bool { return false }

func (*stubVHCI) PlugIn(_ *usbip.AttachedDevice) (uint32, error) {
	return 0, fmt.Errorf("VHCI driver not supported on this platform")
}

func (*stubVHCI) Unplug(_ uint32) error {
	return fmt.Errorf("VHCI driver not supported on this platform")
}
