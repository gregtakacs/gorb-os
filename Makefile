# gorb-os — the one entry point for building and checking the tree.
#
# The firmware itself is an ESP-IDF project (idf.py drives CMakeLists.txt); this file only
# names the steps so a contributor, CI and a release all run the same commands. Every
# target runs from the repo root. See BUILDING.md for what each needs installed.
#
#   make check          every check that needs no board: host suites, apps, hub, editor
#   make firmware       idf.py build (needs ESP-IDF v5.4 sourced: . ~/esp/esp-idf/export.sh)
#   make apps           re-bake the shelf apps into tools/orb-manager/apps/
#   make hub            build tools/orb-manager/bin/orb-manager for this machine
#   make sdk            write build/orb-sdk-<version>.tar.gz, the standalone app SDK
#   make dist           the hub's release archives (unsigned), into tools/orb-manager/bin/dist/
#   make web            the release's web file, into tools/orb-manager/bin/web/ (design §6.1)
#   make check-web      open the shipped editors in headless Chromium: needs Go, Chromium, minutes
#   make check-pictures the studio kit's picture fitter in headless Chromium: needs Node, Chromium
#   make check-runner   the migration runner against a scratch editor: needs Chromium
#   make check-migrations  each app's schema fixtures, opened in its packed editor: needs Node and Chromium

PYTHON ?= python3
NODE   ?= $(shell command -v node 2>/dev/null || ls $$HOME/.local/share/nodeenv-gorb/bin/node 2>/dev/null)

.PHONY: check check-sandbox check-host check-apps check-hub check-editor check-web check-pictures check-runner check-editor-origin check-migrations rebake firmware apps hub sdk dist web

check: check-host check-apps check-hub check-editor
	@echo 'check: host suites, apps, hub and editor all green'

## check-sandbox: the gate a delegate worker runs, in tools/delegate/worker.Dockerfile's image.
## Everything `check` runs, with the firmware built first when ESP-IDF is there (a .c file left
## out of main/CMakeLists.txt then fails here, not on the bench), minus only the suites named in
## SANDBOX_SKIP, each printed as SKIP with its reason. `make check`, the full gate, is what the
## orchestrator runs on the host before landing and what CI runs: nothing is skipped there.
## A worker never edits a suite to make this pass; a failure it cannot fix in its slice it reports.
SANDBOX_SKIP := host_boot_overlay_test.py host_json_test.py host_mdns_test.py
SANDBOX_WHY_host_boot_overlay_test.py := the shipped code calls strlcpy, which glibc has only from 2.38; the worker image is Debian bookworm (2.36)
SANDBOX_WHY_host_json_test.py := -fsanitize=address dies intermittently under WSL2's kernel (libasan's runtime, not the code under test)
SANDBOX_WHY_host_mdns_test.py := -fsanitize=address dies intermittently under WSL2's kernel (libasan's runtime, not the code under test)
SANDBOX_LOG := /tmp/gorb-sandbox.log
check-sandbox:
	@if [ -n "$$IDF_PATH" ] && [ -f "$$IDF_PATH/export.sh" ]; then \
	  echo "firmware: idf.py build"; \
	  if . "$$IDF_PATH/export.sh" >/dev/null 2>&1 && idf.py build >$(SANDBOX_LOG) 2>&1; then echo "ok   firmware builds"; \
	  else grep -E 'error:|undefined reference|ninja: build stopped' $(SANDBOX_LOG) | head -30; exit 1; fi; \
	else echo "SKIP firmware build: no ESP-IDF (IDF_PATH unset)"; fi
	@fail=0; for t in tools/host_*_test.py; do b=$${t#tools/}; \
	  case " $(SANDBOX_SKIP) " in *" $$b "*) echo "SKIP $$t: $$($(MAKE) -s --no-print-directory sandbox-why-$$b)"; continue;; esac; \
	  if $(PYTHON) $$t >$(SANDBOX_LOG) 2>&1; then echo "ok   $$t"; \
	  else echo "FAIL $$t"; grep -E 'FAIL|FAILED|COMPILE|rror' $(SANDBOX_LOG) | head -40; \
	    tail -5 $(SANDBOX_LOG); fail=1; fi; \
	done; exit $$fail
	tools/orb_version.sh --self-test
	@# the hub's bundle guard compares the committed firmware bundle with build/; a slice that
	@# changes main/ always differs until the ship step repacks it, so the guard is off here only
	@echo "SKIP hub bundle guard: a slice that changes main/ differs from the committed bundle until its ship step runs make -C tools/orb-manager firmware; make check arms it"
	$(MAKE) --no-print-directory check-apps check-editor
	$(MAKE) --no-print-directory -C tools/orb-manager check BUILD_DIR=/nonexistent
	@echo 'check-sandbox: green apart from the named skips; the host runs `make check` in full'

