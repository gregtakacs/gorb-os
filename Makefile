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

PYTHON ?= python3
NODE   ?= $(shell command -v node 2>/dev/null || ls $$HOME/.local/share/nodeenv-gorb/bin/node 2>/dev/null)

.PHONY: check check-host check-apps check-hub check-editor firmware apps hub sdk dist

check: check-host check-apps check-hub check-editor
	@echo 'check: host suites, apps, hub and editor all green'

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

## check-hub: the hub's Go suite, vet, every release platform, the vendored runtime
check-hub:
	$(MAKE) -C tools/orb-manager check

## check-editor: the clock editor's and the OS theme runtime's node suites
check-editor:
	@test -n "$(NODE)" || { echo 'no node on PATH (see BUILDING.md: the editor suites need node 20+)'; exit 1; }
	cd apps/clock/editor/_selftest && $(NODE) headless-test.js
	@# the .orbtheme suite runs the Python baker as its reference: any python3 with pillow + numpy
	cd tools/orb-manager/web/runtime/_selftest && PY=$${PY:-$$(command -v $(PYTHON))} $(NODE) headless-test.js

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
