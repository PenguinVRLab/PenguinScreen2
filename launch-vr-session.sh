#!/usr/bin/env bash
# PenguinScreen2 — VR session launcher.
#
# Brings the WiVRn server up on the RIGHT GPU, points OpenXR at it, then
# launches the emulator. Connect the headset from its WiVRn app once this
# is running.
#
# GPU selection (the part you never want to think about):
#   The whole VR chain follows whichever GPU the WiVRn server initializes —
#   the emulator asks the runtime for its device (OpenXR vulkan_enable2), so
#   pinning WiVRn pins everything. On single-GPU machines (Steam Deck class)
#   nothing is forced. On hybrid laptops (iGPU + discrete), WiVRn's default
#   of "first Vulkan device" often lands on the iGPU, whose video encoder
#   produces a macroblocky, smeary stream under load — so we force the
#   discrete GPU:
#     - NVIDIA discrete: expose only the NVIDIA Vulkan driver to WiVRn
#       (VK_DRIVER_FILES) + NVIDIA PRIME offload env.
#     - AMD/Intel discrete: Mesa's device-select layer (MESA_VK_DEVICE_SELECT).
#   Override with:  PSCREEN2_GPU=off        (never force anything)
#                   PSCREEN2_GPU=vvvv:dddd  (force a specific PCI vid:did)
#
# Players:
#   Run from a terminal, the launcher asks how many players (1-4). One player
#   is the ordinary session below, unchanged. With more, every player gets a
#   seat: a WiVRn server of their own, on its own port (9757 for player 1,
#   9758 for player 2, and so on). Then:
#     same game   one game in split screen; player 2 sees the other half of
#                 the split screen (2 players)
#     own game    one game per player, each in its own emulator window, and
#                 each player with memory cards and saves of their own
#   Flags answer the questions for scripts and the Deck:
#     --seats N          number of players, 1-4
#     --mode cast        everyone in one game (2 players)
#     --mode separate    one game per player; give --game PATH once per player,
#                        in player order
#   With no terminal and no flags: one player, exactly as before.
#
# Headsets (the VR runtime):
#   WiVRn is the default and everything above is about it. Two more headsets
#   use Monado, a VR runtime that runs on this PC:
#     --runtime wivrn          Quest and other WiVRn headsets (the default)
#     --runtime monado-psvr2   PS VR2 on its PC adapter, through Monado
#     --runtime penguinlens    PenguinLens glasses, through Monado and the
#                              glasses' head-tracking service ("glasses" is
#                              accepted as the older name of this runtime)
#   The runtime is PLAYER 1's headset. Players 2-4 always wear WiVRn headsets,
#   each on a seat of their own, whatever player 1 wears: every mode above works
#   with a PS VR2 or the glasses on player 1 (same game: player 1 sees the split
#   screen in their headset, player 2 their half in the Quest; own game each:
#   player 1 on the Monado headset, the others on WiVRn). Run from a terminal
#   with Monado installed, the launcher asks which headset player 1 wears, right
#   after how many players. With no terminal and no --runtime: WiVRn, exactly as
#   before. With a Monado headset on player 1 none of the WiVRn steps for PLAYER
#   1 happen (no GPU forcing for them, no WiVRn server, no USB tether, no
#   pairing); the other players' WiVRn seats are set up as always, and their
#   headsets pair on their own seat's server.
#   Where Monado is looked for (the first that has bin/monado-service):
#     PS VR2    PSCREEN2_MONADO_PREFIX, else the PenguinScreen2 flatpak's OWN Monado
#               (the PS VR2 driver is built into every flatpak since 2026-10-15: its
#               service runs as a second instance of the app, on a Vulkan driver
#               built into the flatpak as well, RADV at a known Mesa version), else
#               ~/opt/monado-psvr2, else ~/distrobox/monado/opt/monado-psvr2
#               (SteamOS: inside a distrobox)
#     PSCREEN2_MESA=auto|bundled|runtime   which Vulkan driver drives the PS VR2
#               (auto, the default): the GPU that owns the headset's DisplayPort
#               decides. AMD (the Steam Machine, the Deck, a PC's headset port):
#               the RADV built into the flatpak. NVIDIA: the flatpak RUNTIME's driver,
#               the org.freedesktop.Platform.GL.nvidia-<version> extension flatpak
#               installs to match the host's driver (they must match, as for WiVRn;
#               the launcher says so when they do not), and a PS VR2 on an NVIDIA GPU
#               is a path we have not run ourselves. `runtime` on an AMD GPU is the
#               runtime's Mesa (Flathub's GL.default, kept current by `flatpak update`)
#               for a GPU newer than our RADV knows; a Mesa older than 25.3 is refused
#               there (the PS VR2 picture is wrong on it). ONE Vulkan driver per
#               sandbox: the service and the game both run on the built-in RADV, or
#               both on the runtime's drivers, never one each.
#   The PS VR2 is OFFERED in the headset question only when one is plugged in (USB
#   054c:0cde in /sys): the driver being built in says nothing about who owns a
#   PS VR2. --runtime monado-psvr2 names it, plugged in or not.
#     PenguinLens  PSCREEN2_PENGUINLENS_PREFIX (or the older PSCREEN2_GLASSES_PREFIX),
#               else ~/opt/monado-penguinlens, ~/opt/monado-glasses, else the same two
#               under ~/distrobox/monado/opt
#     PSCREEN2_MONADO_HOST=1   SteamOS: run a prefix that lives in the distrobox ON THE
#               HOST instead (the host's Vulkan driver), never `distrobox enter`; the
#               service gets LD_LIBRARY_PATH=<prefix>/lib:<prefix>/hostlibs (filled by
#               psvr2-display-probe.sh hostlibs). The box's Mesa drives the
#               PS VR2 through a display path the host's Mesa no longer uses; this is
#               the switch that compares the two. RADV_DEBUG, when set, reaches the
#               service either way.
#   PenguinLens, and the PS VR2 on the flatpak's own Monado: the Monado service this
#   launch started is stopped when the game closes (the glasses: its window is the
#   side-by-side panel's size; once the glasses fall back to 2D it would overhang the
#   desktop; the PS VR2: the service holds the headset's
#   display). A service of a Monado build in a prefix is left running, as before.
#     PSCREEN2_PENGUINLENS_DAEMON=DIR   a head-tracking service to start when none is
#               running: DIR/xr_driver/bin/xrDriver with DIR/xr_driver_lib, its
#               config/state/data under DIR (the test layout; the installed
#               service is a systemd user unit and needs nothing here)
#
# The emulator:
#     PSCREEN2_EXE=PATH   run this executable (a test AppImage, a build's
#                         pcsx2-qt) instead of the installed flatpak
#
# A desktop shortcut (SteamOS Desktop Mode: double-click, a terminal opens):
#     bash launch-vr-session.sh --install-desktop [--runtime NAME]
#   writes ~/Desktop/PenguinScreen2-VR[-NAME].sh, which runs this launcher with
#   that runtime and with the PSCREEN2_* settings in force at install time.

set -u

APP_ID="com.penguinvrlabs.penguinscreen2"
# The id the app had before its rename: on a machine with only that older install, find_emu uses it.
OLD_APP_ID="org.penguinvr.penguinscreen2"
GPU_MODE="${PSCREEN2_GPU:-auto}"

# ---------------------------------------------------------------------------
# 0. Players: how many, and how they play
# ---------------------------------------------------------------------------
usage() {
	echo "Usage: bash launch-vr-session.sh [--runtime NAME] [--seats N] [--mode cast|separate] [--game PATH ...]"
	echo "  --runtime NAME   player 1's headset: wivrn (default), monado-psvr2 (PS VR2) or penguinlens"
	echo "                   (PenguinLens glasses); players 2-4 always wear WiVRn headsets"
	echo "  --seats N        players, 1-4 (asked when run from a terminal; 1 otherwise)"
	echo "  --mode cast      everyone in one game, split screen (2 players)"
	echo "  --mode separate  one game per player; --game PATH once per player, in order"
	echo "  --pads auto|LIST each player's controller, checked before anything starts (2-4 players: always):"
	echo "                   auto = the order the system lists them; LIST = one per player, in order,"
	echo "                   each an eventN or part of the controller's name (e.g. --pads dualsense,8bitdo)"
	echo "  --install-desktop  write ~/Desktop/PenguinScreen2-VR[-NAME].sh (with --runtime NAME) and exit"
	echo "  --headset-only   start WiVRn and connect your headset (over the USB-C cable when one is plugged in:"
	echo "                   the WiVRn app opens on the headset by itself), then stop: no game starts"
}
SEATS=""
MODE=""
GAMES=()
RUNTIME=""
INSTALL_DESKTOP=""
HEADSET_ONLY=""
PADS=""
while [ $# -gt 0 ]; do
	case "$1" in
		--install-desktop) INSTALL_DESKTOP=1; shift ;;
		--headset-only) HEADSET_ONLY=1; shift ;;
		--pads|--pads=*)
			if [ "$1" = --pads ]; then PADS="${2:-}"; [ $# -gt 1 ] && shift; else PADS="${1#*=}"; fi
			shift
			[ -n "$PADS" ] || { echo "!! --pads wants 'auto' or a list of controllers."; usage; exit 2; } ;;
		--seats|--mode|--game|--runtime)
			if [ $# -lt 2 ]; then
				echo "!! $1 needs a value."; usage; exit 2
			fi
			case "$1" in
				--seats) SEATS="$2" ;;
				--mode) MODE="$2" ;;
				--game) GAMES+=("$2") ;;
				--runtime) RUNTIME="$2" ;;
			esac
			shift 2 ;;
		--seats=*) SEATS="${1#*=}"; shift ;;
		--mode=*) MODE="${1#*=}"; shift ;;
		--game=*) GAMES+=("${1#*=}"); shift ;;
		--runtime=*) RUNTIME="${1#*=}"; shift ;;
		-h|--help) usage; exit 0 ;;
		*) echo "!! Unknown option: $1"; usage; exit 2 ;;
	esac
done
case "$SEATS" in
	""|[1-4]) ;;
	*) echo "!! --seats wants a number from 1 to 4 (got: $SEATS)."; exit 2 ;;
esac
case "$MODE" in
	""|cast|separate) ;;
	*) echo "!! --mode wants 'cast' or 'separate' (got: $MODE)."; exit 2 ;;
esac
case "$RUNTIME" in
	""|wivrn|monado-psvr2|penguinlens) ;;
	glasses) RUNTIME="penguinlens" ;; # the name before the product had one
	*) echo "!! --runtime wants wivrn, monado-psvr2 or penguinlens (got: $RUNTIME)."; exit 2 ;;
