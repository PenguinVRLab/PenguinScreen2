# PenguinScreen2 — Known Issues

Honest state of this release. None of these stop you from playing; they're
here so a quirk you notice has an explanation instead of a surprise.

## Feature status

- **RetroAchievements is disabled in this build.** The Achievements settings
  page is present but greyed out. This fork isn't yet a registered
  RetroAchievements client; achievements return once it's registered
  (their process allows it). Nothing you do enables it in the meantime.
- **Deep VR tuning ships for the launch games only.** Every game gets the
  universal head-tracked virtual screen out of the box. Per-eye 3D depth and
  head-driven cameras are enabled by per-game profiles, and the tuned set
  starts with the launch titles and grows. A game with no stereo profile
  runs flat-on-a-screen — that's expected, not a fault.

## Visual quirks on specific games

- **ESPN NFL 2K5 — brief rendering flicker in some scenes.** Field texture or
  on-field detail can momentarily flicker, most visible right after loading a
  scene. This is a bug in the underlying **PCSX2 Vulkan renderer** (it happens
  in stock PCSX2 too, not something the VR layer introduced) — we've reported
  it upstream. It's timing-related: once the shader cache is warm (after the
  scene has been shown once), it largely settles. Not harmful, just visible.
- **Very-near objects can feel like "too much depth" in some games.** If
  something close (a car dashboard, a player right in front of you) is hard
  to fuse into one image, that's the per-game stereo strength. The launch
  games are tuned for comfort; for others you can lower the profile's
  `separation` value (see the Quickstart, §6) until it fuses.

## Comfort notes

- **Recenter is your friend.** If the virtual screen drifts off-center or
  ends up too high/low (you shifted position, or the headset's floor level
  moved), recenter puts it straight ahead at your height. Bind it under
  Settings → Hotkeys → VR.
- **Take breaks.** VR fatigue is real and sneaks up on you; stop before you
  feel it, not after.

## Reporting

Found something not listed here? Note the **game**, what you saw, and where
(a specific scene helps enormously). A precise report — "the numbers on a
player's jersey flicker in the right eye at the line of scrimmage" — is worth
ten vague ones.

---

*Built on PCSX2. Issues in the underlying emulator are shared with upstream
PCSX2; issues in the VR presentation layer are ours to fix.*
