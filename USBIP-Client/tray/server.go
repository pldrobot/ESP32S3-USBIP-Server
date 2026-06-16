package tray

import (
	"encoding/json"
	"fmt"
	"io/fs"
	"net/http"
	"time"

	"github.com/pldrobot/usbip-client/config"
	"github.com/pldrobot/usbip-client/discovery"
	"github.com/pldrobot/usbip-client/usbip"
)

// routes wires up all HTTP handlers.
func (m *Manager) routes() http.Handler {
	mux := http.NewServeMux()

	// Static files (embedded HTML/CSS/JS)
	staticContent, _ := fs.Sub(staticFiles, "static")
	mux.Handle("/", http.FileServer(http.FS(staticContent)))

	// API
	mux.HandleFunc("/api/state", m.handleState)
	mux.HandleFunc("/api/connect", m.handleConnect)
	mux.HandleFunc("/api/disconnect", m.handleDisconnect)
	mux.HandleFunc("/api/add", m.handleAdd)
	mux.HandleFunc("/api/remove", m.handleRemove)
	mux.HandleFunc("/api/scan", m.handleScan)
	mux.HandleFunc("/api/scan-host", m.handleScanHost)

	// SSE real-time updates
	mux.HandleFunc("/events", m.handleSSE)

	return mux
}

// ── JSON state ─────────────────────────────────────────────────────────────

type appState struct {
	DriverInstalled bool        `json:"driver_installed"`
	Connections     []connState `json:"connections"`
}

type connState struct {
	Name      string `json:"name"`
	Host      string `json:"host"`
	Port      int    `json:"port"`
	BusID     string `json:"bus_id"`
	Connected bool   `json:"connected"`
	Error     string `json:"error,omitempty"`
}

func (m *Manager) buildState() appState {
	m.mu.RLock()
	defer m.mu.RUnlock()

	st := appState{DriverInstalled: m.driver.IsInstalled()}
	for _, c := range m.cfg.Connections {
		cs := connState{
			Name:  c.Name,
			Host:  c.Host,
			Port:  c.Port,
			BusID: c.BusID,
		}
		if sess, ok := m.sessions[c.Name]; ok {
			if sess.err != "" {
				cs.Error = sess.err
			} else {
				cs.Connected = true
			}
		}
		st.Connections = append(st.Connections, cs)
	}
	return st
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, code int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

// ── handlers ───────────────────────────────────────────────────────────────

func (m *Manager) handleState(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, m.buildState())
}

func (m *Manager) handleConnect(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "POST required")
		return
	}
	name := r.URL.Query().Get("name")
	m.mu.RLock()
	c := m.cfg.Find(name)
	m.mu.RUnlock()
	if c == nil {
		writeErr(w, http.StatusNotFound, "connection not found")
		return
	}
	go m.connect(c) //nolint:errcheck
	writeJSON(w, map[string]string{"status": "connecting"})
}

func (m *Manager) handleDisconnect(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "POST required")
		return
	}
	name := r.URL.Query().Get("name")
	go m.disconnect(name)
	writeJSON(w, map[string]string{"status": "disconnecting"})
}

func (m *Manager) handleAdd(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "POST required")
		return
	}
	var c config.Connection
	if err := json.NewDecoder(r.Body).Decode(&c); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	if c.Host == "" || c.BusID == "" {
		writeErr(w, http.StatusBadRequest, "host and bus_id are required")
		return
	}
	if c.Port == 0 {
		c.Port = 3240
	}
	if c.Name == "" {
		c.Name = fmt.Sprintf("%s [%s]", c.Host, c.BusID)
	}
	if err := m.addConnection(c); err != nil {
		writeErr(w, http.StatusConflict, err.Error())
		return
	}
	writeJSON(w, map[string]string{"status": "added", "name": c.Name})
}

func (m *Manager) handleRemove(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "POST required")
		return
	}
	name := r.URL.Query().Get("name")
	m.removeConnection(name)
	writeJSON(w, map[string]string{"status": "removed"})
}

// handleScan runs a subnet scan in the background and pushes results via SSE.
func (m *Manager) handleScan(w http.ResponseWriter, r *http.Request) {
	go func() {
		m.scanner.ScanSubnet()
		m.notify()
	}()
	writeJSON(w, map[string]string{"status": "scanning"})
}

type scanResult struct {
	Host    string        `json:"host"`
	Port    int           `json:"port"`
	Devices []scanDevice  `json:"devices"`
}

type scanDevice struct {
	BusID    string `json:"bus_id"`
	VendorID uint16 `json:"vendor_id"`
	ProductID uint16 `json:"product_id"`
	Speed    string `json:"speed"`
}

// handleScanHost scans a specific host immediately and returns results.
func (m *Manager) handleScanHost(w http.ResponseWriter, r *http.Request) {
	host := r.URL.Query().Get("host")
	portStr := r.URL.Query().Get("port")
	if host == "" {
		writeErr(w, http.StatusBadRequest, "host required")
		return
	}
	port := 3240
	fmt.Sscanf(portStr, "%d", &port)

	srv, err := discovery.ScanHost(host, port)
	if err != nil {
		writeErr(w, http.StatusBadGateway, err.Error())
		return
	}
	result := scanResult{Host: srv.Host, Port: srv.Port}
	for _, d := range srv.Devices {
		result.Devices = append(result.Devices, scanDevice{
			BusID:     usbip.BusIDString(d.Info),
			VendorID:  d.Info.IDVendor,
			ProductID: d.Info.IDProduct,
			Speed:     usbip.SpeedString(d.Info.Speed),
		})
	}
	writeJSON(w, result)
}

// handleSSE streams state updates as Server-Sent Events.
func (m *Manager) handleSSE(w http.ResponseWriter, r *http.Request) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming not supported", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	ch := m.subscribe()
	defer m.unsubscribe(ch)

	sendState := func() {
		data, _ := json.Marshal(m.buildState())
		fmt.Fprintf(w, "data: %s\n\n", data)
		flusher.Flush()
	}

	sendState() // initial push

	ticker := time.NewTicker(25 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-r.Context().Done():
			return
		case <-ch:
			sendState()
		case <-ticker.C:
			// Keep-alive comment to prevent proxy timeouts
			fmt.Fprintf(w, ": ping\n\n")
			flusher.Flush()
		}
	}
}

// discoveredState is pushed over SSE when scanner finds new servers.
func (m *Manager) buildDiscoveredState() []scanResult {
	servers := m.scanner.Snapshot()
	out := make([]scanResult, 0, len(servers))
	for _, srv := range servers {
		r := scanResult{Host: srv.Host, Port: srv.Port}
		for _, d := range srv.Devices {
			r.Devices = append(r.Devices, scanDevice{
				BusID:     usbip.BusIDString(d.Info),
				VendorID:  d.Info.IDVendor,
				ProductID: d.Info.IDProduct,
				Speed:     usbip.SpeedString(d.Info.Speed),
			})
		}
		out = append(out, r)
	}
	return out
}