esac
# --headset-only: sections 1-3
# exactly as a launch runs them (the server on the right GPU, the USB-C tether, the WiVRn app opened over the cable,
# pairing), then an exit before section 4: one WiVRn headset, no game. The server keeps running (setsid), so a game
# launched afterwards finds it.
if [ -n "$HEADSET_ONLY" ]; then
	case "$RUNTIME" in ""|wivrn) RUNTIME=wivrn ;; *) echo "!! --headset-only connects a WiVRn headset (not --runtime $RUNTIME)."; exit 2 ;; esac
	case "$SEATS" in ""|1) SEATS=1 ;; *) echo "!! --headset-only connects one headset (not --seats $SEATS)."; exit 2 ;; esac
	[ -z "$MODE" ] && [ ${#GAMES[@]} -eq 0 ] || { echo "!! --headset-only starts no game: drop --mode and --game."; exit 2; }
fi
# A Monado runtime names player 1's headset and nothing else: --seats, --mode
# and --game are taken with it, and players 2-4 are WiVRn seats.
# A mode names how several players play; with no terminal to ask how many,
# it must come with --seats, or it would quietly become a one-player launch.
if [ -n "$MODE" ] && [ -z "$SEATS" ] && [ ! -t 0 ]; then
	echo "!! --mode $MODE needs --seats N as well (for example: --seats 2 --mode $MODE)."
	exit 2
fi

# --install-desktop: a double-click shortcut. It reopens itself in a terminal
# (the launcher asks questions and prints what it did), exports the PSCREEN2_*
# settings that were set when it was written, and runs this launcher by its
# absolute path with the runtime given here. Nothing else is started.
if [ -n "$INSTALL_DESKTOP" ]; then
	self=$(readlink -f -- "$0")
	out="$HOME/Desktop/PenguinScreen2-VR${RUNTIME:+-$RUNTIME}.sh"
	mkdir -p "$HOME/Desktop" || exit 1
	{
		printf '#!/usr/bin/env bash\n'
		printf '# Written by launch-vr-session.sh --install-desktop on %s.\n' "$(date -Is)"
		printf '# Double-click: opens a terminal and runs the launcher%s.\n' "${RUNTIME:+ for the $RUNTIME runtime}"
		printf 'if [ ! -t 1 ] && [ -z "${PS2VR_IN_TERM:-}" ]; then\n'
		printf '\tme="$(readlink -f "$0")"\n'
		printf '\tif command -v konsole >/dev/null 2>&1; then PS2VR_IN_TERM=1 exec konsole --hold -e "$me" "$@"; fi\n'
		printf '\tcommand -v notify-send >/dev/null 2>&1 && notify-send "PenguinScreen2 VR" "konsole not found: run $me from a terminal"\n'
		printf '\texit 1\n'
		printf 'fi\n'
		for v in PSCREEN2_EXE PSCREEN2_PENGUINLENS_PREFIX PSCREEN2_GLASSES_PREFIX PSCREEN2_MONADO_PREFIX PSCREEN2_PENGUINLENS_DAEMON PSCREEN2_GPU PSCREEN2_MESA PSCREEN2_POSE_RECORD; do
			[ -n "${!v:-}" ] && printf 'export %s=%q\n' "$v" "${!v}"
		done
		printf 'exec bash %q' "$self"
		[ -n "$RUNTIME" ] && printf ' --runtime %q' "$RUNTIME"
		printf ' "$@"\n'
	} > "$out" && chmod +x "$out" || { echo "!! Could not write $out."; exit 1; }
	bash -n "$out" || { echo "!! $out does not parse; not installed."; rm -f -- "$out"; exit 1; }
	echo ">> Desktop shortcut written: $out"
	echo ">>   runs: bash $self${RUNTIME:+ --runtime $RUNTIME}"
	for v in PSCREEN2_EXE PSCREEN2_PENGUINLENS_PREFIX PSCREEN2_GLASSES_PREFIX PSCREEN2_MONADO_PREFIX PSCREEN2_PENGUINLENS_DAEMON PSCREEN2_GPU PSCREEN2_MESA PSCREEN2_POSE_RECORD; do
		[ -n "${!v:-}" ] && echo ">>   with $v=${!v}"
	done
	exit 0
fi

# One launch at a time for this user. A launch takes this lock before it asks
# or starts anything, and the session keeps it until the launcher and every
# game it started have closed. A second launch meanwhile would find the first
# one's servers and Monado service up and reuse them under a headset that is
# in use: it is refused instead. (A service started outside this launcher
# cannot be told from an idle one.)
# The lock is a plain file in the session's runtime dir, held by ONE small
# watcher process and by nothing else: the launcher closes its own descriptor
# at once, so no program it starts (the emulators and their sandbox, the
# servers, the Monado service, adb's background server, any helper that
# daemonises) inherits it. The watcher lets go when the launcher's process
# (player 1's emulator, which replaces it) and the games listed in the file
# have all exited; a launch that stops without starting a game lets go at once.
LAUNCH_LOCK="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/penguinscreen2-launch.lock"
LAUNCH_LOCK_HOLDER=""
lock_refuse() {
	echo "!! The launch lock cannot be taken ($1): $LAUNCH_LOCK."
	echo "!! Without it a second launch could reuse this one's servers under a headset in"
	echo "!! use. Fix that, then re-run. Nothing was started."
	exit 1
}
command -v flock >/dev/null 2>&1 || lock_refuse "flock is not installed (util-linux)"
# tested in a subshell first: a failed redirection on `exec` ends a POSIX-mode shell silently
( : >> "$LAUNCH_LOCK" ) 2>/dev/null || lock_refuse "the file cannot be opened"
exec 9>>"$LAUNCH_LOCK"
if ! flock -n 9; then
	echo "!! Another VR launch is running, or a game it started is still open."
	echo "!! Close that game (or let that launch finish), then re-run. Nothing was started."
	exit 1
fi
: > "$LAUNCH_LOCK"   # this session's game pids, one per line (players 2-N)
# the watcher: it alone keeps fd 9; its own sleeps do not
bash -c 'trap "" INT HUP
	while :; do
		alive=0
		kill -0 "$0" 2>/dev/null && alive=1
		# read in this shell: a command substitution is a fork that holds fd 9 too
		while read -r p; do kill -0 "$p" 2>/dev/null && alive=1; done < "$1" 2>/dev/null
		[ "$alive" = 1 ] || exit 0
		sleep 0.2 9>&-
	done' "$$" "$LAUNCH_LOCK" < /dev/null > /dev/null 2>&1 &
LAUNCH_LOCK_HOLDER=$!
exec 9>&-
# launch_lock_release: on every exit of this script (not on the exec of player
# 1's emulator, which keeps its pid): no game of this session still running
# means the lock is free now.
launch_lock_release() {
	local p
	for p in $(cat "$LAUNCH_LOCK" 2>/dev/null); do kill -0 "$p" 2>/dev/null && return 0; done
	kill "$LAUNCH_LOCK_HOLDER" 2>/dev/null
	wait "$LAUNCH_LOCK_HOLDER" 2>/dev/null
	return 0
}
trap 'launch_lock_release' EXIT

# ask <question> : one answer into ASK. Only ever called with a terminal on
# stdin; bounded, so a launcher left at the question does not wait forever.
ASK=""
ask() {
	ASK=""
	printf '%s' "$1"
	read -r -t 300 ASK || { ASK=""; echo; }
}

# ---------------------------------------------------------------------------
# 0a. Controllers
# ---------------------------------------------------------------------------
# The launcher cannot pair a controller. The emulator numbers the ones it finds
# SDL-0, SDL-1, ... and a port's settings bind one of those numbers (player 1:
# SDL-0; player 2 sharing one game: SDL-1). Here: every controller the system
# has (an input device with a joystick handler; a pad's motion sensors and
# touchpad are not one), in the order the system lists them (by device path),
# and which one is each player's. A player without one stops the launch.
INPUT_DEVICES="${PSCREEN2_INPUT_DEVICES:-/proc/bus/input/devices}"
# pads_scan: PAD_NAME[i] and PAD_INFO[i] ("USB, event21") of every controller, in that order
pads_scan() {
	local _s bus ev name
	PAD_NAME=(); PAD_INFO=()
	while IFS=$'\t' read -r _s bus ev name; do
		case "$bus" in 0003) bus=USB ;; 0005) bus=Bluetooth ;; *) bus="bus $bus" ;; esac
		PAD_NAME+=("$name"); PAD_INFO+=("$bus, $ev")
	done < <(awk '
		function out() { if (js && ev != "") printf "%s\t%s\t%s\t%s\n", sys, bus, ev, name; js = 0; bus = ev = name = sys = "" }
		/^I:/ { out(); for (i = 2; i <= NF; i++) if ($i ~ /^Bus=/) bus = substr($i, 5) }
		/^N: Name=/ { name = $0; sub(/^N: Name="/, "", name); sub(/"[[:space:]]*$/, "", name) }
		/^S: Sysfs=/ { sys = substr($0, 11) }
		/^H: Handlers=/ { for (i = 2; i <= NF; i++) { h = $i; sub(/^Handlers=/, "", h); if (h ~ /^js[0-9]+$/) js = 1; if (h ~ /^event[0-9]+$/) ev = h } }
		END { out() }
	' "$INPUT_DEVICES" 2>/dev/null | LC_ALL=C sort -t $'\t' -k1,1)
}
pads_list() {
	local i
	[ "${#PAD_NAME[@]}" -gt 0 ] || { echo "!!   (no controllers found)"; return 0; }
	for i in "${!PAD_NAME[@]}"; do echo "!!   $((i + 1)) = ${PAD_NAME[$i]} (${PAD_INFO[$i]})"; done
}
# pads_assign: PAD_OF[n] = player n's controller (an index into PAD_NAME; past
# its end = none), from --pads. Players 2-N with a game each run on data roots
# of their own, where seed_player binds their port to their controller; every
# other player gets the one the emulator gives them, the n-th, so a list that
# names another one for them is refused, with the system's order.
pads_assign() {
	local n i k spec list=()
	PAD_OF=()
	for n in $(seq 1 "$SEATS"); do PAD_OF[n]=$((n - 1)); done
	{ [ -z "$PADS" ] || [ "$PADS" = auto ]; } && return 0
	IFS=, read -r -a list <<< "$PADS"
	if [ "${#list[@]}" -ne "$SEATS" ]; then
		echo "!! --pads names ${#list[@]} controller(s) for $SEATS player(s): one per player, in order."
		return 1
	fi
	for n in $(seq 1 "$SEATS"); do
		spec="${list[$((n - 1))]}"; k=""
		for i in "${!PAD_NAME[@]}"; do
			if [ "${PAD_INFO[$i]##*, }" = "$spec" ] || [[ "${PAD_NAME[$i],,}" == *"${spec,,}"* ]]; then
				if [ -n "$k" ]; then
					echo "!! --pads: '$spec' (player $n) names more than one controller; use its eventN:"
					pads_list
					return 1
				fi
				k=$i
			fi
		done
		if [ -z "$spec" ] || [ -z "$k" ]; then
			echo "!! --pads: no controller matches '$spec' (player $n). The controllers found:"
			pads_list
			return 1
		fi
		for i in $(seq 1 $((n - 1))); do
			[ "${PAD_OF[i]}" = "$k" ] && { echo "!! --pads: players $i and $n both name ${PAD_NAME[$k]}."; return 1; }
		done
		if { [ "$MODE" != separate ] || [ "$n" -eq 1 ]; } && [ "$k" -ne $((n - 1)) ]; then
			echo "!! --pads: player $n gets the system's controller number $n, not ${PAD_NAME[$k]}"
			echo "!!   (the emulator follows the system's order; swap the controllers instead):"
			pads_list
			return 1
		fi
		PAD_OF[n]=$k
	done
}
# pads_check [loud]: every player's controller. Quiet when all is well, unless
# loud (after the headsets step: the table the players read). 1 = a player has
# none, or --pads is wrong.
pads_check() {
	local n k missing="" s=""
	pads_scan
	pads_assign || return 1
	for n in $(seq 1 "$SEATS"); do [ "${PAD_OF[n]}" -lt "${#PAD_NAME[@]}" ] || missing=1; done
	[ -z "$missing" ] && [ "${1:-}" != loud ] && return 0
	[ "$SEATS" -gt 1 ] && s=s
	echo "====================== CONTROLLERS ======================"
	echo ">> $SEATS player$s, ${#PAD_NAME[@]} controller$([ "${#PAD_NAME[@]}" -ne 1 ] && echo s) found:"
	for n in $(seq 1 "$SEATS"); do
		k=${PAD_OF[n]}
		if [ "$k" -lt "${#PAD_NAME[@]}" ]; then
			echo ">>   Player $n  <-  ${PAD_NAME[$k]} (${PAD_INFO[$k]})"
		else
			echo "!!   Player $n  <-  NO CONTROLLER"
		fi
	done
	if [ -n "$missing" ]; then
		echo "!! Every player needs a controller: turn the missing one on or plug it in, then start again."
		return 1
	fi
	echo ">> Holding the wrong one? Swap controllers with the player who has yours."
	echo "========================================================="
}

# ---------------------------------------------------------------------------
# 0b. The headset: WiVRn (everything below this block) or a Monado runtime
# ---------------------------------------------------------------------------
# >>> runtime: functions (definitions only)
# Monado serves its apps on ONE socket in the session's runtime dir, and its
# process is always named monado-service. Liveness is both at once, as for
# WiVRn: the process by EXACT name (never by command line: this script's own
# argv names the binary) AND the socket (a crashed service leaves it behind).
MON_RUN="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
MSOCK="$MON_RUN/monado_comp_ipc"
MON_LOG="$HOME/monado.log"
# SteamOS keeps its system read-only, so Monado is built and run inside a
# distrobox named "monado" whose home is ~/distrobox/monado. That home is
# mounted at the same absolute path inside the box, so a prefix under it has
# ONE absolute path on both sides; "~" does not (on the host it is the host's
# home, inside the box it is the box's).
MON_BOX="monado"
MON_BOX_HOME="$HOME/distrobox/$MON_BOX"
# The flatpak's OWN Monado (the PS VR2 driver vendored into every PenguinScreen2
# flatpak): its install is a prefix like any other (bin/monado-service,
# share/openxr/1/openxr_monado.json under the app's deploy dir, flatpak_files),
# but its service is a second instance of the app, its socket lives in a dir of
# its own (compiled in: XRT_IPC_MSG_SOCK_FILENAME=penguinscreen2-monado/comp_ipc,
# one host dir the two instances share), the manifest the game binds is a
# sandbox path, and the Vulkan driver under both is the one built into the
# flatpak (share/vulkan/icd.d/radeon_icd.x86_64.json: RADV at the Mesa version in
# share/penguinscreen2/mesa-radv.version) unless PSCREEN2_MESA=runtime.
MON_FP_SOCK_DIR="penguinscreen2-monado"
MON_FP_JSON="/app/share/openxr/1/openxr_monado.json"
MON_FP_ICD="/app/share/vulkan/icd.d/radeon_icd.x86_64.json"
MESA_MODE="${PSCREEN2_MESA:-auto}"
# Where the PS VR2 is looked for: the USB devices in sysfs, a read only; and
# where its GPU is looked for: the DRM connectors and cards in sysfs.
USB_SYSFS="${PSCREEN2_USB_SYSFS:-/sys/bus/usb/devices}"
DRM_SYSFS="${PSCREEN2_DRM_SYSFS:-/sys/class/drm}"
NVIDIA_VERSION_FILE="${PSCREEN2_NVIDIA_VERSION_FILE:-/sys/module/nvidia/version}"
# The glasses' head-tracking service publishes the head pose here.
POSE_RECORD="${PSCREEN2_POSE_RECORD:-/dev/shm/penguin_pose_v1}"

# abs_path <path>: a leading ~ becomes $HOME, a relative path becomes absolute.
abs_path() {
	local p="$1"
	case "$p" in
		"~") p="$HOME" ;;
		"~/"*) p="$HOME/${p#"~/"}" ;;
	esac
	case "$p" in
		/*) ;;
		*) p="$PWD/$p" ;;
	esac
	printf '%s\n' "${p%/}"
}
# runtime_prefix <monado-psvr2|glasses>: the install prefix of that runtime's
# Monado, or nothing. A prefix set by its variable is the ONLY candidate: an
# explicit setting that silently fell back to another build would be worse
# than an error.
runtime_prefix() {
	local c fp set_by=""
	case "$1" in
		monado-psvr2)
			set_by="${PSCREEN2_MONADO_PREFIX:-}"
			set -- "$HOME/opt/monado-psvr2" "$MON_BOX_HOME/opt/monado-psvr2"
			# the flatpak's own Monado first, when the flatpak is this launch's
			# emulator (its client library loads only inside that sandbox)
			[ "${EMU:-}" = "flatpak" ] && fp=$(flatpak_files) && set -- "$fp" "$@" ;;
		penguinlens)
			set_by="${PSCREEN2_PENGUINLENS_PREFIX:-${PSCREEN2_GLASSES_PREFIX:-}}"
			set -- "$HOME/opt/monado-penguinlens" "$HOME/opt/monado-glasses" \
				"$MON_BOX_HOME/opt/monado-penguinlens" "$MON_BOX_HOME/opt/monado-glasses" ;;
		*) return 1 ;;
	esac
	[ -n "$set_by" ] && set -- "$(abs_path "$set_by")"
	for c in "$@"; do
		if [ -x "$c/bin/monado-service" ] && [ -f "$c/share/openxr/1/openxr_monado.json" ]; then
			printf '%s\n' "$c"
			return 0
		fi
	done
	return 1
}
runtime_label() {
	case "$1" in
		monado-psvr2) echo "PS VR2 (Monado)" ;;
		penguinlens) echo "PenguinLens (Monado)" ;;
		*) echo "WiVRn" ;;
	esac
}
# flatpak_files: the installed PenguinScreen2 flatpak's files dir (a user install
# first, then a system one), or nothing. The deploy path through current/active
# is one string for every version installed (wivrn_manifest's pattern).
flatpak_files() {
	local d
	for d in "$HOME/.local/share/flatpak" /var/lib/flatpak; do
		if [ -d "$d/app/$APP_ID/current/active/files" ]; then
			printf '%s\n' "$d/app/$APP_ID/current/active/files"
			return 0
		fi
	done
	return 1
}
# mon_in_flatpak <prefix>: is that prefix the flatpak's own Monado?
mon_in_flatpak() {
	case "$1" in */app/"$APP_ID"/current/active/files) return 0 ;; esac
	return 1
}
# mon_sock_for <prefix>: the socket that prefix's service serves on.
mon_sock_for() {
	if mon_in_flatpak "$1"; then printf '%s\n' "$MON_RUN/$MON_FP_SOCK_DIR/comp_ipc"
	else printf '%s\n' "$MON_RUN/monado_comp_ipc"; fi
}
# mon_stop_at_end: is the Monado service this run started stopped when the game
# closes? The glasses' always (see the header); the flatpak's own (it holds the
# headset's display); a prefix's service is left running for the next launch.
mon_stop_at_end() {
	[ -n "$MON_STARTED" ] || return 1
	[ "${RUNTIME:-}" = "penguinlens" ] && return 0
	mon_in_flatpak "${MON_PREFIX:-}"
}
# psvr2_detected: is a PS VR2 plugged in? Its PC adapter enumerates on USB as
# Sony 054c:0cde; sysfs is read, nothing is opened. This is the ONLY thing that
# offers the PS VR2 kind by itself: the driver being installed never does.
psvr2_detected() {
	local d
	for d in "$USB_SYSFS"/*/; do
		[ "$(cat "${d}idVendor" 2>/dev/null)" = "054c" ] || continue
		[ "$(cat "${d}idProduct" 2>/dev/null)" = "0cde" ] && return 0
	done
	return 1
}
# usb_quest_dir: the sysfs directory of a Meta/Oculus headset on USB (idVendor 2833: Quest 2/3/Pro all enumerate
# under it), or nothing. usb_adb_iface <dir>: does that device expose an ADB interface (USB class ff, subclass 42,
# protocol 01: what `adb` binds)? A Quest with USB debugging OFF exposes only its own vendor interfaces (on a
# Quest 3: ff/8a, ff/8b, ff/8c and no ff/42), so adb lists NOTHING, not even
# "unauthorized". That is the first-connect trap: read it from sysfs and say it, never stay silent.
usb_quest_dir() {
	local d
	for d in "$USB_SYSFS"/*/; do
		[ "$(cat "${d}idVendor" 2>/dev/null)" = "2833" ] && { printf '%s\n' "$d"; return 0; }
	done
	return 1
}
usb_adb_iface() {
	local i
	for i in "$1"*:*/; do
		[ "$(cat "${i}bInterfaceClass" 2>/dev/null)" = "ff" ] && [ "$(cat "${i}bInterfaceSubClass" 2>/dev/null)" = "42" ] && return 0
	done
	return 1
}
# usb_net_iface <dir>: the network interface a headset in WiVRn's "USB networking" mode (26.9+, Horizon OS 2.6+)
# presents on that USB device (a cdc_ncm adapter: an interface with a net/ directory), or nothing. That link needs
# no adb and no developer mode; the headset then finds this PC by discovery over the cable.
usb_net_iface() {
	local n
	for n in "$1"*:*/net/*/; do
		[ -d "$n" ] && { basename "$n"; return 0; }
	done
	return 1
}
# headset_transport: how the headset connected to player 1's server is reaching this PC, from the peer address of
# the established connection on :9757: "cable" (a link-local IPv6 scoped to a USB interface, or 127.0.0.1 = the adb
# tether), "wifi" (any other address), or "" (no client). On a Deck, Wi-Fi is the path that dips and drops.
headset_transport() {
	local peer
	have_ss || return 1
	peer=$(ss -tn state established "( sport = :9757 )" 2>/dev/null | tail -n +2 | awk '{print $4}' | head -1)
	[ -n "$peer" ] || { echo ""; return 0; }
	case "$peer" in
		*%questusb*|*%enp*u*i*|*%usb*|127.0.0.1:*|"[::1]":*|::1:*) echo "cable $peer" ;;
		*) echo "wifi $peer" ;;
	esac
}
# usb_cable_fault: the kernel's own verdict on a dead cable in the last 15 minutes ("usb usbN-portM: Cannot enable.
# Maybe the USB cable is bad?", which the kernel logs when a headset falls off the cable and nothing
# said so). Prints the line, or nothing.
usb_cable_fault() {
	command -v journalctl >/dev/null 2>&1 || return 1
	journalctl -k --since "-15min" --no-pager 2>/dev/null | grep -F "Maybe the USB cable is bad" | tail -1 | grep .
}
# edid_maker <edid file>: the three-letter maker id in an EDID (bytes 8-9), e.g.
# SNY for Sony; nothing when the file is empty (no display on that connector).
edid_maker() {
	local b1 b2 w a="@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	read -r b1 b2 < <(od -An -tu1 -j8 -N2 "$1" 2>/dev/null) || return 1
	[ -n "${b2:-}" ] || return 1
	w=$((b1 * 256 + b2))
	printf '%s%s%s\n' "${a:$((w >> 10 & 31)):1}" "${a:$((w >> 5 & 31)):1}" "${a:$((w & 31)):1}"
}
gpu_vendor() {  # <card dir> -> the PCI vendor id without 0x (1002 AMD, 10de NVIDIA, 8086 Intel)
	local v
	v=$(cat "$1/device/vendor" 2>/dev/null) || return 1
	printf '%s\n' "${v#0x}"
}
# headset_gpu_vendor: the vendor of the GPU that owns the PS VR2's DisplayPort: the
# connected DP connector whose display is Sony's; when the headset is not plugged
# in yet, an AMD GPU if the machine has one (the headset's port is on it on every
# machine we know), else the first GPU. Nothing when sysfs shows no GPU. Reads only.
headset_gpu_vendor() {
	local c card v first="" amd=""
	for c in "$DRM_SYSFS"/card*-DP-*; do
		[ "$(cat "$c/status" 2>/dev/null)" = "connected" ] || continue
		[ "$(edid_maker "$c/edid")" = "SNY" ] || continue
		card=${c##*/}; card=${card%%-*}
		v=$(gpu_vendor "$DRM_SYSFS/$card") && { printf '%s\n' "$v"; return 0; }
	done
	for c in "$DRM_SYSFS"/card*; do
		case "${c##*/}" in *-*) continue ;; esac
		v=$(gpu_vendor "$c") || continue
		[ -z "$first" ] && first="$v"
		[ "$v" = "1002" ] && amd="$v"
	done
	[ -n "$amd" ] && { printf '%s\n' "$amd"; return 0; }
	[ -n "$first" ] && { printf '%s\n' "$first"; return 0; }
	return 1
}
# mesa_resolve <prefix>: which Vulkan driver drives the PS VR2 through the flatpak's
# own Monado: MESA_USE = bundled (the RADV built into the flatpak) or runtime (the
# flatpak runtime's drivers: the NVIDIA extension, or Flathub's Mesa); MESA_GPU =
# the headset GPU's vendor as found; MESA_WHY = one line. PSCREEN2_MESA names it
# outright; auto reads the GPU. A flatpak without a RADV of its own is runtime.
MESA_USE=""; MESA_GPU=""; MESA_WHY=""
mesa_resolve() {
	MESA_GPU=$(headset_gpu_vendor) || MESA_GPU=""
	if [ ! -f "$1/share/vulkan/icd.d/radeon_icd.x86_64.json" ]; then
		MESA_USE=runtime; MESA_WHY="this flatpak carries no RADV of its own"
		return 0
	fi
	case "$MESA_MODE" in
		bundled) MESA_USE=bundled; MESA_WHY="PSCREEN2_MESA=bundled" ;;
		runtime) MESA_USE=runtime; MESA_WHY="PSCREEN2_MESA=runtime" ;;
		*)
			case "$MESA_GPU" in
				1002) MESA_USE=bundled; MESA_WHY="the PS VR2's GPU is AMD" ;;
				"") MESA_USE=bundled; MESA_WHY="no GPU found in $DRM_SYSFS; the flatpak's own driver is the default" ;;
				10de) MESA_USE=runtime; MESA_WHY="the PS VR2's GPU is NVIDIA: our RADV cannot drive it" ;;
				*) MESA_USE=runtime; MESA_WHY="the PS VR2's GPU (vendor $MESA_GPU) is not AMD: our RADV cannot drive it" ;;
			esac ;;
	esac
	return 0
}
# mesa_opts <prefix>: MESA_OPTS = the `flatpak run` options that put the PS VR2's
# service AND game on the flatpak's own RADV: the one Vulkan driver file, by
# name, for both instances (one driver per sandbox, never a mix). Empty for a
# prefix's Monado and when the runtime's drivers are used (mesa_resolve).
MESA_OPTS=()
mesa_opts() {
	MESA_OPTS=()
	mon_in_flatpak "$1" || return 0
	[ -n "$MESA_USE" ] || mesa_resolve "$1"
	[ "$MESA_USE" = "bundled" ] || return 0
	MESA_OPTS=("--env=VK_DRIVER_FILES=$MON_FP_ICD")
}
# nvidia_ext_check: on an NVIDIA GPU the sandbox's driver is the GL.nvidia-<version>
# extension and it must match the host's driver exactly (the WiVRn lesson, three
# times); 1 = they do not, with the fix named.
nvidia_ext_check() {
	local hv ext
	hv=$(cat "$NVIDIA_VERSION_FILE" 2>/dev/null) || { echo ">>   (the host's NVIDIA driver version could not be read from $NVIDIA_VERSION_FILE)"; return 0; }
	ext="org.freedesktop.Platform.GL.nvidia-${hv//./-}"
	if flatpak info "$ext" >/dev/null 2>&1; then
		echo ">>   NVIDIA driver $hv on the host; its flatpak extension $ext is installed."
		return 0
	fi
	echo "!! MISMATCH: the host runs NVIDIA driver $hv but the flatpak extension for it is not installed,"
	echo "!!   so inside the sandbox there is no driver for this GPU. Install it, then re-run:"
	echo "!!     flatpak install flathub $ext"
	echo "!! Nothing was launched."
	return 1
}
# gl_ext_branch: the branch of the GL extension the app's runtime mounts (KDE
# 6.10 -> 24.08), from the runtime's own metadata; nothing when it cannot be read.
gl_ext_branch() {
	local rt
	rt=$(flatpak info --show-metadata "$APP_ID" 2>/dev/null | sed -n 's/^runtime=//p' | head -n 1)
	[ -n "$rt" ] || return 1
	flatpak info --show-metadata "$rt" 2>/dev/null | awk -F= '
		/^\[/ { s = $0; next }
		s == "[Extension org.freedesktop.Platform.GL]" {
			if ($1 == "versions") { split($2, a, ";"); print a[1]; p = 1; exit }
			if ($1 == "version") v = $2
		}
		END { if (!p && v != "") print v }'
}
# gl_ext_mesa_version <branch>: the Mesa version of the installed GL extension on
# that branch (what the runtime's RADV is), or nothing.
gl_ext_mesa_version() {
	[ -n "$1" ] || return 1
	flatpak info "org.freedesktop.Platform.GL.default//$1" 2>/dev/null | sed -n 's/^[[:space:]]*Version:[[:space:]]*//p' | head -n 1
}
# mesa_at_least <version> <major> <minor>
mesa_at_least() {
	local maj min
	maj=${1%%.*}; min=${1#*.}; min=${min%%.*}
	case "$maj$min" in *[!0-9]*|"") return 1 ;; esac
	[ "$maj" -gt "$2" ] || { [ "$maj" -eq "$2" ] && [ "$min" -ge "$3" ]; }
}
# mesa_report <prefix>: which Mesa is under the PS VR2's compositor, said before
# anything starts; 1 = refused (a runtime Mesa older than 25.3: the PS VR2's
# picture on it is flat and blocky). Only the flatpak's own
# Monado is judged here: a prefix's service runs on the host's Mesa.
mesa_report() {
	local v br
	mon_in_flatpak "$1" || return 0
	mesa_resolve "$1"
	if [ "$MESA_USE" = "bundled" ]; then
		v=$(cat "$1/share/penguinscreen2/mesa-radv.version" 2>/dev/null)
		echo ">> Vulkan driver under the PS VR2: the flatpak's own RADV ${v:-(version unknown)}, for the service and the game both ($MESA_WHY)."
		echo ">>   PSCREEN2_MESA=runtime uses the flatpak runtime's drivers instead: an NVIDIA GPU, or an AMD GPU newer than that build knows."
		if [ "$MESA_GPU" = "10de" ]; then
			echo "!! The PS VR2's GPU is NVIDIA and RADV cannot drive it: expect the service not to come up (PSCREEN2_MESA=bundled was asked for)."
		fi
		return 0
	fi
	echo ">> Vulkan driver under the PS VR2: the flatpak runtime's, for the service and the game both ($MESA_WHY)."
	if [ "$MESA_GPU" = "10de" ]; then
		echo ">>   An NVIDIA GPU: the sandbox's driver is flatpak's NVIDIA extension. We have not run a PS VR2 on NVIDIA ourselves yet: tell us how it goes."
		nvidia_ext_check || return 1
		return 0
	fi
	br=$(gl_ext_branch)
	v=$(gl_ext_mesa_version "$br")
	echo ">>   Mesa: org.freedesktop.Platform.GL.default//${br:-?} = Mesa ${v:-unknown} (flatpak update keeps it current)."
	if [ -n "$v" ] && ! mesa_at_least "$v" 25 3; then
		echo "!! Mesa $v is older than 25.3: the PS VR2 picture is wrong on it (flat, blocky)."
		echo "!!    flatpak update    then re-run; or leave PSCREEN2_MESA unset for the flatpak's own RADV."
		echo "!! Nothing was launched."
		return 1
	fi
	[ -n "$v" ] || echo ">>   (its version could not be read; the picture needs Mesa 25.3 or newer)"
	return 0
}

# This user's monado-service processes only: another user's service serves
# another runtime dir and is none of this launch's business.
monado_pids() { pgrep -x -u "$(id -u)" monado-service; }
monado_up() { [ -n "$(monado_pids)" ] && [ -S "$MSOCK" ]; }
# mon_listening: 0 = something LISTENS on the socket, 1 = nothing does,
# 2 = cannot tell (no `ss`). `ss` prints a socket's path as it is, spaces and
# all, followed by its inode: matched as a whole string, never field by field.
mon_listening() {
	command -v ss >/dev/null 2>&1 || return 2
	ss -xl 2>/dev/null | SOCK_AT=" $MSOCK " awk 'index($0 " ", ENVIRON["SOCK_AT"]) { f = 1 } END { exit !f }'
}
# mon_is_prefix <pid> <prefix>: is that process the service of that prefix?
# Judged by the executable and by argv[0] (a service started inside the
# distrobox may show its executable through the box's mounts).
mon_is_prefix() {
	local want real exe a0 w
	want="$2/bin/monado-service"
	real=$(realpath -e -- "$want" 2>/dev/null)
	exe=$(readlink "/proc/$1/exe" 2>/dev/null)
	a0=$(tr '\0' '\n' < "/proc/$1/cmdline" 2>/dev/null | head -n 1)
	for w in "$want" "$real"; do
		[ -n "$w" ] || continue
		{ [ "$exe" = "$w" ] || [ "$a0" = "$w" ]; } && return 0
	done
	# the flatpak's service runs inside an instance of the app: the host sees
	# its executable as the sandbox's /app/bin/monado-service, and the
	# instance's own .flatpak-info names the app
	if mon_in_flatpak "$2"; then
		case "$exe|$a0" in
			"/app/bin/monado-service|"*|*"|/app/bin/monado-service"|*"|monado-service")
				grep -qx "name=$APP_ID" "/proc/$1/root/.flatpak-info" 2>/dev/null && return 0 ;;
		esac
	fi
	return 1
}
# mon_known_prefixes: every Monado prefix this launcher knows (each runtime's
# default places and the PSCREEN2_* settings), one per line.
mon_known_prefixes() {
	local v fp
	printf '%s\n' "$HOME/opt/monado-psvr2" "$MON_BOX_HOME/opt/monado-psvr2" \
		"$HOME/opt/monado-penguinlens" "$HOME/opt/monado-glasses" \
		"$MON_BOX_HOME/opt/monado-penguinlens" "$MON_BOX_HOME/opt/monado-glasses"
	fp=$(flatpak_files) && [ -x "$fp/bin/monado-service" ] && printf '%s\n' "$fp"
	for v in PSCREEN2_MONADO_PREFIX PSCREEN2_PENGUINLENS_PREFIX PSCREEN2_GLASSES_PREFIX; do
		[ -n "${!v:-}" ] && abs_path "${!v}"
	done
}
# mon_identify <pid> <prefix>: 0 = that prefix's service; 1 = POSITIVELY the
# service of another prefix this launcher knows; 2 = cannot tell (a service
# whose executable and argv[0] name no known prefix: inside a distrobox they
# may show the box's own view of the path). Only 1 is "another build".
mon_identify() {
	local c IFS=$'\n'
	mon_is_prefix "$1" "$2" && return 0
	for c in $(mon_known_prefixes); do
		[ "$c" = "$2" ] && continue
		mon_is_prefix "$1" "$c" && return 1
	done
	return 2
}
# mon_refuse_other <prefix> <label>: no running service may be another known
# prefix's. A service left running by another headset's launch (the service
# outlives the game) would otherwise drive the wrong headset. Refuse, name it,
# and say how to stop it; never stop it here. A service that cannot be
# identified is refused the same way, never called
# another build; only for a prefix inside the distrobox (in_box), where the
# service may show the box's own view of its path, is it named and used.
mon_refuse_other() {
	local p bad="" anon="" rc
	for p in $(monado_pids); do
		mon_identify "$p" "$1"; rc=$?
		if [ "$rc" -eq 1 ]; then
			bad="$bad $p"
		elif [ "$rc" -eq 2 ]; then
			if in_box "$1"; then
				echo ">> A Monado service is running that could not be matched to $1 (pid $p):"
				echo ">>    $(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)"
				echo ">> Inside the $MON_BOX distrobox a service may show the box's own view of its path: it is used."
			else
				anon="$anon $p"
			fi
		fi
	done
	[ -z "$bad$anon" ] && return 0
	for p in $bad; do
		echo "!! A Monado service from another build is running (pid $p):"
		echo "!!    $(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)"
	done
	for p in $anon; do
		echo "!! A Monado service is running that this launcher cannot match to any build it knows (pid $p):"
		echo "!!    executable: $(readlink "/proc/$p/exe" 2>/dev/null || echo unknown)"
		echo "!!    command:    $(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)"
	done
	echo "!! $2 needs $1/bin/monado-service. This launcher did not"
	echo "!! start that service and will not stop it. With that headset idle, stop it:"
	echo "!!    kill$bad$anon"
	echo "!! then re-run. Nothing was launched."
	return 1
}

