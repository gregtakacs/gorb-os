#!/usr/bin/env bash
# wsl-usb.sh - move the bench orb serial ports between WSL and Windows.
#
# Four rules, each measured on this box (usbipd-win 5.3.0, WSL 2.7.11, mirrored
# networking), and each one the reason this is a script rather than a shell alias:
#
#   1. busid is ephemeral. Windows re-enumerated a board from 1-10 to 1-6 inside one
#      working session, so every request here is by MAC and the busid is looked up.
#   2. Verify every detach. The Windows service keeps an attach record per device and
#      replays it, and an auto-attach watcher re-attaches on its own, so a released port
#      can simply come back a few seconds later. This script watches the device node and
#      retries through the Windows CLI, which clears that record.
#   3. Reading `usbipd list` is not enough. It can report Attached for a port whose
#      device node WSL has already lost. The node and the vhci port are the truth.
#   4. Nothing here needs an administrator token on Windows. Only `bind` and
#      `unbind` do, and both are one-time and already done on this bench.
#
# Run it from inside WSL.
set -uo pipefail

MACS_DEFAULT="44bd8d782618 44bd8d608088"
MACS="${WSL_USB_MACS:-$MACS_DEFAULT}"
USBIPD_HOST="${USBIPD_HOST:-127.0.0.1}"
USBIPD_PORT="${USBIPD_PORT:-3240}"
SUDO="${WSL_USB_SUDO:-sudo}"

USBIPD=""
for c in "${WSL_USB_USBIPD:-}" "usbipd.exe" "/mnt/c/Program Files/usbipd-win/usbipd.exe"; do
  [ -n "$c" ] || continue
  if command -v "$c" >/dev/null 2>&1 || [ -x "$c" ]; then USBIPD="$c"; break; fi
done
PS="$(command -v powershell.exe 2>/dev/null || true)"
[ -n "$PS" ] || PS="/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"

fail() { echo "wsl-usb: $*" >&2; exit 1; }
[ -n "$USBIPD" ] || fail "usbipd.exe not found; set WSL_USB_USBIPD"
[ -x "$PS" ] || fail "powershell.exe not found"

# --- Windows side -------------------------------------------------------------

# The state document is the only Windows-side source carrying the board serial and
# the live busid together. One record per line: mac, busid, com, share, attach.
win_state() {
  "$USBIPD" state 2>/dev/null | tr -d '\r' | python3 -c '
import json,re,sys
d=json.load(sys.stdin).get("Devices",[])
seen=set()
for x in d:
    t=json.dumps(x)
    if "303a" not in t.lower() and "VID_303A" not in t: continue
    m=re.search(r"([0-9A-Fa-f]{2}[:\\-]){5}[0-9A-Fa-f]{2}", t)
    if not m: continue
    mac=re.sub(r"[^0-9a-f]","",m.group(0).lower())
    if mac in seen: continue
    seen.add(mac)
    b=x.get("BusId") or "-"
    dsc=x.get("Description") or ""
    c=re.search(r"\((COM[0-9]+)\)",dsc)
    print("\t".join([mac,b,(c.group(1) if c else "-"),
      ("shared" if x.get("PersistedGuid") else "unshared"),
      ("attached" if x.get("ClientIPAddress") else "free")]))
'
}

win_field() { win_state | awk -F'\t' -v m="$1" -v c="$2" '$1==m{print $c}'; }
busid_of()  { win_field "$1" 2; }
com_of()    { win_field "$1" 3; }
share_of()  { win_field "$1" 4; }
win_of()    { win_field "$1" 5; }

# PowerShell script on stdin, delivered base64 so the bash -> powershell -> CIM quoting
# that kills these helpers never has to happen.
ps_run() {
  local enc
  enc=$(python3 -c 'import sys,base64;sys.stdout.write(base64.b64encode(sys.stdin.buffer.read().decode().encode("utf-16-le")).decode())')
  "$PS" -NoProfile -NonInteractive -EncodedCommand "$enc"
}

# --- WSL side -----------------------------------------------------------------

mac_colon() { echo "$1" | sed 's/\(..\)/\1:/g; s/:$//' | tr 'a-f' 'A-F'; }
node_of() {
  local l t
  l="/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_$(mac_colon "$1")-if00"
  [ -e "$l" ] || return 0
  t=$(readlink -f "$l" 2>/dev/null) || return 0
  [ -e "$t" ] && echo "$t"
  return 0
}

