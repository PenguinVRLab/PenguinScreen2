# Classic Dump - getting your pre-2023 texture packs working again

If you mod NFL 2K5, Madden 09 Deluxe, or another sports title with a
community texture pack, this page is for you.

> **Status: beta.**

## The short version

At some point in 2023, an upstream PCSX2 change (unrelated to VR - it's a
real fix for other games) altered how texture packs are named and matched.
Every pack made before that point - including the packs this community has
been building on for years - stopped matching. **Classic Dump brings that
back**, without giving up the fix that broke it:

- Turn it on, and old packs match again, **at the same time as** anything
  made the modern way. You don't have to choose one or the other, and you
  don't have to convert anything.
- Turn it off (or just don't use it), and everything behaves exactly like a
  normal, unmodified PCSX2 - nothing about your regular dumping or matching
  changes even a little.

## Turning it on

**Settings, then Hotkeys, then Graphics, then "Toggle Classic Texture Dumping"**

Bind that hotkey and press it in-game. One press turns on both classic
matching *and* dumping together (so if regular dumping was off, this turns
it on too - you'll get both eras of names in your dump folder from the same
session). Press it again to turn classic off; your regular dumping keeps
running exactly as it was.

If you only ever want to *load* an old pack and never plan to dump anything
yourself, you don't need the hotkey at all - set this once in the game's
`.ini` file instead:

```ini
[EmuCore/GS]
ClassicTextureNames=true
```

That's the only setting Classic Dump adds. It doesn't touch anything else in
your config.

## What actually changes

- **Loading old packs:** your existing pack folder works as-is. Nothing to
  rename, nothing to convert.
- **Dumping new textures:** with the hotkey on, every texture gets dumped
  **twice** - once with the modern name (so nothing about the current
  pipeline loses coverage) and, for the textures that need it, once more
  with the old-style name and canvas size a pre-2023 pack expects. If a
  texture didn't change between the two eras, it's only dumped once - you
  won't get pointless duplicate files.
- **Priority when both eras have art for the same texture:** the modern
  pack wins. Classic matching only kicks in when nothing modern matched.

## Limits

- **Any game's pack.** The mechanism works at the texture-cache level, not the
  game level, so it is not tied to one game.
- **Compressed (DDS) packs.** If your pack ships compressed textures rather
  than plain PNGs, the crop step for old-style names is skipped for those
  specific files and you'll see the old atlas-style art instead of a
  cropped single texture - a cosmetic leftover, not a crash, and only for
  packs in that format.
- **Windows.** This page does not cover Windows yet.

## Reporting something

If a texture looks wrong with Classic Dump on, the single most useful thing
you can do is turn classic **off**, check whether the same texture looks
correct, then turn it back **on** and describe exactly what's different - a
screenshot of both if you can. That one comparison usually tells us in
seconds whether it's a classic-mode bug or something unrelated.
