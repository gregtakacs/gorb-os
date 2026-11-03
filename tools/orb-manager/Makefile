# orb-manager — build the hub binary, the headless flasher, run checks.
#
# Nothing here needs esptool, Python, or a stub loader, nor an ESP-IDF build dir:
# the hub ships its firmware as a committed bundle (make firmware) and the OS theme
# runtime every app's editor builds with (web/runtime/: the dc-runtime, the theme
# builder, and the vendored React/Babel/fonts), and both are compiled in. The web UI
# is embedded at compile time (main.go //go:embed web), so editing web/ files and
# rebuilding is all a "web build" means here.
#
# An app's editor itself is not the hub's: it travels in the app's .orbapp (the
# clock's sources are apps/clock/editor/), and the hub serves it out of the
# container at /editor/<ip>/<app>/ against /runtime/<id>/ (D43 §9).

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

.PHONY: all build manager orbflash library test vet fmt check run desktop cdp reload flash windows win-test crosscheck clean firmware vendor-runtime vendor-check release dist winres usb-doc-test

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

## library: mirror the two shelf kinds beside one built executable. The hub's own
## resolution ladder names this rung "beside the executable" (library.go:libLadder), and
## it is the rung a downloaded hub lands on: run the binary where no gorb-os clone is
## reachable and every other rung answers nothing, so /api/library reports
## appsSource "none found" and the Apps and Themes faces draw an empty shelf. `release`
## has always copied both dirs for exactly this reason; the dev builds did not, which is
## why a `make manager` binary saw an empty library while the same tree's clone did not.
##
## Merged with `cp -R src/. dst/` rather than wiped, so a rebuild refreshes every shipped
## file and an app the operator imported beside the binary survives it. The source of truth
## stays the tracked apps/ and themes/ in this package: what lands beside the binary is a
## build output, and `clean` removes it.
define LIB_COPY
	@mkdir -p $(1)/apps $(1)/themes
	@cp -R $(CURDIR)/apps/. $(1)/apps/
	@cp -R $(CURDIR)/themes/. $(1)/themes/
	@echo "library: apps/ and themes/ mirrored beside $(1)"
endef

library:
	$(call LIB_COPY,$(BIN_DIR))

manager:
	@mkdir -p $(BIN_DIR)
	CGO_ENABLED=0 $(GO) build -trimpath -ldflags '$(LDFLAGS)' -o $(BIN_DIR)/orb-manager .
	@echo "built $(BIN_DIR)/orb-manager $(VERSION)"
	$(call LIB_COPY,$(BIN_DIR))

## vendor-runtime: download the runtime's third-party dependencies into
## web/runtime/vendor/ so an app's editor works with no internet at all. support.js
## pulls React, ReactDOM and Babel from unpkg.com, and the clock editor's page links 36
## font families from Google; the hub serves the vendored copies at
## /runtime/react@18.3.1/, /runtime/babel@7.29.0/ and /runtime/fonts@1/. Committed for
## the same reason as the firmware bundle: a plain build must produce an
## offline-capable hub. Idempotent — a re-run whose bytes already match produces no
## diff. Requires network access.
VENDOR_DIR := web/runtime/vendor
EDITOR_SRC := $(REPO)/apps/clock/editor
SUPPORT_JS := web/runtime/support@1/support.js
vendor-runtime:
	@test -f "$(EDITOR_SRC)/Watch Composer.dc.html" || { echo "no $(EDITOR_SRC) — run this from the gorb-os clone"; exit 1; }
	$(GO) run ./cmd/orbvendor -out $(VENDOR_DIR) -editor "$(EDITOR_SRC)" -support $(SUPPORT_JS)

## vendor-check: verify the committed vendor tree, fetching nothing. It re-checks every
## file against the sha256 in its own MANIFEST.json and against the SRI the runtime
## demands at load time, and re-reads those pins out of support.js rather than trusting
## the tool's copy — so a hand-edited vendored React fails here, not as a blank editor.
vendor-check:
	@test -f "$(VENDOR_DIR)/MANIFEST.json" || { echo "no $(VENDOR_DIR)/MANIFEST.json — run make vendor-runtime"; exit 1; }
	$(GO) run ./cmd/orbvendor -out $(VENDOR_DIR) -verify

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
## armed, vet, every supported GOOS/GOARCH type-checked including tests, and the
## vendored runtime assets verified against their manifest and SRI pins. Run this
## before every commit that touches the hub.
check: test vet crosscheck vendor-check
	@echo 'check: tests, vet, crosscheck and vendor-check all green'

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