# monado_guard: make a start safe, and never touch a live service.
#   - a socket file nothing listens on, with NO monado-service process, is a
#     crashed service's leftover: cleared (it can only mislead);
#   - a socket something LISTENS on with no monado-service process is never
#     removed: under systemd socket activation (Monado ships monado.socket)
#     systemd holds it and starts the service on the first connection. Which
#     Monado build would answer cannot be told, so it is refused, not reused;
#   - a monado-service with NO socket was not started by this run (it may be
#     starting, stuck, or someone else's): given a few seconds, then refused,
#     never stopped. Returns 0 = safe (or it came up meanwhile), 2 = refused.
monado_guard() {
	local pids who rc
	pids=$(monado_pids | tr '\n' ' '); pids=${pids% }
	if [ -z "$pids" ]; then
		[ -e "$MSOCK" ] || return 0
		mon_listening; rc=$?
		if [ "$rc" = 0 ]; then
			who=$(ss -xlp 2>/dev/null | grep -F " $MSOCK " | grep -oE 'users:\(\(.*\)\)' | head -n 1)
			echo "!! $MSOCK is being served${who:+ by $who}, but no Monado service of"
			echo "!! yours is running: most likely systemd socket activation (monado.socket)."
			echo "!! Which Monado build would answer cannot be told, so it is not used, and"
			echo "!! it is left alone. To launch through this launcher, stop it first:"
			echo "!!    systemctl --user stop monado.socket monado.service"
			return 2
		fi
		rm -f -- "$MSOCK" && echo ">> Cleared a dead Monado socket (no Monado service was running)."
		return 0
	fi
	for _ in $(seq 1 10); do
		[ -S "$MSOCK" ] && return 0
		sleep 0.5
	done
	echo "!! A Monado service is running (pid $pids) but is not serving apps (no $MSOCK)."
	echo "!! This launcher did not start it and will not stop it. Close it yourself"
	echo "!! (with the headset idle), then re-run."
	return 2
}

