# Bench serial ports in WSL: enable, release, recover

The two orb boards on the bench are USB to Windows. Flashing and monitoring from WSL
attaches them through usbipd-win. This page records what survives what, and the exact
command for each case. Everything here was run on this box: usbipd-win 5.3.0, WSL
2.7.11, kernel 6.18.33.2-microsoft-standard-WSL2, `networkingMode=mirrored`.

Use the script. It encodes the rules below and refuses to act on a stale busid.

```
tools/orb-manager/scripts/wsl-usb.sh status      # what WSL can use, and what to run if not
tools/orb-manager/scripts/wsl-usb.sh attach      # both ports into WSL
tools/orb-manager/scripts/wsl-usb.sh detach      # both ports back to Windows
```

`status` is the first command to run whenever anything about a port is in question. It
reads the Windows state and the WSL device node, and names the fix. `help` lists the
verbs and the environment overrides; `list` is the raw Windows view.

Claims on this page were checked by hand and are pinned by `make -C tools/orb-manager
usb-doc-test`, which reads the script and this file rather than the hardware. Run it
after editing either.

## The two boards

| MAC (the USB serial string) | Windows COM | Verified by |
|---|---|---|
| `44:BD:8D:78:26:18` | COM3 | `usbipd state`: InstanceId carries the serial, Description carries the COM port |
| `44:BD:8D:60:80:88` | COM4 | the same |

Neither port was opened for I/O while writing this: opening a CDC-ACM port asserts
DTR, which resets a running ESP32. The mapping above came from the device descriptors
alone.

Which station IP each board holds is not recorded here, and the two were not
matched up in this session: the hub was not running and no mDNS resolver is
installed. Confirm it before trusting it -- the boot line over the board's own
COM port prints `ip:` next to `mac:`, and the Orbs card in the browser prints
both.

Address a port by the by-id path, never by `ttyACM<n>`. Enumeration order decides that
number and it moved during this session. The COM numbers above are this box's; read them
from `list`.

```
/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_44:BD:8D:78:26:18-if00
/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_44:BD:8D:60:80:88-if00
```

Both boards are `303a:1001`, so one VID:PID cannot select one board. `-i 303a:1001` gets
either.

## busid is not stable

`1-6` and `1-10` are Windows enumeration slots. One board held `1-10` in the morning and
`1-6` after lunch. Windows re-enumerates on replug, on detach, and at boot. Never put a
busid in a script or a cron line. The script resolves busid from the MAC on every call
through `usbipd state`.

## Two mechanisms, and why one is not enough

**Windows side.** `usbipd attach --wsl --auto-attach --busid=<id>` re-attaches a device
that leaves the bus. It does not return: it prints "Starting endless attach loop; press
Ctrl+C to quit" and stays. Start it hidden, one process per board. `wsl-usb.sh watch`
does that.

**WSL side.** `sudo usbip attach --remote=127.0.0.1 --busid=<id>` pulls a shared device
from the Linux side. It returns immediately, so it needs no console, and it does not need
a Windows process kept alive.

The Windows-side command covers a board dropping off the bus. The WSL-side command covers
everything else, and is the one that works unattended. Both talk to the same service; a
device is held by one or the other, never both.

## What survives what

Measured, not quoted from documentation.

| Event | Port still there? | Do this |
|---|---|---|
| Nothing, next working hour | Yes, indefinitely | `status` to confirm |
| Board replug, board reset, a flash that reboots the board | No, until a watcher acts | `watch` beforehand; it recovers in seconds |
| Any detach, while a watcher runs | Taken back within seconds, from either side | `unwatch`, then `detach` |
| Any detach, nothing replaying it | No, and it stays gone | `attach` when you want it back |
| `wsl --shutdown`, or WSL update | No. The vhci host lives in the VM | `attach` |
| Windows logoff | No. The watcher dies with the session | `attach`, then `watch` |
| Windows reboot | Not tested. The service may replay its own record, or WSL may come up with nothing | `status`, then `attach` if it says released |

Two separate things can undo a release, and neither shows up in `usbipd list`:

- The `usbipd` service keeps an attach record per device, in the `\Attached` subkey of
  `HKLM\SOFTWARE\usbipd-win\Devices\<guid>`, and replays it. With that record present, a
  port released only on the client side came back inside 8 s while the only `usbipd.exe`
  on the box was the service itself.
- An auto-attach watcher (`watch`) re-attaches anything that leaves the bus. That is its
  whole purpose, and it includes a port you just released.

So `detach` never trusts its own exit code. It polls the device node for several seconds,
and on a bounce it calls the Windows CLI, which clears that record. Releases observed
after that held for the entire window, 12 s and 16 s, with the kernel logging a clean
disconnect. If both attempts bounce, a watcher is running: `unwatch`.

Sharing is a separate thing and it does survive. `usbipd list` shows both boards
`Shared` across every event above; the registry keys under
`HKLM\SOFTWARE\usbipd-win\Devices` hold them. Nothing in this file ever needs `bind`
again on this box.

## The one command per situation

**Every morning, or after a reboot.**

```bash
tools/orb-manager/scripts/wsl-usb.sh status || tools/orb-manager/scripts/wsl-usb.sh attach
tools/orb-manager/scripts/wsl-usb.sh watch     # optional, only if you flash
```

**Ready to flash.** Nothing. Confirm, then use the by-id path:

```bash
tools/orb-manager/scripts/wsl-usb.sh status
make -C tools/orb-manager flash PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_44:BD:8D:78:26:18-if00
```

**Hand the cable to Windows** (an idf.py run in PowerShell, Device Manager, a Windows
build). Detach from the WSL side. A Windows-side detach is a request the auto-attach
watcher answers by taking the port straight back:

```bash
tools/orb-manager/scripts/wsl-usb.sh detach
```

