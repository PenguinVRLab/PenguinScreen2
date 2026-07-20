# PenguinScreen2

Play your PS2 library in VR — a head-tracked big screen for every game, with real
geometric 3D depth and head-driven cameras where profiled, on a headset streamed
from your own PC or Steam Deck.

PenguinScreen2 is **built on [PCSX2](https://pcsx2.net)**, the long-running
open-source PlayStation 2 emulator. The emulation is PCSX2's work and lineage;
PenguinScreen2 adds the VR presentation layer, per-game 3D tuning, and the
profile system. The emulator is free software (**GPL-3.0** — see
`COPYING.GPLv3`); the bundled launch VR profiles are the author's own work,
provided under a separate non-commercial license (see
`bin/resources/vr-profiles/LICENSE.md`).

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

Grab both artifacts from the release page:
- the **flatpak** (SteamOS-tuned; runs on any Linux with flatpak installed), and
- the **source archive** of this exact tree (build it yourself: see `STACK.md`).

Quickstart, per-game expectations, and known issues: see the docs shipped in
this repository.