## cdp: evaluate JS inside the live app window, as the page. The control token is an
## HttpOnly cookie inlined into the served HTML, so no curl can carry it and a screenshot
## cannot prove a handler ran — this does. Pass EXPR, optionally HUB_PORT (default: the
## single hub listening on loopback).
##   make cdp EXPR="document.title"
##   make cdp EXPR="ORB_TOKEN" HUB_PORT=44671
cdp:
	@test -n "$(EXPR)" || { echo "usage: make cdp EXPR='<js>' [HUB_PORT=3779]"; exit 2; }
	@python3 scripts/orb_cdp.py "$(EXPR)" $(HUB_PORT)

## reload: hard-reload the app window, bypassing the cache — the way to pick up a web/
## edit after `make manager` without touching the mouse (Ctrl+Shift+R in the window does
## the same thing by hand).
reload:
	@python3 scripts/orb_cdp.py reload $(HUB_PORT)

## flash: headless full flash of an explicit port, using the firmware compiled
## into the binary — the same bytes the hub flashes.
##   make flash PORT=/dev/ttyACM0   (or PORT=/dev/serial/by-id/...)
##   make flash PORT=... FW=/path/to/custom.zip   (one specific bundle)
flash: orbflash
	@test -n "$(PORT)" || { echo "usage: make flash PORT=/dev/ttyACM0"; exit 2; }
	$(BIN_DIR)/orbflash -port $(PORT) -mode full $(if $(FW),-firmware $(FW),)

## release: cross-compile the full artifact set for distribution, into
## bin/release/<name>-<os>-<arch>/, plus a sha256sums.txt covering every file.
## CGO_ENABLED=0 throughout, so this needs no Mac and no Windows machine — the Go
## compiler just emits the right binary format. Run it from the package dir like the
## other targets, and never ship a build that did not come from a clean tree: VERSION
## carries `git describe --dirty`, so an uncommitted change is visible in the artifact
## name the hub itself reports at /api/status.
## The Windows hub ships -H windowsgui (PE subsystem 2): a double-click opens the
## app window and nothing else, with the log redirected per log_gui_windows.go;
## The release is the hub alone: orbflash is a bench tool (make orbflash) and does not ship.
## The flag rides in a make variable chosen per platform, never in a shell
## variable inside the quoted arg list: the go tool parses $ as command
## substitution inside -ldflags, so '$$guilf' there silently ate the flags.
## Consumes nothing from the network: the firmware bundle and the OS theme runtime
## (its vendored assets included) are all committed and compiled in.
LDFLAGS_GUI   := $(LDFLAGS) -H windowsgui
RELEASE_DIR       ?= $(BIN_DIR)/release
RELEASE_NAME      ?= orb-manager
RELEASE_PLATFORMS ?= darwin/amd64 darwin/arm64 linux/amd64 windows/amd64
release:
	@command -v sha256sum >/dev/null || { echo 'sha256sum is required to sign the release set'; exit 1; }
	@rm -rf $(RELEASE_DIR)
	@for p in $(RELEASE_PLATFORMS); do \
	  os=$${p%/*}; arch=$${p#*/}; \
	  sfx=; ld="$(LDFLAGS)"; [ $$os = windows ] && { sfx=.exe; ld="$(LDFLAGS_GUI)"; }; \
	  d=$(RELEASE_DIR)/$(RELEASE_NAME)-$$os-$$arch; \
	  mkdir -p $$d; \
	  printf 'GOOS=%-8s GOARCH=%-7s' $$os $$arch; \
	  if CGO_ENABLED=0 GOOS=$$os GOARCH=$$arch $(GO) build -trimpath -ldflags "$$ld" -o $$d/$(RELEASE_NAME)$$sfx .; then \
	    echo ' ok'; \
	    cp -R $(CURDIR)/apps $(CURDIR)/themes $$d/; \
	  else \
	    echo ' FAIL'; rm -rf $(RELEASE_DIR); exit 1; \
	  fi; \
	done
	@# Paths are recorded relative to the release dir and sorted, so `sha256sum -c
	@# sha256sums.txt` passes from inside it and a re-run diffs cleanly.
	@cd $(RELEASE_DIR) && find . -type f ! -name sha256sums.txt | sed 's|^\./||' | sort | xargs sha256sum > sha256sums.txt
	@echo "release $(VERSION):"
	@cat $(RELEASE_DIR)/sha256sums.txt

