# Development log

## Origin (2026-10-01)
This project came out of one exploratory session. The owner asked an AI assistant (Claude, running as Claude Code in a Konsole inside the Frame's nested KDE desktop) to analyse the Steam Frame's environment, document it, and possibly change some things. The session started **read-only**. Write access was granted later, step by step, and every system-touching action was announced first and approved by the owner.

The broader notes from that session (hardware, display pipeline, KDE scale, ultrawide options, input devices) live in the parent exploration folder `~/Projects/SteamFrameExploration/` (`README.md` plus numbered `0X-*.md` files) on the original headset. They aren't part of this repo. The parts relevant here are condensed in [steam-frame-background.md](steam-frame-background.md).

## Timeline
1. **Exploration.** We found that KDE runs nested in gamescope's VR overlay, and that the mouse belongs to gamescope while the laser belongs to vrcompositor.
2. **Quick mouse investigation.** It turned out to be only a grip-to-activate shortcut. Its binding showed that the laser takes any pose source, and the Frame HMD already binds a head-gaze laser.
3. **Driver written** (`mouselaser`, ~400 lines of C++). Built on-device with gcc 15.1 / cmake 3.29 / ninja against SteamVR's bundled `openvr_driver.h` (API 2.1.0).
4. **Installed** with `vrpathreg adddriver`, after a backup of `openvrpaths.vrpath`. Then the owner restarted SteamVR.
5. **Verified working**, from the owner's report and from `vrserver.txt`:
   ```
   [Info] - mouselaser: mouselaser: ACTIVE (mouse drives the laser)
   [Info] - mouselaser: mouselaser: inactive (mouse back to desktop)
   [Info] - [Workshop] Successfully loaded binding file '.../vrcompositor_bindings_mouselaser.json' for app 'openvr.component.vrcompositor'.
   [Info] - mouselaser: mouselaser: mouse disappeared
   [Info] - mouselaser: mouselaser: using /dev/input/event6 (MCHOSE G3 A Mouse)
   ```
   - The toggle works, and the compositor loaded our bindings.
   - The mouse is re-found after it sleeps or reconnects. Its event node moved from event5 to event6 and was still found.
6. **Moved into this repo** as `0.1.0-experimental`. Changes made while copying:
   - Removed the doubled log prefix: SteamVR already adds `mouselaser: `.
   - Added a version line on startup.
   - `install.sh` now refuses to register if a `mouselaser` driver is already registered from another folder.

7. **Repo build tested** (2026-10-01, 20:10). The old registration was removed, `./install.sh` was run from the repo, and the owner rebooted. The owner confirmed it works. The log shows a single `mouselaser:` prefix, which confirms the repo build is the one loading, and toggling works.

8. **0.2.0: wheel works as the thumbstick** (2026-10-01). The owner asked for the mouse wheel to act like the controller joystick, to scroll lists.
   - Finding: Steam UI lists read the compositor's `/actions/dualanalog`; the Frame `steam.client` binding is haptics-only. Our bindings only had `scroll_discrete`.
   - Change: added `dualanalog` (position + touch) and `scroll_smooth` bindings. Replaced the 6-frame pulse with a time-based `StickStepper` (one notch = one flick). The tilt wheel drives stick X. Four new `wheel*` settings.
   - Verified offline with a 120 Hz simulation: one notch gives one push, fast notches give separate pushes, and the queue is capped.
   - **On-headset test: works** (owner, after a reboot). Lists scroll with the wheel, but it feels "a bit choppy". Tuning or a smoother mode is a follow-up.

9. **Manage Add-ons.** The driver appears in SteamVR's developer *Manage Add-ons* list, which is expected. From the dashboard JS (`chunk~*.js`): the list shows non-`resourceOnly` drivers, its toggle calls `setEnabled`, which writes `driver_<name>.enable` (the same key our driver reads), and **Unblock** clears `blocked_by_safe_mode`. Documented in the README as an in-VR kill switch.

10. **0.3.0: smoother wheel and tidier texts** (2026-10-01). The owner found 0.2.0's scrolling "a bit choppy" and asked for some adjustment, plus a tidy-up of the text SteamVR shows.
    - Added the `smooth` wheel mode as the default: each notch bumps the stick, which eases back, and spinning holds it (see how-it-works). The 0.2.0 behaviour is still available as `wheelMode: "step"`. New settings: `wheelMode`, `wheelSmoothMin`, `wheelSmoothImpulse`, `wheelSmoothHoldMs`, `wheelSmoothDecayMs`.
    - First tuning (hold 50 ms, decay 120 ms, impulse 0.25) dipped to about 0.45 between notches when spinning at 10/s, which risks retriggering steps. Retuned to 80 / 150 / 0.3, which ramps to a steady hold. Verified offline only.
    - Texts: added `resources/localization/localization.json` (en_US, es_ES) for SteamVR's binding screens. Model `Mouse Laser (virtual)`, manufacturer `frame-unboundedMouse-vibed`, and a clearer binding name and description.
    - The display name in Manage Add-ons can't be changed from the driver: the dashboard looks it up in its own `driverPrettyNames` table and otherwise falls back to the manifest name `mouselaser`.
    - **On-headset test: working** (owner, after a reboot).

11. **Pre-commit review, 0.3.1** (2026-10-01).
    - Logs: clean load, activation and binding load. No crash. The only vrserver coredump predates the driver (2026-09-28). `no suitable devices` and `steam.client … no configured binding` are benign.
    - A clean build from only the files that would be committed works. clang `-Wall -Wextra -Wshadow` reports nothing in our code. No personal data in the tree.
    - **Fix:** `MouseReader` now stops its thread in its destructor. Previously, if SteamVR destroyed the device without `Deactivate()`, the joinable `std::thread` would `std::terminate` vrserver. This never happened in the logs; it is defensive.
    - Added `tests/wheel_sim.cpp` (optional, `-DMOUSELASER_TESTS=ON`) and `CMAKE_EXPORT_COMPILE_COMMANDS`, so clangd finds `openvr_driver.h`.
    - 0.3.1 is built but **not yet rebooted into**.

12. **License: MIT** (2026-10-01).
    - The owner asked for an open-source license. The only Valve code involved is the OpenVR driver header. It is BSD-3-Clause in the public SDK and **not vendored**: we compile against SteamVR's local copy.
    - MIT was chosen over GPL because the driver is a plugin loaded into proprietary `vrserver`, where GPL plugin licensing is a grey area. MIT is also common for SteamVR drivers.
    - Copyright holder: `Andalu30` (the GitHub handle from the remote). The README notes third-party code and trademarks.

13. **Demo media** (2026-10-01). The owner recorded `~/Videos/mousedemo.mp4` (44.2 s, 1920×1080, 20 fps). It was trimmed (first 3 s and last 2 s removed, leaving 39.2 s) and converted to `docs/media/demo.webp`, which the README shows as the hero image.

## State at hand-off
- The driver is registered from this repo (`<repo>/driver/mouselaser`). The old prototype registration has been removed.
- Nothing is vendored. The build depends on SteamVR's bundled header.
- Licensed MIT (see `LICENSE`). SPDX tags are on the sources.