Then stop the watcher too, or it wins eventually:

```bash
tools/orb-manager/scripts/wsl-usb.sh unwatch
```

**A board dropped mid-flash and the port is gone.** The watcher brings it back. Without
one, run `reset`: detach what is left, then attach.

```bash
tools/orb-manager/scripts/wsl-usb.sh reset
```

**Port will not come back at all.** Work outside in, in this order:

```bash
tools/orb-manager/scripts/wsl-usb.sh status     # absent -> the board is not on the bus
sudo dmesg | tail -20                           # usb 1-N: new device -> the host is answering
tools/orb-manager/scripts/wsl-usb.sh list       # Shared but no busid -> replug, busid changed
```

## Privileges

What each step needs. Verified on this box, where the account is in the
dialout group and sudo needs no password.

| Step | Needs |
|---|---|
| `attach`, `detach`, `reset`, `status`, `list` | no Windows admin; sudo inside WSL for the usbip calls |
| `bind` a new board (share it), `unbind` | elevated Windows shell |
| restarting the `usbipd` service | elevated Windows shell |
| talking to `/dev/ttyACM*` | membership in `dialout`; re-login after adding it |

The service is `StartMode=Auto` and `Running`, so a boot starts it. Sharing is stored
under `HKLM\SOFTWARE\usbipd-win\Devices` and survives a reboot. The attach is written
under a `\Attached` subkey, and the service uses it to replay an attach inside a session.
Upstream still calls attach non-persistent and tells you to re-attach after a reboot, and
this box has never been tested across a reboot, so treat the boot case as unknown: run
`status` and act on what it says. Do not schedule a flash at boot.

## One-time setup on a fresh box

Elevated Windows PowerShell, once per board:

```powershell
winget install usbipd            # service, CLI, firewall rule
usbipd list                      # find the busid of the board, while it is plugged in
usbipd bind --busid=<busid>      # share it; survives reboot
```

Inside WSL, once:

```bash
sudo apt-get install -y linux-tools-$(uname -r)   # gives you usbip; the plain metapackage does not
sudo usermod -aG dialout $USER                     # then re-login
```

The distro ships `/usr/bin/usbip` as a wrapper that looks for a binary named after the
running kernel. The WSL kernel is `6.18.33.2-microsoft-standard-WSL2` and no such package
exists, so the wrapper prints "not found for kernel" and exits 2. Install the tools for
the *generic* kernel. `usbip version` printing a version is the check that it worked.

## Optional: no Windows process at all

A systemd user unit can attach at login. It lives exactly as long as the watchers do
(the user manager stops at logoff, `Linger=no`), so it buys one thing: no hidden
PowerShell windows. Write `~/.config/systemd/user/orb-usb-attach.service`:

```ini
[Unit]
Description=Attach the bench orb boards to WSL
After=network.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/home/greg/docker/Projects/gorb-os/tools/orb-manager/scripts/wsl-usb.sh attach
ExecStop=/home/greg/docker/Projects/gorb-os/tools/orb-manager/scripts/wsl-usb.sh detach
TimeoutStartSec=90
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
```

```bash
systemctl --user daemon-reload
systemctl --user enable --now orb-usb-attach.service   # enable
systemctl --user disable --now orb-usb-attach.service  # disable
loginctl enable-linger greg                            # only if you want it without a login
```

That unit calls `usbip` through sudo, so without passwordless sudo it needs a drop-in.
`/usr/bin/usbip` is already validated by the distro wrapper, so naming it is not a hole:

```
greg ALL=(root) NOPASSWD: /usr/bin/usbip attach *, /usr/bin/usbip detach *, /usr/bin/usbip port, /usr/bin/usbip devices
```

Mirrored mode is what makes `127.0.0.1` the right address. On a NAT distro the host
lives on the subnet gateway instead, so point the script at it: `USBIP_HOST=<gateway from
ip route>` as an environment prefix, or persist it in the unit with `Environment=`. The
service itself is unchanged; only the address the client dials moves.

Do not run this unit and `watch` at the same time on the same board: the service and the
unit each try to be the one holding the port, and you get the 2-to-4-second tug-of-war.

## Troubleshooting

**`status` says wedged.** Windows holds an attach, WSL has no node. `reset` clears it.

**`attach` says refused.** Something else holds it. `status` shows what. If Windows says
attached and you want it, `reset`. If you want it in Windows, `detach` then `unwatch`.

**`usbip` prints "not found for kernel".** The wrapper found no matching linux-tools
package. See the setup section.

**`dmesg` shows `vhci_hcd: urb->status -104`.** Expected. It is the reset of a link
mid-transfer, which is what a detach does.

**A board shows `absent`.** Not on the bus: cable, port, or the board is powered off.
Nothing to re-attach.

**`usbipd list` shows no busid for a board you can see in Device Manager.** The Windows
bind was lost or the board enumerated on a different busid. Re-run `bind` elevated.

## Three things previously written down wrong

Recorded so they do not come back.

1. `wsl.exe` is not at `/c/Windows/System32/wsl.exe` from inside a distro; `/c` is not
   mounted there. The path is `/mnt/c/Windows/System32/wsl.exe`, and bare `wsl.exe` is on
   PATH already.
2. Client-side attach works on this box. `sudo usbip attach --remote=127.0.0.1
   --busid=<id>` succeeds. The earlier report that it is refused outside NAT mode was
   wrong: the refusals seen were `Device busy (exported)` and `Device not found`, which
   mean a registration already held the device. Mirrored mode is not a blocker.
3. `wslinfo --networking-interface` is not a valid flag here. Ask `usbipd` instead: an
   attach logs "Detected networking mode 'mirrored'". `ip route` and a pingable gateway
   on the LAN subnet say the same thing.
