# vr-mods: fixes and total conversions, one versioned file each

`vr-mods/` holds three kinds of entry:

- **`kind: fix`**: a fix to a disc's own code or microcode, applied by the emulator like a pnach whether VR is on
  or off.
- **`kind: conversion`**: data about someone else's total conversion: whose it is, the discs (CRCs) that are it,
  its own full VR profile, and what it requires. The game list shows the mod's name and version. The mod itself
  belongs to its authors, never to us (`LICENSE.md`).
- **`kind: feature`**: a switch for code that lives in the emulator itself. It carries no memory writes. A build
  implements each feature at one version and refuses a feature file it does not implement ("this build has no
  feature <name> v<N>"); this build implements none yet.

Kill switch for the fixes: set `PCSX2_VR_MODS=0` in the environment.

## Files

```
vr-mods/<SERIAL>/<name>--v<N>.yaml                              shipped with the emulator
<data folder>/vrmods/<SERIAL>/<name>--v<N>.yaml                 yours; replaces a shipped file of the same <SERIAL>/<name>
```

- **The version is in every file name, no exception, and it is one whole number.** `<name>` is lower-case words
  joined by single hyphens; then `--v` and a whole number from 1 with no leading zero (`cockpit-fix--v1.yaml`,
  `cockpit-fix--v2.yaml`, `some-conversion--v10.yaml`). Dotted or lettered versions (`--v1.1`, `--v1a`) are refused.
- **The newest version runs.** With several versions of one `<name>` installed, only the highest number loads; the
  others are listed at launch as not loaded. A released file never changes: every change ships as a new version.
- **`enabled: true` or `enabled: false` is required in every file**, exactly those two words. It records whether
  the mod is on. Your copy of a file (same name, in your folder) is how you switch a shipped mod; your switch for
  an older version carries to the newest one. If your copy differs from the shipped file in anything but
  `enabled:`, the log says so.
- **The folder is the serial the disc boots with**, and the entry's top key must be that serial.
- **One entry per file.** A second version is a second file.
- **Enable one version at a time.** Two enabled files for one game that write the same `ee` word (or the same VU1
  micro bytes), or where a guard of one reads a word the other writes, refuse each other: neither applies, and the
  log and the screen name both files and the address.
- **`version:`** in the entry must equal the file name's number. A conversion's `mod.version` is its authors' own
  version, written as they write it.
- **`restart_required:`** (`true` or `false`, left out `true`): whether switching the mod needs a restart. The
  Mods menu says "requires restart" under a mod exactly when its file says `true`.
- **`name:`** is a short title of at most 40 characters: what the mod is, in a few words, with no game name, no
  credit and no description of how it works. A longer name still loads (the log says so) and the Mods menu cuts it.
- **`description:`** (optional) is one plain sentence of at most 100 characters saying what the player gets. The
  Mods menu shows it under the name and cuts a longer one. A list or a map there is left out, and the log says so.
- **`origin:`** says whose work the entry is: `PenguinVR` (exactly that) for our own work, or the authors' names as
  plain text (`origin: "Jane Doe and John Roe"`). When nobody knows who wrote it, it may say so in plain text
  (`origin: "unknown (a 2003 forum tweak)"`). The older form, a map naming the tweak and its authors
  (`origin: { name: "...", by: "..." }`), still loads. An empty `origin:`, a list, or a map without both `name` and
  `by` is refused. Every file shipped with the emulator says `origin: PenguinVR`. The menu does not show it.
- **`dependencies:`** names the mods (of the same game) this one needs, each with a version range in npm's syntax:
  `dependencies: { other-fix: ">=1" }`; also `2`, `^1`, `~1`, `1 || 3`, `1 - 3`, `*`. Each version N reads as
  N.0.0. A mod that is on turns its switched-off dependencies on at launch (the screen and log say so, and your copy
  records it). A mod is refused when a dependency is not installed, is refused itself, has no version in range, would
  write the same place as a mod that is on, or depends on itself through a cycle.
- A file that breaks any of these is refused and listed at launch, like a broken VR profile.

## Schema: a fix

The addresses below are placeholders, not taken from any real entry.

```yaml
SLUS-XXXXX:                       # the folder's serial
  - name: "Short title"           # at most 40 characters; shown in the menu and the log (a one-item list or a plain map, either)
    description: "What the player gets, in one sentence."   # optional, at most 100 characters
    origin: PenguinVR             # PenguinVR for our own work, else the authors' names as text
    kind: fix
    version: "1"                  # = the file name's number
    crcs: [0x12345678]            # the RUNNING ELF's CRC; omit = any ELF of this serial
    enabled: false                # REQUIRED: true or false; false keeps the entry on file but never applies it
    restart_required: true        # optional (left out: true)
    dependencies: { other-fix: ">=1" }   # optional: mods of this game it needs, npm-style ranges
    netplay:                      # where it may run; REQUIRED on a shipped file (offline only without it)
      effect: local               # local | respecting | changing (optional)
      runs_in: [offline, lan]     # a non-empty list from offline, lan, online
      gate: { address: 0x00100008, lan: 0x1, online: 0x2 }   # REQUIRED when runs_in lists one of lan/online
      roles: [host]               # optional: host or joiner; one role REQUIRES gate.role:
      # gate: { ..., role: { pointer: 0x0010000C, offset: 0x4, host: 0x95, joiner: 0x96 } }   (or address:)
      # both_must_run: true       # REQUIRED (true or false) for effect: changing in lan/online; true ships enabled: false
      # online_approval:          # REQUIRED when runs_in lists online and the effect is not local:
      #   - { service: "the service", by: "its owner, who approved this mod" }
    guards:                       # EE words that must hold before the `ee` words are written
      - { address: 0x00100000, equals: 0x00000000, width: 4 }   # width 1, 2 or 4
    vu1:                          # VU1 microcode, written into micro memory (any number of patches)
      - micro: 0x3000             # byte address, 8-aligned; the patch must fit below 0x4000
        match:                    # REQUIRED: the program this patch extends, by its own words
          - { micro: 0x0800, words: [0x00000000, 0x00000000] }
        words: [ ... ]            # lower, upper pairs (whole instructions)
    ee:                           # EE words re-asserted every vsync (like pnach `patch=1`)
      - { address: 0x00100004, value: 0x00000000, original: 0x00000000 }
```

Every number is hex. With the network adapter off a mod needs `offline`; with it on, `lan` or `online` (the emulator
cannot tell the two apart at boot, so a mod listing one of them must check the game's own word, named in `gate`).
Gate and role words are logged at runtime when they show a mode or role the mod excludes; nothing is switched off.
On a LAN connection each emulator sends the other its applied mods, its timing settings (EE cycle rate and skip,
speed, frame limiter, GameDB patches on or off) and its active patches and cheats, and logs every difference. A mod
both machines must run (`both_must_run: true`) is named loudly when the other side does not have it.

## Schema: a conversion

```yaml
SLUS-XXXXX:
  kind: conversion
  version: "1"                    # = the file name's number (some-conversion--v1.yaml)
  enabled: false                  # REQUIRED: switches its fixes (below)
  mod:                            # whose mod it is (required)
    name: "Some Conversion"       # the game list shows "Some Conversion (v1.0.0)"
    version: "1.0.0"              # the authors' own version, as they write it
    by: "its authors"             # never us
    url: "https://..."            # optional
  crcs: [0x........]              # required, non-empty; no CRC of the serial's own profile may repeat here
  requires: { extraMemory: true } # offered to the user at boot, never applied silently
  stereo: { ... }                 # its own full VR profile, the same keys as a vr-profiles entry;
  camera: { ... }                 #   nothing is inherited from the base game's profile
  fov: { ... }
  fixes:                          # optional: patches for this conversion, the fix schema without kind/version
    - { name: "...", guards: [...], vu1: [...], ee: [...] }   # they apply to this conversion's crcs only
```

Every number is hex (with or without `0x`). An invalid fix inside an otherwise valid file is skipped with a
warning naming the file and the reason.

## How a `vu1` patch is applied

- **Shadow copy:** every VIF1 `MPG` upload updates a shadow of VU1 micro memory.
- **When it is written:** after an upload, a patch is written if all of its `match` signatures hold in
  the shadow and the patch is not already there.
- **How it is written:** through the path the upload itself took, so the microVU cache sees it and the game's
  next `MSCAL` can run it.
- **Why the signatures are required:** they keep a patch out of a program it was not made for. Sign every
  routine the patch calls or jumps into.
- **Re-application:** a patch that a later upload overwrites is written again on the next matching upload.
- **After a save state load:** every patch is written again.

## How an `ee` word is applied

Every vsync, for the running game: if every guard holds, each word that differs is written. Re-asserting
each vsync is what makes a word survive a loader writing its data after the code, which a one-shot write does
not. Nothing is restored on a game change: that memory belongs to the next program.
