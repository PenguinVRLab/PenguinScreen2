# PenguinScreen2

Play your PS2 library in VR - a head-tracked virtual screen for every game, with real
geometric 3D depth and head-driven cameras on profiled titles, streamed to a headset from your own PC or Steam Deck.

**Which games get what:** [`GAMES.md`](GAMES.md) lists every game with a VR profile in this release (head tracking,
stereoscopic 3D, and more), and where each game stands.

PenguinScreen2 is **built on [PCSX2](https://pcsx2.net)**, the long-running
open-source PlayStation 2 emulator. The emulation is PCSX2's work and lineage;
PenguinScreen2 adds the VR presentation layer, per-game 3D tuning, and the
profile system. The emulator - VR code included - is free software
(**GPL-3.0-or-later** - see `COPYING.GPLv3`); the bundled launch VR profiles
are the author's own work, provided under a separate non-commercial license
(see `bin/resources/vr-profiles/LICENSE.md`).

## What you need

- A PC (SteamOS or desktop Linux) and a standalone headset served by
  [WiVRn](https://github.com/WiVRn/WiVRn) or another OpenXR runtime - see
  `STACK.md` for the exact versions this release was built
  against. Every headset and host we know about, and how to connect each one:
  [`DEVICES.md`](DEVICES.md).
- **A PlayStation VR2 instead?** It connects by cable through Sony's PC adapter, with no WiVRn and no phone app. Follow
  [`PSVR2.md`](PSVR2.md): the flatpak uses Monado, the AppImage uses SteamVR on Linux.
- **Your own PS2 BIOS, dumped from your own console.** No BIOS, game images,
  or copyrighted game data are included or downloaded - ever.

**Two downloads, two headset paths.** `launch-vr-session.sh` is the flatpak's launcher: it serves Quest-class headsets
over WiVRn and the PlayStation VR2 through Monado, and players 2 to 4 are always WiVRn. (Display glasses will need
PenguinLens, a separate install that is not out yet: see [`DEVICES.md`](DEVICES.md).) It never
uses SteamVR. The **AppImage** is the SteamVR path: it uses whichever OpenXR runtime is active on your PC, so start it with
`--vr` while SteamVR is running, and it is the build to add to Steam as a non-Steam game (a flatpak is not the thing to put
in Steam). One headset per path; [`DEVICES.md`](DEVICES.md) lists every headset, [`PSVR2.md`](PSVR2.md) both PS VR2 routes.

**Multi-seat VR multiplayer (experimental, Linux).** Several players, each in their own headset. **Split split screen**:
one game, and each player gets their own half of its split screen. **Multi emulator**: each player runs their own
emulator with their own game file. [`DEVICES.md`](DEVICES.md) has the setup.

## Fully offline by design

PenguinScreen2 never phones home on its own - no update checks, no telemetry,
no analytics. The handful of optional online features (cover-art and font
downloads) only touch the network if you explicitly ask them to. Everything it
needs to run ships in the box, including VR profiles for the launch games. 

Additional per-game profiles are single files - drop them into your
profiles folder and they're live on next boot.


## Mods 

**Conversions the community wrote** A total conversion boots with the serial of the game it was built from. PenguinScreen2 tells those discs apart 
by CRC, lists them under the mod's own name and version, and offers the settings that disc needs such as Extended RAM, the matching texture pack for that game only, or other mod requiremnets. A small YAML file of ours (or yours) is what makes the emulator treat it as that mod.

Same shape as VR profiles: one versioned file per patch or conversion, drop it in your `vrmods` folder, live on next boot.   

**Patches we wrote.** NASCAR Thunder 2004 - The retail cockpit is a forward view only. PenguinScreen2 clips the vehicle cage, then draws your own car around you (the game normally hides it in cockpit view) and blacks out the inside of the shell. You sit in the cabin instead of looking at the back of the livery. The original window net stays, with more of the same squares so a look left still has net. 

A typical PS2 cheat (a pnach) is a poke into the Emotion Engine, the CPU. This patch also rewrites VU1, the chip that transforms and draws the car - that is how the cage can clip and the interior can go black without a custom emulator.

We license the patches we wrote. We do not license anyone else's conversion or texture pack, and we do not ship them.



## Install

From the release page download the **flatpak** (SteamOS-tuned; runs on any
Linux with flatpak installed). A **source archive** of this exact tree is
there too if you'd rather build it yourself (see `STACK.md`).

**1. Install the emulator** (user scope - no root, works on a stock Steam
Deck in Desktop Mode):

    flatpak install --user -y ./PenguinScreen2-*.flatpak

The first install downloads about 1 GB of shared KDE runtime from Flathub -
one time only; a long progress bar here is normal. No other extension is
required. Reinstalling or upgrading? Add `--reinstall`.

**2. One-time VR setup** (from this folder):

    bash setup-configure-deps.sh

Installs the [WiVRn](https://github.com/WiVRn/WiVRn) streaming server (user
scope, from Flathub), creates the two drop folders below, and fetches `adb`
for the wired USB-C headset link. On the **headset**: install the free WiVRn
client from the store (Meta Horizon Store for Quest).

**3. Drop in your own files** - found automatically at next launch:

    BIOS (dumped from your console)  goes in  ~/PS2-BIOS
    games (.iso/.chd/.bin+.cue ...)  goes in  ~/PS2-Games

**4. Play in VR:**

    bash launch-vr-session.sh

It starts the VR link, prints this machine's IP address and the one-time
pairing PIN, walks you through pairing, then launches the emulator. In the
headset's WiVRn app, connect **by IP** (SteamOS ships with auto-discovery
blocked; `bash enable-vr-discovery.sh` is the optional opt-in to fix that -
re-run it after each SteamOS update). **Steam Deck: Use a USB-C Cable** - 
wireless streaming is not supported on the Deck (headset Developer Mode required for the cable). 
Docked Ethernet is the desktop-class path.

**5. No headset handy?**

    flatpak run com.penguinvrlabs.penguinscreen2

runs it as a normal flat PS2 emulator.

## Windows

Windows is supported, and supported less than Linux. Linux and SteamOS come first; Windows gets what carries over,
one release behind.

- **Today's Windows download is the flat emulator.** It plays on a monitor with VR off. It is the same tree as the Linux
  download, so the Linux scripts are in it; they do nothing on Windows.
- **VR on Windows is a separate `.exe`**, built from the same source with VR on. It talks to **SteamVR only**: no WiVRn,
  no Monado, no PS VR2 driver, and no multi-seat (one headset, one player in VR). It is not in this release.
- Expect less hand-holding: the guides, the launcher's checks and the games list are written against Linux. If something
  is wrong on Windows, open an issue with the game and the log, and we fix Windows after Linux.

The full guided walkthrough, per-game expectations, and known issues:
`QUICKSTART.md` and `KNOWN-ISSUES.md`, shipped alongside this file. Games and
their VR features: `GAMES.md`. Headsets, the Steam Deck, the PS VR2 and how to
connect each: `DEVICES.md`. (USB
install-kit users also get a generated `INSTALL.txt` tailored to the exact
kit contents.)

## Credits

- AudioXR (voice commands in VR): Johnathan Popa ([@jpopo713](https://github.com/jpopo713))
- README improvements: Ali Alam ([@AliAlamUTD](https://github.com/AliAlamUTD))
