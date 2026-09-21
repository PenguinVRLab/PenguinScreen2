# Classic Dump — getting your pre-2023 texture packs working again

If you mod NFL 2K5, Madden 09 Deluxe, or another sports title with a
community texture pack, this page is for you.

> **Status: this is a beta feature, not yet in an official release.** The
> code exists, is committed, and has been tested — including one crash found
> and fixed during that testing (see **What's actually been tested**, below)
> — but it hasn't shipped in a public PenguinScreen2 build yet. If you're
> reading this, you were likely handed a specific test build directly; this
> page explains what it does and what to expect from it.

## The short version

At some point in 2023, an upstream PCSX2 change (unrelated to VR — it's a
real fix for other games) altered how texture packs are named and matched.
Every pack made before that point — including the packs this community has
been building on for years — stopped matching. **Classic Dump brings that
back**, without giving up the fix that broke it:

- Turn it on, and old packs match again, **at the same time as** anything
  made the modern way. You don't have to choose one or the other, and you
  don't have to convert anything.
- Turn it off (or just don't use it), and everything behaves exactly like a
  normal, unmodified PCSX2 — nothing about your regular dumping or matching
  changes even a little.

## Turning it on

**Settings → Hotkeys → Graphics → "Toggle Classic Texture Dumping"**

Bind that hotkey and press it in-game. One press turns on both classic
matching *and* dumping together (so if regular dumping was off, this turns
it on too — you'll get both eras of names in your dump folder from the same
session). Press it again to turn classic off; your regular dumping keeps
running exactly as it was.

If you only ever want to *load* an old pack and never plan to dump anything
yourself, you don't need the hotkey at all — set this once in the game's
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
  **twice** — once with the modern name (so nothing about the current
  pipeline loses coverage) and, for the textures that need it, once more
  with the old-style name and canvas size a pre-2023 pack expects. If a
  texture didn't change between the two eras, it's only dumped once — you
  won't get pointless duplicate files.
- **Priority when both eras have art for the same texture:** the modern
  pack wins. Classic matching only kicks in when nothing modern matched.

## What's actually been tested

Everything below was run against a real, current build, on a real texture
pack (the community's own NFL 2K27 pack) — not just reasoned about:

- **Dumping both eras at once, verified byte-for-byte:** ran a real gameplay
  capture through the dumper. The modern names it produced were byte-for-byte
  the same whether classic was on or off — turning classic on never changes
  what you'd normally get. On top of that, it also produced **42 texture
  names that exactly matched real files already in the community's own
  pack** — not just "the right shape," the literal same filenames the old
  tooling produced.
- **Matching, verified on a real play in a real scene:** with the pack
  loaded, the same play rendered three different ways — no pack, pack with
  classic off, pack with classic on — and only the third one showed the
  pack's art. That's the actual proof classic matching engages, not just
  that names line up on paper.
- **A real crash, found and fixed.** During this testing, a specific kind of
  texture — one that only shows part of a larger sheet, which is exactly the
  shape jersey-number art usually takes — caused the game to crash outright.
  It's fixed now and re-tested clean on the exact scene that used to crash.
  Two smaller, related things were fixed at the same time: a texture-atlas
  display bug (a jersey briefly showing a grid of numbers instead of one)
  and a rare bug where a dump session could silently lose a few of the last
  files it wrote if you had classic mode on. If you find anything that looks
  like either of those in this build, it's worth reporting — you may have
  found an edge case the testing didn't cover.

## What hasn't been tested yet

- **Madden 09 Deluxe specifically.** All the testing above used the NFL 2K27
  pack, because that's what was on hand. The mechanism doesn't care which
  game it's running — it works at the texture-cache level, not the
  game level — but nobody has run an actual Madden pack through it yet. If
  you can share one, that closes the last real gap.
- **Compressed (DDS) packs.** If your pack ships compressed textures rather
  than plain PNGs, the crop step for old-style names is skipped for those
  specific files and you'll see the old atlas-style art instead of a
  cropped single texture — a cosmetic leftover, not a crash, and only for
  packs in that format.
- **Windows.** Everything above ran on Linux. Nobody has built or tested
  this on Windows yet.

## Reporting something

If a texture looks wrong with Classic Dump on, the single most useful thing
you can do is turn classic **off**, check whether the same texture looks
correct, then turn it back **on** and describe exactly what's different — a
screenshot of both if you can. That one comparison usually tells us in
seconds whether it's a classic-mode bug or something unrelated.
