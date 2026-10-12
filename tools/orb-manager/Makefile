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

.PHONY: all build manager orbflash test vet fmt run desktop flash windows win-test crosscheck clean

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

## run: start the hub on 127.0.0.1:3779 pointing at this clone's build dir.
## Opens the app window by default; pass extra flags with ARGS, e.g.
##   make run ARGS=--no-window        (headless: print the URL, launch nothing)
##   make run ARGS="--browser=/usr/bin/firefox"   (an explicit Chromium-family path)
run: manager
	$(BIN_DIR)/orb-manager -addr 127.0.0.1:3779 -build $(BUILD_DIR) $(ARGS)

## desktop: alias for run, spelled for the app-window use case.
desktop: run

## flash: headless full flash of an explicit port from this clone's build
##   make flash PORT=/dev/ttyACM0   (or PORT=/dev/serial/by-id/...)
flash: orbflash
	@test -n "$(PORT)" || { echo "usage: make flash PORT=/dev/ttyACM0"; exit 2; }
	$(BIN_DIR)/orbflash -port $(PORT) -mode full -build $(BUILD_DIR)

## windows: cross-compile the Windows binaries from this Linux/WSL host.
## Nothing here needs a Windows machine or a Windows toolchain: the hub is
## CGO_ENABLED=0, so the Go compiler just emits PE files. Copy the result to the
## Windows side to run it there (see README, "Testing on Windows").
WIN_DIR   ?= $(BIN_DIR)/windows-amd64
windows:
	@mkdir -p $(WIN_DIR)
	CGO_ENABLED=0 GOOS=windows GOARCH=amd64 $(GO) build -trimpath -ldflags '$(LDFLAGS)' -o $(WIN_DIR)/orb-manager.exe .
	CGO_ENABLED=0 GOOS=windows GOARCH=amd64 $(GO) build -trimpath -ldflags '$(LDFLAGS)' -o $(WIN_DIR)/orbflash.exe ./cmd/orbflash
	@echo "built $(WIN_DIR)/orb-manager.exe and orbflash.exe $(VERSION)"

## crosscheck: prove every supported platform still compiles, sources AND tests.
## This exists because the port lock reached syscall.Flock, which has no Windows
## equivalent, and nothing but a native run ever noticed: a GOOS=windows build
## had simply never been attempted. vet type-checks _test.go files too, so the
## test suite is covered as well. Override the set with PLATFORMS="linux/amd64".
PLATFORMS ?= linux/amd64 windows/amd64 darwin/arm64
crosscheck:
	@for p in $(PLATFORMS); do \
	  os=$${p%/*}; arch=$${p#*/}; \
	  printf 'GOOS=%-8s GOARCH=%-7s' $$os $$arch; \
	  if CGO_ENABLED=0 GOOS=$$os GOARCH=$$arch $(GO) vet ./... >/tmp/crosscheck.log 2>&1; then \
	    echo ' ok'; \
	  else \
	    echo ' FAIL'; cat /tmp/crosscheck.log; exit 1; \
	  fi; \
	done
	@echo 'all platforms compile'

## win-test: run the Windows test suite from this Linux/WSL host.
## WSL executes .exe files natively, so the Windows-only code — the exclusive
## port lock, the %LOCALAPPDATA% profile path, install-root browser discovery —
## is exercised for real instead of being cross-compiled and hoped for. The
## binaries run from this package dir on purpose: the Theme Editor tests locate
## the clone relative to the working directory and skip when it is not the one
## they were built from. Requires WSL interop; on a plain Linux host use
## make crosscheck, which proves the same code compiles without running it.
WIN_TEST_DIR ?= $(BIN_DIR)/windows-tests
win-test:
	@mkdir -p $(WIN_TEST_DIR)
	CGO_ENABLED=0 GOOS=windows GOARCH=amd64 $(GO) test -c -o $(WIN_TEST_DIR)/orbbind.test.exe ./internal/orbbind/
	CGO_ENABLED=0 GOOS=windows GOARCH=amd64 $(GO) test -c -o $(WIN_TEST_DIR)/main.test.exe .
	@echo '--- orbbind (Windows port lock) ---'
	$(WIN_TEST_DIR)/orbbind.test.exe $(TEST_ARGS)
	@echo '--- orb-manager (Windows window, profile, API) ---'
	$(WIN_TEST_DIR)/main.test.exe $(TEST_ARGS)

clean:
	rm -rf $(BIN_DIR)
