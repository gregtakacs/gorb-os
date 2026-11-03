# Build this app against a pinned gorb-os SDK. `make` fetches the SDK named in sdk.txt
# (once, into .sdk/) and writes build/<slug>.orbapp. `make install ORB=192.168.1.50`
# uploads it to an orb and installs it (the orb reboots to load it).
#
# Inside a gorb-os clone you do not need this file: apps/sdk/orbsdk.py is the same tool.
SDK_VERSION := $(shell cat sdk.txt)
SDK_URL     ?= https://github.com/gregtakacs/gorb-os/releases/download/$(SDK_VERSION)/orb-sdk-$(SDK_VERSION).tar.gz
SDK         ?= .sdk/orb-sdk-$(SDK_VERSION)
ORBSDK      := python3 $(SDK)/orbsdk.py

.PHONY: all build pack install clean
all: pack

$(SDK)/orbsdk.py:
	@mkdir -p .sdk
	curl -fsSL $(SDK_URL) | tar -xz -C .sdk

build: $(SDK)/orbsdk.py
	$(ORBSDK) build .

pack: $(SDK)/orbsdk.py
	$(ORBSDK) pack .

install: $(SDK)/orbsdk.py
	@test -n "$(ORB)" || { echo "usage: make install ORB=<orb ip or name>"; exit 2; }
	$(ORBSDK) install . --orb $(ORB)

clean:
	rm -rf build
