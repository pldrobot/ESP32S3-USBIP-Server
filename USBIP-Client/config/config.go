package config

import (
	"encoding/json"
	"os"
	"path/filepath"
)

type Connection struct {
	Name        string `json:"name"`
	Host        string `json:"host"`
	Port        int    `json:"port"`
	BusID       string `json:"bus_id"`
	AutoConnect bool   `json:"auto_connect"`
}

type Config struct {
	Connections []Connection `json:"connections"`
}

func Load() (*Config, error) {
	data, err := os.ReadFile(configPath())
	if err != nil {
		if os.IsNotExist(err) {
			return &Config{}, nil
		}
		return nil, err
	}
	var cfg Config
	return &cfg, json.Unmarshal(data, &cfg)
}

func (c *Config) Save() error {
	p := configPath()
	if err := os.MkdirAll(filepath.Dir(p), 0755); err != nil {
		return err
	}
	data, err := json.MarshalIndent(c, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(p, data, 0644)
}

func (c *Config) Remove(name string) {
	out := c.Connections[:0]
	for _, conn := range c.Connections {
		if conn.Name != name {
			out = append(out, conn)
		}
	}
	c.Connections = out
}

func (c *Config) Find(name string) *Connection {
	for i := range c.Connections {
		if c.Connections[i].Name == name {
			return &c.Connections[i]
		}
	}
	return nil
}

// configPath returns the config file location.
// Portable mode: next to the executable (if config.json already exists there).
// Default: OS user config dir / usbip-client / config.json.
func configPath() string {
	if exe, err := os.Executable(); err == nil {
		p := filepath.Join(filepath.Dir(exe), "config.json")
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	dir, err := os.UserConfigDir()
	if err != nil {
		dir = "."
	}
	return filepath.Join(dir, "usbip-client", "config.json")
}
