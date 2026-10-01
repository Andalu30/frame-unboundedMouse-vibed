#!/bin/sh
# SPDX-License-Identifier: MIT
# Unregister the mouselaser driver from SteamVR.
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)/driver/mouselaser"
/opt/steamvr/bin/linuxarm64/vrpathreg removedriver "$DIR"
echo "Unregistered. Reboot to unload it."
