# How the driver works

All code is in [`../src/driver.cpp`](../src/driver.cpp). Everything SteamVR loads lives in [`../driver/mouselaser/`](../driver/mouselaser/).

## Pieces

```
vrserver
 └─ loads driver/mouselaser/bin/linuxarm64/driver_mouselaser.so    (driver.vrdrivermanifest: alwaysActivate=true)
     HmdDriverFactory() → MouseLaserProvider (IServerTrackedDeviceProvider)
       └─ MouseLaserDevice (ITrackedDeviceServerDriver), serial "mouselaser-0", class Controller
            ├─ MouseReader thread  ── reads /dev/input/eventX (evdev), EVIOCGRAB when active
            └─ RunFrame()          ── pose + input components, every vrserver frame
vrcompositor
 └─ uses resources/input/vrcompositor_bindings_mouselaser.json (via the profile's default_bindings)
     /actions/lasermouse/in/Pointer ← /user/hand/left/pose/raw while on (activeRole), /user/stylus/... while off → the laser
```

### Why `alwaysActivate: true`
`steamvr.vrsettings` has `activateMultipleDrivers: false` by default. That setting only limits which **HMD** driver is picked. Drivers whose manifest sets `alwaysActivate` are loaded in addition. This is the standard route for add-on trackers.

### MouseReader (evdev thread)
- Scans `/dev/input/event*` for the first device with `EV_REL` (REL_X, REL_Y) and `EV_KEY` (BTN_LEFT), optionally filtered by `deviceNameFilter`. A keyboard's "Consumer Control" node doesn't match.
- `poll()`s with a 200 ms timeout. On `POLLERR`/`POLLHUP` or `ENODEV` (mouse asleep or unplugged) it closes the device and rescans every 2 s.
- Pressing the toggle key (`toggleButton`, default BTN_EXTRA = 276) flips `active`:
  - **on:** `ioctl(EVIOCGRAB, 1)` gives exclusive access, so gamescope stops receiving the mouse.
  - **off:** `ioctl(EVIOCGRAB, 0)` releases the mouse; button state and deltas are cleared.
- While active it accumulates `REL_X`/`REL_Y`/`REL_WHEEL` into atomics and tracks BTN_LEFT, BTN_RIGHT and BTN_MIDDLE. `RunFrame` consumes them with `exchange(0)`.

### RunFrame (pose)
- HMD pose: `VRServerDriverHost()->GetRawTrackedDevicePoses(0, &hmd, 1)`. Device 0 is the HMD.
- On each OFF→ON toggle, yaw and pitch are initialised from the HMD's forward vector (−Z column of `mDeviceToAbsoluteTracking`):
  `yaw = atan2(-fx, -fz)`, `pitch = asin(fy)`.
- Mouse deltas: `yaw -= dx·k`, `pitch -= dy·k` (sign flipped if `invertY`), where `k = sensitivity·π/180`. Pitch is clamped to ±89°.
- Orientation is **world-locked**: `q = yaw(Y) · pitch(X)` = `(cy·cp, cy·sp, sy·cp, −sy·sp)` with half-angle sines and cosines. OpenVR controllers point along −Z, so this aims the ray.
- Position = HMD position + `(0, originOffsetY, 0)` in world space.
- When inactive, it depends on `role`:
  - **stylus (5, default since 0.5.0):** the device stays connected with a valid pose, but its ray is parked pointing straight up (yaw 0, pitch 90°) and every button is released. This matters because a device that goes invalid or disconnects is dropped as the laser pointer until SteamVR sees a new "user interaction". That took about 10 s of quiet in earlier captures, so a quick off/on left the mouse grabbed with no laser.
  - **hand (1/2):** `deviceIsConnected = false` and `poseIsValid = false`, so the real controller of that hand gets its slot back. A connected device with a hand role would compete with it.

### Role and user path
Since 0.5.1 the role changes with laser mode. The device registers with `role` (default stylus, 5). On each toggle, `RunFrame` sets `Prop_ControllerRoleHint_Int32` to `activeRole` (default left hand, 1) when switching on, or back to `role` when switching off, and logs `role N`. This is needed because dragging overlays only works with a hand (see below and the primer). SteamVR applies the change live: each switch is followed by `event 108` (role changed), and the owner reports laser and dragging working with 0.5.1.