# Client-side view: which vhci port holds which host busid, as "busid port".
client_ports() {
  $SUDO usbip port 2>/dev/null | awk '
    /^Port[ ]*[0-9]+:/ { p=$2; gsub(/[^0-9]/,"",p) }
    /usbip:\/\// { if (match($0,/\/1?[0-9]+-[0-9]+$/)) print substr($0,RSTART+1,RLENGTH-1), p }'
}
client_port_of() { client_ports | awk -v b="$1" '$1==b{print $2; exit}'; }

watcher_pids() {
  ps_run <<'PS_EOF' | tr -d '\r' | grep -oE '^[0-9]+$' || true
$ProgressPreference='SilentlyContinue'
$svc = (Get-CimInstance Win32_Service -Filter "Name='usbipd'").ProcessId
# CommandLine reads back empty for some processes, so match on anything that is not the
# service itself: every other usbipd.exe on the box is a CLI we started.
Get-CimInstance Win32_Process -Filter "Name='usbipd.exe'" | Where-Object {
  $_.ProcessId -ne $svc
} | ForEach-Object { $_.ProcessId }
PS_EOF
}

# --- verbs --------------------------------------------------------------------

wait_node() {
  local m="$1" i n
  for i in 1 2 3 4 5 6 7 8 9 10; do
    n=$(node_of "$m"); [ -n "$n" ] && { echo "$n"; return 0; }
    sleep 0.5
  done
  return 1
}

verdict() {
  local m="$1" b sh wn n
  b=$(busid_of "$m"); sh=$(share_of "$m"); wn=$(win_of "$m"); n=$(node_of "$m")
  if [ -n "$n" ]; then printf '%s  %-9s %-8s %-6s %-9s ready\n' "$m" "${b:--}" "${wn:-absent}" "${sh:-?}" "${n:-?}"; return 0; fi
  if [ -z "$b" ]; then printf '%s  %-9s %-8s %-6s %-9s absent: check the cable\n' "$m" - - - -; return 1; fi
  if [ "$sh" != shared ]; then printf '%s  %-9s %-8s %-6s %-9s unshared: ask an admin to run usbipd bind --busid=%s\n' "$m" "$b" "$wn" unshared - "$b"; return 1; fi
  if [ "$wn" = attached ]; then printf '%s  %-9s %-8s %-6s %-9s wedged: Windows says attached, WSL has no node. Run: %s reset\n' "$m" "$b" "$wn" "$sh" - "${0##*/}"; return 1; fi
  printf '%s  %-9s %-8s %-6s %-9s released: run: %s attach\n' "$m" "$b" "$wn" "$sh" - "${0##*/}"
  return 1
}

cmd_status() {
  local rc=0 m
  echo "mac          busid     windows share  node      verdict"
  for m in $MACS; do verdict "$m" || rc=1; done
  local w; w=$(watcher_pids | wc -l)
  echo "usbipd CLI processes on Windows (a watcher re-takes a released port): $w"
  return $rc
}

cmd_list() { "$USBIPD" list 2>&1 | tr -d '\r'; }

cmd_attach() {
  local rc=0 m b n
  for m in $MACS; do
    b=$(busid_of "$m")
    if [ -z "$b" ]; then echo "  $m  skipped: not on the bus"; rc=1; continue; fi
    if [ "$(share_of "$m")" != shared ]; then echo "  $m  skipped: not shared; elevated: usbipd bind --busid=$b"; rc=1; continue; fi
    n=$(node_of "$m")
    if [ -n "$n" ]; then echo "  $m  already $n"; continue; fi
    if ! $SUDO usbip attach --remote="$USBIPD_HOST" --busid="$b" >/dev/null 2>&1; then
      echo "  $m  refused on busid $b; run '$0 reset' if a link is wedged"; rc=1; continue
    fi
    n=$(wait_node "$m") || { echo "  $m  attached on $b but no node appeared"; rc=1; continue; }
    echo "  $m  $n (busid $b, $(com_of "$m"))"
  done
  return $rc
}

