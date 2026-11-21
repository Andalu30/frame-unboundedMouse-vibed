#!/bin/sh
# SPDX-License-Identifier: MIT
# Summarises the driver's latest session from SteamVR's log: which build loaded, which mouse it
# uses, how the laser mode switches went, and known error lines. Run it on the headset after a
# reboot and a few on/off switches. Read-only. Exits 1 if something is clearly wrong.
#
#   tools/check-log.sh [path/to/vrserver.txt]
set -eu
LOG=${1:-"$HOME/.local/share/Steam/logs/vrserver.txt"}
[ -r "$LOG" ] || { echo "Can't read $LOG"; exit 2; }

# Our lines since the driver last loaded (its last "version" line), with SteamVR's prefix removed.
lines=$(grep -a 'mouselaser: ' "$LOG" | sed 's/.*mouselaser: //' |
    awk '/^version /{buf=""} {buf = buf $0 "\n"} END {printf "%s", buf}')
count() { printf '%s\n' "$lines" | grep -c -- "$1" || true; }
fail=0
report() { printf '%-6s%s\n' "$1" "$2"; [ "$1" != FAIL ] || fail=1; }

version=$(printf '%s\n' "$lines" | sed -n 's/^version //p')
if [ -z "$version" ]; then
    report FAIL "no 'version' line: the driver didn't load (registered? see the README)"
    exit 1
fi
report ok "build $version"

if [ "$(count '^disabled by setting')" -gt 0 ]; then
    report info "disabled by the 'enable' setting: no device was added"
    exit 0
fi
[ "$(count '^TrackedDeviceAdded failed')" -eq 0 ] || report FAIL "SteamVR refused the device (TrackedDeviceAdded failed)"
activated=$(printf '%s\n' "$lines" | grep '^activated as device' | tail -n 1)
if [ -n "$activated" ]; then report ok "$activated"; else report FAIL "device never activated"; fi

# Settings that couldn't be read or were out of range (the driver corrected them).
printf '%s\n' "$lines" | sed -n 's/^setting //p' | while IFS= read -r problem; do report warn "setting $problem"; done

mouse=$(printf '%s\n' "$lines" | sed -n 's/^using //p' | tail -n 1)
if [ -n "$mouse" ]; then report ok "mouse: $mouse"; else report warn "no mouse found yet"; fi
[ "$(count "^WARNING: it has no button")" -eq 0 ] ||
    report FAIL "the mouse has no toggle button, so laser mode can't be switched on (see toggleButton)"
printf '%s\n' "$lines" | sed -n 's/^skipped //p' | sort -u | while IFS= read -r skipped; do report info "skipped $skipped"; done

# Laser mode switches. Every switch-on that reaches the grip pulse needs a matching restore
# pulse on the way out, or SteamVR's Laser Mouse/Gamepad mode ends up flipped.
started=$(count '^phase settle')
cancelled=$(printf '%s\n' "$lines" | grep '^phase ' | awk '/^phase settle/{s=1; next} /^phase off/ && s {n++} {s=0} END {print n+0}')
on=$(count '^phase on')
grips=$(count '^phase grip pulse')
restores=$(count '^phase restore pulse')
last=$(printf '%s\n' "$lines" | sed -n 's/^phase //p' | tail -n 1)
report info "switched on $started time(s): $on completed the on sequence, $cancelled cancelled before the grip pulse"
case "$last" in
off | "")
    if [ "$grips" -eq "$restores" ]; then report ok "grip pulses balanced ($grips on, $restores off)"
    else report FAIL "grip pulses unbalanced ($grips on, $restores off): the next switch-on may need a retry"; fi ;;
*) report info "laser mode is currently in phase '$last'" ;;
esac

grab=$(count 'EVIOCGRAB failed')
[ "$grab" -eq 0 ] || report FAIL "couldn't grab the mouse $grab time(s) (another program holding it?)"
gone=$(count '^mouse gone')
back=$(count '^mouse back')
expired=$(count '^mouse still gone')
[ "$gone" -eq 0 ] || report info "mouse dropped out during laser mode $gone time(s): back in time $back, gave up $expired"
changes=$(count '^settings changed (')
if [ "$changes" -gt 0 ]; then
    report info "settings changed $changes time(s) while running: noticed by the settings event $(count '^settings changed (settings event)'), by the 1 s check $(count '^settings changed (1 s check)')"
fi
[ "$(count '^settings changed that apply after')" -eq 0 ] || report info "some changed settings apply only after a SteamVR restart"
dropped=$(count '^events lost')
[ "$dropped" -eq 0 ] || report info "the kernel dropped mouse events $dropped time(s) (buttons were re-read)"

exit "$fail"