SteamVR gives each controller a `/user/...` path based on its role. Hand roles share `/user/hand/left|right` with the real controllers. `TrackedControllerRole_Stylus` (5) maps to `/user/stylus`, which `IsRoleAllowedAsHand()` rules out of hand selection. Treadmill (4) maps to `/user/treadmill`. OptOut (3) has no path unless a tracker role is assigned in SteamVR. The compositor's laser accepts pose sources that aren't hands (the Frame HMD binds `/user/head/pose/raw` to `lasermouse/in/pointer`), so the bindings repeat every hand entry for `/user/stylus`.

### Event diagnostics
The provider logs SteamVR events about our device, plus user-interaction, role-change and dashboard events (`event N device M (active|off)`). There is no driver-side signal for "the laser is up", and these events are what we're watching to look for one.

### RunFrame (inputs)
| component | driven by | bound to (compositor) |
|---|---|---|
| `/input/trigger/click`, `/value` | left button | `lasermouse/LeftClick`, `lasermouse_secondary/SwitchLaserHand` |
| `/input/a/click` | right button | `lasermouse/RightClick` |
| `/input/b/click` | middle button | `lasermouse/MiddleClick` |
| `/input/grip/click` | held **true** while active | `quickmouse/ActivateQuickMouse` (keeps the laser up, as squeezing a grip does on Frame controllers) |
| `/input/thumbstick/y`, `/x` (+ `/touch`) | wheel → Y, tilt wheel → X, via `StickStepper` (see below) | `dualanalog/LeftValue`/`RightValue` (+ touch, mode `joystick`), `scroll_discrete` and `scroll_smooth` (mode `scroll`), the same as the Frame controller's stick |

### Wheel → thumbstick (`StickStepper`)
Steam UI lists are navigated through the compositor's `/actions/dualanalog` stick values, not through scroll events. The Frame controllers' `steam.client` binding only carries haptics. A wheel only produces discrete notches, so `StickStepper` turns them into stick deflection. It is timed with `steady_clock`, independently of the frame rate, and has two modes, chosen with `wheelMode`.

**smooth** (default, 0.3.0):
- On a notch: if the stick is at rest, it jumps to `wheelSmoothMin` (+ `wheelSmoothImpulse` for each extra notch in the same event). Otherwise `wheelSmoothImpulse` is added to its current value. Either way it is capped at `wheelDeflection`.
- For `wheelSmoothHoldMs` after a notch the value holds. After that it decays exponentially with time constant `wheelSmoothDecayMs`, and snaps to 0 below 0.1.
- The result:
  - A single notch is about 130 ms above 0.5, which is one list step, followed by a short glide.
  - Spinning at 10 notches/s ramps up to a steady 0.8–1.0 hold, which scrolls continuously without dips.
  - Slow notches (3/s) remain separate steps.
- These figures come from an offline 120 Hz simulation that drives `StickStepper` directly.

**step** (0.2.0 behaviour):
- The stick is pushed to `±wheelDeflection` for `wheelPressMs`, then held at centre for `wheelReleaseMs`.
- Further notches queue up, to at most `wheelMaxQueued`.
- Reversing direction drops the queue and switches immediately.
- When laser mode turns off, the queue is cleared.

### Render model
`{mouselaser}mouselaser_none` is a single 0.1 mm triangle with a transparent 1×1 texture. Without a model of its own, SteamVR would draw a generic controller at your face.

## Files
| file | purpose |
|---|---|
| `driver.vrdrivermanifest` | Driver name and `alwaysActivate`. |
| `resources/settings/default.vrsettings` | Defaults for the `driver_mouselaser` section. |
| `resources/input/mouselaser_profile.json` | Input profile. Controller type `mouselaser`, declares components and points to the default bindings. |
| `resources/input/vrcompositor_bindings_mouselaser.json` | Bindings for app key `openvr.component.vrcompositor`, the laser and dashboard. |
| `resources/rendermodels/mouselaser_none/` | The invisible model. |
| `resources/localization/localization.json` | Display names (en_US, es_ES) for the controller type and its inputs, as shown in SteamVR's binding screens: "Left Click", "Mouse Wheel" and so on. |

## Changing things safely
- After editing code: `cmake --build build`, then reboot. The `.so` is only loaded when SteamVR starts.
- After editing bindings or the profile: reboot. SteamVR may also cache bindings per controller type under `~/.local/share/Steam/config/` or `~/.config/openvr/`.
- To add a setting: add it to `Settings::Load()`, add the default to `default.vrsettings`, and document it in the README table.