# The environment the service is started with. A service started from a
# terminal over the network has no WAYLAND_DISPLAY, and Monado needs the
# desktop's Wayland connection to lease the headset's display: take it from
# the session's runtime dir when it is not set. Passed by name, so it also
# reaches a service inside the distrobox.
#
# On the X11 desktop (Plasma X11 on the Steam Machine) there is no Wayland:
# the service gets the X display and its authority, and for the PS VR2 the
# switch that takes the headset through X's RandR lease. That is the route
# SteamOS needs; the Wayland lease is refused
# inside Valve's KWin. A Wayland socket file left from an earlier Wayland
# session is never used in an X11 session.
MON_RT=""
#
# Monado's own tuning variables reach a service inside the distrobox only by
# name, so the few a test sets are passed on when set, and only then:
# XRT_COMPOSITOR_DESIRED_MODE (which of the headset's modes, by index in the
# kernel's mode list: on the PS VR2 0 = 4000x2040@119.88, 1 = 1920x1080@60,
# 2 = 4000x2040@89.91), XRT_COMPOSITOR_PRINT_MODES (Monado logs that list) and
# XRT_COMPOSITOR_LOG (its log level). RADV_DEBUG (Mesa's RADV switches, e.g.
# nodcc / nodisplaydcc) the same way, and
# PSVR2_LOG (the PS VR2 driver's log level: info prints the headset's DP RX status
# with the dprx-log patch; the driver's default is warn).
MON_PASS_VARS="XRT_COMPOSITOR_DESIRED_MODE XRT_COMPOSITOR_PRINT_MODES XRT_COMPOSITOR_LOG RADV_DEBUG PSVR2_LOG"
monado_env() {
	local w v
	MON_ENV=()
	if [ "${XDG_SESSION_TYPE:-}" = "x11" ]; then
		if [ -n "${DISPLAY:-}" ]; then
			MON_ENV+=("DISPLAY=$DISPLAY")
			[ -n "${XAUTHORITY:-}" ] && MON_ENV+=("XAUTHORITY=$XAUTHORITY")
			[ "$MON_RT" = "monado-psvr2" ] && MON_ENV+=("XRT_COMPOSITOR_FORCE_RANDR=true")
		fi
	elif [ -n "${WAYLAND_DISPLAY:-}" ]; then
		MON_ENV+=("WAYLAND_DISPLAY=$WAYLAND_DISPLAY")
	else
		for w in "$MON_RUN"/wayland-[0-9]*; do
			[ -S "$w" ] && { MON_ENV+=("WAYLAND_DISPLAY=${w##*/}"); break; }
		done
	fi
	for v in $MON_PASS_VARS; do
		[ -n "${!v:-}" ] && MON_ENV+=("$v=${!v}")
	done
	return 0
}
# in_box <prefix>: true when that prefix lives inside the distrobox's home and
# distrobox is installed (the SteamOS layout).
in_box() {
	command -v distrobox >/dev/null 2>&1 || return 1
	case "$1/" in
		"$MON_BOX_HOME"/*) return 0 ;;
	esac
	return 1
}
# monado_command <prefix>: MON_CMD = how to run that prefix's service. Inside
# the distrobox it is entered by name with the service's ABSOLUTE path.
# PSCREEN2_MONADO_HOST=1 runs a box prefix's service on the host instead, with
# the box-built libraries the host lacks from <prefix>/hostlibs (see the header).
host_run() { in_box "$1" && [ -n "${PSCREEN2_MONADO_HOST:-}" ]; }
monado_command() {
	local svc pfx e front=() opts=()
	pfx="$(abs_path "$1")"
	svc="$pfx/bin/monado-service"
	monado_env
	if mon_in_flatpak "$pfx"; then
		# A second instance of the app. The desktop's display variables go to
		# flatpak ITSELF (it binds the X or Wayland socket it finds through them
		# and writes the sandbox's own authority file; an --env=XAUTHORITY would
		# point the service at a host file it cannot see); Monado's own variables
		# go into the sandbox with --env. --socket=x11 is said on an X11 desktop
		# (explicit over the inferred fallback-x11). One Vulkan driver (MESA_OPTS).
		for e in "${MON_ENV[@]}"; do
			case "$e" in
				DISPLAY=*|XAUTHORITY=*|WAYLAND_DISPLAY=*) front+=("$e") ;;
				*) opts+=("--env=$e") ;;
			esac
		done
		mesa_opts "$pfx"
		MON_CMD=(env "${front[@]}" flatpak run --command=monado-service)
		[ "${XDG_SESSION_TYPE:-}" = "x11" ] && [ -n "${DISPLAY:-}" ] && MON_CMD+=(--socket=x11)
		MON_CMD+=("${MESA_OPTS[@]}" "${opts[@]}" "$APP_ID")
	elif host_run "$1"; then
		MON_ENV+=("LD_LIBRARY_PATH=$pfx/lib:$pfx/hostlibs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}")
		MON_CMD=(env "${MON_ENV[@]}" "$svc")
	elif in_box "$1"; then
		MON_CMD=(distrobox enter "$MON_BOX" -- env "${MON_ENV[@]}" "$svc")
	else
		MON_CMD=(env "${MON_ENV[@]}" "$svc")
	fi
}
# monado_start <prefix>: start the service detached, its stdin held open (it
# exits on end of input), and wait for it to serve. MON_STARTED is the process
# group this call made; MON_PRE the monado-service pids that existed before it.
# The service is registered in STARTED at once, so an interrupt or a failure
# at any later point stops it. Returns 0 once THIS run's service serves, 1 when
# it does not come up, 2 when a service of another build came up meanwhile (it
# must never be the one that serves this launch; mon_refuse_other names it).
MON_STARTED=""
MON_PRE=""
# box_warm: after a reboot the distrobox is stopped, and its first `enter`
# starts the container and runs its setup, which can take tens of seconds: do
# that first, bounded, so the service's own wait below is only the service's.
box_warm() {
	local st
	st=$(podman container inspect -f '{{.State.Running}}' "$MON_BOX" 2>/dev/null)
	[ "$st" = "true" ] && return 0
	echo ">> Starting the $MON_BOX distrobox (the first time after a reboot this can take a minute)..."
	if command -v timeout >/dev/null 2>&1; then
		timeout 180 distrobox enter "$MON_BOX" -- true < /dev/null > /dev/null 2>&1
	else
		distrobox enter "$MON_BOX" -- true < /dev/null > /dev/null 2>&1
	fi || echo ">> (The distrobox did not answer cleanly; trying the service anyway.)"
}
MON_START_PREFIX=""
monado_start() {
	local p
	MON_PRE=" $(monado_pids | tr '\n' ' ')"
	MON_START_PREFIX="$1"
	in_box "$1" && ! host_run "$1" && box_warm
	# the flatpak's service meets the game through this one host dir: made
	# before the service starts (flatpak skips an absent xdg-run path)
	mon_in_flatpak "$1" && mkdir -p "$MON_RUN/$MON_FP_SOCK_DIR"
	monado_command "$1"
	setsid nohup bash -c 'sleep infinity | "$@"' monado-feeder "${MON_CMD[@]}" \
		> "$MON_LOG" 2>&1 < /dev/null &
	MON_STARTED=$!
	STARTED+=("$MON_STARTED:monado:1")
	for _ in $(seq 1 60); do
		if monado_up; then
			for p in $(monado_pids); do
				mon_identify "$p" "$1"; [ $? -eq 1 ] && return 2
			done
			[ -n "$(monado_mine)" ] && return 0
			# inside the distrobox a service of this start may show a path no
			# known prefix has: new since the start and serving, it is taken
			# as this start's (it is stopped through the start's input)
			if in_box "$1"; then
				for p in $(monado_pids); do
					case "$MON_PRE " in *" $p "*) continue ;; esac
					mon_identify "$p" "$1"; [ $? -eq 2 ] || continue
					echo ">> The Monado service now serving (pid $p) could not be matched to $1;"
					echo ">> it appeared after this launch started the service, so it is taken as that one."
					return 0
				done
			fi
		fi
		sleep 0.5
	done
	return 1
}
# monado_mine: the monado-service pids THIS run's monado_start started, and no
# other: new since the start (never a pid that existed before it), and either
# in the process group the start made, or the service of THIS prefix (inside
# the distrobox the service is the box's process; and a service that daemonises,
# forking into a session of its own, leaves the group). Never a service of
# another build, nor one of no known prefix outside the start's group. (With
# the launch lock, a second launch cannot start one meanwhile; a service of
# this same build started by hand during the start wait would count.)
monado_mine() {
	local p st
	[ -n "$MON_STARTED" ] || return 0
	for p in $(monado_pids); do
		case "$MON_PRE " in *" $p "*) continue ;; esac
		if mon_is_prefix "$p" "$MON_START_PREFIX"; then
			echo "$p"
			continue
		fi
		in_box "$MON_START_PREFIX" && continue
		# /proc/<pid>/stat after the name: state, parent, process group
		st=$(cat "/proc/$p/stat" 2>/dev/null) || continue
		st=${st##*) }
		# shellcheck disable=SC2086
		set -- $st
		[ "${3:-}" = "$MON_STARTED" ] && echo "$p"
	done
}
# monado_stop_started: stop only what monado_start started. End its input
# first (the service then exits by itself), then its process group, then its
# service by pid (inside a distrobox the service is not in this run's process
# group). Never a pid that existed before, never anyone else's service.
monado_stop_started() {
	local p mine
	[ -n "$MON_STARTED" ] || return 0
	mine=$(monado_mine | tr '\n' ' ')
	pkill -P "$MON_STARTED" -x sleep 2>/dev/null
	for _ in $(seq 1 10); do
		kill -0 "$MON_STARTED" 2>/dev/null || break
		sleep 0.5
	done
	kill -TERM -- "-$MON_STARTED" 2>/dev/null
	# shellcheck disable=SC2086
	[ -n "$mine" ] && kill -TERM $mine 2>/dev/null
	sleep 1
	for p in $mine; do kill -0 "$p" 2>/dev/null && kill -KILL "$p" 2>/dev/null; done
	kill -KILL -- "-$MON_STARTED" 2>/dev/null
	MON_STARTED=""
	return 0
}
# monado_interrupted <exit code>: the launcher was stopped (Ctrl+C, or its
# terminal window closed) while it waits for the glasses' game, in the
# foreground (launch_player1): nothing else would ever stop the Monado service
# this launch started, so stop it here. Before the game, an interrupt stops
# everything this run started (launch_interrupted, §2).
monado_interrupted() {
	trap '' INT HUP TERM   # a second interrupt must not cut the stopping short
	echo
	if [ -n "$MON_STARTED" ]; then
		monado_stop_started
		echo "!! Interrupted: stopped the Monado service (started by this run)."
	fi
	exit "$1"
}

# glasses_ready: is the glasses' head tracking live? The record's header is
# read in place: bytes 0-3 "PPOS", 4-5 version (1), 12-15 flags (bit 0 glasses
# connected, bit 2 service disabled), 24-31 a counter the service raises with
# every head sample. A counter that does not move is a stale record: the
# glasses would show a frozen picture, so that is a refusal too.
glasses_ready() {
	local st="" magic ver flags c1 c2
	command -v systemctl >/dev/null 2>&1 && st=$(systemctl --user is-active xr-driver 2>/dev/null)
	if [ ! -f "$POSE_RECORD" ]; then
		echo "!! The glasses' head tracking is not running: no $POSE_RECORD."
		glasses_fix "$st"
		return 1
	fi
	magic=$(head -c 4 "$POSE_RECORD" 2>/dev/null)
	ver=$(od -An -tu2 -j4 -N2 "$POSE_RECORD" 2>/dev/null | tr -d ' ')
	if [ "$magic" != "PPOS" ] || [ "$ver" != "1" ]; then
		echo "!! $POSE_RECORD is not a head-pose record this launcher can read"
		echo "!! (expected PPOS version 1). Update the glasses service and this launcher together."
		return 1
	fi
	flags=$(od -An -tu4 -j12 -N4 "$POSE_RECORD" 2>/dev/null | tr -d ' ')
	flags=${flags:-0}
	if [ $(( flags & 4 )) -ne 0 ]; then
		echo "!! The glasses service is switched off. Turn it on with:"
		echo "!!    xr_driver_cli -e"
		return 1
	fi
	c1=$(od -An -tu8 -j24 -N8 "$POSE_RECORD" 2>/dev/null | tr -d ' ')
	sleep 0.4
	c2=$(od -An -tu8 -j24 -N8 "$POSE_RECORD" 2>/dev/null | tr -d ' ')
	if [ $(( flags & 1 )) -eq 0 ] || [ -z "$c1" ] || [ "$c1" = "$c2" ]; then
		if [ $(( flags & 1 )) -eq 0 ]; then
			echo "!! The glasses service sees no glasses: plug them in (USB-C), wait a few seconds."
		else
			echo "!! The glasses' head tracking is stale: $POSE_RECORD stopped updating."
		fi
		glasses_fix "$st"
		return 1
	fi
	# Before the service's calibration ends the pose is fixed straight ahead:
	# wait for it (flags bit 1), bounded.
	if [ $(( flags & 2 )) -eq 0 ]; then
		echo ">> The glasses are calibrating: keep them still (on a flat surface) for a moment..."
		for _ in $(seq 1 40); do
			sleep 0.5
			flags=$(od -An -tu4 -j12 -N4 "$POSE_RECORD" 2>/dev/null | tr -d ' ')
			[ $(( ${flags:-0} & 2 )) -ne 0 ] && break
		done
		if [ $(( ${flags:-0} & 2 )) -eq 0 ]; then
			echo "!! The glasses are still calibrating after 20 s. Leave them still until"
			echo "!! calibration ends, then re-run. Nothing was launched."
			return 1
		fi
		echo ">> Calibrated."
	fi
	return 0
}
# glasses_daemon_start: with PSCREEN2_PENGUINLENS_DAEMON=DIR (the test layout, no
# systemd unit), start that head-tracking service when no record is live.
# Started detached from this launcher's terminal session (SteamOS ends every
# process of an ssh session at logout, so this is for a desktop terminal),
# logged to DIR/daemon.log; a service already running is never started twice.
# Returns 0 when a live record was seen or nothing had to be done; 1 when the
# service could not be started (glasses_ready then says what is missing).
glasses_daemon_start() {
	local d="${PSCREEN2_PENGUINLENS_DAEMON:-}" c1 c2 flags
	[ -n "$d" ] || return 0
	d=$(abs_path "$d")
	if [ -f "$POSE_RECORD" ]; then
		c1=$(od -An -tu8 -j24 -N8 "$POSE_RECORD" 2>/dev/null | tr -d ' ')
		sleep 0.4
		c2=$(od -An -tu8 -j24 -N8 "$POSE_RECORD" 2>/dev/null | tr -d ' ')
		[ -n "$c1" ] && [ "$c1" != "$c2" ] && return 0
	fi
	if pgrep -x -u "$(id -u)" xrDriver >/dev/null 2>&1; then
		echo "!! A glasses service (xrDriver) is already running but $POSE_RECORD is not live."
		echo "!! Not starting another; stop it (kill $(pgrep -x -u "$(id -u)" xrDriver | tr '\n' ' ')) and re-run."
		return 1
	fi
	if [ ! -x "$d/xr_driver/bin/xrDriver" ]; then
		echo "!! PSCREEN2_PENGUINLENS_DAEMON=$d has no xr_driver/bin/xrDriver."
		return 1
	fi
	echo ">> Starting the glasses' head-tracking service from $d (log: $d/daemon.log)..."
	setsid nohup env XDG_CONFIG_HOME="$d/config" XDG_STATE_HOME="$d/state" XDG_DATA_HOME="$d/data" \
		LD_LIBRARY_PATH="$d/xr_driver_lib" "$d/xr_driver/bin/xrDriver" > "$d/daemon.log" 2>&1 < /dev/null &
	for _ in $(seq 1 30); do
		sleep 0.5
		flags=$(od -An -tu4 -j12 -N4 "$POSE_RECORD" 2>/dev/null | tr -d ' ')
		[ $(( ${flags:-0} & 1 )) -ne 0 ] && return 0
	done
	echo ">> (The service is up but reports no glasses yet; checking the record now.)"
	return 0
}
glasses_fix() {
	if [ -n "$1" ] && [ "$1" != "active" ]; then
		echo "!! The glasses service (xr-driver) is not running (systemctl: $1). Start it with:"
		echo "!!    systemctl --user enable --now xr-driver"
	fi
	echo "!! The service starts switched off after install; turn it on with:"
	echo "!!    xr_driver_cli -e"
	echo "!! Then re-run. Nothing was launched: without head tracking the game would"
	echo "!! show in the glasses with a frozen view."
}
# glasses_installed: is the glasses' head-tracking service on this computer (the
# contract's "installed?" for PenguinLens, with the prefix)? Its pose record, its
# systemd user unit, or PSCREEN2_PENGUINLENS_DAEMON's service (the test layout).
# Without one the glasses are not offered: the game would show a frozen view.
glasses_installed() {
	local d="${PSCREEN2_PENGUINLENS_DAEMON:-}"
	[ -f "$POSE_RECORD" ] && return 0
	[ -n "$d" ] && [ -x "$(abs_path "$d")/xr_driver/bin/xrDriver" ] && return 0
	command -v systemctl >/dev/null 2>&1 || return 1
	[ "$(systemctl --user show -p LoadState --value xr-driver 2>/dev/null)" = "loaded" ]
}

# monado_prepare <runtime>: player 1's Monado headset, in place of player 1's
# WiVRn steps (the server, the tether, the pairing). It brings the runtime's
# service up (or reuses it), checks the glasses' head tracking, and leaves
# MON_PREFIX and MON_JSON (the manifest player 1's emulator binds, exported for
# it in §4 only: the other players' WiVRn servers, started before that, never
# see it); the other players' WiVRn seats are untouched by it. A service it
# started is registered in STARTED, so a launch that fails half way takes it
# down again (the one that keeps running after a game is a launch that worked).
# Exits, with nothing else started, when the headset cannot be served.
MON_PREFIX=""
MON_JSON=""
monado_prepare() {
	local rt="$1" prefix json rc
	MON_RT="$rt"
	echo "===================== $(runtime_label "$rt") ====================="
	case "$MESA_MODE" in
		auto|bundled|runtime) ;;
		*) echo "!! PSCREEN2_MESA wants auto, bundled or runtime (got: $MESA_MODE). Nothing was launched."; exit 2 ;;
	esac
	if ! prefix=$(runtime_prefix "$rt"); then
		echo "!! $(runtime_label "$rt") is not installed: no bin/monado-service in"
		if [ "$rt" = "penguinlens" ]; then
			echo "!!    ${PSCREEN2_PENGUINLENS_PREFIX:-${PSCREEN2_GLASSES_PREFIX:-~/opt/monado-penguinlens, ~/opt/monado-glasses or the same under ~/distrobox/monado}}"
			echo "!! Set PSCREEN2_PENGUINLENS_PREFIX (the older PSCREEN2_GLASSES_PREFIX still works) to the Monado build for PenguinLens."
		else
			if [ "${EMU:-}" = "flatpak" ] && [ -z "${PSCREEN2_MONADO_PREFIX:-}" ]; then
				echo "!!    the PenguinScreen2 flatpak (one from before 2026-10-15 has no Monado of its own: update it), nor in"
			fi
			echo "!!    ${PSCREEN2_MONADO_PREFIX:-~/opt/monado-psvr2 or ~/distrobox/monado/opt/monado-psvr2}"
			echo "!! Set PSCREEN2_MONADO_PREFIX to the Monado build with the PS VR2 driver."
		fi
		exit 1
	fi
	MSOCK=$(mon_sock_for "$prefix")
	if mon_in_flatpak "$prefix"; then
		json="$MON_FP_JSON"
		echo ">> Monado: the flatpak's own (commit $(cut -c1-10 "$prefix/share/penguinscreen2/monado.commit" 2>/dev/null || echo unknown)); its service is a second instance of the app, socket $MSOCK."
		mesa_report "$prefix" || exit 1
		if [ "$rt" = "monado-psvr2" ] && ! psvr2_detected; then
			echo ">> No PS VR2 seen on USB yet: plug its PC adapter in before the game starts."
		fi
	else
		json="$prefix/share/openxr/1/openxr_monado.json"
		echo ">> Monado: $prefix"
	fi
	if [ "$rt" = "penguinlens" ]; then
		glasses_daemon_start
		glasses_ready || exit 1
		echo ">> Glasses head tracking is live."
	fi
	# The headset's own GPU drives it (the one its display is plugged into):
	# nothing is forced, and PSCREEN2_GPU (a WiVRn setting) does not apply to
	# it. With more players it applies to THEIR WiVRn seats (§1), so it is only
	# "not used" when nobody wears a WiVRn headset.
	if [ "${SEATS:-1}" -le 1 ]; then
		case "$GPU_MODE" in
			auto|off) ;;
			*) echo ">> PSCREEN2_GPU=$GPU_MODE is for WiVRn only; not used for $(runtime_label "$rt")." ;;
		esac
	fi
	# a running service of ANOTHER known build (a prefix's, or the flatpak's own, on
	# its own socket) is refused and named first, whatever socket it serves
	[ -n "$(monado_pids)" ] && { mon_refuse_other "$prefix" "$(runtime_label "$rt")" || exit 1; }
	if monado_up; then
		echo ">> Monado service already running (pid $(monado_pids | tr '\n' ' ' | sed 's/ $//'); log: $MON_LOG)."
	else
		monado_guard || exit 1
		if monado_up; then
			mon_refuse_other "$prefix" "$(runtime_label "$rt")" || exit 1
			echo ">> Monado service came up (pid $(monado_pids | tr '\n' ' ' | sed 's/ $//'))."
		else
			echo ">> Starting the Monado service (log: $MON_LOG)..."
			monado_start "$prefix"; rc=$?
			if [ "$rc" -eq 2 ]; then
				mon_refuse_other "$prefix" "$(runtime_label "$rt")"
				monado_stop_started
				echo "!! Stopped the Monado service this run started."
				exit 1
			elif [ "$rc" -ne 0 ]; then
				echo "!! The Monado service did not come up — last lines of $MON_LOG:"
				tail -n 8 "$MON_LOG" 2>/dev/null | sed 's/^/!!   /'
				monado_stop_started
				echo "!! Stopped the Monado service this run started. Nothing was launched."
				exit 1
			fi
			echo ">> Monado service running (pid $(monado_mine | tr '\n' ' ' | sed 's/ $//'))."
		fi
	fi
	if [ -n "${XR_RUNTIME_JSON:-}" ] && [ "$XR_RUNTIME_JSON" != "$json" ]; then
		echo ">> Replacing XR_RUNTIME_JSON=$XR_RUNTIME_JSON for this launch."
	fi
	MON_PREFIX="$prefix"
	MON_JSON="$json"
	if [ -n "$MON_STARTED" ]; then
		echo "--- last lines of $MON_LOG ---"
		tail -n 3 "$MON_LOG" 2>/dev/null | sed 's/^/    /'
	fi
	if mon_stop_at_end; then
		echo ">> The Monado service this launch started is stopped when the game closes."
	else
		echo ">> The Monado service keeps running after the game closes; the next launch reuses it."
	fi
	if [ "${SEATS:-1}" -gt 1 ]; then
		echo ">> Player 1 wears the $(runtime_label "$rt"): put it on before the game starts."
	else
		echo ">> Put the headset on before the game starts: VR begins at game boot."
	fi
	return 0
}
# find_emu: which emulator this launch runs, decided ONCE for every path and
# before anything is started, so a wrong PSCREEN2_EXE costs nothing:
#   exe      PSCREEN2_EXE (a test AppImage, a build's pcsx2-qt)
#   flatpak  the installed flatpak (APP_ID)
#            or, when only the older install is there, that one (OLD_APP_ID), said in one line
#   native   pcsx2-qt on PATH
# EMU names it; EMU_CMD runs it (the flatpak's per-launch options go between
# `flatpak run` and the app id, see §4). Returns 1, having said why, when none.
EMU=""
EMU_CMD=()
find_emu() {
	if [ -n "${PSCREEN2_EXE:-}" ]; then
		if [ ! -x "$PSCREEN2_EXE" ] || [ ! -f "$PSCREEN2_EXE" ]; then
			echo "!! PSCREEN2_EXE=$PSCREEN2_EXE is not an executable file. Nothing was launched."
			return 1
		fi
		EMU=exe; EMU_CMD=("$PSCREEN2_EXE")
	elif flatpak info "$APP_ID" >/dev/null 2>&1; then
		EMU=flatpak; EMU_CMD=(flatpak run "$APP_ID")
	elif [ -n "${OLD_APP_ID:-}" ] && flatpak info "$OLD_APP_ID" >/dev/null 2>&1; then
		echo ">> Using the older PenguinScreen2 install ($OLD_APP_ID): $APP_ID is not installed."
		APP_ID="$OLD_APP_ID"
		EMU=flatpak; EMU_CMD=(flatpak run "$APP_ID")
	elif command -v pcsx2-qt >/dev/null 2>&1; then
		EMU=native; EMU_CMD=(pcsx2-qt)
	else
		echo "!! Emulator not found (PSCREEN2_EXE, flatpak $APP_ID or pcsx2-qt on PATH)."
		return 1
	fi
	return 0
}
# The WiVRn server's installed version, from flatpak, or "unknown". The headset's
# WiVRn app must be the SAME version (the two refuse each other otherwise), and the
# server is a plain unpinned Flathub install: a headset app that updated itself
# leaves the PC behind until the PC runs `flatpak update`. A server that was
# already running when the update landed keeps the OLD code until it is stopped.
wivrn_server_version() {
	local v
	v=$(flatpak info io.github.wivrn.wivrn 2>/dev/null | sed -n 's/^[[:space:]]*Version:[[:space:]]*//p' | head -1)
	printf '%s\n' "${v:-unknown}"
}
wivrn_update_hint() {  # <installed version>
	echo ">> WiVRn server $1 installed. The headset's WiVRn app must be the same version."
	echo ">>   If the headset says the versions are incompatible, update the PC side too:"
	echo ">>     flatpak update --user io.github.wivrn.wivrn"
	echo ">>   then headset off, pkill -f wivrn-server, and launch again (a running server keeps the old code)."
}
# wivrn_manifest: the installed WiVRn's OpenXR runtime manifest, or nothing
# (returns 1): WiVRn is installed when it has one (the contract's "installed?").
# Canonical deploy paths first — an unscoped find over whole flatpak trees walks
# the OSTree object store (multi-second stall, and it can pin a stale scope).
wivrn_manifest() {
	local d j
	for d in "$HOME/.local/share/flatpak" /var/lib/flatpak; do
		j="$d/app/io.github.wivrn.wivrn/current/active/files/share/openxr/1/openxr_wivrn.json"
		if [ -f "$j" ]; then printf '%s\n' "$j"; return 0; fi
	done
	j=$(find "$HOME/.local/share/flatpak/app/io.github.wivrn.wivrn" \
		/var/lib/flatpak/app/io.github.wivrn.wivrn \
		-name openxr_wivrn.json 2>/dev/null | head -1)
	[ -n "$j" ] && printf '%s\n' "$j"
}
# <<< runtime: functions

# The emulator, decided here for every path (one player, same game, own game
# each; WiVRn or Monado on player 1), before any question is asked (the
# flatpak's own Monado is a candidate only for the flatpak) and before any
# service is started.
find_emu || exit 1
[ "$EMU" = "exe" ] && echo ">> Emulator: $PSCREEN2_EXE (PSCREEN2_EXE)"

if [ -z "$SEATS" ] && [ -t 0 ]; then
	echo "======================== PLAYERS ========================"
	for _ in 1 2 3; do
		ask ">> How many players? [1-4, Enter = 1]: "
		case "${ASK:-1}" in
			[1-4]) SEATS="${ASK:-1}"; break ;;
			*) echo ">> Please type a number from 1 to 4." ;;
		esac
	done
fi
[ -z "$SEATS" ] && SEATS=1

# Which headset player 1 wears. Asked right after the player count, only with
# a terminal, no --runtime, and more than one kind of headset installed:
# otherwise WiVRn, and nothing below changes. A kind that is not installed is
# never offered (WiVRn: its manifest; the PS VR2: its Monado; the glasses:
# their Monado and their head-tracking service), and when the only kind
# installed is a Monado headset, it is player 1's without a question. Players
# 2-4 are not asked: they wear WiVRn headsets, each on a seat of their own.
if [ -z "$RUNTIME" ] && [ -t 0 ]; then
	RT_CHOICES=()
	wivrn_manifest >/dev/null && RT_CHOICES+=(wivrn)
	# R9: the PS VR2 driver is built into every flatpak, so its being installed
	# says nothing about who owns a PS VR2: offered only when one is plugged in
	runtime_prefix monado-psvr2 >/dev/null && psvr2_detected && RT_CHOICES+=(monado-psvr2)
	runtime_prefix penguinlens >/dev/null && glasses_installed && RT_CHOICES+=(penguinlens)
	if [ "${#RT_CHOICES[@]}" -eq 1 ] && [ "${RT_CHOICES[0]}" != "wivrn" ]; then
		RUNTIME="${RT_CHOICES[0]}"
		echo ">> The only headset installed is the $(runtime_label "$RUNTIME"): player 1 wears it."
	elif [ "${#RT_CHOICES[@]}" -gt 1 ]; then
		echo "======================== HEADSET ========================"
		if [ "$SEATS" -eq 2 ]; then
			echo ">> Player 1's headset (player 2 wears a Quest or another WiVRn headset):"
		elif [ "$SEATS" -gt 2 ]; then
			echo ">> Player 1's headset (players 2-$SEATS wear Quest or other WiVRn headsets):"
		fi
		i=1
		for rt in "${RT_CHOICES[@]}"; do
			case "$rt" in
				wivrn) echo ">> $i = Quest or another WiVRn headset (Wi-Fi or USB cable)" ;;
				*) echo ">> $i = $(runtime_label "$rt")" ;;
			esac
			i=$((i + 1))
		done
		for _ in 1 2 3; do
			if [ "$SEATS" -gt 1 ]; then
				ask ">> Player 1's headset? [1-${#RT_CHOICES[@]}, Enter = 1]: "
			else
				ask ">> Which headset? [1-${#RT_CHOICES[@]}, Enter = 1]: "
			fi
			case "${ASK:-1}" in
				[1-9])
					if [ "${ASK:-1}" -le "${#RT_CHOICES[@]}" ]; then
						RUNTIME="${RT_CHOICES[$(( ${ASK:-1} - 1 ))]}"
						break
					fi ;;
			esac
			echo ">> Please type a number from 1 to ${#RT_CHOICES[@]}."
		done
		if [ -z "$RUNTIME" ] && [ "${RT_CHOICES[0]}" = "wivrn" ]; then
			echo ">> Using 1: Quest or another WiVRn headset."
		elif [ -z "$RUNTIME" ]; then
			# no WiVRn to fall back to, and no answer names a headset
			echo "!! No headset was chosen. Nothing was started."
			exit 2
		fi
	fi
