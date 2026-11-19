# Connecting a PlayStation VR2

This page takes you from "I own a PS VR2" to "I am looking at a PS2 game in it". There are **two routes**, one for
each build of PenguinScreen2. Pick the one for the build you run. Every other headset is in
`DEVICES.md`.

| You run | Route | Where it is on this page |
|---|---|---|
| The **flatpak** | **Monado's native driver**, built into the flatpak. Nothing else to install. | Route 1 |
| The **AppImage** | **SteamVR on Linux**, with Sony's PlayStation VR2 App running under Proton. | Route 2 |

Both routes are wired: the headset connects through Sony's PC adapter, with no pairing, no PIN and no Wi-Fi.
The Monado route is the smoother of the two and the one to pick when you can run the flatpak. The SteamVR route
works, but it takes more setup and has more traps; Route 2 lists them.

## Route 1: the flatpak, through Monado

### What works today

| Computer | Status |
|---|---|
| A **Steam Machine** (or another SteamOS desktop) with **AMD graphics**, in the **Plasma X11 session** | **Supported.** |
| Other desktop Linux with AMD graphics | **Experimental.** The same driver and launcher. |
| Linux with **NVIDIA** graphics | **Experimental.** The launcher chooses the NVIDIA-capable driver path on its own and tells you so. Please report how it goes. |
| Windows | Not in this release. |

PS VR2 support is built into the PenguinScreen2 flatpak: it carries its own PS VR2 driver (Monado) and its own AMD
graphics driver. You do **not** install SteamVR, Sony's PlayStation VR2 app, or any driver for this route. (SteamVR is Route 2, for the AppImage.)

### What you need

- A **PS VR2** and Sony's **PS VR2 PC adapter**. The adapter goes into a **DisplayPort** output of your graphics card
  and a **USB 3** port (and it needs its own power). On a computer with more than one graphics chip, use a DisplayPort
  output of the AMD one.
- The **PenguinScreen2 flatpak**, installed and set up as in `README.md` and `QUICKSTART.md` sections 1 to 3 (your own
  PS2 BIOS and games, `setup-configure-deps.sh` run once). You do not need the WiVRn headset steps in `QUICKSTART.md`
  section 4: those are for Quest-class headsets.
- A flatpak that includes the PS VR2 driver. If yours does not, the launcher says so and tells you to update:
  `flatpak update --user`.
- A **gamepad** to play with.

### Connect it

1. **SteamOS only:** switch to the **Plasma X11 session**. In Konsole, run `steamos-session-select plasma`. It logs
   you out of the desktop and back in. The default Wayland session refuses to hand the headset's display to the
   driver, so on SteamOS this step is not optional. (Other Linux: see "Other Linux" below.)
2. **Plug in** the PS VR2 through its PC adapter, DisplayPort and USB both, and let the headset power on. Do not put
   it on yet.
3. In a terminal, in the folder you unpacked the download into, run:

   ```
   bash launch-vr-session.sh
   ```

   The launcher offers the PS VR2 **only when it sees one plugged in**. If it asks which headset to use, choose the
   PS VR2. To skip the question: `bash launch-vr-session.sh --runtime monado-psvr2`.
4. Read what the launcher prints before the game starts. A good start looks like this:
   - a line saying it is using the flatpak's own Monado;
   - a line saying which Vulkan driver is under the PS VR2, normally "the flatpak's own RADV" on AMD graphics.
5. Put the headset on, and start a game from the PenguinScreen2 library (or **System, Start Disc** for a physical
   disc, as in `QUICKSTART.md` section 3).

There is no pairing, no PIN, no IP address and no Wi-Fi involved: the headset is wired.

### What "working" looks like

The game appears on a virtual screen in front of you in the headset, and the screen follows your head the way
`QUICKSTART.md` section 5 describes. Which games get head-look or 3D, and in which view, is in `GAMES.md`; games with no
profile show the plain flat game on the virtual screen.

### Controls

Play with a **gamepad**. This release does not use the PS VR2 Sense controllers.

### Other Linux

- Your user must be allowed to open the headset's USB device. If the headset is not found and the PS VR2 driver's log
  says it cannot open the USB device, add a udev rule, for example `/etc/udev/rules.d/70-psvr2.rules` containing
  `SUBSYSTEM=="usb", ATTRS{idVendor}=="054c", ATTRS{idProduct}=="0cde", TAG+="uaccess"`, then replug the adapter.
- The headset's display must be handed to the driver by your desktop. This works only in the Plasma X11 session on
  SteamOS; other desktops may refuse it.
- **NVIDIA:** after every NVIDIA driver update, install the flatpak driver extension that matches it, or VR stops
  working: `flatpak install --user flathub org.freedesktop.Platform.GL.nvidia-<version>`, where `<version>` is the
  number in `/sys/module/nvidia/version` with the dots replaced by dashes. The launcher checks this and prints the
  command if they disagree.

