#!/usr/bin/env bash
# PenguinScreen2 — one-time setup.
#
# What this does (and all it does):
#   1. Creates two folders in your home directory:
#        ~/PS2-BIOS    <- drop your dumped PS2 BIOS here (any filename)
#        ~/PS2-Games   <- drop your dumped game images here (any filename)
#   2. Installs the VR runtime dependencies (WiVRn).
#
# That's it — there is no configuration step. On launch the emulator sees a
# BIOS in ~/PS2-BIOS, adopts both folders by itself, and opens straight to
# your game library.
#
# It never downloads or installs a BIOS or games — those are yours, dumped
# from your own console and discs. Running this script again is safe.

set -euo pipefail

BIOS_DIR="$HOME/PS2-BIOS"
GAMES_DIR="$HOME/PS2-Games"
DEPS_OK=1

# ---------------------------------------------------------------------------
# 1. The two drop folders — FIRST, so a network/flatpak failure below can
#    never leave the machine without them (strict-review #16)
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

# ---------------------------------------------------------------------------
# 2. Dependencies (the only step that touches packages) — user scope
#    throughout: no polkit prompts, and it matches the --user app install
#    the INSTALL steps use (strict-review #16 + G7)
# ---------------------------------------------------------------------------
if command -v flatpak >/dev/null; then
	flatpak remote-add --user --if-not-exists flathub \
		https://flathub.org/repo/flathub.flatpakrepo || {
		echo "WARN: could not add the Flathub remote (network?) — WiVRn not installed."
		DEPS_OK=0
	}
	if [ "$DEPS_OK" = "1" ]; then
		flatpak install --user -y flathub io.github.wivrn.wivrn || {
			echo "WARN: WiVRn install failed — install it manually:"
			echo "      flatpak install --user flathub io.github.wivrn.wivrn"
			DEPS_OK=0
		}
	fi
else
	echo "WARN: flatpak not found — install flatpak first, then re-run this script."
	DEPS_OK=0
fi

echo
echo "Two folders are ready in your home directory:"
echo "  $BIOS_DIR   <- your PS2 BIOS goes here"
echo "  $GAMES_DIR  <- your game images go here"
if [ "$DEPS_OK" = "1" ]; then
	echo "WiVRn (the VR link) is installed."
	echo "Drop your files in, then run:  bash launch-vr-session.sh"
	echo "It starts the VR link and walks you through connecting the headset"
	echo "(first time: a one-off PIN pairing; on SteamOS you'll connect by IP —"
	echo "the launcher prints it)."
else
	echo "!! WiVRn is NOT installed yet (see the warning above) — fix that and"
	echo "!! re-run this script before launching. The drop folders are ready."
fi
