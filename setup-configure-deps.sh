#!/usr/bin/env bash
# PenguinScreen2 — one-time setup.
#
# What this does (and all it does):
#   1. Installs the VR runtime dependencies (WiVRn).
#   2. Creates two folders in your home directory:
#        ~/PS2-BIOS    <- drop your dumped PS2 BIOS here (any filename)
#        ~/PS2-Games   <- drop your dumped game images here (any filename)
#
# That's it — there is no configuration step. On its first launch the
# emulator sees a BIOS in ~/PS2-BIOS, adopts both folders by itself, and
# opens straight to your game library.
#
# It never downloads or installs a BIOS or games — those are yours, dumped
# from your own console and discs. Running this script again is safe.

set -euo pipefail

BIOS_DIR="$HOME/PS2-BIOS"
GAMES_DIR="$HOME/PS2-Games"

# ---------------------------------------------------------------------------
# 1. Dependencies (the only step that touches system packages)
# ---------------------------------------------------------------------------
# WiVRn (the VR runtime link between PC and headset) ships on Flathub. Add
# Flathub if needed, then install it. The app's own runtime is pulled when you
# install the .flatpak bundle (see the Quickstart), so this only needs WiVRn.
if command -v flatpak >/dev/null; then
	flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
	flatpak install -y flathub io.github.wivrn.wivrn || \
		echo "WARN: WiVRn install failed — install it manually: flatpak install flathub io.github.wivrn.wivrn"
else
	echo "WARN: flatpak not found — install flatpak first, then re-run this script."
fi

# ---------------------------------------------------------------------------
# 2. The two drop folders
# ---------------------------------------------------------------------------
mkdir -p "$BIOS_DIR" "$GAMES_DIR"

if [ ! -f "$BIOS_DIR/README.txt" ]; then
	cat > "$BIOS_DIR/README.txt" <<'EOF'
Drop your PS2 BIOS file in this folder. Any filename works — it is
recognized by its content. Dump it from a PS2 console you own; it is
never included or downloaded for you.
EOF
fi

if [ ! -f "$GAMES_DIR/README.txt" ]; then
	cat > "$GAMES_DIR/README.txt" <<'EOF'
Drop your PS2 game images in this folder (.iso, .bin, .chd, .cso, .zso,
.gz, .mdf). Any filename works — games are identified by their contents.
They appear in the library at the next launch. Dump them from discs you
own.
EOF
fi

echo
echo "Done. Two folders are ready in your home directory:"
echo "  $BIOS_DIR   <- your PS2 BIOS goes here"
echo "  $GAMES_DIR  <- your game images go here"
echo "Drop your files in, then launch — no further setup."