sandbox-why-%:
	$(info $(SANDBOX_WHY_$*))
	@:

## check-host: every tools/host_*_test.py suite (the C readers and pure functions, on gcc)
check-host:
	@fail=0; for t in tools/host_*_test.py; do \
	  if $(PYTHON) $$t >/tmp/gorb-host-$$$$.log 2>&1; then echo "ok   $$t"; \
	  else echo "FAIL $$t"; grep -E 'FAIL|FAILED|COMPILE|rror' /tmp/gorb-host-$$$$.log | head -40; \
	    tail -5 /tmp/gorb-host-$$$$.log; fail=1; fi; \
	done; rm -f /tmp/gorb-host-$$$$.log; exit $$fail
	tools/orb_version.sh --self-test

## check-apps: the SDK matches the firmware, every app builds, the shelf is fresh
check-apps:
	$(PYTHON) apps/sdk/orbsdk.py binds
	$(PYTHON) tools/make_factory_apps.py --check
	$(PYTHON) tools/make_default_theme.py --check
	$(PYTHON) tools/rebake_shelf.py --check

## check-hub: the hub's Go suite, vet, every release platform, the vendored runtime
check-hub:
	$(MAKE) -C tools/orb-manager check

## check-editor: the clock editor's and the OS theme runtime's node suites
check-editor:
	@test -n "$(NODE)" || { echo 'no node on PATH (see BUILDING.md: the editor suites need node 20+)'; exit 1; }
	cd apps/clock/editor/_selftest && $(NODE) headless-test.js
	@# the same clock suite under an explicit host description: kind "hub" must change nothing
	cd apps/clock/editor/_selftest && GORB_HOST='{"kind":"hub"}' NODE_OPTIONS="--require=$(CURDIR)/tools/orb-manager/web/runtime/_selftest/gorb-host-preload.js" $(NODE) headless-test.js
	@# the .orbtheme suite runs the Python baker as its reference: any python3 with pillow + numpy
	cd tools/orb-manager/web/runtime/_selftest && PY=$${PY:-$$(command -v $(PYTHON))} $(NODE) headless-test.js
	@# the browser baker's moving-row rules against theme_schema's and the shipped orb_clock3.c (32 rows, picked frames)
	$(PYTHON) tools/verify_js_bake.py --rows
	@# OS v1.2.12's sound and disc rules, the baker's half included (the host job has no node)
	$(PYTHON) tools/host_adpcm_test.py
	$(PYTHON) tools/host_disc_test.py
	@# the weather editor's particles against fx.c, to the bit
	$(PYTHON) tools/host_weather_fx_test.py
	@# the hub's map tool (maps@1) against tools/orbmap.py's bytes and orb_map_raster.c's pixels
	$(PYTHON) tools/host_orbmap_js_test.py

## check-pictures: the studio kit's picture fitter, run in a headless Chromium: a picked
## picture is scaled to fit 1024 x 1024 and an SVG rasterised before an editor stores it.
## Deliberately NOT part of `check`, like check-web: it needs Node and Chromium.
check-pictures:
	@test -n "$(NODE)" || { echo 'no node on PATH (see BUILDING.md: the editor suites need node 20+)'; exit 1; }
	$(NODE) tools/orb-manager/web/runtime/_selftest/picture-fit.js