### If it does not work

- **The launcher never offers the PS VR2:** it is not seen on USB. Check the adapter's power and USB cable, then
  unplug the adapter's power, wait a few seconds and plug it back in. Sometimes it first connects at a slow USB speed,
  which the driver cannot use. Or name it directly with `--runtime monado-psvr2`.
- **The launcher says Monado is not installed, or the flatpak has no Monado of its own:** your flatpak is older than
  the PS VR2 driver. Update it (`flatpak update --user`) and run the launcher again.
- **The headset stays black or shows nothing on SteamOS:** you are probably still in the Wayland session. Do step 1.
- **A flat, blocky or broken picture:** the launcher prints which Vulkan driver it uses and refuses one older than Mesa
  25.3. Leave `PSCREEN2_MESA` unset on AMD graphics so the flatpak's own driver is used. If you set it to `runtime`
  for a very new AMD card, your flatpak runtime's Mesa must be 25.3 or newer.
- **Anything else:** `KNOWN-ISSUES.md`, and the launcher's own `!!` lines, which say what to do.

## Route 2: the AppImage, through SteamVR on Linux

This is the way to use a PS VR2 with the **AppImage** build. The AppImage does not carry a headset driver of its own:
it uses whichever OpenXR runtime your computer has active, and on Linux the PS VR2 reaches that runtime through
**SteamVR**. SteamVR on Linux has no PS VR2 driver of its own, so the route stacks community pieces under it:

1. **Sony's SteamVR driver**, from the **PlayStation VR2 App** on Steam (app 2580190). It is a Windows driver.
2. **Proton Experimental**, which runs that Windows driver.
3. **Ignition** (by BnuuySolutions), a Linux driver for SteamVR that talks to Sony's driver inside Proton.
4. **The PS VR2 Toolkit, Ignition build** (a pre-release). Sony's driver alone logs "PlayStation VR2 HMD is not found"
   under Ignition; the Toolkit's own USB handling is what lets it reach the headset.
5. **SteamVRLinuxFixes** (an implicit Vulkan layer), which the Toolkit's Linux instructions require.

None of these are ours or Sony's support: they are community projects, and the Toolkit build is a pre-release. Follow
each project's own page to install it; this page covers what they leave out. This download does not include a setup
script for this route.

### What to expect

On a Steam Machine with AMD graphics, in the Plasma X11 session, this route gives the headset's display at its native
4000x2040 and about 90 Hz, head tracking in 6DOF once a play area is saved, and both Sense controllers paired.
Starting PenguinScreen2 inside that SteamVR session, the last step below, is experimental: please report how it goes.
Other computers and NVIDIA graphics are experimental too.

### What you need

- A **PS VR2** and Sony's **PS VR2 PC adapter** (DisplayPort, USB 3 and its own power), as in Route 1. Use a
  DisplayPort output of the AMD chip on a computer with two graphics chips.
- **Steam**, with **SteamVR** and Sony's **PlayStation VR2 App** installed, and **Proton Experimental**.
- The community pieces above (Ignition, the Toolkit's Ignition build, SteamVRLinuxFixes), installed as their pages say.
- Administrator rights (`sudo`): the display step below writes to the kernel's display settings.
- The **PenguinScreen2 AppImage**, with your own PS2 BIOS and games set up as in `README.md`.
- A **gamepad** to play with. PenguinScreen2 does not use the Sense controllers in this release.

### Connect it

1. **SteamOS: switch to the Plasma X11 session.** In Konsole, run `steamos-session-select plasma`. It logs you out
   and back in. SteamVR cannot take the headset's display on the Wayland session. (Other Linux: the same applies;
   we have only seen it work on X11.)
2. **Plug in** the PS VR2 through its adapter, DisplayPort and USB, and let it power on.
3. **Fix the headset's display information** (the "EDID", below, is the usual reason SteamVR finds the headset and
   then ignores it; see the second trap below). Do this with the headset plugged in and powered.
4. **Start SteamVR** and wait until it shows the headset as connected. If the picture in the headset is black, read
   the traps below before trying again.
5. **Save a play area** (below). Without one, the headset tracks rotation only.
6. **Start PenguinScreen2 in VR**, as "How PenguinScreen2 runs under SteamVR" says next.

### How PenguinScreen2 runs under SteamVR

The VR launcher script (`launch-vr-session.sh`) is **not** part of this route: it offers WiVRn, the PS VR2 through the
flatpak's Monado, and nothing else (display glasses will need the separate PenguinLens install), and for each of them it points OpenXR at that
runtime itself. For SteamVR you start the AppImage yourself.

What the AppImage does with the runtime, and so what you have to do:

