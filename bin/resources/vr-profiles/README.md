# VR profiles

One file per game, named by the disc serial (`SLUS-20851.yaml`). A profile
holds the VR tuning for that game: where the virtual screen sits and how much
stereo depth the picture gets. The profiles are licensed separately from the
emulator; see `LICENSE.md` in this folder.

## Changing a profile

Do not edit the files here. They are replaced on every update and are
read-only in a flatpak install.

1. Copy the game's file into your **user profiles folder**. The emulator
   creates that folder on first use and leaves a short note in it, along with
   a ready-to-edit copy of one shipped profile, `SLUS-20851.yaml` (Ace Combat
   5). Because that copy is in your folder, it is the one in effect for that
   game.
2. Edit the copy. A user file overrides the shipped file for the same serial,
   whole file for whole file.
3. A log line containing `user profile overrides shipped` confirms your copy
   is in effect. It appears whenever the profiles are loaded: when the
   emulator starts, and again after you change a profile file.

To go back to the shipped profile, delete your copy. If you delete the seeded
`SLUS-20851.yaml`, the emulator recreates it as a fresh copy of the shipped one,
which gives the same result. While any copy of a game's profile is in your
folder — including the untouched seed — updates to that game's shipped profile
do not reach you until you delete it.

Every profile is checked at launch. An invalid file is reported in a dialog
that names the file and the reason, and that one game falls back; other games
are unaffected. Keys the emulator does not know are ignored, so a newer profile
still loads on an older build.

## File layout

```yaml
SLUS-20851:              # the disc serial; must match the file name
  name: "Ace Combat 5: The Unsung War"   # display name
  crcs: [0x39B574F0]     # optional: only apply to these disc CRCs (hex)
  tier: stereo           # screen | stereo | immersive
  screen: { ... }        # where the virtual screen sits
  stereo: { ... }        # depth tuning
```

`tier` states what the profile is meant to provide: `screen` is a flat virtual
screen, `stereo` adds per-eye depth, `immersive` adds a head-driven camera. It
is a label: it does not switch anything on or off. A value outside those three
rejects the profile.

## `screen`: the virtual screen

| Key | Meaning |
|---|---|
| `distance` | metres in front of you |
| `height` | metres above the floor |
| `follow` | `head` keeps the screen in front of you as you turn; `world` leaves it fixed in the room |
| `arc` | curve of the screen in degrees; below `5` the screen is flat. Needs a VR runtime with cylinder-layer support, otherwise the screen is shown flat |

A `distance`, `height` or `arc` written here wins over the matching slider in
the settings window for that game. Leave a key out to keep it adjustable from
the settings window.

## `stereo`: depth

| Key | Meaning |
|---|---|
| `separation` | how far apart the two eyes' images are pushed. Larger is deeper. Start small: in the shipped profiles the values directly under `stereo:` (outside `scenes`) run from `0.007` to `0.02` |
| `convergence` | the depth that sits exactly at the screen surface. Content nearer than this stays at the screen; content beyond it goes into the screen. The useful range depends entirely on the game: in the shipped profiles the values directly under `stereo:` run from `0.005` to `6.0`, so adjust from the shipped value and do not copy one game's number to another |

Depth goes *into* the screen, never out toward you, as long as `separation`
is positive. The emulator does not check its sign, and a negative value turns
the depth inside out. A negative `hudCollimate` `disparity` (below) likewise
places aiming marks in front of the screen; it is accepted with a warning. Keep
both positive.

If far scenery looks doubled or is uncomfortable to look at, lower
`separation`. If the whole picture looks flat, raise it, or lower
`convergence`. This applies to the plain pair; when a depth map is in use,
adjust the map's own values instead (see below).

### Different depth for different parts of a game

One `separation` / `convergence` pair rarely suits both gameplay and menus, or
both a cockpit and the world outside it. Two tools cover that.

**`scenes`** switches the pair when the game changes state. The shipped
profiles use this for menus and alternate camera views. Each rule has a `when`
block that identifies the game state; leave those as shipped. The values you
can adjust are each rule's own `separation` and `convergence`. A rule that
leaves one out inherits the base value. The first matching rule wins.

**`map: bands`** gives each depth range its own setting, so near objects and
the distant world can both have comfortable depth at once:

```yaml
stereo:
  map: bands
  splits: [...]          # where one band ends and the next begins, near to far
  bands:                 # exactly one more entry than splits; first is nearest
    - { conv: 2.0, sep: 0.008 }
    - { conv: 2.0, sep: 0.006 }
    - { conv: 8.0, sep: 0.009 }
```

At most 3 splits and 4 bands. `sep` must be greater than 0 and `conv` must not
be negative. The joins between bands are worked out for you so depth stays
continuous. An invalid band set is reported in the log and ignored: in the
block directly under `stereo:` the game falls back to the plain `separation` /
`convergence` pair, and in a scene it falls back to whatever the block under
`stereo:` uses. A band set that exceeds the
comfort budget is loaded with a warning.

`map: log` is a smooth alternative with no band edges:

```yaml
stereo:
  map: log
  log: { w0: 2000, w1: 22000, dfar: 0.02 }   # near anchor, far anchor, depth reached at the far anchor
```

`w1` must be greater than `w0`, `w0` greater than 0, and `dfar` must not be
negative.

While `map` is `bands` or `log`, the plain `separation` / `convergence` pair is
not used, in the base block or in a scene: adjust the band or `log` values
instead. The plain pair takes over only if the map is invalid.

### HUD aiming marks: `hudCollimate`

Flat HUD elements stay at the screen surface, which is right for text. Aiming
marks such as a target bracket are easier to use when they sit at the depth of
the thing they surround. `hudCollimate` moves matched HUD draws to a fixed
depth.

`disparity` is that depth. Values above `0.0135` load with a comfort warning,
and values above `0.01815` make the emulator ignore the `hudCollimate` block;
the rest of the profile still loads. The `rules` list selects which HUD
draws are affected and is specific to each game; leave it as shipped. This
feature works on the Vulkan renderer only.

## Everything else

Shipped profiles also contain `camera`, `writes`, `guards`, `base` and similar
blocks. These tie the profile to one specific game build and are not
documented here. Changing them can crash the game or corrupt a session. Leave
them exactly as shipped.
