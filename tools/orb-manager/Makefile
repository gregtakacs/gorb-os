# orb-manager — build the hub binary, the headless flasher, run checks.
#
# Nothing here needs esptool, Python, or a stub loader, nor an ESP-IDF build dir:
# the hub ships its firmware as a committed bundle (make firmware) and its Theme
# Editor as committed copies (make composer), and both are compiled in. The web UI
# is embedded at compile time (main.go //go:embed web), so editing web/ files and
# rebuilding is all a "web build" means here.
#
# When a gorb-os clone IS reachable, the hub still serves the Theme Editor live
# from tools/watch-composer, so the composer stays one source of truth during
# development. The embedded copies are what an end user gets.

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

.PHONY: all build manager orbflash test vet fmt check run desktop flash windows win-test crosscheck clean firmware composer

all: test build

## build: compile both binaries into ./bin
build: manager orbflash

BUNDLE := internal/firmware/bundle/orb-firmware.zip
MANIFEST := flasher_args.json

## firmware: re-pack the embedded firmware bundle from this clone's build dir.
## The bundle is committed, so a plain `make build` needs no ESP-IDF: this target
## is the release step you run after `idf.py build` when the firmware changed.
## The test suite refuses a bundle that disagrees with $(BUILD_DIR) when
## ORB_BUILD_DIR is set, so a stale bundle cannot slip through unnoticed.
firmware:
	@test -f $(BUILD_DIR)/flasher_args.json || { echo 'no $(BUILD_DIR)/flasher_args.json — run idf.py build first'; exit 1; }
	$(GO) run ./cmd/orbpack -build $(BUILD_DIR) -out $(BUNDLE)
	ORB_BUILD_DIR=$(BUILD_DIR) $(GO) test ./internal/firmware/ -run BuiltInBundle
	@echo "embedded firmware refreshed from $(BUILD_DIR) into $(BUNDLE)"

manager:
	@mkdir -p $(BIN_DIR)
	CGO_ENABLED=0 $(GO) build -trimpath -ldflags '$(LDFLAGS)' -o $(BIN_DIR)/orb-manager .
	@echo "built $(BIN_DIR)/orb-manager $(VERSION)"

## composer: re-copy the Theme Editor into web/composer/ so the shipped binary can
## serve it. The hub prefers the live files under $(REPO)/tools/watch-composer when
## a clone is reachable — that is the developer's live-edit path — and falls back to
## this embedded copy otherwise, which is the only option for a downloaded app.
## Committed for the same reason as the firmware bundle: a plain build must work.
COMPOSER_SRC := $(REPO)/tools/watch-composer
COMPOSER_ENTRY := Watch Composer.dc.html
COMPOSER_JS := dial-renderer.js orbtheme-baker.js support.js watch-engine.js
composer:
	@test -f "$(COMPOSER_SRC)/$(COMPOSER_ENTRY)" || { echo "no $(COMPOSER_SRC) — run this from the gorb-os clone"; exit 1; }
	@mkdir -p web/composer
	@cp "$(COMPOSER_SRC)/$(COMPOSER_ENTRY)" "web/composer/$(COMPOSER_ENTRY)"
	@for f in $(COMPOSER_JS); do cp "$(COMPOSER_SRC)/$$f" web/composer/$$f || exit 1; done
	@echo "copied the Theme Editor (entry + $(words $(COMPOSER_JS)) scripts) into web/composer/"

orbflash:
	@mkdir -p $(BIN_DIR)
	CGO_ENABLED=0 $(GO) build -trimpath -ldflags '$(LDFLAGS)' -o $(BIN_DIR)/orbflash ./cmd/orbflash
	@echo "built $(BIN_DIR)/orbflash $(VERSION)"

## test: run the unit tests (SLIP framing, block checksum, SHA256 recalc, MAC decode, FLASH_BEGIN framing)
##
## The bundle staleness guard inside ./internal/firmware skips unless ORB_BUILD_DIR
## names a real build dir, so a committed bundle that disagrees with the tree passed
## the whole suite silently. Arm it here whenever this clone has a build, and skip it
## cleanly where there is none — an end user's machine, and CI on a source checkout.
test:
	@if [ -f "$(BUILD_DIR)/$(MANIFEST)" ]; then \
	  echo "go test ./... — bundle guard armed against $(BUILD_DIR)"; \
	  ORB_BUILD_DIR="$(BUILD_DIR)" $(GO) test -count=1 ./... ; \
	else \
	  echo "go test ./... — no $(BUILD_DIR)/$(MANIFEST), bundle guard skips"; \
	  $(GO) test ./... ; \
	fi

vet:
	$(GO) vet ./...

## check: the verification ladder in one command — the suite with the bundle guard
## armed, vet, and every supported GOOS/GOARCH type-checked including tests. Run this
## before every commit that touches the hub.
check: test vet crosscheck
	@echo 'check: tests, vet, and crosscheck all green'

fmt:
	$(GO) fmt ./...

## run: start the hub on 127.0.0.1:3779, flashing the built-in firmware.
## Opens the app window by default; pass extra flags with ARGS, e.g.
##   make run ARGS=--no-window        (headless: print the URL, launch nothing)
##   make run ARGS="--browser=/usr/bin/firefox"   (an explicit Chromium-family path)
run: manager
	$(BIN_DIR)/orb-manager -addr 127.0.0.1:3779 $(ARGS)

## desktop: alias for run, spelled for the app-window use case.
desktop: run

## flash: headless full flash of an explicit port, using the firmware compiled
## into the binary — the same bytes the hub flashes.
##   make flash PORT=/dev/ttyACM0   (or PORT=/dev/serial/by-id/...)
##   make flash PORT=... FW=/path/to/custom.zip   (one specific bundle)
flash: orbflash
	@test -n "$(PORT)" || { echo "usage: make flash PORT=/dev/ttyACM0"; exit 2; }
	$(BIN_DIR)/orbflash -port $(PORT) -mode full $(if $(FW),-firmware $(FW),)

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
	@rm -f $(WIN_TEST_DIR)/*.test.exe
	CGO_ENABLED=0 GOOS=windows GOARCH=amd64 $(GO) test -c -o $(WIN_TEST_DIR)/orbbind.test.exe ./internal/orbbind/
	CGO_ENABLED=0 GOOS=windows GOARCH=amd64 $(GO) test -c -o $(WIN_TEST_DIR)/esprom.test.exe ./internal/esprom/
	CGO_ENABLED=0 GOOS=windows GOARCH=amd64 $(GO) test -c -o $(WIN_TEST_DIR)/firmware.test.exe ./internal/firmware/
	CGO_ENABLED=0 GOOS=windows GOARCH=amd64 $(GO) test -c -o $(WIN_TEST_DIR)/main.test.exe .
	@echo '--- orbbind (Windows port lock) ---'
	$(WIN_TEST_DIR)/orbbind.test.exe $(TEST_ARGS)
	@echo '--- esprom (plan, partition table, pad4) ---'
	$(WIN_TEST_DIR)/esprom.test.exe $(TEST_ARGS)
	@echo '--- firmware (bundle load, the embedded zip Windows will flash) ---'
	$(WIN_TEST_DIR)/firmware.test.exe $(TEST_ARGS)
	@echo '--- orb-manager (Windows window, profile, API) ---'
	$(WIN_TEST_DIR)/main.test.exe $(TEST_ARGS)

clean:
	rm -rf $(BIN_DIR)