## check-web: the release's own web file, opened as a site would open it — unpack the
## archive, extract-editors the shelf out of it, and smoke every editor it unpacked in
## headless Chromium (no GorbHost, hub host, web host). Deliberately NOT part of `check`:
## it needs the Go toolchain, a Chromium on PATH and a few minutes, none of which the
## board-free `make check` ladder asks of a contributor.
check-web: web
	$(MAKE) check-pictures
	@test -n "$(NODE)" || { echo 'no node on PATH (see BUILDING.md: the editor suites need node 20+)'; exit 1; }
	@tmp=$$(mktemp -d); \
	  tar -xzf "$$(ls tools/orb-manager/bin/web/orb-web-*.tar.gz | head -1)" -C "$$tmp"; \
	  root=$$(find "$$tmp" -mindepth 1 -maxdepth 1 -type d | head -1); \
	  tools/orb-manager/bin/orb-manager extract-editors "$$root/apps" "$$tmp/editors" && \
	    $(NODE) tools/orb-manager/web/runtime/_selftest/editors-smoke.js --runtime "$$root/runtime" --editors "$$tmp/editors"; \
	  rc=$$?; rm -rf "$$tmp"; exit $$rc

## check-runner: the migration runner, checked against a scratch editor in headless Chromium.
## Deliberately NOT part of `check`, like check-web: it needs a Chromium on PATH.
check-runner:
	@test -n "$(NODE)" || { echo 'no node on PATH (see BUILDING.md: the editor suites need node 20+)'; exit 1; }
	$(NODE) tools/orb-manager/web/runtime/_selftest/runner-check.js

## check-editor-origin: the S2 live gate, end to end. A real headless Chromium drives a real
## hub (built here from this tree) against cmd/orbfake and a stand-in orb-os.net, and the
## eight claims of the editor-origin story are checked in the browser: edit on the app's own
## origin, Rebake all, Send to orb, import by link, no cookie on the editor port, no CSP
## violation in any flow, and a hostile third-party app refused into a wall that moves
## nothing. Deliberately NOT part of `check`, like check-web: it needs Go, Chromium and
## minutes. --json, --keep, --only and --timeout pass through under ARGS=…
check-editor-origin:
	@test -n "$(NODE)" || { echo 'no node on PATH (see BUILDING.md: the live gate needs node 20+)'; exit 1; }
	$(NODE) tools/orb-manager/web/runtime/_selftest/editor-origin-live.js $(ARGS)

## check-migrations: every app's schema fixtures (editor/_fixtures/), opened in that app's
## packed editor by the migration runner (design §6.5 point 4). Deliberately NOT part of
## `check`, like check-web: it needs Node and Chromium. Apps with no editor, and editors
## with no design schema, say so and pass.
check-migrations:
	$(PYTHON) apps/sdk/orbsdk.py check-migrations $(sort $(dir $(wildcard apps/*/app.json)))

## rebake: re-bake every stale shelf default with its app's own editor, in a headless
## Chrome/Chromium (Linux, macOS, Windows or WSL; set ORB_CHROME to pick one). Deliberately
## NOT part of `check`: it needs a browser and a few minutes, and `check-apps` only --checks
## the shelf (make_default_theme.py's / rebake_shelf.py's gates), never re-runs the bakes.
rebake:
	$(PYTHON) tools/rebake_shelf.py

firmware:
	idf.py build

apps:
	$(PYTHON) tools/make_factory_apps.py

hub:
	$(MAKE) -C tools/orb-manager manager

sdk:
	$(PYTHON) apps/sdk/orbsdk.py dist

dist:
	$(MAKE) -C tools/orb-manager dist

web:
	$(MAKE) -C tools/orb-manager web
