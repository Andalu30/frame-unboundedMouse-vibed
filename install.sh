#!/bin/sh
# SPDX-License-Identifier: MIT
# Register the mouselaser driver with SteamVR.
# Only touches ~/.config/openvr/openvrpaths.vrpath (a backup is made first).
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)/driver/mouselaser"
VRPATHS="$HOME/.config/openvr/openvrpaths.vrpath"
VRPATHREG=/opt/steamvr/bin/linuxarm64/vrpathreg

[ -f "$DIR/bin/linuxarm64/driver_mouselaser.so" ] || {
    echo "Driver not built. Run: cmake -S . -B build -G Ninja && cmake --build build"
    exit 1
}

# Refuse if a mouselaser driver is already registered from another path:
# two drivers with the same name would conflict.
OTHER=$(python3 - "$VRPATHS" "$DIR" <<'PY'
import json, os, sys
try:
    paths = json.load(open(sys.argv[1])).get("external_drivers") or []
except FileNotFoundError:
    paths = []
for p in paths:
    if os.path.realpath(p) == os.path.realpath(sys.argv[2]):
        continue
    try:
        if json.load(open(os.path.join(p, "driver.vrdrivermanifest"))).get("name") == "mouselaser":
            print(p)
    except (OSError, ValueError):
        if os.path.basename(p.rstrip("/")) == "mouselaser":
            print(p)
PY
)
if [ -n "$OTHER" ]; then
    echo "Another mouselaser driver is already registered:"
    echo "$OTHER"
    echo "Remove it first:  $VRPATHREG removedriver <path>"
    exit 1
fi

[ -f "$VRPATHS" ] && cp "$VRPATHS" "$VRPATHS.bak-mouselaser"
"$VRPATHREG" adddriver "$DIR"
"$VRPATHREG" show | grep -A5 -i "external drivers" || true
echo "Registered. Reboot to load it (on the Steam Frame, restarting SteamVR also restarts gamescope)."
