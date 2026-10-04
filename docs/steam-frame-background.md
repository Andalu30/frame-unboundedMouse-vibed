# Steam Frame background: why the mouse is trapped, and why a driver fixes it

These are findings from read-only exploration of a Steam Frame on 2026-10-01 (SteamOS 0.4.2 VR, build 20260928.6175029). Paths and versions may change with updates.

## The display chain
The KDE desktop doesn't run on the hardware. It is nested several layers deep:
```
DSI-1 panel (2× 2160x2160 @ 108/120/144 Hz)
 └─ vrcompositor (SteamVR) owns the panel
     └─ gamescope --backend openvr        draws each app as a VR overlay (--virtual-connector-strategy PerAppId)
         │  --output-width 1920 --output-height 1080, --vr-overlay-physical-width 2.67
         └─ Xwayland :0
             └─ kwin_wayland (X11 *windowed* backend), started by /usr/bin/steamos-nested-desktop with --width 1280 --height 800
                 └─ Plasma + apps
```
- Steam's "Desktop" entry is `/usr/share/applications/deckard-nested-desktop.desktop`. "Deckard" is the Frame's internal codename.
- Gamescope's arguments are hardcoded in `/usr/lib/steamos/gamescope-session` and run from the user unit `gamescope-session.service`. That unit is `PartOf=steamvr.service`, which is why restarting SteamVR kills the desktop.
- Gamescope resizes KWin's window to its own 1920×1080 output, whatever size KWin asks for.

## The input path
```
/dev/input/eventX (mouse) → gamescope (libinput) → focused overlay's Xwayland → KWin (as X11 events)
```
- KWin on the X11 windowed backend never uses libinput, so KDE's mouse and keyboard settings pages are empty. `/org/kde/KWin/InputDevice` doesn't exist.
- Gamescope delivers the mouse only to its **focused** overlay, clamped to that surface. **This is the "bounded" mouse the project is named after.**
- **Back/forward side buttons never reach KDE apps.** Gamescope delivers them fine (X button 8/9 on `:0`, seen with `xinput test-xi2`). KWin 6.2.5's X11 windowed backend (`X11WindowedBackend::handleButtonPress`) then discards every X button above 7: its `default:` branch `return`s. Upstream KWin removed that backend in 2025 without fixing it. KWin's Wayland nested backend passes button codes through unchanged.
- SteamVR's laser **Back** action (a controller's B, or this driver's back button) only reaches SteamVR overlays such as Steam's UI. Gamescope doesn't translate it for the windows it hosts.

## The laser pointer
- The laser belongs to **vrcompositor**. It casts a ray from a *pose* and sends the overlay it hits `VREvent_MouseMove/ButtonDown/ButtonUp/...`. Gamescope turns those into pointer input for its clients.
- The actions are in `/opt/steamvr/resources/config/vrcompositor_actions.json`. The important one is `/actions/lasermouse/in/Pointer` (type **pose**, mandatory), plus LeftClick, RightClick, MiddleClick, scroll and others.
- Frame controller bindings (`/opt/steamvr/drivers/frame_controller/resources/input/vrcompositor_bindings_frame_controller.json`):
  - Pointer ← `/user/hand/*/pose/tip`
  - LeftClick ← trigger
  - "Quick mouse" (`/actions/quickmouse/in/ActivateQuickMouse`) ← squeeze the grip. This just brings the laser up while you're in gamepad mode.
- The Frame **HMD** also has a binding (`drivers/frame_hmd/.../vrcompositor_bindings_frame_hmd.json`): Pointer ← `/user/head/pose/raw`, which is a head-gaze laser.
- **Key insight:** the compositor accepts *any* pose source for the laser. A virtual device with a mouse-aimed pose is therefore enough, and no compositor hacking is needed. This driver is built on that.

## Alternatives that were considered
| idea | why not (yet) |
|---|---|
| Quick mouse | Only an "activate" shortcut. The ray still needs a pose. |
| `lasermouse` mailbox (`ws://127.0.0.1:27062`, messages `dump_laser_overlays`, `remote_laser_mouse_events`, `force_activate_laser_mouse`) | The remote-event payload is an undocumented protobuf (`VRLink_LaserMouseEvent`, from `vr_vrlink.proto`, built with protobuf-lite so there's no schema in the binary). It seems to be meant for VRLink PC streaming. |
| Gamescope flags (`--mouse-sensitivity`, `--force-grab-cursor`, overlay size) | They only change behaviour *inside* one overlay. |
| Running KWin on DRM/libinput | SteamVR owns the panel. |

### Mailbox wire protocol, for reference
This is what the dashboard JS does (`/opt/steamvr/resources/webinterface/dashboard/chunk~*.js`):
```
connect ws://127.0.0.1:27062[?secret=...]
send    "mailbox_open <name>"
send    "mailbox_send <target> <json>"    json: {"type": "...", "returnAddress": "<name>", "message_id": N, ...}
recv    JSON; replies mirror message_id
```

## Useful locations
| what | where |
|---|---|
| SteamVR runtime | `/opt/steamvr` (bin/linuxarm64, drivers/, resources/) |
| Driver registration | `~/.config/openvr/openvrpaths.vrpath` (`vrpathreg show/adddriver/removedriver`) |
| User SteamVR settings | `~/.config/openvr/config/steamvr.vrsettings` |
| Logs | `~/.local/share/Steam/logs/vrserver.txt`, `vrcompositor.txt`. They're also forwarded to the journal by `/usr/share/deckard/steamvr_logs_to_journald.py`. |
| OpenVR driver header | `/opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers/openvr_driver.h` (SteamVR 2.1 API: `IServerTrackedDeviceProvider_004`, `ITrackedDeviceServerDriver_005`) |
