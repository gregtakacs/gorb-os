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
- **macOS:** if the release notes say the build is not notarized, macOS says it could
  not verify Orb Manager. Choose **Done**, open **System Settings → Privacy & Security**,
  press **Open Anyway** beside Orb Manager, then open it again and choose **Open Anyway**.
  On macOS 14 and earlier, right-click Orb Manager, choose **Open**, then **Open** again.
  macOS then asks whether Orb Manager may find devices on your local network. Allow it.
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

A new orb comes with the Clock app and the default theme. Apps live on the orb's microSD
card, so an orb set up without one finishes with a note instead: everything else is done.
Put a card in (FAT32), then press **Install** on Clock on the **Apps** tab. The theme
comes with it.

An orb that already runs gorb-os shows up on its own once it is on the same network as
your computer. If your network blocks the orb's announcements, use **Add an orb by
its address** and type the address the orb shows on its **ABOUT** page.

## Using the orb

The knob does three things, the same way in every app:

| Knob | Does |
|---|---|
| Turn | moves through what the app is showing |
| Press | the app's one action |
| Hold | leaves the app for the app list |

Every app has a home view, and turning moves away from it in both directions: one kind
of thing lies clockwise, another anticlockwise. A slow turn moves one step per click. A
quick spin moves several steps per click, and it always stops on the home view, so one
flick back from anywhere takes you home. Leave the knob for 20 seconds and the app goes
back to its home view by itself.

| App | Home | Clockwise | Anticlockwise | Press |
|---|---|---|---|---|
| **Weather** | the weather now | hour by hour, from the next hour | the next five days, from tomorrow | the next place |
| **Flights** | the radar | aircraft, nearest first, each with its details | aircraft, farthest first | the next range |
| **Clock** | the watch face | opens the app list | opens the app list | nothing |

Weather keeps your hour or day when you press for the next place, so you can compare
the same afternoon in two places.

**The app list.** Hold the knob in any app, or turn it on the clock, and the list of
apps appears with the one you came from highlighted. Turn to choose, press to open,
hold to go back where you were. The last row, **settings**, opens the orb's own pages:

- **ABOUT**: the orb's network address, firmware version and SD card, with a QR code that opens
  the orb's own web page on a phone;
- **DISPLAY**: when the display dims;
- **UNITS**: temperature, speed, distance, time and pressure, for every app;
- **APPS**: every app, installed or waiting on the card. Press one to open its page,
  which holds everything about it: its settings (press one to change it with the knob,
  press again to keep it; settings you type, such as places and API keys, are in Orb
  Manager, and pressing "More on Orb Manager" says which), then **Themes**, showing the
  one on the dial. Open it to turn through the app's themes as pictures, each with its
  name; the one on the dial has its name in amber. Pictures fill in as you go, so you
  can spin fast and stop where you like. Press to put the theme on the dial (the orb
  restarts wearing it), or press the one already on the dial to go straight back to the
  app; hold, or press the **Back** card at the top, to go back to the app's page.
  **Permissions** follows, showing how many it may still use
  (open it to allow or revoke network, location, sound and so on, one press each),
  then retry (only when the orb stopped a failing app), uninstall and remove from card.
  A removal takes two presses. An app that was refused something leaves a `*` on APPS,
  on its own row and on its Permissions row;
- the USB drive and sound pages.

APPS also carries **Rotate apps**: off, or every 15 seconds up to every hour. While it is
on, each app's page has an **In rotation** row, and the orb moves on to the next app in
rotation once the knob has been left alone that long. It moves only from an app that is in
the rotation, so opening any other app keeps it there; touching the knob starts the wait
again.

The Clock's page starts with **Tick volume** and **Chime volume**: the ticking (with the
minute, hour and date clicks) and the chimes and strikes each have their own level, under
the orb's own volume on the Sound page.

In settings, a hold goes back to the app you came from.

The display dims when nobody has touched the orb for a while. Moving the orb wakes it.

## Managing an orb from Orb Manager

Click an orb in the **Orbs** list to open its card. The tabs are:

| Tab | What it does |
|---|---|
| **Orb** | the orb's name; its sound, with quiet hours in half-hour steps; its **Location**, set by search, this browser, or a click on the map beside it (the clock's time zone comes from it, and apps that need a place read it); and **App settings**, a page per installed app with every one of its settings, each with its help: the same ones the orb's menu shows, plus the ones you type (Weather's places, Flights' adsb.lol API key and where to get one) |
| **Wi-Fi** | move the orb to another network |
| **Apps** | the apps on this orb and in your library, one row each with its presses in fixed columns: **Install** or **Update** (amber when the library holds a newer build; the tab shows the count), **Uninstall**, the card copy, the library copy; **Import an app** (an `.orbapp` file) and **Restore bundled apps**. The apps on the orb come first, in the order the knob and the carousel walk them: drag a row by its ⠿ grip to move it (or focus the grip and use the arrow keys). Apps only on the card or in your library follow under **Not on this orb**. **Rotate apps** above the list turns rotation on, with how long the dial waits; while it is on, each app on the orb has a checkbox for whether the rotation stops on it. |
| **Themes** | each app's themes under its own tab, the app named over the list; **Refresh from Orb**, **Compose new theme** and **Import a theme** (an `.orbtheme` file) above it. A theme the library holds a newer bake of shows an amber **Update** |
| **Files** | the files on the orb's microSD card |
| **Firmware** | the version the orb runs; **Update firmware** lights up when Orb Manager carries a newer one |

When the orb is behind this Orb Manager, a strip above the tabs says what is out of date, and
**Update everything** brings it level in one go: the firmware first, then every app with an
update, in one progress panel that counts the steps. (App updates show once the Apps tab has
read the orb; the run checks them again before it starts.)

After any restart the orb opens the first app in your carousel order (set on the Apps tab).
Installing or removing an app reboots the orb, because apps load at startup. Uninstalling
an app also erases its settings and keys, so installing it again starts it fresh.

### The Theme Editor

The Theme Editor designs clock faces. Start from a preset, change the dial, hands,
complications, case and colours, and **Send to orb** on the Render step: it saves the design
to the library first, then bakes and sends it. **Save to library**, beside it, keeps the
design without sending; the start page only opens things. **Download .orbtheme** keeps a
file (the theme, with its design inside) that you can open again or share. Anything a theme can do, the editor can rebuild, so there is no "restore bundled
themes" button.

On the Finish step's **Sound** tab every sound (the tick, each quarter and the hour, the
strikes and the jumps) has its own **Your own** file and plays with ▶. **Back to built-in**
puts one sound back to the editor's own, and **Remove all your own sounds** clears every
file you added; either way the next Send carries none of those bytes. A **From scratch**
watch starts with its light **As designed**. Every theme carries a full-size, losslessly compressed picture of
itself, which the orb shows in the carousel and when you choose a theme; themes made before
firmware v1.2.20 need sending again from the editor (or **Rebake all** on the Themes tab). Sounds share the theme's room with the art, so
the tab counts what the bake stores and says when it is getting large (from 512 KB) or
excessive (from 2 MB, where the art may no longer fit); a single sound over ten seconds is
marked long on its own row.

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