fi
[ -z "$RUNTIME" ] && RUNTIME=wivrn
# The WiVRn seats of this session: every player with player 1 on WiVRn, players
# 2-N otherwise. FIRST_WIVRN is the first player who wears one.
if [ "$RUNTIME" = "wivrn" ]; then
	WIVRN_SEATS=$SEATS; FIRST_WIVRN=1
else
	WIVRN_SEATS=$((SEATS - 1)); FIRST_WIVRN=2
fi

if [ "$SEATS" -gt 1 ] && [ -z "$MODE" ]; then
	if [ "$SEATS" -gt 2 ]; then
		# Sharing one game's split screen is a 2-player feature for now.
		MODE=separate
		echo ">> $SEATS players: each player runs their own game."
	elif [ -t 0 ]; then
		echo ">> 1 = same game for both (split screen; player 2 sees the other half)"
		echo ">> 2 = each player runs their own game"
		ask ">> Which? [1/2, Enter = 1]: "
		case "$ASK" in
			2) MODE=separate ;;
			*) MODE=cast ;;
		esac
	else
		MODE=cast
		echo ">> $SEATS players, no --mode given: both play one game (--mode cast)."
	fi
fi
if [ "$SEATS" -eq 1 ]; then
	MODE=""
elif [ "$MODE" = "cast" ] && [ "$SEATS" -ne 2 ]; then
	echo "!! --mode cast is for exactly 2 players; with $SEATS use --mode separate."
	exit 2
fi
if [ "$MODE" != "separate" ] && [ "${#GAMES[@]}" -gt 0 ]; then
	echo "!! --game is for --mode separate."
	exit 2
fi

