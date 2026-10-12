# orb-manager — build the hub binary, the headless flasher, run checks.
#
# Nothing here needs esptool, Python, or a stub loader. The web UI is embedded
# at compile time (see main.go //go:embed web), so there is no separate bundler
# step: editing web/ files and rebuilding is all a "web build" means here. The
# Theme Editor (the watch-composer) is served straight from the repo, so it is
# never copied into the binary and stays one source of truth.

# Find the toolchain. A tarball install into /usr/local/go does not land on a
# default login PATH, so probe PATH first, then the known install locations.
# Override if Go lives elsewhere: make GO=/opt/go/bin/go
GO := $(shell command -v go 2>/dev/null || ls /usr/local/go/bin/go /snap/bin/go $$HOME/go/bin/go 2>/dev/null | head -1)
ifeq ($(GO),)
$(error no go toolchain found on PATH, /usr/local/go/bin, or ~/go/bin — install Go or pass GO=/path/to/go)
endif
BIN_DIR   ?= bin
REPO      ?= $(shell git rev-parse --show-toplevel 2>/dev/null)
BUILD_DIR ?= $(REPO)/build
MODULE    := github.com/gregtakacs/gorb-os/tools/orb-manager

# Tag the binary with the tree's git describe, the same stamp serflash.sh and
# the on-device app_descriptor use, so an orb-manager you picked up off disk
# tells you exactly which tree it was built from.
VERSION ?= $(shell git describe --always --dirty 2>/dev/null || echo dev)
LDFLAGS := -s -w -X main.version=$(VERSION)

.PHONY: all build manager orbflash test vet fmt run flash clean

all: test build

## build: compile both binaries into ./bin
build: manager orbflash

manager:
	@mkdir -p $(BIN_DIR)
	CGO_ENABLED=0 $(GO) build -trimpath -ldflags '$(LDFLAGS)' -o $(BIN_DIR)/orb-manager .
	@echo "built $(BIN_DIR)/orb-manager $(VERSION)"

orbflash:
	@mkdir -p $(BIN_DIR)
	CGO_ENABLED=0 $(GO) build -trimpath -ldflags '$(LDFLAGS)' -o $(BIN_DIR)/orbflash ./cmd/orbflash
	@echo "built $(BIN_DIR)/orbflash $(VERSION)"

## test: run the unit tests (SLIP framing, block checksum, SHA256 recalc, MAC decode, FLASH_BEGIN framing)
test:
	$(GO) test ./...

vet:
	$(GO) vet ./...

fmt:
	$(GO) fmt ./...

## run: start the hub on 127.0.0.1:3779 pointing at this clone's build dir
run: manager
	$(BIN_DIR)/orb-manager -addr 127.0.0.1:3779 -build $(BUILD_DIR)

## flash: headless full flash of an explicit port from this clone's build
##   make flash PORT=/dev/ttyACM0   (or PORT=/dev/serial/by-id/...)
flash: orbflash
	@test -n "$(PORT)" || { echo "usage: make flash PORT=/dev/ttyACM0"; exit 2; }
	$(BIN_DIR)/orbflash -port $(PORT) -mode full -build $(BUILD_DIR)

clean:
	rm -rf $(BIN_DIR)
