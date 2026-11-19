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
  (Quest-class) reachable over your network. (A **PlayStation VR2** works
  too, by cable: see `PSVR2.md` and skip the WiVRn parts.)
- **[WiVRn](https://github.com/WiVRn/WiVRn)** as the OpenXR runtime — the
  wireless link between the PC and the headset. The version this release uses:
  see `STACK.md`.
- **Your own PS2 BIOS**, dumped from a console you own.
- **Your own PS2 games** — dumped disc images (`.iso`, `.bin`, `.chd`,
  `.cso`, `.zso`, `.gz`, `.mdf`) **or a physical PS2 disc in the drive**
  (PenguinScreen2 reads original discs directly — any ordinary DVD-capable
  drive works; no special hardware. Discs do **not** appear in the game
  library: boot them via **System → Start Disc**, see §3).

Nothing here is included or downloaded for you. No BIOS, no games, no phone
home — ever. (See the README's "fully offline by design.")

---

## 2. Install

PenguinScreen2 ships as a single **`.flatpak` bundle** you download. Install it:

```
# one-time: add Flathub (user scope — no password) so the app's runtime can
# be pulled automatically
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
# install PenguinScreen2 from the file you downloaded (user scope, like
# everything in this guide — the two scopes don't share runtimes)
flatpak install --user -y ./PenguinScreen2-<version>.flatpak
```

First install pulls about 1 GB of shared runtime from Flathub — let it
finish. If the install reports a missing runtime, install it explicitly and
retry: `flatpak install --user flathub org.kde.Platform//6.10`.
Reinstalling the same version? Add `--reinstall`.

**Upgrading from 1.0-rc2? Read this before you play.** This release has a new
app ID, `com.penguinvrlabs.penguinscreen2` (1.0-rc2 was
`org.penguinvr.penguinscreen2`). Flatpak treats them as **two separate apps**:
installing this one does **not** remove rc2, and you will see two
PenguinScreen2 icons.

- The **first** time you start the new version, it copies your rc2 settings,
  memory cards, BIOS and headset setup across. It does this **once**, and it
  never changes or deletes anything in rc2's copy.
- After that the two apps keep **separate** saves. Anything you save in rc2
  from then on stays in rc2 and never comes across.

So, once:

1. Start the new version, open your game, and check that your saves are
   there.
2. Then remove rc2, so you can't open the old one by mistake:

```
flatpak uninstall --user org.penguinvr.penguinscreen2
```

Uninstalling keeps rc2's data folder (`~/.var/app/org.penguinvr.penguinscreen2/`).
Delete that folder yourself only when you are sure you no longer need it.

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

> **Have a PlayStation VR2?** It connects by cable and does not use WiVRn. Skip this section and follow `PSVR2.md`.

1. On the **PC**, start the VR session launcher:

   ```
   bash launch-vr-session.sh
   ```

   It brings the WiVRn server up on the PC — **on the right GPU** — and
   points the emulator's OpenXR at it. Leave it running. (On laptops with
   two GPUs it automatically uses the powerful one; see §7 if you ever need
   to override that.)
2. On the **headset**, open the **WiVRn** app and connect to your PC:
   - On most desktop Linux, the PC shows up in the list by itself.
   - **On SteamOS it will NOT** (the OS blocks mDNS announcements) — choose
     **Add server / Connect by IP**, type the address the launcher printed,
     and use port **9757**. (Want it to just appear in the list instead? See
     **Auto-discovery** below — one optional command.)
3. **First time only — pairing.** A fresh WiVRn accepts no headset until
   it's paired once. Connect from the headset; when it asks for a **PIN**,
   read the PIN the launcher printed **in its terminal on the PC** and enter
   it on the headset. The headset drops into a WiVRn waiting room — paired
   forever. The launcher detects an unpaired install and walks you through
   this.
4. Back on the PC, the emulator is up. Boot a game from the library — or,
   for a physical disc, use **System → Start Disc** (a disc in the drive
   never shows up in the library by itself).

You should now be looking at the game on a virtual screen in the headset,
tracking with your head.

**Steam Deck (and Deck-class hardware): use the USB-C cable.** One machine
emulating, encoding, *and* running the Wi-Fi radio cannot sustain the stream.
Plug a **data-capable USB-C cable** from the
Deck to the headset instead; the launcher detects it and routes the stream
over the cable (no Wi-Fi involved):

1. One-time, on the headset, **one of two ways**:
   - **Easiest (WiVRn app 26.9 or newer on Horizon OS 2.6 or newer, no
     developer account):** in the headset's WiVRn app, Settings → System →
     **USB networking** → On. Then run `bash enable-vr-discovery.sh` once on
     the Deck (it asks for your password; it lets the headset find the Deck
     over the cable and adds the network profile the link needs).
   - **Otherwise:** turn on **Developer Mode** (Meta Horizon phone app →
     Devices → your headset → Headset settings → Developer Mode; it needs a
     free developer account) and run `bash setup-configure-deps.sh` online
     once so it fetches `adb` (no root needed).
   The launcher tells you which of the two it sees, or that it sees neither.
2. Plug in and run `bash launch-vr-session.sh`. On the Developer Mode route the
   headset shows "Allow USB debugging?" the first time: put it on, tick
   *Always allow*, OK. The launcher waits for that; no need to run it again.
3. First connect only: the launcher prints a **PIN**. Its WiVRn app opens on
   the headset by itself; type the PIN when it asks. Paired for good.
   If the app shows its server list instead, connect by hand:
   **Add server / Connect by IP → `127.0.0.1`**, port 9757, and
   turn on **TCP only** (the cable carries TCP only; without it the connection
   times out). It persists as "localhost". Not connecting? Turn the headset's
   Wi-Fi off while it is on the cable.

To connect the headset **without starting a game** (to check the cable, or
before you pick a game), run `bash launch-vr-session.sh --headset-only`: it
does steps 2-3 and stops. A later `bash launch-vr-session.sh` finds WiVRn
already running.

On a desktop PC with a proper GPU, Wi-Fi streaming works well — the cable is
the *Deck* answer, not a general requirement.

**Optional — Auto-discovery (skip typing the IP).** SteamOS ships with the
mDNS announcement disabled — that announce is what lets the headset find the PC
by itself, which is why you Connect by IP above. To turn it on, run once (it
asks for your password):

```
bash enable-vr-discovery.sh
```

Then **stop the VR link and start it again** — whether the PC advertises itself
is decided when the link starts, so a session already running won't pick up the
change:

```
pkill -f wivrn-server     # take the headset off first
bash launch-vr-session.sh
```

After that the PC appears in the headset's server list by name — no IP typing.
**Re-run `enable-vr-discovery.sh` once after a SteamOS *update*** (updates reset
system files and revert the change; plain reboots are fine). Prefer to just type
the IP each time? You never need this script.

> WiVRn's own window (`flatpak run io.github.wivrn.wivrn`) is where codec,
> bitrate and unpair live. It attaches to the running link — it won't start a
> second one.

**When WiVRn updates — keep both sides on the same version.** The headset's
WiVRn app updates itself through its store; the PC side is a plain Flathub
install that only updates when you tell it to. A newer app refuses an older
server ("incompatible version" on connect), so whenever the headset app has
updated, update the PC too:

```
flatpak update --user io.github.wivrn.wivrn
```

Then **take the headset off, stop the old server, and launch again** — a server
that was already running keeps the old code until it is restarted:

```
pkill -f wivrn-server     # headset off first
bash launch-vr-session.sh
```

The launcher prints the installed server version every time it runs, next to
the update command, so a mismatch is visible before you put the headset on.

**If the picture goes bad** — smearing or corrupted blocks, especially right
after a connection drop or a "connection reset" — **restart the WiVRn app on
the headset FIRST**, then reconnect. A broken session can leave the headset's
video decoder wedged in a state no PC-side restart can reach; closing and
reopening the headset app is the actual fix, and it takes ten seconds.

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
  `~/.var/app/com.penguinvrlabs.penguinscreen2/config/PenguinScreen2/vrprofiles/`
  (a `README.txt` appears there on first run explaining the same).
- One file per game. Every profile is checked at launch; a broken file is
  reported by name so you know exactly what to fix.

---

## 7. If something goes wrong

- **No image in the headset** — is `launch-vr-session.sh` still running on
  the PC, and did the headset's WiVRn app connect? Both must be up.
- **WiVRn's own window says "Server failed to start"** — expected on SteamOS:
  the OS blocks the mDNS announcement and WiVRn treats that refusal as fatal.
  Start VR with `bash launch-vr-session.sh` instead (it starts the server in a mode
  that doesn't announce, then tells you the IP to connect to). To make the
  dashboard work — and get auto-discovery — run `enable-vr-discovery.sh` once
  (§4).
- **The PC never appears in the headset's server list** — normal on SteamOS
  (mDNS publishing is blocked). Use **Connect by IP** with the address the
  launcher printed.
- **"Connection refused" when connecting by IP** — the headset isn't paired.
  On a *first* headset the launcher prints the **PIN** in its terminal. For **any
  other** headset (a second one, a factory-reset one, or after restoring a
  backup) WiVRn will not offer a PIN by itself — ask for one:
  ```
  flatpak run --command=wivrnctl io.github.wivrn.wivrn pair
  ```
  Then enter that PIN on the headset. (Also check you used port **9757**.)
- **Your BIOS file doesn't appear in the list** — names never matter (files
  are detected by content), so a missing entry means the file isn't a valid
  BIOS dump. Re-dump it from your console.
- **Blocky / smeary image on a two-GPU laptop** — the launcher forces the
  discrete GPU (the weak one's video encoder causes exactly this). If it
  mis-detected your hardware, relaunch with `PSCREEN2_GPU=vvvv:dddd` (your
  GPU's PCI id) or `PSCREEN2_GPU=off`.
- **A game boots flat / no depth** — it has no stereo profile yet; that's
  expected for non-launch titles (Screen tier).
- **Start Disc says "no CD/DVD-ROM devices"** — three causes, in order:
  the drive must be DVD-capable (`udevadm info /dev/sr0 | grep ID_CDROM_DVD`
  should print `=1`); your user must be able to read it (be in the `optical`
  group on SteamOS/Arch, `cdrom` on Debian/Ubuntu — check with `groups`);
  and flatpak builds **before rc3** could not see drives at all (a sandbox
  hole was missing) — update to the current build.
- **"Invalid VR profile" dialog at launch** — a profile file you edited has
  a mistake; the dialog names the file and the reason. Fix or remove it.
- **Known visual quirks on specific games** — see `KNOWN-ISSUES.md`.

Built on PCSX2. Your BIOS and games are yours; the tuned VR profiles are our
work (CC BY-NC); the emulator — VR code included — is free software
(GPL-3.0-or-later). Have fun.
