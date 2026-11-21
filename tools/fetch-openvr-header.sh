#!/bin/sh
# SPDX-License-Identifier: MIT
# Downloads Valve's OpenVR driver header (public SDK, BSD-3-Clause) at a pinned version, so the
# driver can be built where SteamVR isn't installed: a PC, or CI. On the headset the build uses
# SteamVR's own copy instead. The checksum pins the exact file.
#
#   tools/fetch-openvr-header.sh <dir>     then configure with -DOPENVR_HEADERS=<dir>
set -eu
# v2.15.6 has the driver interfaces the Frame's SteamVR uses:
# IServerTrackedDeviceProvider_004 and ITrackedDeviceServerDriver_005.
TAG=v2.15.6
SHA256=1036efe998d63e82d1d3db2b32a2f58df4a8eeaf5280f50aaf28220ff60a40ab
DIR=${1:?usage: $0 <dir>}

mkdir -p "$DIR"
curl -fsSL -o "$DIR/openvr_driver.h.part" \
    "https://raw.githubusercontent.com/ValveSoftware/openvr/$TAG/headers/openvr_driver.h"
echo "$SHA256  $DIR/openvr_driver.h.part" | sha256sum -c --quiet -
mv "$DIR/openvr_driver.h.part" "$DIR/openvr_driver.h"
echo "openvr_driver.h $TAG in $DIR"
