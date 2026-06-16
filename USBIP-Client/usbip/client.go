// Package usbip implements a pure-Go USB/IP protocol client.
// Protocol version 0x0111, all multi-byte fields are big-endian.
package usbip

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"sync/atomic"
	"time"
)

const (
	protocolVersion = 0x0111

	opReqDevlist = 0x8005
	opRepDevlist = 0x0005
	opReqImport  = 0x8003
	opRepImport  = 0x0003
)

// DeviceInfo mirrors the usbip_usb_device network struct (312 bytes, big-endian).
type DeviceInfo struct {
	Path        [256]byte
	BusID       [32]byte
	BusNum      uint32
	DevNum      uint32
	Speed       uint32
	IDVendor    uint16
	IDProduct   uint16
	BCDDevice   uint16
	DevClass    uint8
	DevSubClass uint8
	DevProtocol uint8
	ConfigVal   uint8
	NumConfigs  uint8
	NumIfaces   uint8
}

type InterfaceInfo struct {
	Class, SubClass, Protocol, Padding uint8
}

type Device struct {
	Info       DeviceInfo
	Interfaces []InterfaceInfo
}

// AttachedDevice holds the open TCP connection for a claimed USB/IP device.
type AttachedDevice struct {
	Conn    net.Conn
	DevInfo DeviceInfo
	Host    string
	BusID   string
	TCPPort uint16
	seqNum  uint32
}

func (d *AttachedDevice) Detach() {
	if d.Conn != nil {
		d.Conn.Close()
		d.Conn = nil
	}
}

func (d *AttachedDevice) NextSeq() uint32 {
	return atomic.AddUint32(&d.seqNum, 1)
}

// Client connects to a USB/IP server.
type Client struct {
	addr    string
	timeout time.Duration
}

func NewClient(host string, port int) *Client {
	return &Client{
		addr:    fmt.Sprintf("%s:%d", host, port),
		timeout: 4 * time.Second,
	}
}

// NewClientFast creates a client with a short timeout; useful for network scanning.
func NewClientFast(host string, port int) *Client {
	return &Client{
		addr:    fmt.Sprintf("%s:%d", host, port),
		timeout: 600 * time.Millisecond,
	}
}

// ListDevices queries OP_REQ_DEVLIST and returns the device list.
func (c *Client) ListDevices() ([]Device, error) {
	conn, err := net.DialTimeout("tcp", c.addr, c.timeout)
	if err != nil {
		return nil, fmt.Errorf("connect: %w", err)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(c.timeout))

	// OP_REQ_DEVLIST: version(2) + code(2) + status(4)
	req := [8]byte{}
	binary.BigEndian.PutUint16(req[0:], protocolVersion)
	binary.BigEndian.PutUint16(req[2:], opReqDevlist)
	if _, err := conn.Write(req[:]); err != nil {
		return nil, fmt.Errorf("send devlist req: %w", err)
	}

	// OP_REP_DEVLIST header: version(2) + code(2) + status(4) + ndev(4)
	var hdr [12]byte
	if _, err := io.ReadFull(conn, hdr[:]); err != nil {
		return nil, fmt.Errorf("read devlist header: %w", err)
	}
	if status := binary.BigEndian.Uint32(hdr[4:]); status != 0 {
		return nil, fmt.Errorf("server error status %d", status)
	}
	ndev := binary.BigEndian.Uint32(hdr[8:])

	devices := make([]Device, 0, ndev)
	for i := uint32(0); i < ndev; i++ {
		var raw [312]byte
		if _, err := io.ReadFull(conn, raw[:]); err != nil {
			return nil, fmt.Errorf("read device %d: %w", i, err)
		}
		var dev Device
		parseDeviceInfo(&dev.Info, raw[:])
		dev.Interfaces = make([]InterfaceInfo, dev.Info.NumIfaces)
		for j := range dev.Interfaces {
			var ib [4]byte
			if _, err := io.ReadFull(conn, ib[:]); err != nil {
				return nil, fmt.Errorf("read iface %d/%d: %w", i, j, err)
			}
			dev.Interfaces[j] = InterfaceInfo{ib[0], ib[1], ib[2], ib[3]}
		}
		devices = append(devices, dev)
	}
	return devices, nil
}