- **VR is switched on only by the `--vr` flag** on that one launch. Nothing in your environment turns it on, and a
  bare launch plays flat.
- **PenguinScreen2 does not pick a runtime.** It uses the standard OpenXR loader built into it, which takes the
  runtime named by the `XR_RUNTIME_JSON` variable if it is set, and otherwise your active runtime from
  `active_runtime.json` (in `~/.config/openxr/1/`, then the system's `xdg` and `/etc` folders).
- **So tell it to use SteamVR.** With SteamVR already running, in a terminal:

  ```
  XR_RUNTIME_JSON=<SteamVR folder>/steamxr_linux64.json ./PenguinScreen2-<version>.AppImage --vr
  ```

  `<SteamVR folder>` is where Steam put SteamVR: in Steam, right-click **SteamVR**, **Manage**, **Browse local
  files**. If you have set SteamVR as your active OpenXR runtime (SteamVR's settings have a button for it), you can
  leave the variable out.
- Then start a game from the PenguinScreen2 library with the headset on, or **System, Start Disc**, as in
  `QUICKSTART.md` section 3.

If PenguinScreen2 says no OpenXR runtime is available, SteamVR is not running or `XR_RUNTIME_JSON` names the wrong file.

SteamVR does not offer OpenXR's cylinder layer, so the virtual screen is drawn **flat** on this route rather than
curved. Which games get head-look or 3D is the same as Route 1 (`GAMES.md`).

### The traps

- **Wayland refuses, and X11 alone is not enough.** On the Wayland desktop SteamVR logs
  `CannotDRMLeaseDisplay` and never gets the headset's display. On X11 it found the headset's output and still
  refused it until the display information was fixed (next point).
- **The headset is "found, then ignored".** SteamVR picks the headset's output by its *preferred* display mode. The
  PS VR2's display information marks a 1920x1080 mode as preferred, and its real 4000x2040 mode is not. The fix we
  used: set the "preferred" bit on the 4000x2040 timing (in our headset's data, a flags byte went from `0x04` to
  `0x84`, with the section checksum fixed to match), load the edited data through the kernel's
  `/sys/kernel/debug/dri/<card>/<connector>/edid_override`, then unplug and replug the display virtually
  (`trigger_hotplug` 0, then 1). It lasts until you reset it or reboot, not just until you replug the cable, and
  it needs the headset powered.
- **Never force the connector on.** Setting the connector's `force` to `on` lit the headset and then failed the
  link ("Link Training Error"), and SteamVR's compositor aborted ("Failed Watchdog timeout ... Aborting"). Leave
  `force` unset; a plain re-detect put it right.
- **The audio hazard.** After the display is re-detected, and after SteamVR stops, the computer's HDMI audio card can
  switch to an AC3 surround passthrough profile. A TV or receiver on that output then plays the bitstream as a loud
  screech. Turn the volume down before you start and stop SteamVR, and set the card's profile back to plain stereo
  afterwards.
- **6DOF needs a saved play area.** Without one the driver runs in rotation-only tracking. Run the Toolkit's play-area
  app with the headset on and powered, **finish it with Save and quit from inside the app**, and do not stop SteamVR
  while it runs: stopping it early throws the setup away. If you start it from an SSH session, the system can kill it
  when the session closes; start it from the desktop.
- **Sense controllers pair only in pairing mode.** Hold **PS + Create** on the left and **PS + Options** on the right
  until the light blinks fast, then pair them over Bluetooth. A scan finds nothing otherwise. PenguinScreen2 plays with a
  gamepad.
- **Normal noise.** A few "failed to wait for present" lines at every SteamVR start, and a crash line as the compositor
  shuts down, are what this stack does; they are not your setup.

### If it does not work

- **Black headset, or SteamVR says it cannot get the display:** the Wayland session (step 1), or the display
  information not fixed (step 3). Check SteamVR's compositor log for `CannotDRMLeaseDisplay` or "looking for 4000x2040".
- **SteamVR does not see the headset at all:** the adapter's power or USB cable; unplug the adapter's power, wait a
  few seconds, plug it back in. The Toolkit's libusb may need the headset reset after a bad start.
- **The headset tracks turning but not walking or crouching:** no saved play area.
- **PenguinScreen2 starts flat on your monitor:** the `--vr` flag was missing.
- **Anything else:** `KNOWN-ISSUES.md`.

## What is not in this release

- **The flatpak with SteamVR.** The flatpak's sandbox does not include SteamVR's files, so SteamVR with Sony's
  PlayStation VR2 App is the AppImage's route (Route 2), and Monado is the flatpak's (Route 1). Pick the route for the
  build you run.
- A setup script for Route 2: this download lists the steps; it does not run them for you.
- The PS VR2 Sense controllers as game controllers, on either route.
- Windows.
