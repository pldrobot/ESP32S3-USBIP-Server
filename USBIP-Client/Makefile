# usbip-client Makefile
# Requires Go + a C compiler (MinGW/GCC for fyne)

BINARY_WIN = usbip-client.exe
BINARY_LIN = usbip-client

LDFLAGS = -ldflags "-s -w"
WIN_LDFLAGS = -ldflags "-H windowsgui -s -w"

.PHONY: all windows linux tidy clean

all: windows linux

windows:
	GOOS=windows GOARCH=amd64 CGO_ENABLED=1 CC=x86_64-w64-mingw32-gcc \
	  go build $(WIN_LDFLAGS) -o $(BINARY_WIN) .

linux:
	GOOS=linux GOARCH=amd64 CGO_ENABLED=1 \
	  go build $(LDFLAGS) -o $(BINARY_LIN) .

# Build for the current platform (dev use)
dev:
	go build $(LDFLAGS) -o usbip-client .

tidy:
	go mod tidy

clean:
	rm -f $(BINARY_WIN) $(BINARY_LIN) usbip-client

# Cross-compile Windows on Linux with mingw-w64:
#   sudo apt install gcc-mingw-w64-x86-64
#   make windows