## dist: the downloadable archives, from a fresh `make release`, into bin/dist/:
## orb-manager-<tag>-linux-amd64.tar.gz and -windows-amd64.zip anywhere, plus
## -macos.dmg ("Orb Manager.app", universal) when run on a Mac, which has the lipo and
## hdiutil the bundle needs. Nothing here signs: CI signs the staged Windows exes and
## Mac app between packaging/dist.sh's stage and pack steps (.github/workflows/release.yml,
## RELEASING.md). A local dist is an unsigned build for trying the packaging out.
DIST_DIR ?= $(BIN_DIR)/dist
DIST_PLATFORMS ?= linux windows $(if $(filter Darwin,$(shell uname -s)),macos,)
dist: release
	@rm -rf $(DIST_DIR)
	@for p in $(DIST_PLATFORMS); do \
	  packaging/dist.sh stage $$p $(RELEASE_DIR) $(VERSION) $(DIST_DIR) && \
	  packaging/dist.sh pack $$p $(VERSION) $(DIST_DIR) || exit 1; \
	done

## winres: the Windows exe's icon and version resource, as rsrc_windows_amd64.syso in this
## dir, which the next GOOS=windows build of the hub links in. Fetches go-winres, so it is
## a separate, opt-in step: CI runs it before `make release`; a plain build skips it and
## ships an exe with the default icon. WINVER sets the version (x.y.z).
WINRES ?= github.com/tc-hib/go-winres@v0.3.3
winres:
	$(GO) run $(WINRES) simply --arch amd64 --manifest none \
	  --icon packaging/windows/orb-manager.ico --product-name "Orb Manager" \
	  --file-description "Orb Manager" --copyright "Copyright (c) Greg Takacs" \
	  --original-filename orb-manager.exe \
	  $(if $(WINVER),--file-version $(WINVER).0 --product-version $(WINVER).0,)

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
	$(call LIB_COPY,$(WIN_DIR))

## windows-gui: the one-window hub, exactly what `release` ships for Windows.
## -H windowsgui makes the exe PE subsystem 2, so a double-click shows only the
## app window and no console; the log redirects to %TEMP%\orb-manager.log when
## no console is attached (log_gui_windows.go). Run from cmd.exe to watch it
## print like the console build — the flag suppresses the allocated window,
## not an inherited console. `make windows` stays console on purpose: it is
## what win-test and the bench flows execute.
WIN_GUI_DIR ?= $(BIN_DIR)/windows-gui
windows-gui:
	@mkdir -p $(WIN_GUI_DIR)
	CGO_ENABLED=0 GOOS=windows GOARCH=amd64 $(GO) build -trimpath -ldflags '$(LDFLAGS_GUI)' -o $(WIN_GUI_DIR)/orb-manager.exe .
	@echo "built $(WIN_GUI_DIR)/orb-manager.exe $(VERSION) (GUI subsystem: window only, no console)"
	$(call LIB_COPY,$(WIN_GUI_DIR))

## crosscheck: prove every supported platform still compiles, sources AND tests.
## This exists because the port lock reached syscall.Flock, which has no Windows
## equivalent, and nothing but a native run ever noticed: a GOOS=windows build
## had simply never been attempted. vet type-checks _test.go files too, so the
## test suite is covered as well. The set is the release set on purpose: an
## artifact nobody ever type-checked is an artifact nobody should ship, and
## darwin/amd64 was exactly that gap once. Override with PLATFORMS="linux/amd64".
PLATFORMS ?= $(RELEASE_PLATFORMS)
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

## usb-doc-test: the pins on docs/wsl-usb-reattach.md and scripts/wsl-usb.sh. The
## runbook records behaviour measured once, by hand, so it drifts: a path, a verb or an
## exit code can go stale and nothing else here would notice. These are static checks, so
## they pass with no Windows, no cable and no board. Deliberately not part of test or
## check — those run on a Go toolchain alone, and win-test runs on Windows where python3
## is not guaranteed.
usb-doc-test:
	python3 -m unittest discover -s scripts -p '*_test.py'

clean:
	rm -rf $(BIN_DIR) rsrc_windows_*.syso
