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
#    never leave the machine without them
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
#    the INSTALL steps use
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

# ---------------------------------------------------------------------------
# 3. USB tether support (adb) — needed for the Steam Deck's USB-C headset cable
# ---------------------------------------------------------------------------
# Wi-Fi streaming is not viable on Deck-class hardware (field-confirmed), so
# the launcher supports a USB-C tether via adb. adb needs NO root: Google's
# platform-tools is a plain zip that runs from the home directory (Apache-2.0).
# Skipped silently when adb already exists, when offline, or SETUP_USB=off.
ADB_DIR="$HOME/.local/share/penguinscreen2/platform-tools"
if [ "${SETUP_USB:-auto}" != "off" ] && ! command -v adb >/dev/null 2>&1 && [ ! -x "$ADB_DIR/adb" ]; then
	PT_ZIP="$(mktemp -u).zip"
	echo "Fetching adb (USB headset tether support, ~13 MB, no root needed)..."
	if curl -fsSL -o "$PT_ZIP" \
			https://dl.google.com/android/repository/platform-tools-latest-linux.zip 2>/dev/null; then
		mkdir -p "$(dirname "$ADB_DIR")"
		# python3 zipfile: SteamOS ships python3 but not always unzip
		if python3 -c "
import zipfile,sys,os,stat
zipfile.ZipFile('$PT_ZIP').extractall('$(dirname "$ADB_DIR")')
p=os.path.join('$ADB_DIR','adb'); os.chmod(p, os.stat(p).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
" 2>/dev/null && [ -x "$ADB_DIR/adb" ]; then
			echo "adb installed to $ADB_DIR (the launcher finds it there automatically)."
		else
			echo "WARN: adb unpack failed — USB tether unavailable (Wi-Fi still works)."
		fi
		rm -f "$PT_ZIP"
	else
		echo "WARN: could not download adb (offline?) — USB tether unavailable for now."
		echo "      Re-run this script online, or install 'android-tools' yourself."
	fi
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
	# Offer auto-discovery where it's actually blocked. Deliberately NOT run
	# automatically: it needs admin rights and edits a system config, so it stays
	# the user's explicit choice (everything else in this script is user-scope).
	if grep -qsE '^[[:space:]]*disable-user-service-publishing[[:space:]]*=[[:space:]]*yes' \
			/etc/avahi/avahi-daemon.conf; then
		echo
		echo "Optional — headset AUTO-DISCOVERY (so you never type an IP):"
		echo "  This OS ships with the network announcement disabled, so the headset"
		echo "  can't find this PC by itself. To turn it on (asks for your password):"
		echo "      bash enable-vr-discovery.sh"
		echo "  Re-run that once after a SteamOS update. Happy typing the IP? Skip it."
	fi
else
	echo "!! WiVRn is NOT installed yet (see the warning above) — fix that and"
	echo "!! re-run this script before launching. The drop folders are ready."
fi
