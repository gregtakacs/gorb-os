#!/bin/sh
# Install Orb Manager for the current user: the binaries and their apps/ and themes/ into
# ~/.local/share/orb-manager (the hub's "beside the executable" library rung), commands
# linked into ~/.local/bin, plus a menu entry and icon. The library is merged, not
# replaced, so an app or theme you imported survives an upgrade. Pass --udev to also
# install the USB rule system-wide (asks for sudo).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin="$HOME/.local/bin"
app="$HOME/.local/share/orb-manager"
mkdir -p "$bin" "$app" "$HOME/.local/share/applications" "$HOME/.local/share/icons/hicolor/256x256/apps"
cp "$here/orb-manager" "$here/orbflash" "$app/"
mkdir -p "$app/apps" "$app/themes"
cp -R "$here/apps/." "$app/apps/"
cp -R "$here/themes/." "$app/themes/"
ln -sf "$app/orb-manager" "$bin/orb-manager"
ln -sf "$app/orbflash" "$bin/orbflash"
cp "$here/orb-manager.png" "$HOME/.local/share/icons/hicolor/256x256/apps/orb-manager.png"
sed "s|^Exec=.*|Exec=$app/orb-manager|" "$here/orb-manager.desktop" > "$HOME/.local/share/applications/orb-manager.desktop"
echo "installed into $app (commands linked in $bin)"
if [ "${1:-}" = --udev ]; then
  sudo cp "$here/60-orb.rules" /etc/udev/rules.d/60-orb.rules
  sudo udevadm control --reload-rules && sudo udevadm trigger
  echo "USB rule installed: replug the orb"
fi
