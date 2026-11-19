# vr-mods licensing

## These files are DATA, not part of the emulator

The entries in this folder (`*.yaml`) are original works of **Patrick Carey** (<patrickfcarey@gmail.com>),
authored from scratch: fixes to a game's code or microcode, VR tuning, and descriptions of discs. The emulator
*reads* them at runtime. They are **not** program code and **not** derived from PCSX2. Like the VR profiles
(`../vr-profiles/LICENSE.md`), each is a separate work that the emulator's GPL-3.0 license does not cover.

## What is ours, and what is not

- **Ours:** every `kind: fix` entry, and in a `kind: conversion` entry everything we wrote: the VR profile, the
  requirements, the `fixes:` we made for that conversion, and the facts we recorded (its CRCs, its name, its
  version, its authors).
- **Not ours:** the total conversion a `kind: conversion` entry describes. It belongs to the authors named in the
  entry's `mod.by`. No part of it is in this folder, and nothing here licenses it, grants any right to it, or
  claims it. Its name appears only to identify it.
- **Not ours:** the games these entries fix or describe. No part of any game is in this folder.
- **Not ours: credited values.** An entry whose header says **CREDIT** carries patch values published by the
  authors it names (for example a community 60 fps patch). Those values are theirs, not ours, and are included with
  credit; everything else in such an entry (its guards, notes and the facts we recorded) is ours.

## License for the shipped entries

Copyright © 2026 Patrick Carey. All rights reserved except as granted here.

The entries in this folder that carry the header `SPDX-License-Identifier: CC-BY-NC-4.0` are licensed under
**Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)**:
<https://creativecommons.org/licenses/by-nc/4.0/>. You may **use, share, and adapt** them for **non-commercial**
purposes, with attribution. An entry that does not carry that header is not licensed by this file.

**Commercial rights are expressly reserved.** Any commercial use (selling an entry, bundling one into a paid
product or release, or distributing entries for commercial advantage) requires a **separate written license**
from the copyright holder. Enquiries: <patrickfcarey@gmail.com>.

---

*Nothing here alters the license of the emulator's program code, which remains GPL-3.0. This file governs only
the data in this folder. Each file's own SPDX header names the license that file carries.*
