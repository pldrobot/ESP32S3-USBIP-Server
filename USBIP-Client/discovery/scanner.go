// Package discovery finds USB/IP servers on the local network.
//
// Two methods are supported:
//  1. UDP broadcast listener on port 3241 — ESP32 / Linux usbipd broadcasts
//     a packet in the format "USBIP:v1:<host>:<port>\n" every few seconds.
//  2. TCP subnet scan — connects to port 3240 on every host in the /24
//     subnet and issues OP_REQ_DEVLIST with a 600 ms timeout.
package discovery

import (
	"fmt"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/pldrobot/usbip-client/usbip"
)

const (
	broadcastPort  = 3241
	defaultUSBIPPort = 3240
	subnetWorkers  = 64
)

// FoundServer is a USB/IP server that responded on the network.
type FoundServer struct {
	Host    string
	Port    int
	Devices []usbip.Device
}

// Scanner discovers servers passively (UDP) and actively (TCP scan).
type Scanner struct {
	mu       sync.Mutex
	servers  map[string]*FoundServer // keyed "host:port"
	stopCh   chan struct{}
	once     sync.Once
}

func New() *Scanner {
	return &Scanner{
		servers: make(map[string]*FoundServer),
		stopCh:  make(chan struct{}),
	}
}

// StartListener begins listening for UDP broadcast announcements from ESP32/usbipd.
// onUpdate is called on the UI goroutine-safe side whenever a new server is found.
func (s *Scanner) StartListener(onUpdate func([]FoundServer)) {
	s.once.Do(func() {
		go s.listenLoop(onUpdate)
	})
}

// Stop shuts down the background listener.
func (s *Scanner) Stop() {
	close(s.stopCh)
}

// ScanSubnet actively probes every .1–.254 address in each local /24 network.
// This is a blocking call that typically completes in 1–3 seconds.
func (s *Scanner) ScanSubnet() []FoundServer {
	targets := localSubnetHosts()

	type result struct{ srv *FoundServer }
	results := make(chan result, len(targets))
	sem := make(chan struct{}, subnetWorkers)
	var wg sync.WaitGroup

	for _, host := range targets {
		wg.Add(1)
		sem <- struct{}{}
		go func(h string) {
			defer wg.Done()
			defer func() { <-sem }()
			srv, err := ScanHost(h, defaultUSBIPPort)
			if err == nil {
				results <- result{srv}
			}
		}(host)
	}

	go func() { wg.Wait(); close(results) }()

	var found []FoundServer
	for r := range results {
		s.merge(r.srv)
		found = append(found, *r.srv)
	}
	return found
}

// ScanHost queries a specific server and returns its device list.
func ScanHost(host string, port int) (*FoundServer, error) {
	client := usbip.NewClientFast(host, port)
	devices, err := client.ListDevices()
	if err != nil {
		return nil, fmt.Errorf("%s:%d: %w", host, port, err)
	}
	return &FoundServer{Host: host, Port: port, Devices: devices}, nil
}

// Snapshot returns all currently discovered servers.
func (s *Scanner) Snapshot() []FoundServer {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]FoundServer, 0, len(s.servers))
	for _, v := range s.servers {
		out = append(out, *v)
	}
	return out
}

// listenLoop reads UDP broadcast packets and probes new senders.
func (s *Scanner) listenLoop(onUpdate func([]FoundServer)) {
	conn, err := net.ListenPacket("udp4", fmt.Sprintf("0.0.0.0:%d", broadcastPort))
	if err != nil {
		return
	}
	defer conn.Close()

	go func() {
		<-s.stopCh
		conn.Close()
	}()

	buf := make([]byte, 512)
	for {
		conn.SetReadDeadline(time.Now().Add(5 * time.Second))
		n, addr, err := conn.ReadFrom(buf)
		if err != nil {
			select {
			case <-s.stopCh:
				return
			default:
				continue
			}
		}

		msg := strings.TrimSpace(string(buf[:n]))
		host, port, err := parseBroadcast(msg, addr)
		if err != nil {
			continue
		}

		go func(h string, p int) {
			srv, err := ScanHost(h, p)
			if err != nil {
				return
			}
			s.merge(srv)
			if onUpdate != nil {
				onUpdate(s.Snapshot())
			}
		}(host, port)
	}
}

func (s *Scanner) merge(srv *FoundServer) {
	key := fmt.Sprintf("%s:%d", srv.Host, srv.Port)
	s.mu.Lock()
	s.servers[key] = srv
	s.mu.Unlock()
}

// parseBroadcast parses the ESP32/usbipd broadcast format:
//
//	"USBIP:v1:<host>:<port>"
//
// If the host field is empty or "0.0.0.0", falls back to the sender's address.
func parseBroadcast(msg string, sender net.Addr) (host string, port int, err error) {
	parts := strings.Split(msg, ":")
	if len(parts) < 4 || parts[0] != "USBIP" || parts[1] != "v1" {
		return "", 0, fmt.Errorf("not a USBIP broadcast")
	}
	host = parts[2]
	_, err = fmt.Sscanf(parts[3], "%d", &port)
	if err != nil {
		return "", 0, err
	}
	if host == "" || host == "0.0.0.0" {
		if udpAddr, ok := sender.(*net.UDPAddr); ok {
			host = udpAddr.IP.String()
		}
	}
	if port <= 0 {
		port = defaultUSBIPPort
	}
	return
}

// localSubnetHosts returns x.x.x.1–254 for each local IPv4 interface.
func localSubnetHosts() []string {
	ifaces, err := net.Interfaces()
	if err != nil {
		return nil
	}
	seen := make(map[string]bool)
	var hosts []string
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, _ := iface.Addrs()
		for _, addr := range addrs {
			ipNet, ok := addr.(*net.IPNet)
			if !ok {
				continue
			}
			ip4 := ipNet.IP.To4()
			if ip4 == nil {
				continue
			}
			// Scan the /24 of our IP to avoid probing huge networks.
			base := fmt.Sprintf("%d.%d.%d.", ip4[0], ip4[1], ip4[2])
			for i := 1; i <= 254; i++ {
				h := fmt.Sprintf("%s%d", base, i)
				if !seen[h] {
					seen[h] = true
					hosts = append(hosts, h)
				}
			}
		}
	}
	return hosts
}
