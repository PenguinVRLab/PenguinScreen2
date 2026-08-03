#!/usr/bin/env bash
# enable-vr-discovery.sh — turn ON headset auto-discovery for PenguinScreen2 VR.
#
# WHAT & WHY
#   The WiVRn server advertises itself on your LAN over mDNS (Avahi) so the
#   headset finds this PC by itself — no typing IP addresses into a headset.
#   SteamOS (and a few other distros) ship Avahi with user-service publishing
#   DISABLED, which both blocks discovery AND makes the WiVRn server fail to
#   start ("Server failed to start" / "Cannot create entry group ... Not
#   permitted"). This script flips that Avahi setting back on.
#
#   Run it with sudo. It is idempotent — safe to run as many times as you like.
#   If Avahi refuses the new config, your original is put back automatically.
#
# STEAMOS: OS *updates* reset system files, so after a SteamOS update you re-run
#   this once. (Plain reboots are fine — only version updates revert it.)
#   Note: a fresh Steam Deck has NO password on the `deck` account, so sudo
#   cannot work until you set one — run `passwd` once in Desktop Mode.
#
# DON'T want discovery? You don't need this at all. The VR launcher already
#   falls back to connect-by-IP and prints the address to type into the headset.
#
# USAGE:  sudo ./enable-vr-discovery.sh
set -euo pipefail

CONF=/etc/avahi/avahi-daemon.conf
KEY=disable-user-service-publishing   # the per-user publishing switch (our target)
MASTER=disable-publishing             # the [publish] master switch — overrides KEY

say() { printf '>> %s\n' "$*"; }
warn() { printf '!! %s\n' "$*" >&2; }
die() { printf '!! %s\n' "$*" >&2; exit 1; }

# --- become root if we aren't (NOT `exec`, so we can explain a sudo failure) --
if [ "$(id -u)" -ne 0 ]; then
	command -v sudo >/dev/null 2>&1 || die "this needs admin rights and 'sudo' isn't available — run it as root"
	say "Editing the system Avahi config needs admin rights — re-running with sudo."
	sudo -- bash "$0" "$@" && exit 0
	die "sudo did not succeed. On a fresh Steam Deck the 'deck' account has NO
!! password, so sudo can't authenticate: open Desktop Mode, run  passwd  once to
!! set one, then re-run this script. (Don't want to? You don't need discovery —
!! the VR launcher connects by IP without any of this.)"
fi

[ -f "$CONF" ] || die "$CONF not found — is Avahi installed? (headset discovery needs it)"

# --- SteamOS: unlock the read-only root, but ONLY if it is actually locked ----
# Re-locking unconditionally would clobber the state of someone who deliberately
# unlocked their system (e.g. a pacman workflow in progress).
RELOCK=0
if command -v steamos-readonly >/dev/null 2>&1; then
	if [ "$(steamos-readonly status 2>/dev/null || true)" = "enabled" ]; then
		say "SteamOS: unlocking the read-only filesystem for one edit..."
		steamos-readonly disable || die "could not unlock the filesystem (steamos-readonly disable failed)"
		RELOCK=1
	fi
fi
relock() {
	[ "$RELOCK" -eq 1 ] || return 0
	steamos-readonly enable >/dev/null 2>&1 \
		|| warn "WARNING: could not re-lock the SteamOS filesystem. Run:  sudo steamos-readonly enable"
}
trap relock EXIT

# --- back the original up once (first run only) ------------------------------
BAK="$CONF.penguinvr.bak"
[ -f "$BAK" ] || cp -a "$CONF" "$BAK"
# A snapshot of THIS run, so we can roll back even on a later re-run.
PREV=$(mktemp); cp -a "$CONF" "$PREV"
cleanup_tmp() { rm -f "$PREV" "${NEW:-}" 2>/dev/null || true; }
trap 'cleanup_tmp; relock' EXIT

