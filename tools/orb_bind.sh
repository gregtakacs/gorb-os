#!/usr/bin/env bash
# Shared device binding for every orb-facing tool. Source it; never execute it.
#
# Each clone names exactly one physical orb in the untracked .orb-target at its
# root, and every device tool refuses to run without it. Code syncs between the
# clones; the binding does not. No tool falls back to /dev/ttyACMx: a tool bound
# to an orb that is not attached says so, instead of touching whichever board
# happened to enumerate as ACM0. That positional guess is exactly how the two
# orbs could once be crossed -- the old serflash.sh hardcoded /dev/ttyACM0.
#
# Gates, cheapest first:
#   orb_resolve_port   the bound orb's by-id symlink exists
#   orb_lock_port      no second orb tool holds the port (flock, fails loudly)
#   orb_gate_mac       esptool read_mac says the chip on that port IS the orb
#   orb_gate_ip        this host's ARP table says ORB_IP still belongs to it
#
# Stated limit: the LAN gate trusts the local ARP cache. That is right on this
# flat bench LAN, not hostile-network safe -- the fw reports no mac on
# /api/v1/status, and the owner declined adding one (2026-10-11).

ORB_TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ORB_ESPTOOL_PY="${ORB_ESPTOOL_PY:-/home/greg/.espressif/python_env/idf5.4_py3.14_env/bin/python}"

orb_die() {
  printf 'orb_bind: %s\n' "$*" >&2
  exit 1
}

# Reads <clone root>/.orb-target into ORB_LABEL / ORB_BASE_MAC / ORB_SERIAL /
# ORB_IP and sets ORB_ROOT to the clone root. Missing file or keys abort.
orb_require() {
  ORB_ROOT=$(git -C "$ORB_TOOLS_DIR" rev-parse --show-toplevel 2>/dev/null) \
    || orb_die "tools/ is not inside a git working tree"
  local bind="$ORB_ROOT/.orb-target"
  [ -f "$bind" ] || orb_die "no $bind -- this clone is unbound. Write ORB_LABEL,
ORB_BASE_MAC, ORB_SERIAL (a /dev/serial/by-id path) and ORB_IP into it first."
  # shellcheck disable=SC1090
  . "$bind"
  local k
  for k in ORB_LABEL ORB_BASE_MAC ORB_SERIAL; do
    [ -n "${!k:-}" ] || orb_die "$bind: $k missing"
  done
  export ORB_ROOT
}

orb_mac_norm() { tr 'A-Z' 'a-z' <<<"$1" | tr '-' ':'; }

# Sets ORB_PORT, or aborts naming the missing by-id path.
orb_resolve_port() {
  ORB_PORT="$ORB_SERIAL"
  [ -e "$ORB_PORT" ] || orb_die "orb $ORB_LABEL is not attached: no
$ORB_PORT (unplugged? usbipd not attached?). No other port was touched."
}

# Advisory lock, held for the life of the script: a per-orb sidecar file, NOT
# the tty itself -- pyserial takes its own flock on the port when esptool or a
# capture opens it, and a lock on the device would fight the very tool the
# gate must let in. One lock file per bound MAC means two writers on ONE orb
# collide loudly (the second refuses) while the two bench lines work in
# parallel. Called by orb_gate_mac, so every serial entry passes through it.
orb_lock_port() {
  # Normalized like orb_bind.py's load(): lowercase, no separators, so the two
  # twins take THE SAME lock file whatever case the binding was written in.
  local lf="/tmp/orb-${ORB_LABEL}-$(orb_mac_norm "$ORB_BASE_MAC" | tr -d ':').lock"
  exec 9>"$lf" || orb_die "cannot open the lock file $lf"
  flock -n 9 || orb_die "another orb tool holds $ORB_LABEL ($ORB_BASE_MAC) -- refusing to double-write"
}

# The chip's own MAC must equal the binding. Catches the crossed cable and the
# stale symlink that orb_resolve_port cannot: the by-id name is host furniture,
# the silicon is the device.
orb_gate_mac() {
  local port="$1" out have want
  orb_lock_port
  out=$("$ORB_ESPTOOL_PY" -m esptool --chip esp32s3 -p "$port" read_mac 2>&1) \
    || orb_die "read_mac failed on $port:
$out"
  have=$(sed -n 's/^MAC: *\([0-9a-fA-F:]*\).*/\1/p' <<<"$out" | head -1 | tr -d '[:space:]')
  [ -n "$have" ] || orb_die "read_mac printed no MAC on $port:
$out"
  have=$(orb_mac_norm "$have")
  want=$(orb_mac_norm "$ORB_BASE_MAC")
  if [ "$have" != "$want" ]; then
    orb_die "WRONG ORB: the chip on $port reports MAC $have, but $ORB_LABEL is
bound to $want ($ORB_SERIAL). Crossed cable or stale by-id symlink; nothing was
written."
  fi
  printf 'orb_bind: %s confirmed as %s (MAC %s)\n' "$port" "$ORB_LABEL" "$have" >&2
}

# Sets ORB_RESOLVED_IP (argv override, else ORB_IP) and proves via ip neigh --
# one ping and a retry to refresh a stale entry -- that the address answers as
# the bound orb's MAC. The first five octets are the identity; the same nic's
# STA/AP/BT MACs differ only in the low octet (orb_net.c:657 builds the AP
# SSID from ESP_MAC_WIFI_SOFTAP), so a +1 there is tolerated, nothing else.
orb_gate_ip() {
  local ip="${1:-${ORB_IP:-}}" want have hiw low hih loh d attempt
  [ -n "$ip" ] || orb_die "$ORB_LABEL has no ORB_IP in $ORB_ROOT/.orb-target --
provision it onto the LAN first."
  want=$(orb_mac_norm "$ORB_BASE_MAC")
  have=""
  for attempt in 1 2; do
    have=$(ip neigh show "$ip" 2>/dev/null | sed -n 's/.*lladdr \([0-9a-fA-F:]*\).*/\1/p' | head -1)
    [ -n "$have" ] && break
    ping -c1 -W1 "$ip" >/dev/null 2>&1
    sleep 1
  done
  [ -n "$have" ] || orb_die "no ARP answer from $ip ($ORB_LABEL) in two attempts"
  have=$(orb_mac_norm "$have")
  if [ "$have" != "$want" ]; then
    hiw=${want%:*}; low=$(( 16#${want##*:} ))
    hih=${have%:*}; loh=$(( 16#${have##*:} ))
    d=$(( (loh - low + 256) % 256 ))
    if [ "$hih" != "$hiw" ] || [ "$d" -gt 1 ]; then
      orb_die "WRONG ORB on the LAN: $ip answers as $have but $ORB_LABEL is
$want (+1 low octet tolerated). That is likely the OTHER orb's address -- fix
ORB_IP in $ORB_ROOT/.orb-target; do not route around this gate."
    fi
  fi
  printf 'orb_bind: %s confirmed as %s (ARP %s)\n' "$ip" "$ORB_LABEL" "$have" >&2
  ORB_RESOLVED_IP="$ip"
}