# One game per player: every player's game is collected and checked HERE,
# before any server starts, so a typo costs nothing.
# clean_path: a path typed or dragged into a terminal (quotes, file://, spaces).
clean_path() {
	local p="$1"
	p="${p#"${p%%[![:space:]]*}"}"
	p="${p%"${p##*[![:space:]]}"}"
	case "$p" in
		\'*\'|\"*\") p="${p:1:${#p}-2}" ;;
	esac
	p="${p#file://}"
	printf '%s\n' "$p"
}
if [ "$MODE" = "separate" ]; then
	if [ "${#GAMES[@]}" -gt "$SEATS" ]; then
		echo "!! $SEATS players but ${#GAMES[@]} --game paths."
		exit 2
	fi
	n=$(( ${#GAMES[@]} + 1 ))
	tries=0
	while [ "$n" -le "$SEATS" ]; do
		if [ ! -t 0 ]; then
			echo "!! --mode separate needs --game PATH once per player ($SEATS players, ${#GAMES[@]} given)."
			exit 2
		fi
		ask ">> Player $n's game (drag the file here, or type its path): "
		g=$(clean_path "$ASK")
		if [ -f "$g" ]; then
			GAMES+=("$g"); n=$((n + 1)); tries=0
		else
			echo ">> Not a file: ${g:-<nothing typed>}"
			tries=$((tries + 1))
			[ "$tries" -ge 3 ] && { echo "!! No game for player $n — nothing was started."; exit 2; }
		fi
	done
	SEEN=""
	for g in "${GAMES[@]}"; do
		if [ ! -f "$g" ]; then
			echo "!! Not a file: $g"
			exit 2
		fi
		r=$(realpath -- "$g" 2>/dev/null || printf '%s' "$g")
		case "$SEEN" in
			*"|$r|"*)
				echo "!! The same game twice: $g"
				echo "!! Two copies of one game would write the same save data at once. Give each player a different game."
				exit 2 ;;
		esac
		SEEN="$SEEN|$r|"
		# The emulator's sandbox sees ~/PS2-Games directly; a file anywhere else
		# is handed over on its own, so a game made of several files (.cue +
		# .bin and the like) only works from ~/PS2-Games.
		if [ "$EMU" = "flatpak" ]; then
			case "$g" in
				"$HOME/PS2-Games/"*) ;;
				*)
					case "${g,,}" in
						*.cue|*.m3u|*.gdi|*.ccd|*.mds)
							echo "!! $g is made of several files: move them into ~/PS2-Games and use that path."
							exit 2 ;;
					esac ;;
			esac
		fi
	done
	# Every player after the first gets a data root of its own: its own memory
	# cards, shader cache, save states and logs. N emulators on ONE data root
	# write one Mcd001.ps2 and one shader cache at the same time, with no lock
	# (the last one to close wins the card). The emulator takes the root for one
	# launch by flag (-datapath DIR: the root is DIR/PenguinScreen2); player 1
	# keeps the usual one. Each extra root is seeded from player 1's settings at
	# every launch, with player 1's BIOS, so it plays the same.
	if [ "$EMU" = "flatpak" ]; then
		# the emulator's sandbox sees its own ~/.var/app dir at the same path
		P1_CONF="$HOME/.var/app/$APP_ID/config"
	else
		# A portable build keeps its data beside the program whatever -datapath
		# says, so its players cannot get memory cards of their own.
		d=$(dirname -- "$(readlink -f -- "$(command -v "${EMU_CMD[0]}")")")
		if [ -e "$d/portable.ini" ] || [ -e "$d/portable.txt" ]; then
			echo "!! This emulator build is portable ($d/portable.ini or portable.txt): every"
			echo "!! copy of it shares one set of memory cards, and two games writing them at once"
			echo "!! corrupt them. One game per player needs the installed (non-portable) emulator."
			echo "!! Nothing was started."
			exit 2
		fi
		case "${XDG_CONFIG_HOME:-}" in
			/*) P1_CONF="$XDG_CONFIG_HOME" ;;
			*) P1_CONF="$HOME/.config" ;;
		esac
	fi
	P1_ROOT="$P1_CONF/PenguinScreen2"
	PLAYERS_BASE="$P1_CONF/PenguinScreen2-players"
	if [ ! -f "$P1_ROOT/inis/PenguinScreen2.ini" ]; then
		echo "!! Player 1's emulator settings were not found ($P1_ROOT/inis/PenguinScreen2.ini)."
		echo "!! The other players' settings are copied from them: start PenguinScreen2 once"
		echo "!! and finish its setup (the BIOS), then re-run. Nothing was started."
		exit 2
	fi
	echo ">> Every player keeps their own memory cards and saves (players 2-$SEATS: in $PLAYERS_BASE)."
fi
# Every player's controller, checked here too, before anything starts.
if [ -z "$HEADSET_ONLY" ] && { [ "$SEATS" -gt 1 ] || [ -n "$PADS" ]; }; then
	pads_check || { echo "!! Nothing was started."; exit 2; }
fi

# ---------------------------------------------------------------------------
# 1. Decide the GPU forcing (hybrid machines only)
# ---------------------------------------------------------------------------
# For the WiVRn servers of this session, whoever wears them: with a Monado
# headset on player 1 and nobody on WiVRn there is nothing to force (the
# Monado headset is driven by the GPU its display is plugged into).
WIVRN_ENV=()
describe_gpus() { lspci -nn 2>/dev/null | grep -Ei 'VGA compatible controller|3D controller'; }

GPUS="$(describe_gpus)"
NGPU=$(printf '%s\n' "$GPUS" | grep -c . || true)

if [ "$WIVRN_SEATS" -le 0 ]; then
	: # no WiVRn seat: nothing to force
elif [ "$GPU_MODE" = "off" ]; then
	echo ">> GPU forcing disabled (PSCREEN2_GPU=off)."
elif [[ "$GPU_MODE" =~ ^[0-9a-fA-F]{4}:[0-9a-fA-F]{4}$ ]]; then
	echo ">> GPU forced by user: $GPU_MODE (Mesa device-select)."
	WIVRN_ENV+=("--env=MESA_VK_DEVICE_SELECT=${GPU_MODE}!")
elif [ "$NGPU" -le 1 ]; then
	echo ">> Single GPU — no forcing needed."
else
	echo ">> Hybrid graphics detected:"
	printf '%s\n' "$GPUS" | sed 's/^/     /'
	if printf '%s\n' "$GPUS" | grep -qi 'nvidia'; then
		# Make the NVIDIA driver the only Vulkan device WiVRn can
		# see (path is where the flatpak GL extension mounts the ICD).
		NV_ICD=/usr/lib/x86_64-linux-gnu/GL/vulkan/icd.d/nvidia_icd.json
		WIVRN_ENV+=("--env=VK_DRIVER_FILES=$NV_ICD"
			"--env=VK_ICD_FILENAMES=$NV_ICD"
			"--env=__NV_PRIME_RENDER_OFFLOAD=1"
			"--env=__GLX_VENDOR_LIBRARY_NAME=nvidia")
		echo ">> Forcing WiVRn onto the NVIDIA discrete GPU (hardware video encode)."
	else
		# AMD/Intel hybrid: both GPUs speak through Mesa, so select by PCI id —
		# prefer the discrete (a "3D controller", or the non-Intel VGA device).
		DID=$(printf '%s\n' "$GPUS" | grep -i '3D controller' | grep -oE '\[[0-9a-f]{4}:[0-9a-f]{4}\]' | tr -d '[]' | head -1)
		[ -z "$DID" ] && DID=$(printf '%s\n' "$GPUS" | grep -vi 'intel' | grep -oE '\[[0-9a-f]{4}:[0-9a-f]{4}\]' | tr -d '[]' | head -1)
		if [ -n "$DID" ]; then
			WIVRN_ENV+=("--env=MESA_VK_DEVICE_SELECT=${DID}!")
			echo ">> Forcing WiVRn onto discrete GPU $DID (Mesa device-select)."
		else
			echo ">> Could not identify the discrete GPU — leaving selection to WiVRn."
		fi
	fi
fi

# ---------------------------------------------------------------------------
# 2. Start (or reuse) the WiVRn server — HEADLESS, on the right GPU
# ---------------------------------------------------------------------------
# Auto-discovery vs the Avahi block: WiVRn's server advertises itself over
# mDNS/Avahi so the headset finds this PC by itself. SteamOS (and some distros)
# ship Avahi with user-service publishing DISABLED — and there the publish
# attempt makes the server ABORT on startup ("Server failed to start" /
# "Cannot create entry group ... Not permitted"). So when that block is present
# we start with --no-publish-service: the server comes up and the headset
# connects by IP. Running enable-vr-discovery.sh (sudo) unblocks Avahi; this
# check then leaves publishing ON and auto-discovery returns — no edit here.
PUBLISH_ARGS=()
PUBLISH_BLOCKED=0
if grep -qsE '^[[:space:]]*disable-user-service-publishing[[:space:]]*=[[:space:]]*yes' \
		/etc/avahi/avahi-daemon.conf; then
	PUBLISH_BLOCKED=1
	PUBLISH_ARGS+=(--no-publish-service)
fi

# We run the server directly (--command=wivrn-server), NOT the dashboard GUI:
# the dashboard's own server-start hits the same Avahi abort on SteamOS, and a
# headless server is deterministic and needs no window. The pairing PIN it would
# have shown in that window is printed to the log — we surface it in §3.
# >>> seats: functions (definitions only)
SOCK="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/wivrn/comp_ipc"
# Match the server by EXACT process name, never by command line: our own
# `flatpak run --command=wivrn-server ...` invocation contains that string in its
# argv, so a `pgrep -f` here would match THIS SCRIPT and report a live server
# when none exists. WiVRn also binds the socket BEFORE it can fail on avahi, so
# a crashed server leaves the socket behind — the two checks together (exact
# process AND socket) are what make this honest.
#
# Every player's server is a process with that same name, so "a wivrn-server
# exists" cannot mean "player 1's server exists" once there are more players.
# Each server is told apart by the runtime dir in its environment (player 1:
# the session's own; player N: a seat dir of its own, see below). With only
# player 1's server running, every wivrn-server counts as player 1's, exactly
# as before.

# A seat = one player = one WiVRn server with its own runtime dir (the WiVRn
# client finds its server at $XDG_RUNTIME_DIR/wivrn/comp_ipc, so a separate
# dir is a separate server), its own port and its own log. Player 1 is the
# session default, untouched. Players 2-4 live under the session's runtime
# dir, which the emulator's sandbox is allowed to reach, and which is cleared
# at logout, so a dead server's socket cannot outlive a reboot.
RUN_BASE="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
SEAT_BASE="$RUN_BASE/penguinscreen2-seats"
seat_rt() { if [ "$1" -le 1 ]; then echo "$RUN_BASE"; else echo "$SEAT_BASE/seat$1"; fi; }
seat_sock() { echo "$(seat_rt "$1")/wivrn/comp_ipc"; }
seat_port() { echo $((9757 + $1 - 1)); }
seat_log() { if [ "$1" -le 1 ]; then echo "$HOME/wivrn.log"; else echo "$HOME/wivrn-seat$1.log"; fi; }
# Where player $1's server (players 2-4) leaves the address of its own D-Bus
# session bus (see seat_start): in the seat dir, which the emulator's sandbox
# cannot reach, never in the socket dir it can.
seat_bus() { echo "$(seat_rt "$1")/bus"; }

# Which player a wivrn-server pid serves, from the runtime dir in its
# environment: player 1 only when that dir IS player 1's own ($RUN_BASE),
# players 2-4 their seat dirs (compared as real paths). Any other dir, or one
# that cannot be read, is "foreign": a server this launcher never signals
# (another supervisor's, e.g. a second WiVRn under its own runtime dir, maybe
# with a headset on it), so stopping player 1 never stops it.
# One sandbox equivalent: player 1's server is started
# WITHOUT --env=XDG_RUNTIME_DIR (start_server), so inside the flatpak it sees
# /run/user/UID, which flatpak backs with the host's runtime dir; where
# $RUN_BASE is another path, a FLATPAK'd server (its root holds /.flatpak-info)
# showing /run/user/UID is player 1's. A seat 2-4 server is always handed its
# dir by --env, so it never takes that branch.
pid_rt() { tr '\0' '\n' < "/proc/$1/environ" 2>/dev/null | sed -n 's/^XDG_RUNTIME_DIR=//p' | head -1; }
same_rt() { [ "$(realpath -m -- "$1" 2>/dev/null)" = "$(realpath -m -- "$2" 2>/dev/null)" ]; }
seat_of_pid() {
	local rt n
	rt=$(pid_rt "$1")
	[ -n "$rt" ] || { echo foreign; return 0; }
	same_rt "$rt" "$RUN_BASE" && { echo 1; return 0; }
	[ "$rt" = "/run/user/$(id -u)" ] && [ -e "/proc/$1/root/.flatpak-info" ] && { echo 1; return 0; }
	for n in 2 3 4; do same_rt "$rt" "$SEAT_BASE/seat$n" && { echo "$n"; return 0; }; done
	echo foreign
}
# foreign_note: name every foreign WiVRn server, left alone (never signalled).
foreign_note() {
	local p
	for p in $(pgrep -x wivrn-server); do
		[ "$(seat_of_pid "$p")" = foreign ] || continue
		echo "!! Left alone: WiVRn server pid $p (runtime dir $(pid_rt "$p" | sed 's/^$/unreadable/')) is not a seat of this launcher; it is never touched."
	done
}
# pids of player $1's server: exact process name, never the command line.
# TWO LISTS. seat_pids = the processes that MAY be player $1's,
# the reading a process check makes: for player 1 that includes every FOREIGN
# server (so a process check alone is fooled by one: liveness is the listener,
# and the no-ss fallback stays conservative: it never clears a socket while one
# runs). seat_own_pids = the processes that ARE player $1's (its own runtime
# dir): the ONLY list anything is ever signalled from (seat_guard's debris,
# stop_started). A foreign server is in the first list and never in the second.
seat_pids() {
	local p s
	for p in $(pgrep -x wivrn-server); do
		s=$(seat_of_pid "$p")
		if [ "$s" = "$1" ] || { [ "$1" = 1 ] && [ "$s" = foreign ]; }; then echo "$p"; fi
	done
}
seat_own_pids() {
	local p
	for p in $(pgrep -x wivrn-server); do
		[ "$(seat_of_pid "$p")" = "$1" ] && echo "$p"
	done
	return 0
}

# A socket is alive only while something LISTENS on it. The file alone proves
# nothing: a server that dies leaves it behind. Where `ss` is missing, fall
# back to the launcher's usual pair: the player's own server process AND the
# socket file. `ss` prints the path unescaped (a runtime dir with a space in
# it): matched as one string followed by the inode, never field by field.
have_ss() { command -v ss >/dev/null 2>&1; }
# `ss` present but failing (a netlink error) cannot see a listener: that is
# "could not look", never "nothing listens". ss_refuse stops what this run
# started (stop_started leaves servers alone while ss fails) and exits 2.
ss_broken() { have_ss && ! ss -xl >/dev/null 2>&1; }
ss_refuse() {
	echo "!! Cannot look at the sockets: \`ss -xl\` fails here ($(ss -xl 2>&1 >/dev/null | head -1))."
	echo "!! Player $1's server is not checked, cleared or stopped: could not look is not the same as dead."
	stop_started
	echo "!! Nothing was launched."
	exit 2
}
sock_listening() {
	ss -xl 2>/dev/null | SOCK_AT=" $1 " awk 'index($0 " ", ENVIRON["SOCK_AT"]) { f = 1 } END { exit !f }'
}
seat_up() {
	local s
	s=$(seat_sock "$1")
	[ -S "$s" ] || return 1
	if have_ss; then
		ss_broken && ss_refuse "$1"
		sock_listening "$s"
	else
		[ -n "$(seat_pids "$1")" ]
	fi
}
# The sockets whose LOCAL address is tcp/udp port $1, in every state. A paused
# WiVRn session child keeps a seat's UDP port as a CONNECTED socket (state
# ESTAB), which `ss -l` never lists: the port read as free and a new server was
# started into it.
port_sockets() { ss -Htunap 2>/dev/null | awk -v p="$1" '$5 ~ ("[:.]" p "$")'; }
# wivrn-server pids holding tcp/udp port $1. Only WiVRn's own processes are
# ever candidates for cleanup; anything else on the port is reported, never
# touched.
port_wivrn_holders() {
	local p
	for p in $(port_sockets "$1" | grep -oE 'pid=[0-9]+' | cut -d= -f2 | sort -u); do
		[ "$(cat "/proc/$p/comm" 2>/dev/null)" = "wivrn-server" ] && echo "$p"
	done
}
port_held() { port_sockets "$1" | grep -q .; }

# Make player $1's seat safe to start, and never touch a live server.
#   1. a socket nobody listens on is cleared (it can only mislead);
#   2. that player's server processes with no listener (a crashed server, or a
#      connection child it left behind holding the port) are stopped;
#   3. the port must be free before a start (a dead server's port can stay
#      busy for up to ~90 s), polled, bounded.
seat_guard() {
	local n="$1" s port debris p
	s=$(seat_sock "$n"); port=$(seat_port "$n")
	seat_up "$n" && return 0
	foreign_note
	if ! have_ss; then
		# Without ss, a listener cannot be seen: clear the socket only when
		# no process of this player exists at all.
		[ -z "$(seat_pids "$n")" ] && rm -f -- "$s"
		return 0
	fi
	if [ -e "$s" ]; then
		rm -f -- "$s" && echo ">> Player $n: cleared a dead server socket (nothing was listening)."
	fi
	# Only THIS player's own server processes are ever stopped. A WiVRn server
	# with some other runtime dir holding the port was not started by this
	# launcher and may have a headset connected (stopping a server under a
	# connected headset crashes WiVRn): report it and refuse, never touch it.
	for p in $(port_wivrn_holders "$port"); do
		if [ "$(seat_of_pid "$p")" != "$n" ]; then
			echo "!! Player $n: port $port is in use by another WiVRn server (pid $p) that this"
			echo "!! launcher did not start. Close that server (with no headset connected to it),"
			echo "!! then re-run."
			return 2
		fi
	done
	debris=$(seat_own_pids "$n" | tr '\n' ' ')
	if [ -n "${debris// /}" ]; then
		echo ">> Player $n: stopping leftover server process(es) with no live socket: $debris"
		# shellcheck disable=SC2086
		kill -TERM $debris 2>/dev/null
		sleep 2
		for p in $debris; do kill -0 "$p" 2>/dev/null && kill -KILL "$p" 2>/dev/null; done
		echo ">> (If that headset's picture looks smeared after it reconnects, restart"
		echo ">>  the WiVRn app ON THE HEADSET first.)"
	fi
	for _ in $(seq 1 45); do
		port_held "$port" || return 0
		sleep 2
	done
	echo "!! Player $n: port $port is still in use after 90 s — not starting a server on it."
	port_sockets "$port" | sed 's/^/!!   /'
	return 2
}

# Player $1's server settings: player 1's WiVRn settings (encoder, bitrate...)
# with this seat's port. They ride WiVRn's -f flag, because the flatpak
# replaces any XDG_CONFIG_HOME it is given; the same forced config dir is why
# every seat shares one list of paired headsets.
seat_config() {
	local base="$HOME/.var/app/io.github.wivrn.wivrn/config/wivrn/config.json"
	if command -v python3 >/dev/null 2>&1; then
		python3 - "$base" "$(seat_port "$1")" <<'PY'
import json, sys
try:
    c = json.load(open(sys.argv[1]))
    if not isinstance(c, dict):
        c = {}
except Exception:
    c = {}
c.pop("application", None)   # never start player 1's auto-launch app twice
c["port"] = int(sys.argv[2])
print(json.dumps(c, indent=1))
PY
	else
		printf '{ "port": %s }\n' "$(seat_port "$1")"
	fi
}

# Where player $1's server settings live: in the seat dir, NOT in its wivrn/
# socket dir. Only the socket dir is opened to the emulator's sandbox, so the
# emulator can neither read nor replace the file a server is started with.
seat_cfg() { echo "$(seat_rt "$1")/config.json"; }
# Write it without ever following a link planted at that path: a fresh file
# made by mktemp (exclusive create), then renamed over whatever is there.
seat_write_config() {
	local cfg tmp
	cfg=$(seat_cfg "$1")
	tmp=$(mktemp "$(seat_rt "$1")/.config.XXXXXX") || return 1
	if seat_config "$1" > "$tmp" && mv -f -- "$tmp" "$cfg"; then
		return 0
	fi
	rm -f -- "$tmp"
	return 1
}

# Players 2-4 each run their server under a D-Bus session bus of their own
# (dbus-run-session). On the user's bus a second server shares player 1's bus
# names, and a player-2 server there has been seen to die on a stray connection
# that the same server under dbus-run-session survived; a new headset is also
# paired over the seat's OWN bus. Player 1 stays on the user's bus.
have_drs() { command -v dbus-run-session >/dev/null 2>&1; }

# Start player $1's server (players 2-4), headless, on the same GPU as player
# 1's. Returns 0 once its socket is live, 2 when the guard refused to start it
# (it has already said why), 3 when this computer lacks something a seat needs
# (also said), 1 when the server did not come up. SEAT_START_PID is the process
# this call started (empty when it started none), registered in STARTED at once.
SEAT_START_PID=""
seat_start() {
	local n="$1" rt log ep rc
	SEAT_START_PID=""
	if ! have_drs; then
		echo "!! Player $n needs dbus-run-session, which is not installed on this computer"
		echo "!! (Ubuntu/Debian package: dbus-daemon). Install it, then re-run."
		return 3
	fi
	rt=$(seat_rt "$n"); log=$(seat_log "$n")
	mkdir -p "$rt/wivrn" || return 1
	chmod 700 "$SEAT_BASE" "$rt" 2>/dev/null
	# The seat's own runtime dir hides the desktop's audio sockets, and WiVRn
	# stops during session setup with "No audio backend available". Link the
	# session's audio endpoints into the seat dir.
	for ep in pipewire-0 pipewire-0.lock pipewire-0-manager pipewire-0-manager.lock pulse; do
		[ -e "$RUN_BASE/$ep" ] && ln -sfn "$RUN_BASE/$ep" "$rt/$ep"
	done
	seat_write_config "$n" || return 1
	seat_guard "$n"; rc=$?
	[ "$rc" -eq 0 ] || return 2
	# The seat's bus address is written down first (seat_bus), by the shell
	# dbus-run-session starts for the server: it is how this launcher reaches
	# THAT server (pairing, seat_pairing_pin) when player 1's server is not
	# there to pair on. Then the server replaces that shell.
	rm -f -- "$(seat_bus "$n")"
	setsid nohup dbus-run-session -- bash -c 'printf "%s\n" "${DBUS_SESSION_BUS_ADDRESS:-}" > "$0"; exec "$@"' \
		"$(seat_bus "$n")" flatpak run "${WIVRN_ENV[@]}" \
		--env=XDG_RUNTIME_DIR="$rt" --filesystem="xdg-run/${rt#"$RUN_BASE"/}" \
		--command=wivrn-server io.github.wivrn.wivrn \
		-f "$(seat_cfg "$n")" --no-publish-service \
		> "$log" 2>&1 < /dev/null &
	SEAT_START_PID=$!
	STARTED+=("$SEAT_START_PID:server:$n")
	for _ in $(seq 1 40); do seat_up "$n" && return 0; sleep 0.5; done
	return 1
}
# seat_pairing_pin <player> <seconds>: open pairing on player $1's server for
# that long and print the PIN it hands out. The server takes the call on its
# own bus only (io.github.wivrn.Server, EnablePairing(timeout) -> PIN; the
# dashboard and `wivrnctl` see the user's bus, which is player 1's server). The
# bus address comes from seat_bus. Prints nothing (returns 1) when the address
# is unknown, busctl is missing, or the server refused (it refuses while a
# headset session is active).
seat_pairing_pin() {
	local addr out
	addr=$(head -n 1 -- "$(seat_bus "$1")" 2>/dev/null)
	[ -n "$addr" ] || return 1
	command -v busctl >/dev/null 2>&1 || return 1
	out=$(busctl --address="$addr" call io.github.wivrn.Server /io/github/wivrn/Server \
		io.github.wivrn.Server EnablePairing i "$2" 2>/dev/null) || return 1
	printf '%s\n' "$out" | sed -n 's/^s "\(.*\)"$/\1/p' | head -n 1 | grep .
}

# Everything THIS run started, so a launch that fails half way can take it down
# again, and never anything it did not start. Entries:
# "<pid>:<server|emulator|monado>:<player>" (monado: player 1's Monado service,
# when monado_prepare started it).
STARTED=()
# stop_pids <pid>...: each was started in a session of its own, so TERM its
# process group (its children with it), then the pid; KILL whatever is left
# after 5 s.
stop_pids() {
	local p alive
	for p in "$@"; do kill -TERM -- "-$p" 2>/dev/null || kill -TERM "$p" 2>/dev/null; done
	for _ in $(seq 1 10); do
		alive=0
		for p in "$@"; do kill -0 "$p" 2>/dev/null && alive=1; done
		[ "$alive" = 0 ] && return 0
		sleep 0.5
	done
	for p in "$@"; do kill -KILL -- "-$p" 2>/dev/null; kill -KILL "$p" 2>/dev/null; done
	return 0
}
# Stop what this run started, newest first, and say what was stopped. A server
# with a headset connected is left running and named: stopping WiVRn under a
# connected headset crashes it.
stop_started() {
	local i e pid what n port
	# Always followed by an exit: a second Ctrl+C (or TERM) while it stops must not
	# cut it short and leave the rest running.
	trap '' INT HUP TERM
	for (( i = ${#STARTED[@]} - 1; i >= 0; i-- )); do
		e=${STARTED[$i]}; pid=${e%%:*}; e=${e#*:}; what=${e%%:*}; n=${e#*:}
		if [ "$what" = "server" ]; then
			port=$(seat_port "$n")
			if ss_broken; then
				echo "!! Left player $n's server running: \`ss\` fails, so a connected headset cannot be ruled out."
				continue
			fi
			if ss -tn state established "( sport = :$port )" 2>/dev/null | tail -n +2 | grep -q .; then
				echo "!! Left player $n's server running: a headset is connected to it."
				continue
			fi
			# shellcheck disable=SC2046
			stop_pids "$pid" $(seat_own_pids "$n")
			echo "!! Stopped player $n's server (started by this run)."
			foreign_note
		elif [ "$what" = "monado" ]; then
			monado_stop_started
			echo "!! Stopped the Monado service (started by this run)."
		else
			# one that already exited (it stopped right away) has nothing to stop
			kill -0 "$pid" 2>/dev/null || continue
			stop_pids "$pid"
			echo "!! Stopped player $n's emulator (started by this run, pid $pid)."
		fi
	done
	STARTED=()
}
# seat_watch <game pid> (G15h, cast mode): while player 1's game runs, watch the
# servers of players 2-N; a server that stops (its process gone, or its socket no
# longer listening) is reported LOUDLY, once, naming the player and the runtime dir
# it served. It never restarts a server and never stops anything: restarting
# WiVRn under a connected headset crashes it, and the game decides about its
# own seat (SeatSession's FAILED line). Ends when the game does.
seat_watch() {
	local game="$1" n pid e down=" "
	while kill -0 "$game" 2>/dev/null; do
		for n in $(seq 2 "$SEATS"); do
			case "$down" in *" $n "*) continue ;; esac
			ss_broken && continue   # could not look: no report, and never ss_refuse from here
			pid=""
			for e in "${STARTED[@]}"; do [ "${e#*:}" = "server:$n" ] && pid=${e%%:*}; done
			if seat_up "$n" && { [ -z "$pid" ] || kill -0 "$pid" 2>/dev/null; }; then continue; fi
			kill -0 "$game" 2>/dev/null || break
			down="$down$n "
			echo "!! ================================================================"
			echo "!! PLAYER $n's WiVRn server stopped during the game: seat $n FAILED."
			echo "!!   runtime dir: $(seat_rt "$n")   server log: $(seat_log "$n")"
			echo "!!   It is NOT restarted. Player 1's game goes on; to bring player $n back,"
			echo "!!   quit the game and run this launcher again."
			echo "!! ================================================================"
		done
		sleep "${PSCREEN2_SEAT_WATCH_SECS:-2}"
	done
}
# launch_interrupted <exit code>: the launcher was stopped (Ctrl+C, its terminal
# window closed, a TERM) while it still sets the session up: everything this run
# started (the Monado service, the players' servers, the games already running)
# is stopped through stop_started, as for a launch that fails half way, and
# nothing else. Set before anything starts (§2); player 1's emulator exec'd in
# this script's place drops it.
launch_interrupted() {
	trap '' INT HUP TERM   # a second interrupt must not cut the stopping short
	echo
	echo "!! Interrupted."
	stop_started
	echo "!! Nothing more was launched."
	exit "$1"
}

server_proc() { [ -n "$(seat_pids 1)" ]; }
# Player 1 is judged like every other player: alive only while something
# LISTENS on its socket. A process check alone is fooled by any WiVRn server
# whose runtime dir is not a seat's (another seat on this PC, another user's):
# it counts as player 1's, so a dead player-1 socket read as a live server,
# nothing was started, and the game ran flat. Without `ss`, the old pair.
server_up() { seat_up 1; }
# <<< seats: functions

# Start the server once; sets STARTED_BY_US on success. $1 = extra args.
start_server() {
	# Clear a stale socket left by a crashed server — but never a LIVE server's.
	# With `ss`: a socket nobody listens on is dead, whoever else is running.
	# Without it: only when no server process is alive at all.
	if have_ss; then
		sock_listening "$SOCK" || rm -f "$SOCK"
	else
		server_proc || rm -f "$SOCK"
	fi
	setsid nohup flatpak run "${WIVRN_ENV[@]}" --command=wivrn-server \
		io.github.wivrn.wivrn "$@" \
		> "$HOME/wivrn.log" 2>&1 < /dev/null &
	for _ in $(seq 1 40); do server_up && return 0; sleep 0.5; done
	return 1
}

# The WiVRn manifest, looked up before anything starts (wivrn_manifest). Every
# player but player 1 is a WiVRn seat and binds it whatever player 1 binds: it
# is looked up on its own whenever anyone wears WiVRn, and a pre-set
# XR_RUNTIME_JSON never reaches those players.
WIVRN_JSON=""
if [ "$WIVRN_SEATS" -gt 0 ] && { [ -z "${XR_RUNTIME_JSON:-}" ] || [ "$SEATS" -gt 1 ]; }; then
	WIVRN_JSON=$(wivrn_manifest)
fi
# Players 2-N are WiVRn seats and nothing else may bind them: without the
# manifest they would run flat, or, with player 1 on Monado, be handed the
# wrong runtime, whatever player 1 wears. A refusal, not a warning, and before
# any service, server or pairing (one player on WiVRn keeps its warning, §4).
if [ "$SEATS" -gt 1 ] && [ -z "$WIVRN_JSON" ]; then
	if [ "$SEATS" -eq 2 ]; then
		echo "!! WiVRn OpenXR runtime manifest not found: player 2 cannot enter VR."
	else
		echo "!! WiVRn OpenXR runtime manifest not found: players 2-$SEATS cannot enter VR."
	fi
	echo "!! Check that WiVRn is installed (flatpak io.github.wivrn.wivrn), then re-run."
	echo "!! Nothing was started."
	exit 1
fi

# From here on this run starts things: an interrupt stops what it started.
trap 'launch_interrupted 130' INT
trap 'launch_interrupted 129' HUP
trap 'launch_interrupted 143' TERM

# Player 1 on a Monado headset: its service first (it is seat 1, and nothing
# else has started yet, so a headset that cannot be served costs nothing).
if [ "$RUNTIME" != "wivrn" ]; then
	monado_prepare "$RUNTIME"
fi

STARTED_BY_US=0
if [ "$RUNTIME" != "wivrn" ]; then
	: # player 1's headset is not WiVRn: no server of player 1's to start or reuse
elif server_up; then
	# A GPU override can only apply when THIS script starts the server — an
	# explicit PSCREEN2_GPU that silently no-ops is worse than an error.
	if [ "$GPU_MODE" != "auto" ] && [ "$GPU_MODE" != "off" ]; then
		echo "!! PSCREEN2_GPU=$GPU_MODE cannot apply: a WiVRn server is ALREADY running"
		echo "!! and GPU selection happens at server start. To apply the override:"
		echo "!!   1. Disconnect the headset (take it off / close its WiVRn app)."
		echo "!!   2. Stop the server:  pkill -f wivrn-server"
		echo "!!      (never stop it while the headset is connected — known crash)"
		echo "!!   3. Re-run:  PSCREEN2_GPU=$GPU_MODE bash $0"
		exit 1
	fi
	echo ">> WiVRn server already running (GPU forcing not re-applied)."
	wivrn_update_hint "$(wivrn_server_version)"
else
	wivrn_update_hint "$(wivrn_server_version)"
	if [ "$PUBLISH_BLOCKED" = "1" ]; then
		echo ">> Starting WiVRn server (headless; mDNS publishing off — this OS blocks it, connect by IP)."
	else
		echo ">> Starting WiVRn server (headless; mDNS auto-discovery on)."
	fi
	STARTED_BY_US=1
	if ! start_server "${PUBLISH_ARGS[@]}"; then
		# The server aborts whenever it cannot advertise — and the avahi config
		# is only ONE way that happens (the daemon can also be absent, stopped or
		# masked, which no config check can see). Rather than enumerate causes,
		# retry once with advertising off: it fixes all of them, and costs a few
		# seconds only on a machine that was going to fail anyway.
		if [ "$PUBLISH_BLOCKED" != "1" ]; then
			echo ">> Server did not start — retrying with mDNS publishing off..."
			PUBLISH_BLOCKED=1
			start_server --no-publish-service || true
		fi
	fi
	if ! server_up; then
		echo "!! WiVRn server did not come up — last lines of ~/wivrn.log:"
		tail -n 8 "$HOME/wivrn.log" 2>/dev/null | sed 's/^/!!   /'
		echo "!! Nothing above about Avahi? Then check that WiVRn is installed and"
		echo "!! working:  flatpak run io.github.wivrn.wivrn"
		exit 1
	fi
	# Loud verification. NOTE: the headless server logs almost nothing until a
	# headset actually connects — its Vulkan/GPU lines appear at CONNECT time,
	# not at start — so an unconditional "here's the GPU" dump prints an empty
	# section and reads like a failure. Report the selection WE made (that part
	# is knowable now), and only echo log lines if the server really wrote any.
	sleep 2
	if [ "${#WIVRN_ENV[@]}" -gt 0 ]; then
		echo "--- GPU selection forced by this script ---"
		printf '    %s\n' "${WIVRN_ENV[@]}"
	fi
	GPU_LINES=$(grep -iE "physical device|GPU|NVIDIA|Radeon|Intel|nvenc|vaapi" "$HOME/wivrn.log" 2>/dev/null | head -6)
	if [ -n "$GPU_LINES" ]; then
		echo "--- WiVRn GPU (from its log) ---"
		printf '%s\n' "$GPU_LINES"
	else
		echo ">> (WiVRn logs its GPU/encoder details when the headset connects — see ~/wivrn.log then.)"
	fi
fi

# Pairing: a fresh WiVRn accepts no headset until it has been paired once via
# its window (PIN). Paired headsets live in known_keys.json — check both the
# flatpak data dir and the native config dir. One list,
# shared with the wait-loop heartbeat so they can never poll different files.
KNOWN_KEYS_CANDIDATES=(
	"$HOME/.var/app/io.github.wivrn.wivrn/config/wivrn/known_keys.json"
	"${XDG_CONFIG_HOME:-$HOME/.config}/wivrn/known_keys.json"
)
paired_now() {
	local k
	for k in "${KNOWN_KEYS_CANDIDATES[@]}"; do
		[ -s "$k" ] && grep -q '"key"' "$k" 2>/dev/null && return 0
	done
	return 1
}
# How many headsets this PC has paired. Every player's seat shares this one
# list (the flatpak forces WiVRn's config dir), so "some headset is paired"
# says nothing about whether EACH player's headset is.
paired_count() {
	local k c best=0
	for k in "${KNOWN_KEYS_CANDIDATES[@]}"; do
		[ -s "$k" ] || continue
		c=$(grep -o '"key"' "$k" 2>/dev/null | wc -l)
		[ "$c" -gt "$best" ] && best=$c
	done
	echo "$best"
}
# Enough headsets paired for this session: one WiVRn headset = any paired
# headset (as always); N of them = at least N. The headsets that count are
# the WiVRn ones (every player with player 1 on WiVRn, players 2-N otherwise).
paired_enough() {
	if [ "$WIVRN_SEATS" -le 1 ]; then
		paired_now
	else
		[ "$(paired_count)" -ge "$WIVRN_SEATS" ]
	fi
}

# ---------------------------------------------------------------------------
# 2b. USB-C tether (the Steam Deck path): player 1's WiVRn headset only
# ---------------------------------------------------------------------------
# On Deck-class hardware, Wi-Fi streaming cannot keep up (the same box is
# emulating, encoding, AND radioing). A USB-C cable to the headset removes the
# radio entirely: `adb reverse tcp:9757 tcp:9757` makes the HEADSET's own
# localhost:9757 arrive at this PC over the cable, so the WiVRn client connects
# to 127.0.0.1 and never touches Wi-Fi.
#
# Requirements (both one-time): a data-capable USB-C cable, and Developer Mode
# enabled on the headset (adb refuses otherwise — the headset shows an "Allow
# USB debugging?" prompt on first use; accept it, tick "always").
# PSCREEN2_USB=off skips all of this.
#
# With the tether up, the headset's WiVRn app is opened over the same cable with
# WiVRn's own documented link (its README): `wivrn+tcp://localhost` connects with
# TCP only, which is all `adb reverse` carries (without it WiVRn 26.x times out
# its UDP stream handshake). Best effort: an app that is not installed, or one
# that does not open, falls back to the manual steps printed in §3.
WIVRN_PKG=org.meumeu.wivrn
find_adb() {
	command -v adb 2>/dev/null && return 0
	local a="$HOME/.local/share/penguinscreen2/platform-tools/adb"
	[ -x "$a" ] && { echo "$a"; return 0; }
	return 1
}
USB_ACTIVE=0
USB_OPENED=0
USB_PIN=""
# usb_pairing_pin: open pairing on player 1's server for 10 minutes and print the PIN it hands out (`wivrnctl pair`
# prints "PIN: NNNNNN"). Nothing printed = the server refused or wivrnctl is missing.
usb_pairing_pin() {
	flatpak run --command=wivrnctl io.github.wivrn.wivrn pair -d 10 2>/dev/null | sed -n 's/^PIN: *\([0-9]\{4,8\}\).*/\1/p' | head -n 1 | grep .
}
USB_NET=""
if [ "$RUNTIME" = "wivrn" ] && [ "${PSCREEN2_USB:-auto}" != "off" ] && QUEST_DIR=$(usb_quest_dir) && USB_NET=$(usb_net_iface "$QUEST_DIR"); then
	# The headset is already a network adapter on the cable (its WiVRn app's "USB networking"): nothing to tether.
	# The link carries IPv6 link-local only; NetworkManager takes the interface DOWN when its IPv4 attempt times
	# out unless a profile allows that (NetworkManager shows "connecting (getting IP configuration)",
	# then "disconnected"). operstate says which it is.
	if [ "$(cat "$QUEST_DIR"*:*/net/"$USB_NET"/operstate 2>/dev/null | head -1)" != "up" ]; then
		echo "!! USB NETWORK link on the cable ($USB_NET) is DOWN: NetworkManager dropped it (no IPv4 there, by design)."
		echo "!!   One time: run  bash enable-vr-discovery.sh  (it adds the profile that keeps this link up), then"
		echo "!!   unplug and replug the cable and run this again."
	else
		echo ">> USB NETWORK link up on the cable ($USB_NET): the headset connects over it, no adb needed."
	fi
	if [ "$PUBLISH_BLOCKED" = "1" ]; then
		echo ">>   It finds this PC by discovery, which this OS blocks until 'bash enable-vr-discovery.sh' has run"
		echo ">>   once (it also adds the network profile the link needs). Without it: connect by this PC's address."
	fi
