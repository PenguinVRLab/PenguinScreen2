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

## VR connection

- **Steam Deck: wireless VR streaming is NOT supported — use the USB-C cable.**
  Field-tested: the Deck can run the emulator and the VR session, but one
  machine emulating, encoding, and driving the Wi-Fi radio at once cannot
  sustain a usable stream. The supported Deck path is a **data-capable USB-C
  cable** to the headset (see the Quickstart §4 — the launcher handles it).
  Wireless works well from desktop-class PCs; on the Deck it will disappoint,
  and we would rather say so here than let you discover it.

- **VR only starts when you launch with the VR launch script — by design.**
  Running the emulator directly (a plain `pcsx2-qt`, or launching it from a
  desktop icon / Steam) always plays **flat on your monitor**, even with VR
  enabled in Settings. This is deliberate: it guarantees the emulator never
  grabs a headset runtime you didn't ask for (e.g. a background Monado). To
  play in VR, start it with `launch-vr-session.sh`. The VR settings page shows
  a banner telling you when the current session is flat and how to fix it.
- **On SteamOS, WiVRn's own window can say "Server failed to start".** SteamOS
  ships the system mDNS service (Avahi) with announcements disabled, and WiVRn's
  server treats being refused permission to announce itself as fatal — so
  starting it from its own dashboard fails on that OS. **`launch-vr-session.sh`
  works around this** (it starts the server in a mode that doesn't announce),
  which is why the headset then connects **by IP**. Want the PC to appear in the
  headset's list by itself instead? Run `enable-vr-discovery.sh` once — see the
  Quickstart, §4. This is an OS/WiVRn interaction, not an emulator fault.
- **The PC's address can change.** Home networks hand out addresses that change
  after a reboot or a lease expiry, so a headset entry that worked yesterday can
  fail today. The launcher prints the current address every time — re-add the
  server on the headset if it differs. (Auto-discovery, above, avoids this
  entirely.)

## Visual quirks on specific games

- **ESPN NFL 2K5 — brief flicker just after a scene loads.** On-field detail
  can flicker for a moment the first time a scene is shown, and settles once
  that scene has been displayed once. The fingerprint (it goes away when warm)
  points at shader/pipeline compilation rather than anything VR-specific — but
  we have not proven that, and we don't label a bug as somebody else's without
  evidence. Not harmful, just visible. *(A separate and much sharper
  field-geometry flicker in this game was a synchronization bug; it is **fixed**
  in this build.)*
- **A game can boot "flat" if the headset was asleep when it started.** VR
  initializes once per game boot; if the headset had dozed off (taken off,
  proximity sensor) at that moment, that boot stays on the flat desktop
  window. Keep the headset on while a game starts — or quit to the library
  and boot the game again with the headset awake. A mid-session retry is on
  the roadmap.
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