# --- section-aware rewrite ---------------------------------------------------
# Avahi's parser is strict: a key in the WRONG group ("Invalid configuration
# key") or an assignment before any group ("Assignment outside group") is a
# FATAL startup error — the daemon then refuses to start at all. So we must edit
# INSIDE [publish] only, and neutralize any active copy of the key that sits
# somewhere else. (A blind global sed is what makes avahi die.)
rewrite() {   # rewrite <key> <insert-if-missing:0|1> < in > out
	awk -v key="$1" -v insert="$2" '
	BEGIN { sec=""; done=0 }
	{
		line = $0
		if (line ~ /^[[:space:]]*\[/) {                       # a group header
			if (sec == "publish" && done == 0 && insert == 1) { print key "=no"; done = 1 }
			sec = (line ~ /^[[:space:]]*\[publish\][[:space:]]*\r?$/) ? "publish" : "other"
			print line; next
		}
		if (line ~ ("^[[:space:]]*#?[[:space:]]*" key "[[:space:]]*=")) {
			if (sec == "publish") {
				if (done == 0) { print key "=no"; done = 1 }   # first one wins
				next                                           # drop duplicates
			}
			# outside [publish]: an ACTIVE assignment here is fatal to avahi —
			# comment it out. A comment there is harmless; leave it alone.
			if (line ~ ("^[[:space:]]*" key "[[:space:]]*=")) {
				print "#" line "   # disabled by enable-vr-discovery.sh (wrong section)"
				next
			}
			print line; next
		}
		print line
	}
	END {
		if (insert == 1 && done == 0) {
			if (sec == "publish") { print key "=no" }
			else { print ""; print "[publish]"; print key "=no" }
		}
	}'
}

NEW=$(mktemp)
rewrite "$KEY" 1 < "$CONF" > "$NEW"

# The [publish] MASTER switch overrides our key: with disable-publishing=yes,
# nothing is ever advertised and WiVRn still fails — flipping only KEY would
# report success while discovery stayed broken.
MASTER_FLIPPED=0
if grep -qE "^[[:space:]]*${MASTER}[[:space:]]*=[[:space:]]*yes" "$NEW"; then
	rewrite "$MASTER" 0 < "$NEW" > "$NEW.2" && mv "$NEW.2" "$NEW"
	MASTER_FLIPPED=1
fi

# Write THROUGH the path (preserves a symlink, the inode, owner and mode).
cat "$NEW" > "$CONF"

# --- verify the edit landed in the right place -------------------------------
in_publish() { sed -n '/^[[:space:]]*\[publish\]/,/^[[:space:]]*\[[^]]*\]/p' "$CONF"; }
restore() { cat "$PREV" > "$CONF"; }

if ! in_publish | grep -qE "^[[:space:]]*${KEY}[[:space:]]*=[[:space:]]*no[[:space:]]*\r?$"; then
	restore
	die "could not place ${KEY} inside [publish] — your config is unchanged (backup: $BAK)"
fi
if grep -vE "^[[:space:]]*#" "$CONF" | grep -qE "^[[:space:]]*${MASTER}[[:space:]]*=[[:space:]]*yes"; then
	restore
	die "'${MASTER}=yes' is set — that master switch blocks ALL advertising and we
!! could not safely change it. Your config is unchanged (backup: $BAK)."
fi
say "Avahi: user-service publishing ENABLED (${KEY}=no)."
[ "$MASTER_FLIPPED" = "1" ] && say "Also cleared the '${MASTER}=yes' master switch (it blocks all advertising)."

# --- restart Avahi, then PROVE it actually came back -------------------------
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
	if systemctl is-enabled avahi-daemon.service 2>/dev/null | grep -q masked; then
		say "Note: avahi-daemon was masked (disabled system-wide) — unmasking it so discovery can work."
		systemctl unmask avahi-daemon.service avahi-daemon.socket >/dev/null 2>&1 || true
	fi
	systemctl enable avahi-daemon.service >/dev/null 2>&1 || true
	systemctl restart avahi-daemon.service >/dev/null 2>&1 \
		|| systemctl start avahi-daemon.service >/dev/null 2>&1 || true
	# A config Avahi rejects is fatal to it. If it did not come back, put the
	# old config back and say so — never leave the machine worse than we found it.
	sleep 1
	if ! systemctl is-active --quiet avahi-daemon.service; then
		restore
		systemctl restart avahi-daemon.service >/dev/null 2>&1 || true
		die "Avahi refused to start with the new setting, so your ORIGINAL config was
!! restored (backup also at $BAK). Nothing is broken — VR still works by IP.
!! Details:  journalctl -u avahi-daemon -n 20"
	fi
	say "Avahi restarted and running."
else
	say "No systemd here — restart Avahi yourself for the change to take effect."
fi

# --- open LAN ports IF (and only if) a firewall is active — usually a no-op ---
#   5353/udp = mDNS (discovery), 9757/tcp+udp = WiVRn stream.
if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
	firewall-cmd --permanent --add-port=5353/udp >/dev/null 2>&1 || true
	firewall-cmd --permanent --add-port=9757/tcp >/dev/null 2>&1 || true
	firewall-cmd --permanent --add-port=9757/udp >/dev/null 2>&1 || true
	firewall-cmd --reload >/dev/null 2>&1 || true
	say "firewalld: opened 5353/udp + 9757/tcp+udp."
elif command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -qi '^Status: active'; then
	ufw allow 5353/udp >/dev/null 2>&1 || true
	ufw allow 9757/tcp >/dev/null 2>&1 || true
	ufw allow 9757/udp >/dev/null 2>&1 || true
	say "ufw: opened 5353/udp + 9757/tcp+udp."
fi

echo
say "Done — headset auto-discovery is ON."
say "Relaunch your VR session; the headset's WiVRn app will now list this PC by itself."
if command -v steamos-readonly >/dev/null 2>&1; then
	say "Reminder: after a future SteamOS *update*, run this once more (updates reset system files)."
fi
