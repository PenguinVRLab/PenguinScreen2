# Supported devices and how to connect them

PenguinScreen2 runs on Linux (desktop Linux, SteamOS, the Steam Deck) and streams to a VR headset. This page lists
every headset and host we know about, and how to connect each one.

- **Supported**: works, and is covered by the guides that ship with this release.
- **Experimental**: built and shipped; it may not work on every setup. Please tell us how it goes.
- **Not in this release**: in development; nothing in this download sets it up.

## At a glance

| Device | Linux PC or SteamOS desktop | Steam Deck | Windows |
|---|---|---|---|
| **Meta Quest 3** (through WiVRn) | **Supported** over Wi-Fi | **Supported** over a USB-C cable, or docked on wired Ethernet; the Deck's own Wi-Fi is not supported | Not covered by this guide yet |
| **Other WiVRn headsets** (Quest 2, Quest 3S, Quest Pro, Pico and others) | **Experimental**: they use the same app and path as the Quest 3 | As the Quest 3 | Not covered by this guide yet |
| **PlayStation VR2** (with Sony's PC adapter) | **The flatpak**, through Monado: **Supported** on SteamOS (a Steam Machine with AMD graphics, in the Plasma X11 session); **Experimental** on other Linux. **The AppImage**, through SteamVR on Linux: the supported route for that build (`PSVR2.md`, Route 2) | Not covered by this guide yet | Not covered by this guide yet |
| **Display glasses** (RayNeo Air 3s) | Not in this release | Not in this release | Not in this release |
| **No headset** (flat play) | **Supported** | **Supported** | Not covered by this guide yet |

## Meta Quest and other WiVRn headsets

The headset runs the free **WiVRn** app; your PC runs the WiVRn server, which `setup-configure-deps.sh` installs. The
full walkthrough is in `QUICKSTART.md`; the short version:

1. Install the PenguinScreen2 flatpak and run `bash setup-configure-deps.sh` once (see `README.md`).
2. On the headset, install **WiVRn** from the store (Meta Horizon Store for Quest).
3. Run `bash launch-vr-session.sh`. It starts the server and prints this computer's IP address and a pairing PIN.
4. In the headset's WiVRn app, choose **Add server / Connect by IP**, enter the address it printed and port **9757**.
   On SteamOS the server never appears in the list on its own; connecting by IP is the normal way. (Optional:
   `bash enable-vr-discovery.sh` turns automatic discovery on; it asks for your password, and on SteamOS you must
   run it again after each system update. On a Steam Deck, set a password first with `passwd` in Konsole.)
5. The first time only, enter the PIN on the headset. The launcher then opens PenguinScreen2 in VR mode; start a
   game from its library with the headset on.

Keep the **headset app and the PC server on the same WiVRn version**: a mismatch shows "incompatible version".
Update the server with `flatpak update --user io.github.wivrn.wivrn`, with the headset disconnected.

## Steam Deck

Use **Desktop Mode** for installing and launching (Game Mode is a bonus where it works). Connect by IP as above.
`bash launch-vr-session.sh --install-desktop` puts a double-click shortcut on the desktop.

- **Over the Deck's own Wi-Fi: not supported.** It connects, but the picture is not good enough to play. Use one of
  the two routes below.
- **Over a USB-C cable: supported.** A USB-C data cable from the Deck straight to the headset:
  1. One time, one of two ways. **Easiest:** with the WiVRn app 26.9 or newer on Horizon OS 2.6 or newer, turn on
     **USB networking** in the headset's WiVRn app (Settings → System) and run `bash enable-vr-discovery.sh` once on
     the Deck (your password; it lets the headset find the Deck over the cable and adds the network profile the link
     needs). No developer account. **Otherwise:** turn on **Developer Mode** for the headset (Meta Horizon phone app →
     Devices → your headset → Headset settings → Developer Mode, with a free developer account) and run
     `bash setup-configure-deps.sh` while online so it downloads `adb`. The launcher says which it sees, or that the
     headset is offering neither yet.
  2. Plug the cable in and run `bash launch-vr-session.sh`. The first time, the headset shows "Allow USB
     debugging?": put it on, tick *Always allow*, OK. The launcher waits for that and then prints `USB tether ACTIVE`.
  3. First connect only: the launcher prints a **PIN** before it opens the headset's WiVRn app over the cable; type
     it when the app asks. Paired for good. If the app shows its server list instead, connect by hand:
     **Add server / Connect by IP**, address `127.0.0.1` (the same as `localhost`), port 9757,
     and turn on **TCP only** (the cable carries TCP only; without that switch the connection times out).
     Not connecting? Turn the headset's Wi-Fi off while it is on the cable.
  4. To connect the headset without starting a game, run `bash launch-vr-session.sh --headset-only`.
  4. Do not unplug during a game. The cable serves player 1 only.
- **Docked on wired Ethernet, headset on Wi-Fi: supported.** Put the Deck in a dock with an Ethernet cable to your
  router, so only the headset's half of the link is wireless. Connect by IP as above, using the address the launcher
  prints. Keep the headset on a strong Wi-Fi signal, ideally 5 GHz or better, close to the access point.

## PlayStation VR2 (Linux)

**Supported on SteamOS** with AMD graphics (a Steam Machine, for example). On other Linux it is
experimental. The flatpak carries its own PS VR2 driver (Monado) and its own AMD graphics driver, so nothing else
needs installing on SteamOS. That is the flatpak's route. **The AppImage** uses SteamVR on Linux instead, with Sony's
PlayStation VR2 App under Proton; the full steps, and the traps, are in `PSVR2.md` (Route 2).

- **You need:** a PS VR2 and Sony's **PS VR2 PC adapter**, plugged into a graphics card's DisplayPort and a USB 3
  port. **AMD graphics** is supported; NVIDIA is experimental, through the flatpak's NVIDIA driver extension.
- **On SteamOS** (a Steam Machine or a desktop running SteamOS): switch to the **Plasma X11 session** first, for
  example with `steamos-session-select plasma` in Konsole (it logs you out of the desktop). The default Wayland
  session refuses to hand the headset's display to the driver.
- **Controls:** use a gamepad.
- **On other Linux:** your user needs access to the headset's USB device. If the headset is not found and the PS VR2
  driver's log says it cannot open the USB device, add a udev rule, for example `/etc/udev/rules.d/70-psvr2.rules` containing
  `SUBSYSTEM=="usb", ATTRS{idVendor}=="054c", ATTRS{idProduct}=="0cde", TAG+="uaccess"`, then replug.
- **Start:** plug the headset in, then run `bash launch-vr-session.sh`. It offers the PS VR2 when it sees one plugged
  in; `bash launch-vr-session.sh --runtime monado-psvr2` picks it directly.
- **If the headset is not found:** unplug the adapter's power, wait a few seconds, and plug it back in. Sometimes it
  first connects at a slow USB speed, which the driver cannot use.
- **Using SteamVR with Sony's PlayStation VR2 App:** that is the **AppImage's** route, not the flatpak's. The AppImage
  uses whichever OpenXR runtime is active, and SteamVR on Linux is its supported path for the PS VR2. Start the AppImage
  with `--vr` while SteamVR is running; `PSVR2.md` (Route 2) has the setup and its traps. The flatpak's sandbox does
  not include SteamVR, so with the flatpak use Monado as above.

## Multi-seat VR multiplayer (experimental, Linux)

Several players, each in their own headset. When run from a terminal, the launcher asks how many players are
joining. Players 2 to 4 each use their own WiVRn headset. Every WiVRn headset pairs on its own player's port (player 1 on 9757 when player 1 uses WiVRn), then connects on its
own port: 9758, 9759 or 9760. If your system has a firewall, open 9758-9760 for TCP and UDP too (for example
`sudo ufw allow 9758:9760/tcp` and `sudo ufw allow 9758:9760/udp`); `enable-vr-discovery.sh` opens only 9757.

- **Split split screen** (2 players, one game): the game's own split screen is split, and each player gets their own
  half in their own headset, on their own port. TimeSplitters is the only game with it today.
- **Multi emulator** (2 to 4 players): each player runs their own emulator with their own game file, memory cards and
  saves.

The USB cable serves player 1 only. Windows has no multi-seat VR multiplayer.

## Windows

Windows is not covered by this guide yet. The download is one tree for every
system, so the Linux download carries `launch-vr-session.bat` and `launch-vr-session.ps1` too; on Linux, ignore them.

## Display glasses

Not in this release. RayNeo Air 3s support is in development: the launcher's `penguinlens` option needs PenguinLens,
a separate install that is not out yet.

## No headset

`flatpak run com.penguinvrlabs.penguinscreen2` runs PenguinScreen2 as a normal flat PS2 emulator. VR only ever starts
from `launch-vr-session.sh`.

## Troubleshooting

- **Never stop the WiVRn server while a headset is connected**; it crashes. Take the headset off or quit its app
  first.
- **"Connection refused"** almost always means the headset is not paired yet, not a firewall problem. Pair a second
  or reset headset with `flatpak run --command=wivrnctl io.github.wivrn.wivrn pair`.
- **"Incompatible version"**: update the WiVRn app on the headset and the server on the PC to the same version.
- **A smeared or broken picture after a dropout:** restart the WiVRn app on the headset first.
- **The address changed:** your router gave the PC a new IP address; use the one the launcher prints now.
- **The game booted flat:** the headset was asleep when the game started. Keep it awake until the game is running.
- **Blocky picture on a laptop with two graphics chips:** set `PSCREEN2_GPU` to choose the chip (see `QUICKSTART.md`).
- **NVIDIA, after a driver update:** VR stops working until the flatpak's matching driver extension is installed:
  `flatpak install --user flathub org.freedesktop.Platform.GL.nvidia-<version>`, where `<version>` is the number in
  `/sys/module/nvidia/version` with dots replaced by dashes.

More known problems and their fixes: `KNOWN-ISSUES.md`. Games and their VR features: `GAMES.md`.
