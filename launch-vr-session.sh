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
# 2. Start (or reuse) the WiVRn server
# ---------------------------------------------------------------------------
SOCK="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/wivrn/comp_ipc"
server_up() { pgrep -f "wivrn-serve[r]" >/dev/null && [ -S "$SOCK" ]; }

STARTED_BY_US=0
if server_up; then
	# A GPU override can only apply when THIS script starts the server — an
	# explicit PSCREEN2_GPU that silently no-ops is worse than an error
	# (strict-review #7).
	if [ "$GPU_MODE" != "auto" ] && [ "$GPU_MODE" != "off" ]; then
		echo "!! PSCREEN2_GPU=$GPU_MODE cannot apply: a WiVRn server is ALREADY running"
		echo "!! and GPU selection happens at server start. To apply the override:"
		echo "!!   1. Disconnect the headset (take it off / close its WiVRn app)."
		echo "!!   2. Stop the server:  flatpak kill io.github.wivrn.wivrn"
		echo "!!      (never stop it while the headset is connected — known crash)"
		echo "!!   3. Re-run:  PSCREEN2_GPU=$GPU_MODE bash $0"
		exit 1
	fi
	echo ">> WiVRn server already running (GPU forcing not re-applied)."
else
	echo ">> Starting WiVRn..."
	setsid nohup flatpak run "${WIVRN_ENV[@]}" io.github.wivrn.wivrn \
		> "$HOME/wivrn.log" 2>&1 < /dev/null &
	STARTED_BY_US=1
	for _ in $(seq 1 40); do server_up && break; sleep 0.5; done
	if ! server_up; then
		echo "!! WiVRn did not come up — see ~/wivrn.log"
		exit 1
	fi
	# Loud verification: say which GPU it actually initialized.
	sleep 2
	echo "--- WiVRn GPU (from its log) ---"
	grep -iE "physical device|GPU|NVIDIA|Radeon|Intel|nvenc|vaapi" "$HOME/wivrn.log" | head -6 || true
fi

# ---------------------------------------------------------------------------
# 3. Tell the user EXACTLY how to connect the headset (SteamOS field reality)
# ---------------------------------------------------------------------------
# LAN IP: `hostname` does not exist on SteamOS — read the routing table, then
# fall back to the interface table (works with no default route), then to
# hostname -I where it exists (strict-review G8).
LAN_IP=$(ip route get 1.1.1.1 2>/dev/null | grep -oE 'src [0-9.]+' | awk '{print $2}')
[ -z "$LAN_IP" ] && LAN_IP=$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)
[ -z "$LAN_IP" ] && LAN_IP=$(hostname -I 2>/dev/null | awk '{print $1}')

# Auto-discovery: SteamOS ships avahi with user service publishing disabled,
# so the headset can NEVER find this PC by itself there. Detect the OS
# directly (primary signal — the server log only exists when WE started the
# server, strict-review #11); the log line is confirmation when present.
IS_STEAMOS=0
grep -qs '^ID=steamos' /etc/os-release && IS_STEAMOS=1
if [ "$IS_STEAMOS" = "1" ] || grep -qsi "Cannot create entry group" "$HOME/wivrn.log"; then
	DISCOVERY="will NOT appear automatically (this OS blocks mDNS publishing)"
elif [ "$STARTED_BY_US" = "1" ]; then
	DISCOVERY="should appear in the list automatically"
else
	DISCOVERY="may or may not appear in the list (server was already running)"
fi

# Pairing: a fresh WiVRn accepts no headset until it has been paired once via
# its window (PIN). Paired headsets live in known_keys.json — check both the
# flatpak data dir and the native config dir (strict-review #45).
paired_now() {
	local k
	for k in "$HOME/.var/app/io.github.wivrn.wivrn/config/wivrn/known_keys.json" \
		"${XDG_CONFIG_HOME:-$HOME/.config}/wivrn/known_keys.json"; do
		[ -s "$k" ] && grep -q '"key"' "$k" 2>/dev/null && return 0
	done
	return 1
}
PAIRED=0
paired_now && PAIRED=1

echo
echo "================== HEADSET CONNECTION =================="
echo ">> This PC's address: ${LAN_IP:-<could not detect — check your network>}"
echo ">> In the headset's WiVRn app, this PC $DISCOVERY."
echo ">>    Not listed? Choose 'Add server' / 'Connect by IP' and type: ${LAN_IP:-<PC IP>}"
echo ">>    Connected before but failing now? The PC's address may have CHANGED —"
echo ">>    compare with the address above and re-add the server if it differs."
if [ "$PAIRED" = "0" ]; then
	echo ">>"
	echo ">> FIRST RUN — pair the headset once (takes a minute, never again):"
	echo ">>    0. On the headset: install the WiVRn app first if you haven't (free)."
	echo ">>    1. In the WiVRn window on THIS PC, follow its first-run wizard, or"
	echo ">>       click 'Pair a new headset' (Headsets page) — a PIN appears."
	echo ">>    2. In the headset's WiVRn app, connect to this PC (by IP if not listed)."
	echo ">>    3. Enter the PIN when asked. The headset drops into a waiting room — done."
	echo ">>"
	echo ">> Waiting for pairing to complete (this continues automatically)..."
	# Gate on the pairing STATE, not on a keypress: with no tty (double-click
	# launch) a `read` would hit EOF and fall straight through, launching the
	# emulator unpaired — the exact failure this gate exists to stop
	# (strict-review #3). On a tty, Enter skips the wait (advanced users).
	[ -t 0 ] && echo ">>    (or press Enter to skip waiting — advanced)"
	while ! paired_now; do
		if [ -t 0 ]; then
			if read -r -t 2 _ 2>/dev/null; then
				echo ">> Skipping the pairing wait."
				break
			fi
		else
			sleep 2
		fi
	done
	paired_now && echo ">> Headset paired."
fi
echo ">> Keep the headset ON and connected while a game starts — VR initializes"
echo ">> at game boot, and a dozing headset means a flat first boot."
echo "========================================================"

# ---------------------------------------------------------------------------
# 4. Point OpenXR at WiVRn and launch the emulator
# ---------------------------------------------------------------------------
# Canonical deploy paths first — an unscoped find over whole flatpak trees
# walks the OSTree object store (multi-second stall, and it can pin a stale
# scope; strict-review #12). Respect a pre-set XR_RUNTIME_JSON.
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
		echo ">> WARNING: WiVRn OpenXR manifest not found; using the system's active runtime."
	fi
fi

echo ">> Launching..."
if flatpak info "$APP_ID" >/dev/null 2>&1; then
	exec flatpak run --env=XR_RUNTIME_JSON="${XR_RUNTIME_JSON:-}" "$APP_ID"
elif command -v pcsx2-qt >/dev/null 2>&1; then
	exec pcsx2-qt
else
	echo "!! Emulator not found (flatpak $APP_ID or pcsx2-qt on PATH)."
	exit 1
fi
