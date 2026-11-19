# Game capabilities

PenguinScreen2 plays **every** PS2 game in VR on a large virtual screen that stays in place as you look
around. The games below go further: each has a VR profile, shipped in this release, that adds real 3D depth
and, for most, head tracking inside the game itself.

- ✅ **Current end state**: this is how it's meant to work.
- 🟠 **In progress**: playable, still being worked on.
- ❌ **Very rough**: expect problems.

VR starts only from the VR launcher (`launch-vr-session.sh`); a plain launch plays flat. VR always runs on
the Vulkan renderer, and the VR launcher switches to it for you. 3D depth also needs a hardware renderer:
with the Software renderer selected, games show on the virtual screen without depth. Profiles match exact
disc versions: a different printing of the same game may get no profile, and then plays on the virtual
screen like any other game. This list is generated from the profiles this release ships.

| Game | Status | 3D depth | Head tracking | More |
|---|---|---|---|---|
| **Ace Combat 5: The Unsung War**<br>SLUS-20851 · North America | 🟠 | depth; multi-layer depth in the cockpit | your head turns the camera (left-right, up-down, tilt): cockpit view, in flight | the game's field of view matched to the headset: cockpit view, in flight<br>aiming brackets drawn at the target's depth |
| **ESPN NFL 2K5**<br>SLUS-20919 · North America | 🟠 | depth; menus get their own setting | your head turns the camera (left-right): the first-person camera during play<br>turning your head nudges the camera (left-right): every other camera | the game's field of view matched to the headset: first-person camera |
| **Gran Turismo 4 Spec II**<br>SCUS-97436 · North America | ✅ | multi-layer depth | your head turns the camera (left-right, up-down): the cockpit (interior) view while racing | the game's field of view matched to the headset: cockpit view |
| **Grand Theft Auto III**<br>SLUS-20062 · North America | 🟠 | depth, tuned for the third-person camera | none | none |
| **King's Field IV: The Ancient City**<br>SLUS-20318 · North America | 🟠 | depth | your head turns the camera (left-right, up-down): first-person play; off in menus and pause | none |
| **NASCAR Thunder 2004**<br>SLUS-20824 · North America | 🟠 | multi-layer depth | your head turns the camera (left-right, up-down): the Cockpit camera: Gameplay Settings, then Camera Angle, or Circle during a race | none |
| **Richard Burns Rally**<br>SLES-52323 · Europe | 🟠 | depth | none | two hidden in-car cameras: the bumper view becomes the driver's seat, the chase view a mid-cabin view |
| **Star Wars: Racer Revenge**<br>SLUS-20268 · North America | 🟠 | depth | your head turns the camera (left-right): the pod (first-person) view, up to the game's own 30-degree limit | hand-tracked twin throttles: grab the pod's two levers (needs tracked hand controllers) |
| **TimeSplitters**<br>SLUS-20090 · North America | 🟠 | depth | head movement added to the game's own look (left-right, up-down): first-person play; the stick keeps working | the screen follows your head<br>split split screen: the game's split screen is split, and each of 2 players gets their own half in their own headset (the only game with it today; see DEVICES.md, multi-seat VR multiplayer) |

## Good to know, game by game

**ESPN NFL 2K5**

- Also matches the community "NFL 2K27" edition of the disc.
- A brief flicker can show for a moment after a load.
- Outside the first-person camera, head turns are sent to the game as the right stick: past a small turn the camera pans, and a hard turn while carrying the ball jukes, just as the stick would.

**Gran Turismo 4 Spec II**

- This profile is for Gran Turismo 4 Spec II v1.10, the community overhaul that runs on the GT4 Online Public Beta disc. An unmodified beta disc gets no profile.
- In the game, set Options, then Screen, to Progressive, or the picture doubles.
- Objects very close to you inside the cockpit show little or no depth.

**Grand Theft Auto III**

- Turn off the game's Trails (motion blur) display option for a more comfortable picture.

**King's Field IV: The Ancient City**

- Start from a fresh boot: reloading a save state can corrupt the picture.
- Unlike TimeSplitters, the screen does not follow your head: at the game's 20 frames per second that would judder.

**NASCAR Thunder 2004**

- Our cockpit patches for this game come with this release: they clip the roll cage at your eyes, draw your own car around you with a black interior, and add window-net squares so a look to the left still has net (README, Patches we wrote).
- The game's cockpit has no rear half: past about 60 degrees of head turn you see the open track.

**Richard Burns Rally**

- The game's Triangle camera cycle can get stuck on one of the hidden cameras.

**Star Wars: Racer Revenge**

- There is no pause detection yet: while paused, head turns and the levers still act on the game. Pause with your head straight and your hands off the levers.

**TimeSplitters**

- Turn the game's Auto Lookahead option off.
