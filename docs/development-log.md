# Development log

One short entry per version: what changed, why, and whether it was tested on the headset. How things work is explained in the other docs.

The driver came out of an exploratory session on a Steam Frame with an AI assistant (Claude Code), with the owner approving every change to the system. The findings that led to it are in [steam-frame-background.md](steam-frame-background.md) and [steamvr-primer.md](steamvr-primer.md).

## Versions

**0.1.0.** First version. A virtual controller, aimed by the mouse, drives the compositor's laser. The forward side button toggles it and grabs the mouse. The mouse is found again after it sleeps or reconnects. `install.sh` refuses to register over another `mouselaser` folder. *Headset: works.*

**0.2.0.** The wheel acts as the thumbstick. Steam UI lists read the compositor's `/actions/dualanalog`, so wheel notches drive a virtual stick (`StickStepper`, one notch = one flick), and the tilt wheel drives X. *Headset: works, felt a bit choppy.*

**0.3.0.** Smooth wheel mode, now the default: each notch bumps the stick, which eases back, and spinning holds it. `wheelMode: "step"` keeps the 0.2.0 behaviour. Added labels (en_US, es_ES) for SteamVR's binding screens. A driver can't change its name in Manage Add-ons: the dashboard uses its own table. *Headset: works.*

**0.3.1.** `MouseReader` also stops its thread in its destructor, so a thread still running can't take vrserver down. Added an offline wheel simulation, the MIT license (the OpenVR header is BSD-3 and not vendored) and the demo video.

**0.4.0.** The back side button sends the laser's Back, the same action as the right controller's B. It can't reach KDE: gamescope doesn't forward it, and KWin's nested X11 backend drops X buttons 8 and up (see steam-frame-background.md). *Headset: works in the Steam UI.*

**0.5.0.** The device is a stylus (`/user/stylus`) instead of the left hand, so it doesn't compete with the real left controller. The bindings repeat every hand entry for the stylus. Earlier left-hand attempts were reverted: a device whose pose goes invalid stops being the laser pointer until a new "user interaction", so a quick off/on failed. *Headset: aims and clicks, but can't drag overlays. The compositor attaches drags only to hands or the head, so a stylus drag follows the head.*

**0.5.1.** Hybrid role: stylus while off, `activeRole` (left hand) while on, so dragging works. SteamVR applies role changes live (`event 108`). While laser mode is on, the real left controller loses its laser; `activeRole: 2` moves that to the right hand. Added the SteamVR primer. *Headset: works, including quick off/on.*

**0.6.0.** Fixes "laser mode sometimes needs several presses". On the Frame a grip click *toggles* Laser Mouse ↔ Gamepad mode. The driver held its grip while on, so every activation flipped the mode and nothing flipped it back, and activations alternated between working and failing. Now the grip is pulsed once on the way in and once on the way out, followed by a bumper pulse bound only to `SwitchLaserHand`. Also:
- clicks shorter than a frame are latched;
- the toggle fires on release;
- a vanished mouse keeps laser mode for `reconnectGraceS`;
- Valve's emulated mice are skipped.

Logging every SteamVR event showed that event 525 (dashboard device changed) never reaches drivers, so the driver can't see who owns the laser. *Headset: fixed, but the laser took ~240 ms to appear.*

**0.6.1.** `claimDelayMs` default lowered from 200 to 50 ms. *Headset: every activation showed the laser, about 100 ms after the button is released.*

**0.6.2.** Testing. The driver's logic moved to `src/core.h` (no SteamVR, no system calls) so it can be unit tested; `driver.cpp` keeps the I/O. No intended behaviour change: the old and new on/off sequences were run side by side on random input and matched on every frame. Added:
- doctest unit tests, including a randomised check that grip pulses always pair up;
- a check that the resource files agree with the code and the README;
- sanitizer builds;
- `tools/check-log.sh` for the headset;
- static analysis: stricter compiler warnings, clang-tidy and clang-format configs, and GitHub Actions CI on arm64 running all of it.

The clang-tidy pass led to small fixes: thread-safe `strerror_r` for errors logged from the mouse thread, the device owned by a `std::unique_ptr`, compiler-checked log formats, and a shorter `LoadSettings`.

The new checks found two cases of drift, both fixed: four settings missing from `default.vrsettings`, and missing or stale input labels. The docs now describe the stylus disconnecting while off, which is what the code has always done. See [testing.md](testing.md). *Headset: behaves like 0.6.1. Toggling, aim and click, drag, wheel, Back, the real controllers and unplug/replug all pass, and every activation showed the laser.*

**0.6.3.** Fixes from the code review, each with unit tests:
- **Settings are checked.** Out-of-range numbers are clamped, and a role or button that can't work goes back to its default. Example: with `toggleButton: 275`, Back stayed on 275 and could never fire; it's now turned off. Each correction is logged as `setting ...`, a value of the wrong type as "can't be read", and the values in use as one `settings: ...` line.
- **Mouse choice.** A mouse with the toggle button wins over one without, which matters for receivers that create several mouse-like devices. If the chosen mouse has no toggle button, the log warns that laser mode can't be switched on. Valve devices and mice not matching `deviceNameFilter` are logged once as `skipped`.
- **Lost events.** When the kernel drops mouse events (`SYN_DROPPED`), the rest of that batch is ignored and the buttons are read again, so a lost release can't leave a click held.
- **Less log noise.** SteamVR events are no longer logged; 0.6.0 had already found what they could tell. The queue is emptied as before 0.5.0.
- **Build.** The version is set once, in `CMakeLists.txt`. An x86_64 build goes to `bin/linux64` instead of `bin/linuxarm64`.

`tools/check-log.sh` reports the new lines. *Headset: behaves like 0.6.2. With the default settings there are no corrections and no unreadable values, so SteamVR does read `default.vrsettings`, and the SteamVR event lines are gone. A real `SYN_DROPPED` happened during the test and the buttons were re-read. A long unplug in laser mode ended it after the 60 s grace time.*

**0.7.0.** Settings page and live settings.
- `settingsschema.vrsettings` adds a "Mouse Laser" section to SteamVR's settings, the way Babble adds "Expression Tracking": a JSON file SteamVR draws, no UI code. It has sensitivity, invert vertical aim, wheel mode, the laser mode and Back buttons, the laser hand and (advanced) the switch-on delay.
- Settings changed while running apply within a second: the driver re-reads them on `VREvent_AnyDriverSettingsChanged`, and every second in case drivers don't get that event. The log says which one noticed. Buttons, roles, the device filter and the reconnect grace time still need a SteamVR restart: the mouse thread reads them, or they change the device's role. The page marks them, and the log names them.
- New checks keep the page, the code and the README in step.
- The section didn't show at first. vrserver reads the `settingsschema.vrsettings` of every registered driver, disabled ones too, and tags each section with `"driver": <driver name>`. The dashboard hides a tagged section unless that driver runs the headset, or the section sets `show_without_hmd`. Babble's section sets it; ours now does too, and the resource test checks it.

*Headset: the page shows and every control works. Sensitivity, Invert Vertical Aim and Wheel applied live, always noticed by the settings event (never the 1 s check). Laser Hand logged that it needs a restart, and after one the right controller lost its laser while laser mode was on. Choosing the Back button for laser mode turns the Back button off, as the page says.*

## State
- Current version: 0.7.0-experimental, tested on the headset (0.6.2 and 0.6.3 were tested on the way).
- Nothing is vendored. The driver builds against SteamVR's bundled `openvr_driver.h`; the tests don't need it.
