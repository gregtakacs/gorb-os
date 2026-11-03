#!/bin/sh
# Install Orb Manager for the current user: the binary into ~/.local/share/orb-manager,
# linked into ~/.local/bin, plus a menu entry and icon. The apps and themes are inside the
# binary and land in ~/.config/orb-manager on first run. Pass --udev to also install the
# USB rule system-wide (asks for sudo).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin="$HOME/.local/bin"
app="$HOME/.local/share/orb-manager"
mkdir -p "$bin" "$app" "$HOME/.local/share/applications" "$HOME/.local/share/icons/hicolor/256x256/apps"
cp "$here/orb-manager" "$app/"
ln -sf "$app/orb-manager" "$bin/orb-manager"
cp "$here/orb-manager.png" "$HOME/.local/share/icons/hicolor/256x256/apps/orb-manager.png"
sed "s|^Exec=.*|Exec=$app/orb-manager|" "$here/orb-manager.desktop" > "$HOME/.local/share/applications/orb-manager.desktop"
echo "installed into $app (orb-manager linked in $bin)"
if [ "${1:-}" = --udev ]; then
  sudo cp "$here/60-orb.rules" /etc/udev/rules.d/60-orb.rules
  sudo udevadm control --reload-rules && sudo udevadm trigger
  echo "USB rule installed: replug the orb"
fi