// Attach sends OP_REQ_IMPORT and returns the attached device with an open TCP connection.
// The caller must call dev.Detach() when done.
func (c *Client) Attach(busID string, tcpPort uint16) (*AttachedDevice, error) {
	conn, err := net.DialTimeout("tcp", c.addr, c.timeout)
	if err != nil {
		return nil, fmt.Errorf("connect: %w", err)
	}
	conn.SetDeadline(time.Now().Add(c.timeout))

	// OP_REQ_IMPORT: version(2) + code(2) + status(4) + busid[32]
	var req [8 + 32]byte
	binary.BigEndian.PutUint16(req[0:], protocolVersion)
	binary.BigEndian.PutUint16(req[2:], opReqImport)
	copy(req[8:], busID)
	if _, err := conn.Write(req[:]); err != nil {
		conn.Close()
		return nil, fmt.Errorf("send import req: %w", err)
	}

	// OP_REP_IMPORT header: version(2) + code(2) + status(4)
	var hdr [8]byte
	if _, err := io.ReadFull(conn, hdr[:]); err != nil {
		conn.Close()
		return nil, fmt.Errorf("read import header: %w", err)
	}
	if status := binary.BigEndian.Uint32(hdr[4:]); status != 0 {
		conn.Close()
		return nil, fmt.Errorf("server rejected import: status %d", status)
	}

	var devBuf [312]byte
	if _, err := io.ReadFull(conn, devBuf[:]); err != nil {
		conn.Close()
		return nil, fmt.Errorf("read device info: %w", err)
	}

	// Remove deadline — TCP connection stays open for URB traffic.
	conn.SetDeadline(time.Time{})

	host, _, _ := net.SplitHostPort(c.addr)
	dev := &AttachedDevice{
		Conn:    conn,
		Host:    host,
		BusID:   busID,
		TCPPort: tcpPort,
	}
	parseDeviceInfo(&dev.DevInfo, devBuf[:])
	return dev, nil
}

func parseDeviceInfo(info *DeviceInfo, buf []byte) {
	copy(info.Path[:], buf[0:256])
	copy(info.BusID[:], buf[256:288])
	info.BusNum = binary.BigEndian.Uint32(buf[288:])
	info.DevNum = binary.BigEndian.Uint32(buf[292:])
	info.Speed = binary.BigEndian.Uint32(buf[296:])
	info.IDVendor = binary.BigEndian.Uint16(buf[300:])
	info.IDProduct = binary.BigEndian.Uint16(buf[302:])
	info.BCDDevice = binary.BigEndian.Uint16(buf[304:])
	info.DevClass = buf[306]
	info.DevSubClass = buf[307]
	info.DevProtocol = buf[308]
	info.ConfigVal = buf[309]
	info.NumConfigs = buf[310]
	info.NumIfaces = buf[311]
}

// BusIDString returns the busID as a Go string (strips trailing nulls).
func BusIDString(info DeviceInfo) string {
	for i, c := range info.BusID {
		if c == 0 {
			return string(info.BusID[:i])
		}
	}
	return string(info.BusID[:])
}

// PathString returns the device path as a Go string.
func PathString(info DeviceInfo) string {
	for i, c := range info.Path {
		if c == 0 {
			return string(info.Path[:i])
		}
	}
	return string(info.Path[:])
}

// SpeedString returns a human-readable USB speed label.
func SpeedString(speed uint32) string {
	switch speed {
	case 1:
		return "Low"
	case 2:
		return "Full"
	case 3:
		return "High"
	case 4:
		return "Wireless"
	case 5:
		return "Super"
	case 6:
		return "Super+"
	default:
		return "Unknown"
	}
}