# Client-side first: it needs no round trip to the Windows CLI. A release can still be
# undone -- the service keeps an `Attached` record per device and replays it -- so every
# detach is verified, and a bounced one is retried through the Windows CLI, which clears
# that record. The node coming and staying gone is the only evidence that counts.
cmd_detach() {
  local rc=0 m b p
  for m in $MACS; do
    b=$(busid_of "$m")
    p=$(client_port_of "${b:-none}")
    if [ -n "$p" ]; then
      $SUDO usbip detach --port="$p" >/dev/null 2>&1 || { echo "  $m  detach failed on vhci port $p"; rc=1; continue; }
      if ! released_for "$m" 6; then
        "$USBIPD" detach --busid="$b" >/dev/null 2>&1
        if released_for "$m" 8; then echo "  $m  released via Windows (the first release was replayed)"
        else echo "  $m  STILL HELD after two attempts; a watcher is running: '$0 unwatch'"; rc=1; continue; fi
      else
        echo "  $m  released (vhci port $p)"
      fi
      continue
    fi
    if [ "$(win_of "$m")" = attached ]; then
      "$USBIPD" detach --busid="$b" >/dev/null 2>&1
      released_for "$m" 8 && echo "  $m  released via Windows" \
                          || { echo "  $m  STILL HELD; run '$0 unwatch'"; rc=1; continue; }
      continue
    fi
    echo "  $m  already free"
  done
  return $rc
}

# True when the device node has stayed absent for the whole window.
released_for() {
  local m="$1" n="$2" i
  for i in $(seq 1 "$n"); do
    sleep 1
    [ -n "$(node_of "$m")" ] && return 1
  done
  return 0
}

cmd_reset() {
  echo "detaching"
  cmd_detach >/dev/null 2>&1
  sleep 1
  echo "reattaching"
  cmd_attach
}

# The watcher is the only thing that covers a board dropping off the bus mid-flash. It
# has to be started detached from this shell and hidden, because --auto-attach never
# returns: it prints "Starting endless attach loop" and stays. Start-Process -PassThru
# hands back the launcher, which exits, so the count is re-read afterwards rather than
# printed per board -- a PID printed here would be a number that is already dead.
cmd_watch() {
  local m b n=0
  for m in $MACS; do
    b=$(busid_of "$m")
    [ -n "$b" ] || { echo "  $m  absent, nothing to watch"; continue; }
    ps_run <<PS_EOF >/dev/null 2>&1
\$ProgressPreference='SilentlyContinue'
Start-Process -FilePath 'C:\\Program Files\\usbipd-win\\usbipd.exe' -ArgumentList 'attach','--wsl','--auto-attach','--busid','$b' -WindowStyle Hidden
PS_EOF
    n=$((n+1))
    echo "  $m  watching busid $b"
  done
  sleep 3
  echo "  watchers alive: $(watcher_pids | wc -l) of $n started"
}

cmd_unwatch() {
  ps_run <<'PS_EOF' | tr -d '\r'
$ProgressPreference='SilentlyContinue'
$svc = (Get-CimInstance Win32_Service -Filter "Name='usbipd'").ProcessId
$found = Get-CimInstance Win32_Process -Filter "Name='usbipd.exe'" | Where-Object {
  $_.ProcessId -ne $svc
}
if (-not $found) { 'no watchers'; exit }
$found | ForEach-Object { Stop-Process -Id $_.ProcessId -Force; "stopped $($_.ProcessId)" }
PS_EOF
}

cmd_help() {
  # The header comment is the help text, so it cannot drift from the implementation.
  awk 'NR>1 && !/^#/{exit} NR>1{sub(/^# ?/,""); print}' "$0"
  cat <<H
Verbs:
  status    what WSL can use right now, and what to run if it cannot  (default)
  list      the raw Windows view, busids and COM ports included, no privileges
  attach    hand both ports to WSL                          (needs sudo)
  detach    hand both ports back to Windows                 (needs sudo)
  reset     detach then attach: the fix for a wedged link   (needs sudo)
  watch     keep a hidden auto-attach loop running per board, so a board that
            drops off the bus mid-flash comes back without you
  unwatch   stop those loops. They die at logoff on their own
  help      this

Env: WSL_USB_MACS, USBIPD_HOST (mirrored -> 127.0.0.1), WSL_USB_SUDO, WSL_USB_USBIPD
H
}

case "${1:-status}" in
  status)  cmd_status ;;
  list)    cmd_list ;;
  attach)  cmd_attach ;;
  detach)  cmd_detach ;;
  reset)   cmd_reset ;;
  watch)   cmd_watch ;;
  unwatch) cmd_unwatch ;;
  help|-h|--help) cmd_help ;;
  *) fail "unknown verb '$1'; try help" ;;
esac
