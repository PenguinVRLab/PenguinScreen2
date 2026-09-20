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

set -u

APP_ID="org.penguinvr.penguinscreen2"
GPU_MODE="${PSCREEN2_GPU:-auto}"

# ---------------------------------------------------------------------------
# 1. Decide the GPU forcing (hybrid machines only)
# ---------------------------------------------------------------------------
WIVRN_ENV=()
describe_gpus() { lspci -nn 2>/dev/null | grep -Ei 'VGA compatible controller|3D controller'; }

GPUS="$(describe_gpus)"
NGPU=$(printf '%s\n' "$GPUS" | grep -c . || true)

if [ "$GPU_MODE" = "off" ]; then
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
		# Rig-proven: make the NVIDIA driver the only Vulkan device WiVRn can
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
SOCK="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/wivrn/comp_ipc"
# Match the server by EXACT process name, never by command line: our own
# `flatpak run --command=wivrn-server ...` invocation contains that string in its
# argv, so a `pgrep -f` here would match THIS SCRIPT and report a live server
# when none exists. WiVRn also binds the socket BEFORE it can fail on avahi, so
# a crashed server leaves the socket behind — the two checks together (exact
# process AND socket) are what make this honest.
server_proc() { pgrep -x wivrn-server >/dev/null; }
server_up() { server_proc && [ -S "$SOCK" ]; }

# Start the server once; sets STARTED_BY_US on success. $1 = extra args.
start_server() {
	# Clear a stale socket left by a crashed server — but ONLY when no server
	# process is alive, so we can never unlink a LIVE server's socket.
	server_proc || rm -f "$SOCK"
	setsid nohup flatpak run "${WIVRN_ENV[@]}" --command=wivrn-server \
		io.github.wivrn.wivrn "$@" \
		> "$HOME/wivrn.log" 2>&1 < /dev/null &
	for _ in $(seq 1 40); do server_up && return 0; sleep 0.5; done
	return 1
}

STARTED_BY_US=0
if server_up; then
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
else
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

# ---------------------------------------------------------------------------
# 2b. USB-C tether (the Steam Deck path)
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
find_adb() {
	command -v adb 2>/dev/null && return 0
	local a="$HOME/.local/share/penguinscreen2/platform-tools/adb"
	[ -x "$a" ] && { echo "$a"; return 0; }
	return 1
}
USB_ACTIVE=0
if [ "${PSCREEN2_USB:-auto}" != "off" ] && ADB=$(find_adb); then
	# `adb get-state` is quiet and definitive: "device" = authorized and ready.
	USB_STATE=$("$ADB" get-state 2>/dev/null || true)
	if [ "$USB_STATE" = "device" ]; then
		if "$ADB" reverse tcp:9757 tcp:9757 >/dev/null 2>&1; then
			USB_ACTIVE=1
			echo ">> USB tether ACTIVE: headset localhost:9757 now reaches this PC over the cable."
		else
			echo ">> USB headset detected but 'adb reverse' failed — falling back to Wi-Fi."
		fi
	elif [ "$USB_STATE" = "unauthorized" ]; then
		echo ">> USB headset detected but NOT authorized: put the headset on and accept"
		echo ">>   the 'Allow USB debugging?' prompt (tick 'always'), then re-run."
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
PAIRED=0
paired_now && PAIRED=1

echo
echo "================== HEADSET CONNECTION =================="
if [ "$USB_ACTIVE" = "1" ]; then
	echo ">> USB-C TETHER (recommended — no Wi-Fi involved):"
	echo ">>    In the headset's WiVRn app: Add server / Connect by IP ->  127.0.0.1"
	echo ">>    port 9757. (One-time; it stays in the list as 'localhost'.)"
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
	echo ">>"
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
	echo ">>"
	echo ">> Waiting for pairing to complete (this continues automatically)..."
	# Gate on the pairing STATE, not on a keypress: with no tty (double-click
	# launch) a `read` would hit EOF and fall straight through, launching the
	# emulator unpaired — the exact failure this gate exists to stop.
	# On a tty, Enter skips the wait (advanced users).
	[ -t 0 ] && echo ">>    (or press Enter to skip waiting — advanced)"
	# Bounded (10 min): with no tty there is no way out of an unbounded loop, so
	# a pairing that never completes would hang the launcher forever with no
	# output at all.
	WAITED=0
	while ! paired_now && [ "$WAITED" -lt 600 ]; do
		if [ -t 0 ]; then
			if read -r -t 2 _ 2>/dev/null; then
				echo ">> Skipping the pairing wait."
				break
			fi
		else
			sleep 2
		fi
		WAITED=$((WAITED + 2))
		# Heartbeat (a silent wait reads as a hang
		# and got skipped). Every 20 s: prove we're alive, name the exact file
		# we poll, and report its state — a stuck pairing then diagnoses
		# itself from the console instead of becoming a field mystery.
		if [ $((WAITED % 20)) -eq 0 ]; then
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
	if paired_now; then
		echo ">> Headset paired."
	elif [ "$WAITED" -ge 600 ]; then
		echo ">> Still not paired after 10 minutes — continuing anyway."
		echo ">> (The game will run flat until the headset pairs and connects.)"
	fi
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

# ---------------------------------------------------------------------------
# 4. Point OpenXR at WiVRn and launch the emulator
# ---------------------------------------------------------------------------
# Canonical deploy paths first — an unscoped find over whole flatpak trees
# walks the OSTree object store (multi-second stall, and it can pin a stale
# scope). Respect a pre-set XR_RUNTIME_JSON.
if [ -z "${XR_RUNTIME_JSON:-}" ]; then
	WIVRN_JSON=""
	for d in "$HOME/.local/share/flatpak" /var/lib/flatpak; do
		j="$d/app/io.github.wivrn.wivrn/current/active/files/share/openxr/1/openxr_wivrn.json"
		if [ -f "$j" ]; then WIVRN_JSON="$j"; break; fi
	done
	if [ -z "$WIVRN_JSON" ]; then
		WIVRN_JSON=$(find "$HOME/.local/share/flatpak/app/io.github.wivrn.wivrn" \
			/var/lib/flatpak/app/io.github.wivrn.wivrn \
			-name openxr_wivrn.json 2>/dev/null | head -1)
	fi
	if [ -n "$WIVRN_JSON" ]; then
		export XR_RUNTIME_JSON="$WIVRN_JSON"
	else
		echo "!! WiVRn OpenXR runtime manifest not found — VR will NOT start this session"
		echo "!! (the emulator only enters VR when a runtime is explicitly selected, so it"
		echo "!! never grabs a stray system runtime by accident). Check that WiVRn is"
		echo "!! installed, then re-run. The game will otherwise run flat."
	fi
fi

echo ">> Launching..."
# Only pass the runtime override when we actually HAVE one: `--env=VAR=` sets it
# to the empty string, which is worse than leaving the system's active runtime
# alone (it would override a perfectly good runtime with nothing).
XR_ENV=()
[ -n "${XR_RUNTIME_JSON:-}" ] && XR_ENV+=("--env=XR_RUNTIME_JSON=$XR_RUNTIME_JSON")
if flatpak info "$APP_ID" >/dev/null 2>&1; then
	# --vr is what actually arms VR. XR_RUNTIME_JSON above only tells the OpenXR
	# loader WHICH runtime to bind; it never means "enable VR".
	exec flatpak run "${XR_ENV[@]}" "$APP_ID" --vr
elif command -v pcsx2-qt >/dev/null 2>&1; then
	exec pcsx2-qt --vr
else
	echo "!! Emulator not found (flatpak $APP_ID or pcsx2-qt on PATH)."
	exit 1
fi