elif [ "$RUNTIME" = "wivrn" ] && [ "${PSCREEN2_USB:-auto}" != "off" ] && ADB=$(find_adb); then
	# `adb get-state` is quiet and definitive: "device" = authorized and ready.
	USB_STATE=$("$ADB" get-state 2>/dev/null || true)
	# FIRST CONNECT: the very first
	# time a cable is plugged in, the headset shows "Allow USB debugging?" and adb reports `unauthorized` until the
	# player accepts it. Telling them to re-run was the hoop. Wait here instead, up to 3 minutes, saying exactly what
	# to do; the state flips to `device` the moment they accept.
	if [ "$USB_STATE" = "unauthorized" ]; then
		echo ">> USB headset found on the cable, waiting for you to allow it (first time only):"
		echo ">>    put the headset ON, find the 'Allow USB debugging?' box, tick 'Always allow', press OK."
		# PSCREEN2_USB_AUTH_WAIT: seconds to wait (default 180; the tests shorten it).
		USB_AUTH_WAIT=${PSCREEN2_USB_AUTH_WAIT:-180}
		for i in $(seq 1 $(( (USB_AUTH_WAIT + 1) / 2 ))); do
			sleep 2
			USB_STATE=$("$ADB" get-state 2>/dev/null || true)
			[ "$USB_STATE" = "device" ] && { echo ">> Allowed. Thanks."; break; }
			[ $((i % 15)) -eq 0 ] && echo ">>    still waiting for 'Allow USB debugging?' on the headset ($((i * 2)) s of $USB_AUTH_WAIT)..."
		done
	fi
	if [ "$USB_STATE" = "device" ]; then
		if "$ADB" reverse tcp:9757 tcp:9757 >/dev/null 2>&1; then
			USB_ACTIVE=1
			echo ">> USB tether ACTIVE: headset localhost:9757 now reaches this PC over the cable."
			# A headset this PC has never paired will be asked for a PIN as soon as its app connects: have the
			# PIN READY and on screen BEFORE the app opens, not buried in a later paragraph. `wivrnctl pair` opens
			# pairing for 10 minutes and returns the PIN; the server's own first-run PIN (in its log) is the fallback.
			if ! paired_now; then
				USB_PIN=$(usb_pairing_pin || true)
				[ -z "$USB_PIN" ] && [ "$STARTED_BY_US" = "1" ] && \
					USB_PIN=$(grep -a "PIN code" "$HOME/wivrn.log" 2>/dev/null | tail -1 | grep -oE '[0-9]{4,8}' | tail -1)
				echo ">>"
				echo ">> ===================  FIRST CONNECT: YOUR PAIRING PIN IS  ${USB_PIN:-<see below>}  ==================="
				echo ">>    The headset's WiVRn app opens by itself now. When it asks for a PIN, type:  ${USB_PIN:-<see below>}"
				echo ">>    (one time only; after that the headset is paired for good)"
				echo ">> ======================================================================================="
				echo ">>"
			fi
			# `am start` exits 0 even when it fails; its output says "Error" then.
			if server_up && "$ADB" shell pm path "$WIVRN_PKG" 2>/dev/null | grep -q '^package:' \
				&& AM_OUT=$("$ADB" shell am start -a android.intent.action.VIEW \
					-d "wivrn+tcp://localhost" "$WIVRN_PKG" 2>&1) \
				&& ! printf '%s\n' "$AM_OUT" | grep -qi 'error'; then
				USB_OPENED=1
				echo ">> Opened the WiVRn app on the headset: it connects over the cable (TCP only)."
			else
				echo ">> Could not open the WiVRn app on the headset; connect it by hand (steps below)."
			fi
		else
			echo ">> USB headset detected but 'adb reverse' failed — falling back to Wi-Fi."
		fi
	elif [ "$USB_STATE" = "unauthorized" ]; then
		echo ">> The headset never allowed USB debugging ($USB_AUTH_WAIT s). Wi-Fi steps follow; or accept the"
		echo ">>   'Allow USB debugging?' box on the headset and run this again."
	elif FAULT=$(usb_cable_fault); then
		echo "!! No headset on USB, and the kernel says why: $FAULT"
		echo "!!   The cable (or its plug) failed: reseat it or use another data-capable USB-C cable; if the headset"
		echo "!!   still does not appear, switch 'USB networking' off and on in its WiVRn app or restart it."
		echo "!!   Until then the headset connects over Wi-Fi, which dips and drops on a Deck."
	elif QUEST_DIR=$(usb_quest_dir); then
		# A headset IS on the cable but adb sees nothing: the one state the old script passed over in silence,
		# which is how a first connect "did nothing".
		if usb_adb_iface "$QUEST_DIR"; then
			echo "!! A Quest is on the USB cable and offers USB debugging, but adb cannot open it."
			echo "!!   Unplug and replug the cable and run this again. If it keeps happening, this PC is"
			echo "!!   refusing adb access to the device (a udev rule for vendor 2833 fixes that)."
		else
			echo "!! A Quest is on the USB cable, but it is not offering a connection over it yet. Two ways, one time:"
			echo "!!   EASIEST (no developer account): in the headset's WiVRn app (26.9 or newer, Horizon OS 2.6+):"
			echo "!!     Settings > System > 'USB networking' > ON, and run 'bash enable-vr-discovery.sh' once here."
			echo "!!   OR (older app or headset software): USB debugging, which needs Developer Mode on the headset:"
			echo "!!     phone's Meta Horizon app > Menu > Devices > your headset > Headset settings > Developer Mode"
			echo "!!     (a free developer account), then accept 'Allow USB debugging?' on the headset."
			echo "!!   Then unplug and replug the cable and run this again."
		fi
		echo ">> Wi-Fi steps follow meanwhile."
	fi
	# no state at all = no cable / no dev mode: stay silent, Wi-Fi path follows.
fi

# ---------------------------------------------------------------------------
# 3. Tell the user EXACTLY how to connect the headset (SteamOS field reality)
# ---------------------------------------------------------------------------
# LAN IP: `hostname` does not exist on SteamOS — read the routing table, then
# fall back to the interface table (works with no default route), then to
# hostname -I where it exists.
LAN_IP=$(ip route get 1.1.1.1 2>/dev/null | grep -oE 'src [0-9.]+' | awk '{print $2}')
[ -z "$LAN_IP" ] && LAN_IP=$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)
[ -z "$LAN_IP" ] && LAN_IP=$(hostname -I 2>/dev/null | awk '{print $1}')

# Auto-discovery reality: we know it directly from PUBLISH_BLOCKED — the same
# Avahi-config check that decided whether the server may publish at all. The
# headset can only auto-find this PC when the server is allowed to advertise
# over mDNS, so the two are one decision.
if [ "$PUBLISH_BLOCKED" = "1" ]; then
	DISCOVERY="will NOT appear automatically — this OS blocks mDNS (run 'sudo ./enable-vr-discovery.sh' once to turn discovery on)"
elif [ "$STARTED_BY_US" = "1" ]; then
	DISCOVERY="should appear in the list automatically"
else
	DISCOVERY="may or may not appear in the list (server was already running)"
fi

# pairing_wait: wait, bounded, until enough headsets are paired.
# Gate on the pairing STATE, not on a keypress: with no tty (double-click
# launch) a `read` would hit EOF and fall straight through, launching the
# emulator unpaired — the exact failure this gate exists to stop.
# On a tty, "s" then Enter skips the wait (advanced users). Only on purpose:
# whatever was typed BEFORE the wait began (a second Enter at the players
# question) is thrown away first, bounded and without waiting for more, and
# a bare Enter is not a skip. Otherwise one stray Enter launched an unpaired
# headset (a flat game).
pairing_wait() {
	local k
	if [ -t 0 ]; then
		echo ">>    (or type s and press Enter to skip waiting — advanced)"
		for _ in $(seq 1 64); do
			read -r -t 0 2>/dev/null || break
			read -r -t 0.05 _ 2>/dev/null || break
		done
	fi
	# Bounded (10 min): with no tty there is no way out of an unbounded loop, so
	# a pairing that never completes would hang the launcher forever with no
	# output at all.
	WAITED=0
	while ! paired_enough && [ "$WAITED" -lt 600 ]; do
		if [ -t 0 ]; then
			if read -r -t 2 SKIP 2>/dev/null; then
				case "$SKIP" in
					s|S)
						echo ">> Skipping the pairing wait."
						break ;;
					*) echo ">>    (to skip the wait, type s and press Enter)" ;;
				esac
			fi
		else
			sleep 2
		fi
		WAITED=$((WAITED + 2))
		# Heartbeat (a silent wait reads as a hang
		# and got skipped). Every 20 s: prove we're alive, name the exact file
		# we poll, and report its state — a stuck pairing then diagnoses
		# itself from the console instead of becoming a field mystery.
		if [ $((WAITED % 20)) -eq 0 ] && [ "$WIVRN_SEATS" -gt 1 ]; then
			echo ">>    still waiting (${WAITED}s of 600) — $(paired_count) of $WIVRN_SEATS headsets paired so far."
		elif [ $((WAITED % 20)) -eq 0 ]; then
			echo ">>    still waiting (${WAITED}s of 600) — polling known_keys.json every 2 s:"
			for k in "${KNOWN_KEYS_CANDIDATES[@]}"; do
				if [ -s "$k" ]; then
					echo ">>      $k — present, no \"key\" entry yet (PIN not accepted yet)"
				elif [ -e "$k" ]; then
					echo ">>      $k — created but still empty"
				else
					echo ">>      $k — not created yet (enter the PIN on the headset)"
				fi
			done
		fi
	done
	if paired_enough && [ "$WIVRN_SEATS" -gt 1 ]; then
		echo ">> $(paired_count) headsets paired."
	elif paired_enough; then
		echo ">> Headset paired."
	elif [ "$WAITED" -ge 600 ]; then
		echo ">> Still not paired after 10 minutes — continuing anyway."
		echo ">> (The game will run flat until the headset pairs and connects.)"
	fi
}
PAIRED=0
paired_enough && PAIRED=1

# Player 1 on WiVRn: how that headset connects, and the pairing of every
# player's headset on player 1's server, before the other players' servers
# start (a headset paired once is paired for every seat). Player 1 on a Monado
# headset: there is no server of player 1's; the other players pair on their
# own seat's server once it is up (§3c).
if [ "$RUNTIME" = "wivrn" ]; then
echo
echo "================== HEADSET CONNECTION =================="
if [ "$USB_ACTIVE" = "1" ]; then
	echo ">> USB-C TETHER (recommended — no Wi-Fi involved):"
	if [ "$USB_OPENED" = "1" ]; then
		echo ">>    The WiVRn app on the headset was opened and is connecting over the cable:"
		echo ">>    put the headset on. If it shows its server list instead, connect by hand:"
	fi
	echo ">>    In the headset's WiVRn app: Add server / Connect by IP ->  127.0.0.1"
	echo ">>    port 9757, and turn ON 'TCP only' (the cable carries TCP only; without it the"
	echo ">>    connection times out). One-time: it stays in the list as 'localhost'."
	echo ">>    The cable carries the stream. Do not unplug mid-session."
	echo ">>"
	echo ">> Wi-Fi fallback if you unplug:"
else
	echo ">> Wi-Fi connection:"
fi
echo ">> This PC's address: ${LAN_IP:-<could not detect — check your network>}"
echo ">> In the headset's WiVRn app, this PC $DISCOVERY."
echo ">>    Not listed? Choose 'Add server' / 'Connect by IP' and type: ${LAN_IP:-<PC IP>}"
echo ">>    Connected before but failing now? The PC's address may have CHANGED —"
echo ">>    compare with the address above and re-add the server if it differs."
if [ "$PAIRED" = "0" ]; then
	# The headless server prints its pairing PIN to the log — surface it here,
	# since there is no dashboard window to read it from. Only trust that log if
	# WE started this server: otherwise it is some earlier run's file and the PIN
	# in it belongs to a server that is no longer running (a confidently WRONG
	# number is worse than none).
	PIN=""
	[ "$STARTED_BY_US" = "1" ] && \
		PIN=$(grep -a "PIN code" "$HOME/wivrn.log" 2>/dev/null | tail -1 | grep -oE '[0-9]{4,8}' | tail -1)
	# No PIN from the log (a server this run did not start)? Ask the server for one (`wivrnctl pair`, 10 min),
	# the same way the cable path does, so a first connect never sends the player to type a command.
	[ -z "$PIN" ] && [ "$SEATS" -le 1 ] && PIN=$(usb_pairing_pin || true)
	echo ">>"
	if [ "$SEATS" -gt 1 ]; then
		# WiVRn offers a PIN by itself only while it knows NO headset, so after
		# the first one every further headset needs a PIN asked for explicitly.
		# Pair them all here, on player 1's server, before the other players'
		# servers start: a headset paired once is paired for every seat.
		KNOWN=$(paired_count)
		echo ">> PAIR EVERY PLAYER'S HEADSET (once per headset, never again):"
		echo ">>    This PC knows $KNOWN headset(s); this session has $SEATS players."
		echo ">>    Pair them one at a time, all at the address above (port 9757)."
		echo ">>    For each player's headset that has never connected to this PC:"
		echo ">>      0. Install the free WiVRn app on it first if you haven't."
		echo ">>      1. In its WiVRn app, connect to this PC (by IP if not listed)."
		if [ -n "$PIN" ] && [ "$KNOWN" -eq 0 ]; then
			echo ">>      2. The first headset's PIN is:   $PIN"
			echo ">>         Each headset after it needs a new PIN, from:"
		else
			echo ">>      2. When it asks for a PIN, get one with:"
		fi
		echo ">>           flatpak run --command=wivrnctl io.github.wivrn.wivrn pair"
		echo ">>      3. It drops into a WiVRn waiting room: paired. Close that headset's"
		echo ">>         WiVRn app before pairing the next one (one headset at a time)."
	elif [ "$USB_ACTIVE" = "1" ]; then
		[ -n "$USB_PIN" ] && PIN="$USB_PIN"
		echo ">> FIRST CONNECT over the cable — pair the headset once (never again):"
		if [ "$USB_OPENED" = "1" ]; then
			echo ">>    1. Put the headset on: its WiVRn app is open and connecting to this PC."
		else
			echo ">>    1. Put the headset on and open its WiVRn app (install the free app first if you haven't):"
			echo ">>       Add server / Connect by IP -> 127.0.0.1, port 9757, 'TCP only' ON."
		fi
		if [ -n "$PIN" ]; then
			echo ">>    2. When it asks for a PIN, type:   $PIN"
		else
			echo ">>    2. When it asks for a PIN, get one with:"
			echo ">>         flatpak run --command=wivrnctl io.github.wivrn.wivrn pair"
		fi
		echo ">>    3. It drops into WiVRn's waiting room: paired and connected."
		echo ">>    Not connecting? Turn the headset's Wi-Fi OFF while on the cable (WiVRn's own advice)."
	else
		echo ">> FIRST RUN — pair the headset once (takes a minute, never again):"
		echo ">>    0. On the headset: install the free WiVRn app first if you haven't."
		echo ">>    1. In the headset's WiVRn app, connect to this PC (by IP if not listed)."
		if [ -n "$PIN" ]; then
			echo ">>    2. When it asks for a PIN, enter:   $PIN"
		else
			echo ">>    2. When it asks for a PIN, get one with:"
			echo ">>         flatpak run --command=wivrnctl io.github.wivrn.wivrn pair"
		fi
		echo ">>    3. The headset drops into a WiVRn waiting room — paired forever."
	fi
	echo ">>"
	echo ">> Waiting for pairing to complete (this continues automatically)..."
	pairing_wait
fi
# ALWAYS show this: WiVRn only offers pairing automatically while NO headset is
# known, so a replacement unit, a factory-reset one, or a restored backup can
# never pair from the steps above — and would otherwise just get "connection
# refused" with nothing on screen explaining why.
echo ">>"
echo ">> Adding ANOTHER headset, or one that was reset? Ask for a new PIN with:"
echo ">>    flatpak run --command=wivrnctl io.github.wivrn.wivrn pair"
echo ">> (WiVRn's own window — settings, codec, unpair — is: flatpak run io.github.wivrn.wivrn"
echo ">>  it attaches to the running server; it does not start a second one.)"
echo ">> Keep the headset ON and connected while a game starts — VR initializes"
echo ">> at game boot, and a dozing headset means a flat first boot."
echo "========================================================"
fi

