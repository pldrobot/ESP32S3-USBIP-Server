// Package tray implements the USB/IP system tray application.
//
// On Windows this package is CGo-free: the system tray uses Win32 syscalls
// (getlantern/systray) and the management UI is a web page served on localhost.
// On Linux, getlantern/systray requires libgtk-3-dev (CGo).
//
// Management UI: opens automatically in the default browser on startup.
// Re-open any time via the tray menu → "Open Manager".
package tray

import (
	"context"
	"fmt"
	"net"
	"net/http"
	"os/exec"
	"runtime"
	"sync"

	"github.com/getlantern/systray"

	"github.com/pldrobot/usbip-client/config"
	"github.com/pldrobot/usbip-client/discovery"
	"github.com/pldrobot/usbip-client/usbip"
	"github.com/pldrobot/usbip-client/vhci"
)

type session struct {
	conn *config.Connection
	dev  *usbip.AttachedDevice
	port uint32
	err  string // last connect error, empty if connected OK
}

// Manager holds all shared application state.
type Manager struct {
	cfg     *config.Config
	driver  vhci.VHCI
	scanner *discovery.Scanner

	mu       sync.RWMutex
	sessions map[string]*session // keyed by Connection.Name

	httpAddr string
	srv      *http.Server

	// SSE subscriber channels
	subMu sync.Mutex
	subs  map[chan struct{}]struct{}
}

// Run starts the system tray and blocks until the user selects Quit.
func Run() {
	cfg, _ := config.Load()
	mgr := &Manager{
		cfg:      cfg,
		driver:   vhci.New(),
		scanner:  discovery.New(),
		sessions: make(map[string]*session),
		subs:     make(map[chan struct{}]struct{}),
	}

	// Start HTTP server on a random free port.
	ln, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		// Fallback to a fixed port if the OS can't assign one.
		ln, _ = net.Listen("tcp4", "127.0.0.1:18342")
	}
	mgr.httpAddr = ln.Addr().String()
	mgr.srv = &http.Server{Handler: mgr.routes()}
	go mgr.srv.Serve(ln)

	// UDP broadcast listener for ESP32/usbipd announcements.
	mgr.scanner.StartListener(func(_ []discovery.FoundServer) {
		mgr.notify()
	})

	// Auto-connect entries.
	for i := range cfg.Connections {
		if cfg.Connections[i].AutoConnect {
			c := &cfg.Connections[i]
			go mgr.connect(c) //nolint:errcheck
		}
	}

	systray.Run(mgr.onReady, mgr.onExit)
}

func (m *Manager) onReady() {
	systray.SetIcon(iconBytes())
	systray.SetTitle("USB/IP Client")
	systray.SetTooltip("USB/IP Client — click to manage USB devices")

	mOpen := systray.AddMenuItem("Open Manager", "Open device manager in browser")
	systray.AddSeparator()
	mQuit := systray.AddMenuItem("Quit", "Exit USB/IP Client")

	// Open management UI automatically on first launch.
	go openBrowser("http://" + m.httpAddr)

	for {
		select {
		case <-mOpen.ClickedCh:
			openBrowser("http://" + m.httpAddr)
		case <-mQuit.ClickedCh:
			m.srv.Shutdown(context.Background())
			m.scanner.Stop()
			systray.Quit()
			return
		}
	}
}

func (m *Manager) onExit() {
	m.scanner.Stop()
}

// ── session lifecycle ──────────────────────────────────────────────────────

func (m *Manager) connect(c *config.Connection) error {
	client := usbip.NewClient(c.Host, c.Port)
	dev, err := client.Attach(c.BusID, uint16(c.Port))
	if err != nil {
		m.setErr(c.Name, err.Error())
		return err
	}
	port, err := m.driver.PlugIn(dev)
	if err != nil {
		dev.Detach()
		m.setErr(c.Name, err.Error())
		return err
	}
	m.mu.Lock()
	m.sessions[c.Name] = &session{conn: c, dev: dev, port: port}
	m.mu.Unlock()
	m.notify()
	return nil
}

func (m *Manager) disconnect(name string) {
	m.mu.Lock()
	sess, ok := m.sessions[name]
	if ok {
		delete(m.sessions, name)
	}
	m.mu.Unlock()
	if !ok {
		return
	}
	if sess.dev != nil {
		sess.dev.Detach()
	}
	if sess.port > 0 {
		_ = m.driver.Unplug(sess.port)
	}
	m.notify()
}

func (m *Manager) setErr(name, msg string) {
	m.mu.Lock()
	m.sessions[name] = &session{err: msg}
	m.mu.Unlock()
	m.notify()
}

func (m *Manager) addConnection(c config.Connection) error {
	m.mu.Lock()
	for _, existing := range m.cfg.Connections {
		if existing.Name == c.Name {
			m.mu.Unlock()
			return fmt.Errorf("connection named %q already exists", c.Name)
		}
	}
	m.cfg.Connections = append(m.cfg.Connections, c)
	m.mu.Unlock()
	_ = m.cfg.Save()
	m.notify()
	return nil
}

func (m *Manager) removeConnection(name string) {
	m.disconnect(name)
	m.mu.Lock()
	m.cfg.Remove(name)
	m.mu.Unlock()
	_ = m.cfg.Save()
	m.notify()
}

// ── SSE fan-out ────────────────────────────────────────────────────────────

func (m *Manager) subscribe() chan struct{} {
	ch := make(chan struct{}, 1)
	m.subMu.Lock()
	m.subs[ch] = struct{}{}
	m.subMu.Unlock()
	return ch
}

func (m *Manager) unsubscribe(ch chan struct{}) {
	m.subMu.Lock()
	delete(m.subs, ch)
	m.subMu.Unlock()
}

func (m *Manager) notify() {
	m.subMu.Lock()
	for ch := range m.subs {
		select {
		case ch <- struct{}{}:
		default:
		}
	}
	m.subMu.Unlock()
}

// ── helpers ────────────────────────────────────────────────────────────────

func openBrowser(url string) {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", url)
	case "linux":
		cmd = exec.Command("xdg-open", url)
	case "darwin":
		cmd = exec.Command("open", url)
	default:
		return
	}
	_ = cmd.Start()
}
