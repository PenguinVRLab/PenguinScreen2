# PenguinScreen2 — Quickstart

From nothing to a PS2 game in VR. Read top to bottom the first time; it takes
about 20 minutes, most of it one-time setup. **You bring two things the
software never includes: your own PS2 BIOS and your own game discs/images.**

> **MVP scope:** this release targets **desktop mode** (launch from the
> desktop, headset streamed). Big Picture / Game Mode is a bonus where it
> works and is called out as such — it never blocks the desktop path.

---

## 1. What you need

- A PC running **SteamOS or desktop Linux**, and a **standalone VR headset**
  (Quest-class) reachable over your network.
- **[WiVRn](https://github.com/WiVRn/WiVRn)** as the OpenXR runtime — the
  wireless link between the PC and the headset. Exact validated version:
  see `STACK.md`.
- **Your own PS2 BIOS**, dumped from a console you own.
- **Your own PS2 games** — dumped disc images (`.iso`, `.bin`, `.chd`,
  `.cso`, `.zso`, `.gz`, `.mdf`) **or a physical PS2 disc in the drive**
  (PenguinScreen2 reads original discs directly).

Nothing here is included or downloaded for you. No BIOS, no games, no phone
home — ever. (See the README's "fully offline by design.")

---

## 2. Install

PenguinScreen2 ships as a single **`.flatpak` bundle** you download. Install it:

```
# one-time: add Flathub so the app's runtime can be pulled automatically
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
# install PenguinScreen2 from the file you downloaded
flatpak install --bundle PenguinScreen2-<version>.flatpak
```

If the install reports a missing runtime, install it explicitly and retry:
`flatpak install flathub org.kde.Platform//6.10`.

Then run the one-time VR setup below.

**One-time setup** (installs the WiVRn VR runtime, and prepares everything):

```
./setup-configure-deps.sh
```

This is the *only* script that touches system packages, and it installs the
minimum — the VR runtime and its dependencies. It also creates two folders
in your home directory:

- **`~/PS2-BIOS`** — drop your dumped PS2 BIOS file here
- **`~/PS2-Games`** — drop your dumped game images here

The emulator knows these folders: on first launch it finds your BIOS there
and adopts both automatically.

---

## 3. Drop your files in — that's the whole setup

Copy your BIOS into `~/PS2-BIOS` and your games into `~/PS2-Games`.
**Any filenames work, for both** — BIOS images are recognized by their
content, and games are identified by what's inside the image, so there is
nothing to rename and nothing to configure. From here on you're hands-off:
new games dropped into the folder simply appear in the library at the next
launch.

First launch opens **straight to your game library** — no setup screens.
The BIOS is found automatically; pick a game and play.

Two cases where you'll see a screen anyway:

- **Using a gamepad?** The keyboard is mapped out of the box; a gamepad
  needs one visit to **Settings → Controllers → Automatic Mapping** (once,
  ever).
- **Skipped the script, or dropped no files?** The Setup Wizard appears and
  walks you through the same choices by hand — its BIOS page also detects
  dropped files automatically. (RetroAchievements appears there but is
  disabled in this build — see Known Issues.)

---

## 4. Headset — connect over WiVRn

1. On the **PC**, start the VR session launcher:

   ```
   ./launch-vr-session.sh
   ```

   It brings the WiVRn server up on the PC — **on the right GPU** — and
   points the emulator's OpenXR at it. Leave it running. (On laptops with
   two GPUs it automatically uses the powerful one; see §7 if you ever need
   to override that.)
2. On the **headset**, open the **WiVRn** app and connect to your PC:
   - On most desktop Linux, the PC shows up in the list by itself.
   - **On SteamOS it will NOT** (the OS blocks mDNS announcements) — choose
     **Add server / Connect by IP** and type the address the launcher
     printed (it detects and prints your PC's IP for exactly this reason).
3. **First time only — pairing.** A fresh WiVRn accepts no headset until
   it's paired once: in the WiVRn window on the PC click **Pair new
   headset** (a PIN appears), connect from the headset, enter the PIN.
   The headset drops into a WiVRn waiting room — paired forever. The
   launcher detects an unpaired install and walks you through this.
4. Back on the PC, the emulator is up. Boot a game from the library (or
   insert a disc).

You should now be looking at the game on a virtual screen in the headset,
tracking with your head.

---

## 5. Playing — views, comfort, recenter

- **The virtual screen tracks your head.** Look around; the screen stays put
  in the world. This works for **every** game.
- **Depth (stereo 3D)** and **head-driven camera** are enabled per game by a
  **profile** — see §6.
- **Recenter** re-places the screen straight ahead **at your current eye
  height** — use it whenever the screen has drifted (you moved, sat down,
  or the headset's floor level shifted). Default: hold **L1+R1+L3+R3** on
  your controller — any controller, it reads the virtual pad, no setup. A
  keyboard binding also lives in **Settings → Hotkeys → VR**.
- **Comfort:** if a game feels like too much depth or objects "double" up
  close, that's the per-game stereo tuning — the shipped launch games are
  tuned; see §6 for adjusting others.

---

## 6. Per-game profiles — what "tuned" means

PenguinScreen2 renders games at one of three tiers, chosen by the game's
**profile**:

- **Screen** — a head-tracked flat virtual screen. **Every game gets this**,
  no profile needed.
- **Stereo** — real per-eye geometric depth on that screen. Needs a tuned
  profile.
- **Immersive** — head-driven in-game camera. Needs a deeply tuned profile.

**This release ships tuned profiles for its launch games.** Other games run
at the universal Screen tier out of the box.

**Profiles are plain files you can edit and add:**
- Shipped profiles live in the install's `resources/vr-profiles/` (read-only).
- **To customize one, copy its file into your user profiles folder and edit
  the copy** — a user file overrides the shipped one for that game. Your
  folder:
  `~/.var/app/org.penguinvr.penguinscreen2/config/PenguinScreen2/vrprofiles/`
  (a `README.txt` appears there on first run explaining the same).
- One file per game. Every profile is checked at launch; a broken file is
  reported by name so you know exactly what to fix.

---

## 7. If something goes wrong

- **No image in the headset** — is `launch-vr-session.sh` still running on
  the PC, and did the headset's WiVRn app connect? Both must be up.
- **The PC never appears in the headset's server list** — normal on SteamOS
  (mDNS publishing is blocked). Use **Connect by IP** with the address the
  launcher printed.
- **"Connection refused" when connecting by IP** — the headset isn't paired
  yet. In the WiVRn window on the PC: **Pair new headset** → enter the PIN
  on the headset. One-time.
- **Your BIOS file doesn't appear in the list** — names never matter (files
  are detected by content), so a missing entry means the file isn't a valid
  BIOS dump. Re-dump it from your console.
- **Blocky / smeary image on a two-GPU laptop** — the launcher forces the
  discrete GPU (the weak one's video encoder causes exactly this). If it
  mis-detected your hardware, relaunch with `PSCREEN2_GPU=vvvv:dddd` (your
  GPU's PCI id) or `PSCREEN2_GPU=off`.
- **A game boots flat / no depth** — it has no stereo profile yet; that's
  expected for non-launch titles (Screen tier).
- **"Invalid VR profile" dialog at launch** — a profile file you edited has
  a mistake; the dialog names the file and the reason. Fix or remove it.
- **Known visual quirks on specific games** — see `KNOWN-ISSUES.md`.

Built on PCSX2. Your BIOS and games are yours; the VR is ours; the emulator
is free software. Have fun.