# ---------------------------------------------------------------------------
# 3b. Every other player's server, and which headset connects where
# ---------------------------------------------------------------------------
# Started AFTER pairing, so each server reads a paired-headset list that
# already holds every player's headset.
SEAT_DIRS=""
if [ "$SEATS" -gt 1 ]; then
	echo
	echo "======================= PLAYERS ======================="
	for n in $(seq 2 "$SEATS"); do
		if seat_up "$n"; then
			echo ">> Player $n: server already running (port $(seat_port "$n"))."
		else
			echo ">> Player $n: starting a server on port $(seat_port "$n")..."
			seat_start "$n"; rc=$?
			if [ "$rc" -eq 2 ] || [ "$rc" -eq 3 ]; then
				# The guard (2) or the requirement check (3) has already printed
				# why; this run's log would be an older run's file, so it is not shown.
				[ "$rc" -eq 2 ] && echo "!! Player $n's server was not started: its port or seat is in use (see above)."
				stop_started
				echo "!! Nothing was launched."
				exit 1
			elif [ "$rc" -ne 0 ]; then
				echo "!! Player $n's server did not come up — last lines of $(seat_log "$n"):"
				tail -n 8 "$(seat_log "$n")" 2>/dev/null | sed 's/^/!!   /'
				stop_started
				echo "!! Nothing was launched. Re-run to try again (leftovers are cleaned up first)."
				exit 1
			fi
		fi
		SEAT_DIRS="$SEAT_DIRS${SEAT_DIRS:+:}$(seat_rt "$n")"
	done
	if [ "$RUNTIME" != "wivrn" ]; then
		echo ">> Player 1 wears the $(runtime_label "$RUNTIME"): plug it in, put it on before the game starts."
		echo ">> Every other player connects their own headset to this PC, each on their own"
	else
		echo ">> Each player connects their own headset to this PC, each on their own"
	fi
	echo ">> port. In that headset's WiVRn app: 'Add server' / 'Connect by IP',"
	echo ">> type the address and set the port:"
	for n in $(seq "$FIRST_WIVRN" "$SEATS"); do
		echo ">>    Player $n:  ${LAN_IP:-<PC IP>}  port $(seat_port "$n")"
	done
	echo ">> The app keeps each entry, so next time every player just picks theirs."
	if [ "$USB_ACTIVE" = "1" ]; then
		echo ">> The USB-C cable carries player 1 only (one cable at a time); the others use Wi-Fi."
	elif [ "$RUNTIME" != "wivrn" ]; then
		if [ "$SEATS" -eq 2 ]; then
			echo ">> (A USB-C cable to a headset is for a WiVRn player 1 only; player 2 uses Wi-Fi.)"
		else
			echo ">> (A USB-C cable to a headset is for a WiVRn player 1 only; players 2-$SEATS use Wi-Fi.)"
		fi
	fi
	if [ "$MODE" = "cast" ]; then
		echo ">> Same game: choose 2-player split screen in the game. Player 1 sees the"
		echo ">> split screen as usual; player 2's headset shows player 2's half."
	else
		echo ">> Own game each: every player's game opens in its own emulator window."
	fi
	# -----------------------------------------------------------------------
	# 3c. Player 1 on a Monado headset: the other players' headsets pair HERE,
	#     each on its own player's server (there is no server of player 1's to
	#     pair on, and the dashboard cannot see a seat's bus). The PIN comes from
	#     that server over its bus (seat_pairing_pin); an already-paired headset
	#     is let straight through without one.
	# -----------------------------------------------------------------------
	if [ "$RUNTIME" != "wivrn" ] && [ "$PAIRED" = "0" ]; then
		KNOWN=$(paired_count)
		echo ">>"
		echo ">> PAIR EVERY WiVRn PLAYER'S HEADSET (once per headset, never again):"
		if [ "$SEATS" -eq 2 ]; then
			echo ">>    This PC knows $KNOWN headset(s); player 2 brings 1 WiVRn headset."
		else
			echo ">>    This PC knows $KNOWN headset(s); players 2-$SEATS bring $WIVRN_SEATS WiVRn headsets."
		fi
		echo ">>    Each pairs on its own player's server, one headset at a time. For each"
		echo ">>    headset that has never connected to this PC:"
		echo ">>      0. Install the free WiVRn app on it first if you haven't."
		echo ">>      1. In its WiVRn app, 'Add server' / 'Connect by IP': the address and"
		echo ">>         that player's port, from the list above."
		echo ">>      2. When it asks for a PIN, enter that player's:"
		for n in $(seq 2 "$SEATS"); do
			if pin=$(seat_pairing_pin "$n" 600); then
				echo ">>           Player $n (port $(seat_port "$n")):   $pin"
			else
				echo ">>           Player $n (port $(seat_port "$n")):   no PIN: pairing could not be opened"
				echo ">>             on this player's server. Pair that headset once in a one-player"
				echo ">>             WiVRn session (bash launch-vr-session.sh), then re-run. This"
				echo ">>             launch still waits below (10 minutes at most), then starts anyway."
			fi
		done
		echo ">>      3. It drops into a WiVRn waiting room: paired."
		echo ">>"
		echo ">> Waiting for pairing to complete (this continues automatically)..."
		pairing_wait
	fi
	echo "========================================================"
fi

if [ -n "$HEADSET_ONLY" ]; then
	# The point of --headset-only is a CONNECTED headset. "Steps done" is not that: wait (90 s) for a client on
	# player 1's port and say which it is. The TCP check is what the seat logic already trusts (stop_started).
	CONNECTED=0
	# PSCREEN2_CONNECT_WAIT: seconds to wait for the headset (default 90; the tests shorten it).
	CONNECT_WAIT=${PSCREEN2_CONNECT_WAIT:-90}
	if have_ss; then
		for _ in $(seq 1 $(( (CONNECT_WAIT + 1) / 2 ))); do
			if ss -tn state established "( sport = :9757 )" 2>/dev/null | tail -n +2 | grep -q .; then CONNECTED=1; break; fi
			sleep 2
		done
	fi
	if [ "$CONNECTED" = "1" ]; then
		# Which way is it connected? The peer address says: a link-local IPv6 with the USB interface's scope
		# (fe80::...%questusb0 / %enp...u...) or 127.0.0.1 (the adb tether) = the cable; anything else = Wi-Fi.
		# On a Deck, Wi-Fi is the path that dips and drops.
		TRANSPORT=$(headset_transport)
		case "$TRANSPORT" in
			cable*) echo ">> CONNECTED over the USB CABLE (peer ${TRANSPORT#cable }): the headset is streaming from this PC. No game was started." ;;
			*)
				echo ">> CONNECTED over Wi-Fi (peer ${TRANSPORT#wifi }), NOT the cable: on a Deck expect dips and drops."
				echo ">>   For the cable: plug it in, turn 'USB networking' on in the headset's WiVRn app, and run this again." ;;
		esac
		echo ">> Start a game whenever you like: bash launch-vr-session.sh (it finds this server running)."
		exit 0
	fi
	if have_ss; then
		echo "!! NOT CONNECTED after $CONNECT_WAIT s: WiVRn is running but no headset has connected to it."
		if [ "$USB_ACTIVE" = "1" ]; then
			echo "!!   On the headset: open the WiVRn app, pick 'localhost' (127.0.0.1, port 9757, TCP only),"
			echo "!!   enter the PIN above if asked, and turn the headset's Wi-Fi OFF while on the cable."
		else
			echo "!!   On the headset: open the WiVRn app and connect to ${LAN_IP:-<this PC IP>} (port 9757)."
		fi
		echo "!!   Then run this again; it will say CONNECTED."
		exit 1
	fi
	echo ">> --headset-only: WiVRn is running and the headset steps are done (no 'ss' here to confirm the"
	echo ">> connection). Start a game whenever you like: bash launch-vr-session.sh."
	exit 0
fi

# The headset's transport, said once at every launch (not only --headset-only): a player on a Deck whose cable
# has fallen off plays on Wi-Fi without knowing, and every stall and drop after that looks like a bug.
if [ "$RUNTIME" = "wivrn" ]; then
	TRANSPORT=$(headset_transport 2>/dev/null || true)
	case "$TRANSPORT" in
		cable*) echo ">> Headset: connected over the USB cable." ;;
		wifi*)
			if grep -qs '^ID=steamos' /etc/os-release; then
				echo "!! Headset: connected over Wi-Fi, NOT the cable (peer ${TRANSPORT#wifi }). On a Deck expect dips, drops and"
				echo "!!   stalls at boot. For the cable: plug it in and turn 'USB networking' on in the headset's WiVRn app."
			else
				echo ">> Headset: connected over Wi-Fi (peer ${TRANSPORT#wifi })."
			fi ;;
	esac
fi

# Every headset is on: now the controllers, LOUDLY. One that went away
# since the check before the servers started stops the launch.
if [ -z "$HEADSET_ONLY" ] && { [ "$SEATS" -gt 1 ] || [ -n "$PADS" ]; }; then
	if ! pads_check loud; then
		stop_started
		echo "!! Player 1's game was not started."
		exit 1
	fi
fi

# ---------------------------------------------------------------------------
# 4. Point OpenXR at each player's runtime and launch the emulator(s)
# ---------------------------------------------------------------------------
# Player 1 binds XR_RUNTIME_JSON: the WiVRn manifest (WIVRN_JSON, found before
# anything started; or one set before the launcher ran, respected as always),
# or the Monado manifest monado_prepare chose. Every other player binds the
# WiVRn manifest whatever player 1 binds.
# A runtime set before the launcher ran is player 1's. Players 2-N use the
# WiVRn install their servers run (the flatpak's), whatever was set: say so.
if [ "$RUNTIME" = "wivrn" ] && [ "$SEATS" -gt 1 ] && [ -n "${XR_RUNTIME_JSON:-}" ] \
	&& [ -n "$WIVRN_JSON" ] && [ "$XR_RUNTIME_JSON" != "$WIVRN_JSON" ]; then
	if [ "$SEATS" -eq 2 ]; then
		echo ">> XR_RUNTIME_JSON=$XR_RUNTIME_JSON is for player 1 only: player 2 uses the WiVRn runtime its server runs ($WIVRN_JSON)."
	else
		echo ">> XR_RUNTIME_JSON=$XR_RUNTIME_JSON is for player 1 only: players 2-$SEATS use the WiVRn runtime their servers run ($WIVRN_JSON)."
	fi
fi
if [ "$RUNTIME" = "wivrn" ] && [ -z "${XR_RUNTIME_JSON:-}" ]; then
	if [ -n "$WIVRN_JSON" ]; then
		export XR_RUNTIME_JSON="$WIVRN_JSON"
	else
		echo "!! WiVRn OpenXR runtime manifest not found — VR will NOT start this session"
		echo "!! (the emulator only enters VR when a runtime is explicitly selected, so it"
		echo "!! never grabs a stray system runtime by accident). Check that WiVRn is"
		echo "!! installed, then re-run. The game will otherwise run flat."
	fi
fi
# Player 1 on a Monado headset binds its manifest; exported only now, so the
# WiVRn servers started above never inherited it.
[ "$RUNTIME" != "wivrn" ] && export XR_RUNTIME_JSON="$MON_JSON"

echo ">> Launching..."
# Only pass a runtime override when we actually HAVE one: `--env=VAR=` sets it
# to the empty string, which is worse than leaving the system's active runtime
# alone (it would override a perfectly good runtime with nothing).
# Player 1's launch options and every other player's are two lists, because
# with a Monado headset on player 1 they differ: player 1 gets that runtime's
# manifest, and (in the sandbox) its prefix opened read-only for this launch
# (the usual prefixes are also built into the emulator's permissions); the
# other players get the WiVRn manifest. In the sandbox the options go before
# the app id; outside it player 1 inherits XR_RUNTIME_JSON as exported above
# and the other players are given the WiVRn manifest by name (PN_ENV).
P1_OPTS=()
PN_OPTS=()
PN_ENV=()
if [ "$RUNTIME" != "wivrn" ]; then
	if mon_in_flatpak "$MON_PREFIX"; then
		# the flatpak's own Monado: its manifest is a sandbox path (no hole), and
		# the game runs on the same Vulkan driver as the service (one per sandbox)
		mesa_opts "$MON_PREFIX"
		P1_OPTS+=(--env=XR_RUNTIME_JSON="$MON_JSON" "${MESA_OPTS[@]}")
	else
		P1_OPTS+=(--filesystem="$MON_PREFIX:ro" --env=XR_RUNTIME_JSON="$MON_JSON")
	fi
elif [ -n "${XR_RUNTIME_JSON:-}" ]; then
	P1_OPTS+=("--env=XR_RUNTIME_JSON=$XR_RUNTIME_JSON")
fi
if [ -n "$WIVRN_JSON" ]; then
	PN_OPTS+=("--env=XR_RUNTIME_JSON=$WIVRN_JSON")
	PN_ENV+=(env "XR_RUNTIME_JSON=$WIVRN_JSON")
fi
# One player: --vr, nothing else. Same game for two: this one emulator also
# shows player 2's half of the split screen on seat 2 (--vr-cast 2), finding
# seat 2's server through --vr-seat-dirs: for this launch only, never written
# into the user's settings.
EMU_ARGS=(--vr)
if [ "$MODE" = "cast" ]; then
	EMU_ARGS+=(--vr-cast 2 --vr-seat-dirs "$SEAT_DIRS")
	# Seat 2's runtime manifest by flag as well, and it is the WiVRn manifest,
	# always: the seat is chosen by its runtime dir, player 1 may bind another
	# runtime, and a [VR] XrSeatRuntimeJsons left in the user's ini (a stale
	# flatpak deploy path, say) must never decide it.
	[ -n "$WIVRN_JSON" ] && EMU_ARGS+=(--vr-seat-jsons "$WIVRN_JSON")
	# G15g: in cast mode the ONE emulator also drives the WiVRn seats, so on a
	# machine with more than one GPU it gets the same GPU env as the WiVRn servers
	# (WIVRN_ENV), when every stack in it is WiVRn. Not with a Monado headset on
	# player 1 (the process env is shared, and that headset is driven by the GPU its
	# display is on), and not outside the flatpak (WIVRN_ENV's driver file is the
	# sandbox's path). One GPU, or PSCREEN2_GPU=off: nothing to pin.
	if [ "$NGPU" -gt 1 ] && [ "${#WIVRN_ENV[@]}" -gt 0 ]; then
		if [ "$RUNTIME" != "wivrn" ]; then
			echo ">> GPU: the game is not pinned (player 1's $(runtime_label "$RUNTIME") headset shares its process); player 2's runtime picks its own GPU."
		elif [ "$EMU" != "flatpak" ]; then
			echo ">> GPU: the game is not pinned (PSCREEN2_EXE runs outside the flatpak, where the servers' driver path does not apply)."
		else
			P1_OPTS+=("${WIVRN_ENV[@]}")
			echo ">> GPU: the game is pinned to the WiVRn servers' GPU (cast: it drives player 2's seat too)."
		fi
	fi
fi
# launch_player1 <command...>: this script becomes player 1's emulator. The
# exception: a Monado service THIS launch started that is stopped when the
# game closes (mon_stop_at_end: the glasses, whose window is the side-by-side
# panel's size and would overhang the desktop once they fall back to 2D; the
# flatpak's own service, which holds the PS VR2's display): then the emulator
# runs in the foreground and the service is stopped after it.
launch_player1() {
	local rc
	if mon_stop_at_end; then
		# the session is launched: an interrupt now stops the glasses' service only
		trap 'monado_interrupted 130' INT
		trap 'monado_interrupted 129' HUP
		trap 'monado_interrupted 143' TERM
		"$@"
		rc=$?
		echo ">> The game closed (exit $rc); stopping the Monado service this launch started."
		monado_stop_started
		exit "$rc"
	fi
	exec "$@"
}

# One game per player: one emulator per player. A process can exec only once,
# so players N..2 start detached first (--vr-seat N binds that player's
# server), each checked alive, then this script becomes player 1's emulator.
if [ "$MODE" = "separate" ]; then
	# EMU, P1_ROOT and PLAYERS_BASE were settled with the games, before any
	# server started.
	# seed_player <n>: player n's data root, its settings copied from player
	# 1's. In [Folders], what a game writes while it runs (cards, cache, save
	# states, snapshots, logs, videos) is the player's own, even where player 1
	# set an absolute path; everything else (the BIOS first) points at player
	# 1's. Written to a fresh file and renamed over the old one.
	seed_player() {
		local root tmp
		root="$PLAYERS_BASE/player$1/PenguinScreen2"
		mkdir -p "$root/inis" || return 1
		tmp=$(mktemp "$root/inis/.PenguinScreen2.ini.XXXXXX") || return 1
		if awk -v p1="$P1_ROOT" -v sdl="${PAD_OF[$1]:-}" '
			BEGIN {
				n = split("Bios:bios Cheats:cheats Patches:patches Covers:covers GameSettings:gamesettings UserResources:resources Textures:textures InputProfiles:inputprofiles VRProfiles:vrprofiles", a, " ")
				for (i = 1; i <= n; i++) { split(a[i], kv, ":"); share[kv[1]] = kv[2]; order[++no] = kv[1] }
				n = split("MemoryCards:memcards Cache:cache Savestates:sstates Snapshots:snaps Logs:logs Videos:videos", a, " ")
				for (i = 1; i <= n; i++) { split(a[i], kv, ":"); own[kv[1]] = kv[2]; order[++no] = kv[1] }
			}
			function rest(   i, k) {
				for (i = 1; i <= no; i++) {
					k = order[i]
					if (k in seen) continue
					if (k in own) print k " = " own[k]; else print k " = " p1 "/" share[k]
				}
			}
			/^[[:space:]]*\[/ {
				if (inf) rest()
				inf = ($0 ~ /^[[:space:]]*\[Folders\][[:space:]]*$/)
				inp = ($0 ~ /^[[:space:]]*\[Pad1\][[:space:]]*$/)
				if (inf) hadf = 1
				print; next
			}
			inf && /=/ {
				k = $0; sub(/[[:space:]]*=.*/, "", k); sub(/^[[:space:]]+/, "", k)
				v = $0; sub(/^[^=]*=[[:space:]]*/, "", v); sub(/[[:space:]]+$/, "", v)
				if (k in own) { print k " = " own[k]; seen[k] = 1; next }
				if (k in share) {
					if (v == "") v = share[k]
					if (v !~ /^\//) v = p1 "/" v
					print k " = " v; seen[k] = 1; next
				}
			}
			inp && sdl != "" { gsub(/SDL-0\//, "SDL-" sdl "/") }   # port 1: the controller of this player
			{ print }
			END { if (inf) rest(); if (!hadf) { print ""; print "[Folders]"; rest() } }
		' "$P1_ROOT/inis/PenguinScreen2.ini" > "$tmp" && mv -f -- "$tmp" "$root/inis/PenguinScreen2.ini"; then
			return 0
		fi
		rm -f -- "$tmp"
		return 1
	}
	# game_args <path>: how the path reaches the emulator. The sandbox sees
	# ~/PS2-Games itself; any other file is forwarded through the portal.
	game_args() {
		GAME_PRE=()
		GAME_ARGS=(-- "$1")
		if [ "$EMU" = "flatpak" ]; then
			case "$1" in
				"$HOME/PS2-Games/"*) ;;
				*) GAME_PRE=(--file-forwarding); GAME_ARGS=(-- @@ "$1" @@) ;;
			esac
		fi
	}
	for n in $(seq "$SEATS" -1 2); do
		game_args "${GAMES[$((n - 1))]}"
		log="$HOME/penguinscreen2-seat$n.log"
		echo ">> Player $n: starting $(basename "${GAMES[$((n - 1))]}")..."
		if ! seed_player "$n"; then
			echo "!! Player $n's data root could not be prepared ($PLAYERS_BASE/player$n)."
			stop_started
			echo "!! Player 1's game was not started."
			exit 1
		fi
		if [ "$EMU" = "flatpak" ]; then
			setsid nohup flatpak run "${PN_OPTS[@]}" "${GAME_PRE[@]}" "$APP_ID" \
				-datapath "$PLAYERS_BASE/player$n" \
				--vr-seat "$n" --vr-seat-dirs "$SEAT_DIRS" "${GAME_ARGS[@]}" \
				> "$log" 2>&1 < /dev/null &
		else
			setsid nohup "${PN_ENV[@]}" "${EMU_CMD[@]}" -datapath "$PLAYERS_BASE/player$n" \
				--vr-seat "$n" --vr-seat-dirs "$SEAT_DIRS" "${GAME_ARGS[@]}" \
				> "$log" 2>&1 < /dev/null &
		fi
		pid=$!
		STARTED+=("$pid:emulator:$n")
		echo "$pid" >> "$LAUNCH_LOCK"   # the session's lock lasts while this game runs
		# An emulator that cannot start (a bad path, an older build that does
		# not know a flag) exits within seconds: watch it that long, bounded.
		for _ in 1 2 3 4 5 6; do
			kill -0 "$pid" 2>/dev/null || break
			sleep 0.5
		done
		if ! kill -0 "$pid" 2>/dev/null; then
			echo "!! Player $n's emulator stopped right away — last lines of $log:"
			tail -n 8 "$log" 2>/dev/null | sed 's/^/!!   /'
			# Players after this one are already running, and the servers this
			# run started are up: take all of it down again, named.
			stop_started
			echo "!! Player 1's game was not started."
			exit 1
		fi
		echo ">> Player $n: running (pid $pid; log $log)."
	done
	game_args "${GAMES[0]}"
	echo ">> Player 1: starting $(basename "${GAMES[0]}")..."
	if [ "$EMU" = "flatpak" ]; then
		launch_player1 flatpak run "${P1_OPTS[@]}" "${GAME_PRE[@]}" "$APP_ID" --vr "${GAME_ARGS[@]}"
	else
		launch_player1 "${EMU_CMD[@]}" --vr "${GAME_ARGS[@]}"
	fi
fi

# --vr is what actually arms VR. XR_RUNTIME_JSON above only tells the OpenXR
# loader WHICH runtime to bind; it never means "enable VR".
# Cast: the seats' servers are watched while the game runs (seat_watch, G15h).
if [ "$MODE" = "cast" ]; then seat_watch "$$" < /dev/null & fi
if [ "$EMU" = "flatpak" ]; then
	launch_player1 flatpak run "${P1_OPTS[@]}" "$APP_ID" "${EMU_ARGS[@]}"
else
	launch_player1 "${EMU_CMD[@]}" "${EMU_ARGS[@]}"
fi
