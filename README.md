# PenguinScreen2

Play your PS2 library in VR — a head-tracked big screen for every game, with real
geometric 3D depth and head-driven cameras where profiled, on a headset streamed
from your own PC or Steam Deck.

PenguinScreen2 is **built on [PCSX2](https://pcsx2.net)**, the long-running
open-source PlayStation 2 emulator. The emulation is PCSX2's work and lineage;
PenguinScreen2 adds the VR presentation layer, per-game 3D tuning, and the
profile system. The emulator — VR code included — is free software
(**GPL-3.0-or-later** — see `COPYING.GPLv3`); the bundled launch VR profiles
are the author's own work, provided under a separate non-commercial license
(see `bin/resources/vr-profiles/LICENSE.md`).

## What you need

- A PC (SteamOS or desktop Linux) and a standalone headset served by
  [WiVRn](https://github.com/WiVRn/WiVRn) or another OpenXR runtime — see
  `STACK.md` for the exact versions this release was built and validated
  against.
- **Your own PS2 BIOS, dumped from your own console.** No BIOS, game images,
  or copyrighted game data are included or downloaded — ever.

## Fully offline by design

PenguinScreen2 never phones home on its own — no update checks, no telemetry,
no analytics. The handful of optional online features (cover-art and font
downloads) only touch the network if you explicitly ask them to. Everything it
needs to run ships in the box, including VR profiles for the launch games. Additional per-game profiles are single files — drop them into your
profiles folder and they're live on next boot.

## Install

From the release page you want the **flatpak** (SteamOS-tuned; runs on any
Linux with flatpak installed). A **source archive** of this exact tree is
there too if you'd rather build it yourself (see `STACK.md`).

**1. Install the emulator** (user scope — no root, works on a stock Steam
Deck in Desktop Mode):

    flatpak install --user -y ./PenguinScreen2-*.flatpak

The first install downloads about 1 GB of shared KDE runtime from Flathub —
one time only; a long progress bar here is normal. No other extension is
required. Reinstalling or upgrading? Add `--reinstall`.

**2. One-time VR setup** (from this folder):

    bash setup-configure-deps.sh

Installs the [WiVRn](https://github.com/WiVRn/WiVRn) streaming server (user
scope, from Flathub), creates the two drop folders below, and fetches `adb`
for the wired USB-C headset link. On the **headset**: install the free WiVRn
client from the store (Meta Horizon Store for Quest).

**3. Drop in your own files** — found automatically at next launch:

    BIOS (dumped from your console)  ->  ~/PS2-BIOS
    games (.iso/.chd/.bin+.cue ...)  ->  ~/PS2-Games

**4. Play in VR:**

    bash launch-vr-session.sh

It starts the VR link, prints this machine's IP address and the one-time
pairing PIN, walks you through pairing, then launches the emulator. In the
headset's WiVRn app, connect **by IP** (SteamOS ships with auto-discovery
blocked; `bash enable-vr-discovery.sh` is the optional opt-in to fix that —
re-run it after each SteamOS update). **Steam Deck: play wired** — docked
with Ethernet, or direct USB-C to the headset (headset Developer Mode
required for the cable).

**5. No headset handy?**

    flatpak run org.penguinvr.penguinscreen2

runs it as a normal flat PS2 emulator — nothing is lost.

The full guided walkthrough, per-game expectations, and known issues:
`QUICKSTART.md` and `KNOWN-ISSUES.md`, shipped alongside this file. (USB
install-kit users also get a generated `INSTALL.txt` tailored to the exact
kit contents.)
