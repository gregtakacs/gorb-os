Orb Manager @VERSION@ for Linux (x86-64)
========================================

Quick start, no install:
    ./orb-manager
It opens its own window; close the window to quit.

Install for your user (menu entry, icon, orb-manager in ~/.local/bin):
    ./install.sh            or    ./install.sh --udev   (also the USB rule, needs sudo)

Needs: Google Chrome, Chromium, Microsoft Edge or Brave. Orb Manager draws its
window with that browser; it does not install one.

USB: to flash or set up an orb over its USB-C cable your user must be able to
open the serial port. Either run ./install.sh --udev (installs 60-orb.rules,
which grants the logged-in user access to Espressif boards), or join the dialout
group and log in again:
    sudo usermod -aG dialout "$USER"

Firewall: orbs are found on the local network by mDNS (UDP 5353); allow it if
your firewall blocks it.

Licences: LICENSE.txt (Orb Manager) and THIRD_PARTY_NOTICES.txt.
