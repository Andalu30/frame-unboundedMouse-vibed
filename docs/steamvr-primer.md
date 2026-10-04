# SteamVR and the Steam Frame: a primer

This primer is for a developer who is comfortable with Linux and C++ but has never looked inside SteamVR. It explains the parts of SteamVR this project touches, and what is specific to the Steam Frame, in the order you need them. It brings together everything learned while building `mouselaser`. The deeper notes it draws on are listed at the end.

> **How sure are we?** SteamVR is closed source. Facts here come from three places: Valve's public OpenVR header (`openvr_driver.h`), the JSON and settings files SteamVR ships in `/opt/steamvr`, and what we observed in logs and experiments on one headset. Where something is a guess, it says so. Everything was observed on SteamOS 0.4.2 (VR), build 20260928.6175029, in October 2026. Valve can change any of it in an update.

## Contents
1. [The 30-second picture](#1-the-30-second-picture)
2. [The Steam Frame as a Linux computer](#2-the-steam-frame-as-a-linux-computer)
3. [How a session starts, and why restarting SteamVR kills your terminal](#3-how-a-session-starts-and-why-restarting-steamvr-kills-your-terminal)
4. [SteamVR's processes](#4-steamvrs-processes)
5. [Drivers](#5-drivers)
6. [Tracked devices, poses and roles](#6-tracked-devices-poses-and-roles)
7. [SteamVR Input: from button to action](#7-steamvr-input-from-button-to-action)
8. [The compositor, overlays and the laser pointer](#8-the-compositor-overlays-and-the-laser-pointer)
9. [The display chain: how the KDE desktop gets into the headset](#9-the-display-chain-how-the-kde-desktop-gets-into-the-headset)
10. [The input chain: where a mouse event really goes](#10-the-input-chain-where-a-mouse-event-really-goes)
11. [How mouselaser fits in](#11-how-mouselaser-fits-in)
12. [Lessons learned the hard way](#12-lessons-learned-the-hard-way)
13. [Cheat sheet: files, logs and tools](#13-cheat-sheet-files-logs-and-tools)
14. [Glossary](#14-glossary)
15. [Open questions](#15-open-questions)

---

## 1. The 30-second picture

On a PC, SteamVR is an app you start on top of your desktop. **On the Steam Frame it's the other way round: SteamVR *is* the session.** It owns the display, and everything else, including Steam's UI and the KDE desktop, is a flat panel ("overlay") that SteamVR draws floating in 3D.

```
                ┌──────────────────────── SteamVR ─────────────────────────┐
 tracking  ───► │ vrserver   loads drivers; knows every device and its pose │
 (cameras)      │   ├─ cv driver        → headset + Frame controllers       │
                │   └─ mouselaser       → our virtual "mouse laser" device  │
                │                                                           │
                │ vrcompositor  draws the 3D scene + all overlays on the    │
                │               panel; owns the laser pointer               │
                └────────────▲────────────────────────────▲─────────────────┘
                             │ overlays                   │ overlays
                        Steam UI                gamescope (one overlay per app)
                                                      └─ KDE desktop (nested)
```

Two facts explain almost everything in this project:
- **The laser pointer belongs to SteamVR's compositor.** It can point at any overlay.
- **The physical mouse belongs to gamescope.** It can only move inside the one overlay gamescope has focused.

`mouselaser` bridges the two. It pretends to be a SteamVR controller whose aim comes from the mouse.

---

## 2. The Steam Frame as a Linux computer

| | |
|---|---|
| SoC | Qualcomm Snapdragon 8 Gen 3 (SM8650), 8 ARM cores, **aarch64**. Build everything for `linuxarm64`. |
| GPU | Adreno 750. Vulkan via Mesa Turnip. KWin's OpenGL goes through **zink** (GL on Vulkan). |
| RAM | 15 GiB plus zram swap. |
| Display | One DSI panel, `2×2160×2160` (one square per eye), 108/120/144 Hz. |
| OS | SteamOS `VARIANT_ID=vr`, Arch-based, internal codename **"Deckard"** (you'll see it in file names like `deckard-nested-desktop.desktop`). |
| User | `steamos`, in the `input` group, so it can read `/dev/input/event*` directly. |
| Remote access | `xrdp` runs, and SSH works over Wi-Fi or USB networking (`usb0`). Keep one of them ready when testing drivers. |

**The OS image is read-only.** `/usr` comes from an A/B system image. Anything you change there is lost on the next update, and changing it needs root plus disabling read-only mode. That's why this project lives entirely in user space: a driver registered from your home folder, and settings in `~/.config/openvr`.

---

## 3. How a session starts, and why restarting SteamVR kills your terminal

```
SDDM (autologin)
 └─ gamescope-session.service   (systemd --user; PartOf=steamvr.service)
     └─ gamescope --backend openvr ...       ← talks to SteamVR as an overlay app
         └─ Steam  -deckard -gamepadui -vrgamepadui ...
             └─ "Desktop" entry → steamos-nested-desktop
                 └─ kwin_wayland (nested) → Plasma → Konsole → this Claude session
```

- `gamescope-session.service` is declared `PartOf=steamvr.service`. **If SteamVR restarts, systemd restarts gamescope too, and with it Steam and the whole nested desktop.** Any terminal running inside the desktop dies. That's why we always reboot instead of "just restarting SteamVR".
- The nested desktop gets its own runtime directory: `XDG_RUNTIME_DIR=/run/user/1000/nested_plasma`. Commands that need the real user session, like `systemctl --user`, need `XDG_RUNTIME_DIR=/run/user/1000` in front of them.
- The desktop's size (1280×800) is hardcoded in `/usr/bin/steamos-nested-desktop`. Gamescope then stretches its window to 1920×1080 anyway (see [section 9](#9-the-display-chain-how-the-kde-desktop-gets-into-the-headset)).

---

## 4. SteamVR's processes

SteamVR lives in `/opt/steamvr`. Binaries are in `bin/linuxarm64/`.

| Process | What it does | Why you care |
|---|---|---|
| `vrserver` | The core. Loads **drivers**, keeps the list of tracked devices and their poses, runs the input system, stores settings. | **Our driver's `.so` is loaded into this process.** If our code crashes, vrserver crashes, and SteamVR goes with it. |
| `vrcompositor` | Renders the final image for each eye: the 3D scene, every overlay, and the laser pointer. Owns the panel. | It decides where the laser points and which overlay gets clicks. |
| `vrdashboard`, `vrwebhelper` | The SteamVR dashboard UI. It's a web app (Chromium-based helper) whose JS is in `resources/webinterface/dashboard/`. | Reading that JS is how we found how "Manage Add-ons" and the mailbox work. |
| `vrmonitor`, `vrstartup` | Status window and launcher. | Rarely matters on the Frame. |
| `vrcmd` | A command-line client. `vrcmd --help` lists options such as `--events`, `--overlays`, `--pollposes` and `--info`. | Handy for watching what SteamVR does. Some options *change* state (`--set-settings-*`, `--send-vrevent`); stick to the read-only ones unless you mean it. |
| `XRService`, `dsp_service`, `eyetracking` | Frame-specific: computer-vision tracking and eye tracking, partly on the Hexagon DSP. | They feed the `cv` driver. |
| `v4l2cam`, `proxmicmute` | Passthrough cameras; mic mute from the proximity sensor. | Not relevant here. |

Each process has a log in `~/.local/share/Steam/logs/`: `vrserver.txt`, `vrcompositor.txt` and so on. The previous run is kept as `*.previous.txt`.

---

## 5. Drivers

### What a driver is
A SteamVR **driver** is a shared library (`.so`) that `vrserver` loads. Its job is to **add tracked devices** (headsets, controllers, trackers) and to keep telling SteamVR where they are and what their buttons are doing. Drivers don't draw anything and don't talk to apps. Apps only ever see devices, poses and actions.

A driver folder looks like this (ours):
```
driver/mouselaser/
├── driver.vrdrivermanifest            ← "who am I" (JSON)
├── bin/linuxarm64/driver_mouselaser.so  ← the code; folder name = platform
└── resources/
    ├── settings/default.vrsettings    ← default values of our settings section
    ├── input/                         ← input profile + default bindings (section 7)
    ├── localization/                  ← names shown in SteamVR's binding UI
    └── rendermodels/                  ← 3D model drawn for the device
```

### The manifest fields that matter
| Field | Meaning |
|---|---|
| `name` | The driver's name. It decides the settings section (`driver_<name>`) and the `{name}` prefix used in resource paths like `{mouselaser}/input/...`. |
| `alwaysActivate` | Normally SteamVR loads only **one** "HMD driver" (the one that finds a headset). Drivers with `alwaysActivate: true` load **in addition**, which is the normal route for add-on devices like ours. |
| `resourceOnly` | `true` means "no code, just resources". It supplies profiles, bindings and models for devices that another driver creates. |
| `hmd_presence` | USB IDs that mean "my headset is plugged in". |

### The Frame's own drivers (`/opt/steamvr/drivers/`)
- **`cv`** is the only one that matters at runtime. Its `driver_cv.so` adds the **headset and both Frame controllers** (the log shows `Driver 'cv' started activation of tracked device ...` three times). It gets tracking from `XRService`. `hmd_presence: 28DE.2300` (28DE is Valve's USB vendor ID).
- **`frame_hmd`** and **`frame_controller`** are **resource-only**. They supply the input profiles, bindings and models for the devices `cv` creates. **This is where to look to see how Valve binds the Frame hardware.**
- `prism` (display redirect, disabled in settings), and `htc`, `indexcontroller`, `indexhmd` and `oculus`, which are resources for PC headsets that SteamVR ships everywhere.

### How SteamVR finds a driver
- Built-in drivers live in `/opt/steamvr/drivers/`.
- Extra drivers are listed in `~/.config/openvr/openvrpaths.vrpath` under `external_drivers`. You edit that list with `vrpathreg adddriver <folder>` / `removedriver <folder>` (`/opt/steamvr/bin/linuxarm64/vrpathreg`). Our `install.sh` does this after making a backup.
- The SteamVR dashboard's developer **Manage Add-ons** page lists every driver that isn't resource-only. Its toggle writes `driver_<name>.enable` to `steamvr.vrsettings`, which our driver honours as a kill switch.

### A driver's life cycle (OpenVR driver API)
```
vrserver starts
 └─ dlopen(driver_mouselaser.so)
     └─ HmdDriverFactory("IServerTrackedDeviceProvider_004")  → returns our provider
         └─ provider.Init(context)          read settings, then:
             └─ VRServerDriverHost()->TrackedDeviceAdded("mouselaser-0", Controller, device)
                 └─ device.Activate(index)  set properties, create input components
 every frame (~ display rate):
     provider.RunFrame()                    push pose + button states, drain events
 shutdown:
     device.Deactivate(), provider.Cleanup()
```
- **Two interfaces:** the *provider* (`IServerTrackedDeviceProvider`) is the driver as a whole; each *device* is an `ITrackedDeviceServerDriver`.
- **Version strings** like `IServerTrackedDeviceProvider_004` are how SteamVR checks the driver was built against a compatible API. The Frame's SteamVR is API 2.1. We compile against the header SteamVR itself ships: `/opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers/openvr_driver.h`.
- **Everything is read once at startup.** Settings, bindings and the `.so` itself. A code or binding change needs a **rebuild and a reboot** on the Frame (see section 3).
- `RunFrame` runs on vrserver's main thread. Blocking there stalls SteamVR, so we read the mouse on our own thread and hand values over through atomics.

### Settings
- Every driver gets a section named `driver_<name>` in SteamVR's settings.
- Defaults come from the driver's `resources/settings/default.vrsettings`. User overrides go in `~/.config/openvr/config/steamvr.vrsettings`. SteamVR's own defaults are in `/opt/steamvr/resources/settings/default.vrsettings` (for example the laser's `laserLength`, `cursorScale` and `laserMouseDebugging`).
- A driver reads them with `VRSettings()->GetInt32/GetFloat/GetBool/GetString(section, key, &err)`.

---

## 6. Tracked devices, poses and roles

### Devices and indices
Every device gets an **index** from 0 to 63. **Index 0 is always the HMD.** Ours usually lands on index 1 because it's added before the controllers; the log says `activated as device 1`. Index `4294967295` (`0xFFFFFFFF`, `k_unTrackedDeviceIndexInvalid`) in an event means "no particular device".

Each device has a **class**:

| Class | Example |
|---|---|
| `HMD` (1) | The Frame headset |
| `Controller` (2) | Frame controllers, **and our virtual device** |
| `GenericTracker` (3) | Vive trackers on feet or waist |
| `TrackingReference` (4) | Lighthouse base stations (not used by the Frame, which tracks with cameras) |
| `DisplayRedirect` (5) | Devices that show video elsewhere |

### Properties
A device describes itself with **properties**: key/value pairs set in `Activate()`. The ones we set:
- `Prop_ControllerType_String = "mouselaser"`: which input profile and bindings apply (section 7).
- `Prop_InputProfilePath_String`: where the input profile JSON is.
- `Prop_RenderModelName_String`: the 3D model to draw. Ours is an invisible 0.1 mm triangle; without it SteamVR draws a generic controller at your face.
- `Prop_ControllerRoleHint_Int32`: the device's **role** (below).
- Model number and manufacturer, shown in SteamVR's UI.

### Poses
A **pose** is "where the device is and which way it points". The driver sends it with `TrackedDevicePoseUpdated()` as a `DriverPose_t`:

| Field | Meaning |
|---|---|
| `vecPosition` | Position in metres. |
| `qRotation` | Orientation as a quaternion `(w, x, y, z)`. |
| `qWorldFromDriverRotation`, `vecWorldFromDriverTranslation` | Transform from the driver's own space into SteamVR's world. We use identity because we compute in world space directly. |
| `deviceIsConnected` | Whether the device is there at all. |
| `poseIsValid` | Whether the position can be trusted right now. |
| `result` | Tracking state, for example `TrackingResult_Running_OK` or `Running_OutOfRange`. |

**Coordinates:** right-handed, metres, **+Y is up**, and **a controller points along its −Z axis**. So to aim a ray, you rotate −Z towards the target. We build the rotation from a yaw (around Y) and a pitch (around X), driven by mouse movement.

You can read other devices' poses from a driver with `VRServerDriverHost()->GetRawTrackedDevicePoses()`. We read the HMD's (index 0) so the ray starts at your head.

### Roles: which "hand" a device is
A controller declares a **role**, and the role decides its **user path**. User paths are how the input system (section 7) refers to "the left hand" without caring which physical device that is.

| Role (`ETrackedControllerRole`) | Value | User path | Notes |
|---|---|---|---|
| `LeftHand` | 1 | `/user/hand/left` | Only one device holds this slot at a time. |
| `RightHand` | 2 | `/user/hand/right` | Same. |
| `OptOut` | 3 | none, unless you assign a tracker role (e.g. `/user/waist`) in SteamVR | For generic trackers. |
| `Treadmill` | 4 | `/user/treadmill` | Locomotion devices. Never treated as a hand. |
| `Stylus` | 5 | `/user/stylus` | Pen-like pointers (Logitech's VR Ink stylus). Never treated as a hand. |

`IsRoleAllowedAsHand()` in the header returns true only for Invalid, LeftHand and RightHand. Other user paths SteamVR knows about include `/user/head`, `/user/hand/secondary`, `/user/keyboard`, `/user/waist`, `/user/foot/...`, `/user/knee/...` and the Vive tracker role paths.

**Why this mattered for us:** up to 0.4.x, `mouselaser` registered as the **left hand**. Whenever it was connected it competed with your real left controller for `/user/hand/left`, and one of them lost its laser. 0.5.0 registered as a **stylus**, which has its own slot, but a stylus can't drag overlays (section 8). Since 0.5.1 it is a stylus while laser mode is off and switches to the left hand while it's on. **The role hint can be changed while SteamVR runs:** update `Prop_ControllerRoleHint_Int32`, and SteamVR re-assigns roles and sends `TrackedDeviceRoleChanged` (108).

### Activity events
SteamVR tracks whether each device is in use and sends events about it. Ones we've seen and care about:

| Event | Id | Meaning |
|---|---|---|
| `TrackedDeviceActivated` / `Deactivated` | 100 / 101 | A device came up or went away. |
| `TrackedDeviceUserInteractionStarted` / `Ended` | 103 / 104 | The user started or stopped using a device (moving it, pressing buttons). "Ended" fires after a timeout with no activity. |
| `TrackedDeviceRoleChanged` | 108 | Hand assignments changed. |
| `PropertyChanged` | 111 | A device property changed. Many at startup. |

These matter because **the compositor uses them to decide which device drives the laser** (section 8). A driver can see them with `VRServerDriverHost()->PollNextEvent()`. Since 0.5.0 the driver logs them as `mouselaser: event N device M`. One unexplained observation: our device gets 100/101 on every toggle even though it stays connected.

---

## 7. SteamVR Input: from button to action

This is the part that takes longest to understand, and the part that matters most to us. **Apps never ask "is the trigger pressed?". They ask "is the *Click* action on?"**, and a **binding** decides which button means Click on each kind of controller.

### The chain
```
 driver                     input profile          binding (per app, per controller type)        app
 ──────                     ─────────────          ─────────────────────────────────────         ───
 component handle   ──►  /input/trigger   ──►  /user/hand/left/input/trigger  ──►  /actions/lasermouse/in/LeftClick
 (UpdateBooleanComponent)   "type": trigger         mode: button, input: click                     (boolean action)
 pose (TrackedDevicePoseUpdated)          ──►  /user/hand/left/pose/raw       ──►  /actions/lasermouse/in/Pointer
                                                                                                    (pose action)
```

1. **Components.** In `Activate()` the driver creates named inputs, such as `CreateBooleanComponent(c, "/input/trigger/click", &handle)`, then updates them every frame with `UpdateBooleanComponent(handle, value, 0)`. Scalars (`/input/thumbstick/x`) work the same way with floats.
2. **Input profile** (`resources/input/mouselaser_profile.json`). This declares, for the controller type, which sources exist (`/input/trigger`, `/input/a`, `/input/thumbstick`, `/pose/raw`, ...) and what kind each one is (button, trigger, joystick, pose). It also lists **default bindings**, one file per app. `input_bindingui_mode` tells the binding UI whether to show left/right hands (`controller_handed`), a single device (`single_device`) or an HMD (`hmd`).
3. **User path.** The device's role puts its sources under a user path, so `/input/trigger` on a left-hand device becomes `/user/hand/left/input/trigger`. On a stylus it's `/user/stylus/input/trigger`.
4. **Bindings** connect user-path sources to an app's **actions**. A binding has a **mode** that interprets the raw input: `button` (click), `trigger`, `joystick` (position + touch), `scroll` (turn a stick into scroll ticks), and so on.
5. **Actions** are what the app defines in its **action manifest**: named, typed inputs (`boolean`, `vector1/2`, `pose`, `vibration`) grouped in **action sets** (`/actions/<set>/in/<name>`). Each action has a `requirement` (`mandatory`, `suggested`, `optional`).

### Who is "the app" here?
Not just games. **The compositor itself is an input app**, with app key `openvr.component.vrcompositor`. Its actions are in `/opt/steamvr/resources/config/vrcompositor_actions.json`. That's why our driver ships `vrcompositor_bindings_mouselaser.json`: it tells the compositor how our device maps onto the compositor's laser actions.

Other apps you'll see in the logs:
- `steam.client` is Steam's UI. The Frame controllers' binding for it is haptics only, which is why the log line `steam.client (mouselaser) has no configured binding` is harmless.
- `system.generated.*` are apps without their own manifests, which get a generic "legacy" binding.

### The compositor's action sets (the ones we bind)
| Action set | Key actions | What it's for |
|---|---|---|
| `/actions/lasermouse` | `Pointer` (**pose, mandatory**), `LeftClick` (mandatory), `RightClick`, `MiddleClick`, `Back`, `Home`, `LockMousePosition`, `Move*`, `Enter` | The laser pointer itself. |
| `/actions/lasermouse_secondary` | `SwitchLaserHand` | Pulling the trigger on the *other* hand moves the laser to it. |
| `/actions/quickmouse` | `ActivateQuickMouse` | "Squeeze the grip to bring up the laser." |
| `/actions/scroll_discrete`, `/actions/scroll_smooth` | `Scroll` (vector2) | Scrolling the overlay under the laser. |
| `/actions/dualanalog` | `LeftValue`, `RightValue`, touches, clicks | Stick input to overlays. **Steam's UI lists navigate with this**, which is why the wheel drives the virtual thumbstick. |
| `/actions/system` | `ToggleDashboard`, volume, screenshot... | System buttons. |
| `/actions/locomotion`, `/actions/roomsetup`, `/actions/quickrecenter` | | Not used by us. |

### Where bindings come from, and caching
- **Default bindings** ship with the driver and are referenced from the input profile's `default_bindings`.
- If you edit bindings in SteamVR's **Controller Bindings** UI, SteamVR saves your own copy under `~/.local/share/Steam/config/` or `~/.config/openvr/`, and **that copy wins over the default**. If you change our JSON and nothing happens, check for a saved copy. (There was none for `mouselaser` when 0.5.0 was installed.)
- The log confirms loading: `[Workshop] Successfully loaded binding file '.../vrcompositor_bindings_mouselaser.json' for app 'openvr.component.vrcompositor'`.

### Where to learn by example
`/opt/steamvr/drivers/frame_controller/resources/input/` has the real Frame controller profile (`frame_controller_profile.json`) and its compositor bindings. `/opt/steamvr/drivers/frame_hmd/resources/input/` has the headset's. Reading them side by side with ours is the fastest way to understand the format.

---

## 8. The compositor, overlays and the laser pointer

### Overlays
An **overlay** is a flat 2D image that the compositor places in 3D space. Steam's UI, the SteamVR dashboard, notifications and every gamescope window are overlays. Overlays can receive **mouse events** from the compositor: `VREvent_MouseMove`, `MouseButtonDown/Up`, `ScrollDiscrete/Smooth`, focus enter and leave, all in the overlay's own 2D coordinates.

### How the laser works
Every frame, while the laser is up:
1. The compositor reads the **`Pointer` pose** action, from whichever device currently drives the laser.
2. It casts a ray from that pose along −Z and finds the overlay it hits.
3. It draws the beam and a cursor dot, and sends that overlay mouse events at the hit point. `LeftClick`, `RightClick` and the others become button events.
4. For a gamescope overlay, **gamescope turns those events into pointer input** for the X/Wayland windows inside it.

**The compositor doesn't care what the pose comes from.** The Frame headset's own binding feeds `Pointer` from `/user/head/pose/raw` (a head-gaze laser, clicking with the headset's system button). That was the key insight behind this project: no compositor hacking is needed, only a device with a pose the mouse controls.

### When the laser is up, and which device drives it
This is the least documented part. It's all inferred from behaviour, logs and strings in `vrcompositor`:
- **On the Frame, controllers are in "gamepad" mode most of the time**, and the laser is hidden. It comes up when the dashboard or an overlay that wants it is open, or while **quick mouse** is held (squeezing a grip). Our driver holds its virtual grip down while laser mode is on, to keep quick mouse active.
- **The compositor keeps one "pointer device".** It picks it from user activity (the interaction-started events in section 6) and from `SwitchLaserHand`.
- **Observed (0.4.x captures):** a device that **disconnects or reports an invalid pose is dropped as the pointer device**, and isn't picked up again until SteamVR sees a new "user interaction started". That took about 10 s of quiet. A quick off/on toggle therefore left the mouse grabbed with no laser. That's why 0.5.0 keeps the device connected with a valid pose (parked pointing at the sky) while laser mode is off.
- The compositor also has settings like `modalGamepadAndLaser` and `laserMouseDebugging` in SteamVR's default settings. We haven't experimented with them.

### Dragging overlays needs a hand (or the head)
The laser's *aim* comes from the `Pointer` action, which any user path can feed. **Grabbing and moving** an overlay (the dashboard, floating windows) is done by the compositor's scene graph (`CGrabTransform`), which attaches the overlay to a *device*. The only device paths in `vrcompositor` are `/user/hand/left`, `/user/hand/right` and `/user/head`. A `/user/stylus` pointer can aim and click, but a drag follows the **head**. Tested with 0.5.0, see the development log, entry 17. That's why 0.5.1 becomes a hand while laser mode is on.

### Other ways into the laser (considered, not used)
- **The `lasermouse` mailbox.** SteamVR has an internal message bus served by vrserver as a WebSocket on `ws://127.0.0.1:27062` (local only). The compositor listens on a mailbox called `lasermouse` with messages such as `dump_laser_overlays`, `force_activate_laser_mouse` and `remote_laser_mouse_events`. The last one is how **VRLink** (PC↔headset streaming) sends laser input. Its payload is an undocumented protobuf, so it's a dead end without reverse engineering. Protocol details are in [steam-frame-background.md](steam-frame-background.md).
- **Gamescope flags** (`--mouse-sensitivity`, `--force-grab-cursor`). These only change behaviour *inside* one overlay.

---

## 9. The display chain: how the KDE desktop gets into the headset

```
DSI-1 panel (2× 2160×2160)
 └─ vrcompositor (SteamVR)             owns the panel
     └─ gamescope --backend openvr     each Steam app = one VR overlay (PerAppId), 2.67 m wide
         │  output 1920×1080
         └─ Xwayland :0                gamescope's X server
             └─ kwin_wayland           "X11 windowed" backend: KWin is just an X11 window on :0
                 └─ Plasma, Konsole, Firefox ...  (Wayland clients of KWin; KWin also runs Xwayland :2)
```

What follows from this:
- **KDE never touches the hardware.** Its "Display" settings show a single fake output, `X11-0`, at 1920×1080@60. Resolution and refresh choices there do nothing. **Scale** does work (it's set to 0.8, giving a 2400×1350 logical desktop).
- **The aspect ratio and pixel size are gamescope's** (`--output-width/height` in `/usr/lib/steamos/gamescope-session`), not KDE's. An ultrawide desktop would mean overriding that script with a systemd user drop-in (see the parent folder's `07-aspect-ratio-ultrawide.md`).
- `KWIN_FORCE_SW_CURSOR=1` is set, presumably because a hardware cursor plane can't pass through a VR overlay.

---

## 10. The input chain: where a mouse event really goes

```
mouse ─► /dev/input/event5 (evdev)
           ├─► gamescope (libinput) ─► focused overlay's Xwayland ─► KWin (as X11 events) ─► KDE apps
           └─► mouselaser (reads evdev directly; EVIOCGRAB while laser mode is on)
```

- **evdev** is the kernel's raw input interface: `/dev/input/eventN` streams `input_event` structs (`EV_REL` for motion and wheel, `EV_KEY` for buttons such as `BTN_LEFT`, `BTN_SIDE` (275, "back") and `BTN_EXTRA` (276, "forward")). Any process with read access can listen.
- **`EVIOCGRAB`** is an `ioctl` that gives one reader **exclusive** access. While we hold it, gamescope receives nothing from the mouse, so the KDE cursor freezes. That's intended in laser mode.
- **Gamescope clamps the mouse to the focused overlay.** This is the "bounded" mouse the project's name refers to.
- **KDE's mouse and keyboard settings pages are empty.** KWin on the X11 windowed backend never opens input devices itself (no libinput), so its device D-Bus API doesn't exist. Per-device options like acceleration, natural scroll and left-handed mode are therefore gamescope's or libinput's business, not KDE's. The keyboard layout, cursor theme and shortcuts still work.
- **Back/forward side buttons never reach KDE apps.** Gamescope delivers them (as X buttons 8 and 9), but KWin 6.2.5's X11 windowed backend throws away every X button above 7. Upstream deleted that backend in 2025 rather than fixing it. KWin's Wayland nested backend passes them through; a "Desktop (Wayland)" launcher to try that exists but is untested (parent folder notes `04` and `09`).
- **SteamVR's laser `Back` action** reaches SteamVR overlays such as Steam's UI. Gamescope only forwards laser motion, buttons 1 to 3 and scrolling to its windows, so Back never reaches KDE either.
- **Bluetooth mice sleep.** The MCHOSE mouse disconnects after about 25 s idle and may come back as a different `eventN`. The driver closes the device on `ENODEV`/`POLLHUP` and rescans every 2 s.

---

## 11. How mouselaser fits in

With all of the above, the driver is short. Details are in [how-it-works.md](how-it-works.md).

1. **Provider** (`alwaysActivate` driver) adds one device: class `Controller`, controller type `mouselaser`. Its role is **Stylus** while laser mode is off and **left hand** while it's on (0.5.1, to allow dragging overlays).
2. **Mouse thread** finds the first evdev device with relative X/Y and a left button. The forward side button toggles laser mode and `EVIOCGRAB`.
3. **Every frame** (`RunFrame`):
   - Position = HMD position, a little lower (`originOffsetY`).
   - Orientation = yaw and pitch built up from mouse movement. When laser mode turns on, it starts from where you're looking. While off, it's parked pointing straight up.
   - Components: left button → trigger, right → A, middle → B, back side button → back, wheel → virtual thumbstick (with a smoothing model, because a wheel only sends notches). Grip is held while laser mode is on (quick mouse).
4. **Bindings** (`vrcompositor_bindings_mouselaser.json`) map those onto the compositor's `lasermouse`, `quickmouse`, `scroll_*` and `dualanalog` actions, for `/user/stylus` and both hands.

---

## 12. Lessons learned the hard way

| Lesson | Why |
|---|---|
| **Never restart SteamVR from inside the desktop.** Reboot instead. | gamescope is `PartOf=steamvr.service`; the desktop and your terminal die with it. |
| **Rebuild *and* reboot to test anything.** | The `.so`, settings and bindings are only read at startup. |
| **Bump the version string on every change.** | It's the only quick way to know which build is running. We once lost track: git was reverted but the old `.so` was still in place, and the log showed `0.4.3` while the source said `0.4.0`. The `.so` is gitignored, so reverting git doesn't touch it. |
| **A crash in the driver takes SteamVR down.** | The driver runs inside `vrserver`. Keep SSH/RDP ready, know the [recovery steps](../README.md#recovery), and never let a joinable `std::thread` be destroyed (that calls `std::terminate`). |
| **Don't disconnect or invalidate the pointer device to "turn it off".** | The compositor drops it as the laser pointer, and only takes it back after a new user interaction (about 10 s of quiet). |
| **Don't register as a hand unless you want to replace that hand.** | Hand slots are exclusive; the real controller loses its laser. If you need hand-only features, such as dragging overlays, switch the role hint to a hand only while you need it. |
| **Saved user bindings beat the driver's defaults.** | If a binding change "does nothing", look for a saved copy. |
| **Benign log noise:** `Driver mouselaser has no suitable devices`, and `steam.client (mouselaser) has no configured binding`. | The first is logged because our driver provides no HMD; the device is added right after. The second, because only compositor bindings are shipped (Steam's own Frame binding is haptics only anyway). |
| **SteamVR adds the driver name to log lines itself.** | Logging `"mouselaser: ..."` yourself gives `mouselaser: mouselaser: ...`. |
| **The display name in Manage Add-ons can't be changed.** | The dashboard looks names up in its own table and falls back to the manifest `name`. |

---

## 13. Cheat sheet: files, logs and tools

### Files
| What | Where |
|---|---|
| SteamVR runtime | `/opt/steamvr/` (`bin/linuxarm64/`, `drivers/`, `resources/`) |
| OpenVR driver header | `/opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers/openvr_driver.h` |
| Compositor actions | `/opt/steamvr/resources/config/vrcompositor_actions.json` |
| Frame controller profile + bindings | `/opt/steamvr/drivers/frame_controller/resources/input/` |
| Frame HMD profile + bindings | `/opt/steamvr/drivers/frame_hmd/resources/input/` |
| SteamVR default settings | `/opt/steamvr/resources/settings/default.vrsettings` |
| Your settings | `~/.config/openvr/config/steamvr.vrsettings` |
| Registered external drivers | `~/.config/openvr/openvrpaths.vrpath` |
| Logs | `~/.local/share/Steam/logs/vrserver.txt`, `vrcompositor.txt` (also forwarded to the journal) |
| Dashboard web UI (JS) | `/opt/steamvr/resources/webinterface/dashboard/` |
| Session scripts | `/usr/lib/steamos/gamescope-session`, `/usr/bin/steamos-nested-desktop` |

### Commands
```sh
# Is our driver loaded, which version, what is it doing?
grep -a 'mouselaser:' ~/.local/share/Steam/logs/vrserver.txt | tail -40

# Everything SteamVR said about our driver (loading, bindings)
grep -a -i mouselaser ~/.local/share/Steam/logs/vrserver.txt | grep -v legacy_bindings | tail

# Which devices were added, and by which driver
grep -a "started activation of tracked device" ~/.local/share/Steam/logs/vrserver.txt | tail

# Registered drivers
/opt/steamvr/bin/linuxarm64/vrpathreg show

# Which input devices exist (find your mouse's eventN)
cat /proc/bus/input/devices

# vrcmd: list options; --events / --overlays / --pollposes are useful for watching
/opt/steamvr/bin/linuxarm64/vrcmd --help

# systemd user units from inside the nested desktop
XDG_RUNTIME_DIR=/run/user/1000 systemctl --user status gamescope-session
```

---

## 14. Glossary

| Term | Meaning |
|---|---|
| **OpenVR** | Valve's API for VR. Apps use `openvr.h`; drivers use `openvr_driver.h`. SteamVR is the runtime that implements it. |
| **OpenXR** | The cross-vendor standard VR API. SteamVR also implements it (`libopenxr_loader.so`, `helloxr`). Not used here. |
| **vrserver / vrcompositor** | SteamVR's core process and its renderer (section 4). |
| **Driver** | A `.so` loaded by vrserver that adds devices (section 5). |
| **Provider** | The driver-wide object (`IServerTrackedDeviceProvider`). |
| **Tracked device** | Anything SteamVR knows the pose of: HMD, controller, tracker. |
| **Pose** | Position + orientation of a device. |
| **Role** | Left hand, right hand, stylus...; decides the user path (section 6). |
| **User path** | `/user/hand/left`, `/user/head`, `/user/stylus`...: role-based names the input system binds to. |
| **Component** | A named input a driver creates, e.g. `/input/trigger/click`. |
| **Input profile** | JSON describing a controller type's inputs and default bindings. |
| **Action / action set** | What an app wants (`LeftClick`), grouped by purpose (`/actions/lasermouse`). |
| **Binding** | The mapping from user-path inputs to an app's actions. |
| **App key** | An input app's identity; the compositor's is `openvr.component.vrcompositor`. |
| **Overlay** | A 2D panel the compositor places in 3D. |
| **Dashboard** | SteamVR's system menu overlay. |
| **Laser mouse** | The compositor's laser pointer that turns overlays into mouse targets. |
| **Quick mouse** | Squeeze-grip shortcut that brings up the laser. |
| **Mailbox** | SteamVR's internal WebSocket message bus on port 27062. |
| **VRLink** | Valve's PC↔headset streaming. |
| **gamescope** | Valve's compositor; here it runs as an OpenVR overlay app and hosts Steam and the desktop. |
| **Nested desktop** | KDE Plasma running as a window inside gamescope. |
| **evdev / EVIOCGRAB** | The kernel's raw input interface, and the call that grabs a device exclusively. |
| **Deckard** | Internal codename of the Steam Frame. |
| **cv driver** | The Frame's built-in SteamVR driver for headset and controllers, fed by XRService. |

---

## 15. Open questions
- **Stylus role:** the compositor accepts a `/user/stylus` pointer for aiming and clicking, but not for dragging overlays (see section 8). Switching role at runtime (stylus while off, hand while on) works (0.5.1). The cost: while laser mode is on, the real left controller loses its laser, because the mouse holds the left-hand slot.
- **Exactly how the compositor picks the pointer device.** Is it interaction events, `SwitchLaserHand`, or something else? The 0.5.0 event logging is meant to answer this.
- **Is there a driver-side signal for "the laser is up"?** It would let the driver grab the mouse only when the laser actually works.
- **The mailbox.** Does it need a `?secret=`, and what does `dump_laser_overlays` return?
- **KWin on the Wayland backend.** Would it fix back/forward buttons, and does gamescope show it properly?

## Sources
- In this repo: [steam-frame-background.md](steam-frame-background.md), [how-it-works.md](how-it-works.md), [development-log.md](development-log.md).
- Parent folder `~/Projects/SteamFrameExploration/` on the original headset (not in this repo): `01-system-overview.md`, `02-display-and-resolution.md`, `03-input-devices.md`, `04-proposed-changes.md`, `05-open-questions.md`, `06-vr-laser-pointer-and-mouse.md`, `07-aspect-ratio-ultrawide.md`, `08-quickmouse-and-laser-mailbox.md`, `09-mouse-back-button-kde.md`.
- Valve's public OpenVR SDK and its wiki (github.com/ValveSoftware/openvr) for the general driver and input concepts.
