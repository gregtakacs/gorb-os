# Orb Manager user guide

Orb Manager is the desktop program that sets up and looks after your orbs. It flashes
a new orb, puts it on your Wi-Fi, installs apps and clock themes, and updates the
firmware. It is one file with nothing to install, and it runs on Windows, macOS and
Linux.

You only need this page to use an orb. To build the firmware or write code for the
project, see [BUILDING.md](../BUILDING.md) and [CONTRIBUTING.md](../CONTRIBUTING.md).

## What you need

- **An orb.** This is a Waveshare ESP32-S3-Touch-AMOLED-1.75 board (the round 466×466
  display) with a KY-040 rotary knob wired to its H2 header (A=GPIO18, B=GPIO17,
  push=GPIO16). The knob is how you work the orb itself.
- **A USB-C data cable.** A charge-only cable powers the board but Orb Manager cannot
  see it.
- **A 2.4 GHz Wi-Fi network.** The ESP32-S3 has no 5 GHz radio.
- **Chrome, Edge, Brave or Chromium.** Orb Manager draws its window with one of them.
  Edge is already on every Windows 10/11 PC. Orb Manager does not install a browser.

## Download and start

Get the file for your computer from the
[Releases page](https://github.com/gregtakacs/gorb-os/releases/latest):

| You have | Download | Start it |
|---|---|---|
| Windows 10/11 | `orb-manager-<version>-windows-amd64.zip` | Unzip it and double-click `orb-manager.exe` |
| macOS 12 or later | `orb-manager-<version>-macos.dmg` | Open the .dmg, drag **Orb Manager** to Applications, open it |
| Linux x86-64 | `orb-manager-<version>-linux-amd64.tar.gz` | Unpack it and run `./orb-manager` (`./install.sh` adds a menu entry) |

Orb Manager opens in its own window. Close the window to quit.

The first time it starts:

- **Windows:** the firewall asks whether Orb Manager may use private networks. Allow
  it, because that is how it finds your orbs. If the build is unsigned, SmartScreen
  warns first: choose **More info**, then **Run anyway**.
- **macOS:** if the release notes say the build is not notarized, right-click
  Orb Manager, choose **Open**, then **Open** again.
- **Linux:** to use the USB cable, your user needs access to the serial port. Run
  `./install.sh --udev` once (it needs sudo), or join the `dialout` group with
  `sudo usermod -aG dialout "$USER"` and log in again.

`sha256sums.txt` on the release page lists the checksum of every download, if you want
to check one.

## Set up a new orb

1. Plug the orb into the computer with the USB-C cable.
2. A **New orb** button appears in the **Orbs** list. Press it, give the orb a name and
   start the flash. The bar shows the whole flash, which takes about a minute.
3. When the orb reboots, the **Bring it onto Wi-Fi** dialog opens. Pick your network
   from the list (the orb scanned it itself), type the password, and say where the orb
   is: type a city, or press **Use my location**. The orb takes its time zone from that
   place, so there is no time zone to pick.
4. The orb joins the network and shows up in the **Orbs** list. From then on Orb
   Manager reaches it over Wi-Fi, and you can unplug the cable.

A new orb comes with the Clock app and the default theme.

An orb that already runs gorb-os shows up on its own once it is on the same network as
your computer. If your network blocks the orb's announcements, use **Add an orb by
its address** and type the address the orb shows on its **ABOUT** page.

## Using the orb

| Knob | Does |
|---|---|
| Turn | move to the next or previous app |
| Press | open the app on the glass (an app's editor, for example the colour picker) |
| Hold | close it and go back; from an app, hold to reach the menu |

The menu holds the orb's own pages:

- **ABOUT**: the orb's network address, firmware version and SD card, with a QR code that opens
  the orb's own web page on a phone;
- **DISPLAY**: when the display dims;
- **PERMISSIONS**: what each app may use (network, sound, location and so on). An app
  that was refused something leaves a `*` here;
- **APPS**: the installed apps;
- a page per app with that app's options, and **Themes**, which picks the clock's face.

The display dims when nobody has touched the orb for a while. Moving the orb wakes it.

## Managing an orb from Orb Manager

Click an orb in the **Orbs** list to open its card. The tabs are:

| Tab | What it does |
|---|---|
| **Orb** | the orb's name, and its home location: the clock's time zone comes from it, and apps that need a place read it |
| **Wi-Fi** | move the orb to another network |
| **Apps** | the apps on this orb and in your library: **Install**, **Uninstall**, **Import an app** (an `.orbapp` file), **Restore bundled apps**, and **App keys** for apps that need an API key |
| **Themes** | the clock faces on this orb: pick one, **Import a theme** (an `.orbtheme` file), or open the **Theme Editor** to design your own |
| **Files** | the files on the orb's microSD card |
| **Firmware** | the version the orb runs; **Update firmware** lights up when Orb Manager carries a newer one |

Installing or removing an app reboots the orb, because apps load at startup. Uninstalling
an app also erases its settings and keys, so installing it again starts it fresh.

### The Theme Editor

The Theme Editor designs clock faces. Start from a preset, change the dial, hands,
complications, case and colours, and **Send** the result straight to the orb. **Save**
keeps an `.orbtheme` file (the theme, with its design inside) that you can open again or
share. Anything a theme can do, the editor can rebuild, so there is no "restore bundled
themes" button.

## Your library

Orb Manager keeps the apps and themes it installs in a library folder:

| OS | Folder |
|---|---|
| Windows | `%AppData%\orb-manager` |
| macOS | `~/Library/Application Support/orb-manager` |
| Linux | `~/.config/orb-manager` |

The first run fills it with the apps and the default theme that come with that release.
After that it is yours:

- Delete something and it stays deleted. A new release does not put it back.
- A new release updates a shipped file only if you have not changed it.
- **Restore bundled apps** on the Apps tab puts back any shipped app you deleted.

## Updating

Download the new release and run it in place of the old one; your library stays where it
is. If the new release carries newer firmware, each orb's card lights **Update firmware**.
The update goes over Wi-Fi, and the orb keeps running its old firmware until the new
image has checked out.

## When something goes wrong

| Problem | Try |
|---|---|
| The orb is not in the list | Check that the orb and the computer are on the same network. On Windows, allow Orb Manager through the firewall. Use **Add an orb by its address** if your router blocks multicast. |
| **New orb** never appears for a plugged-in board | Try another cable (charge-only cables are common). On Linux, check the serial permission above. |
| The window does not open | Install Chrome, Edge, Brave or Chromium. Orb Manager needs one of them. |
| An app shows as failed on the dial | The orb stopped an app that misbehaved twice. Press **retry** on the dial, or reinstall it from the Apps tab. |
| The orb is stuck or you want to start over | Plug it in by USB and use **Reset this orb** on its card. **Settings** forgets Wi-Fi and settings; **Firmware** also removes gorb-os, leaving a board you can set up again with **New orb**. |

On Windows the program's log is `%TEMP%\orb-manager.log`. On macOS and Linux, start it
from a terminal to see the log.

## Reporting a problem

Open an issue at <https://github.com/gregtakacs/gorb-os/issues/new/choose>. Say which
Orb Manager version you run (it is in the name of the file you downloaded), your
operating system, and the firmware version shown on the orb's Firmware tab. Questions and show-and-tell go in
[Discussions](https://github.com/gregtakacs/gorb-os/discussions).
